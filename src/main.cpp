//
// main.cpp — Raylib frontend / application loop.
//
// Top-level state machine over two screens:
//   - Editor: author a classless point-buy build (BuildEditorScreen)
//   - Battle: fight with the chosen build vs. a selected enemy preset
//
// Characters are materialised from CharacterBuilds against the SpellCatalog (the
// skill dictionary); all rules live in core/.
//
//   Battle controls:
//     Left click   : move the active player unit along the shortest path
//     Right click  : cast the SELECTED spell at the hovered tile
//     1..9         : select a spell from the active unit's loadout
//     Space/Enter  : end the player's turn
//     R            : regenerate the arena (keep builds)
//     Tab          : return to the build editor
//     Esc          : quit
//
#include "core/AI.h"
#include "core/Battle.h"
#include "core/Build.h"
#include "core/Creatures.h"
#include "core/Grid.h"
#include "core/Match.h"
#include "core/Ruleset.h"
#include "core/Spells.h"
#include "data/BuildRepository.h"
#include "data/CatalogJson.h"
#include "data/CreatureJson.h"
#include "data/RulesetJson.h"
#include "render/BuildEditorScreen.h"
#include "render/ContentPaths.h"
#include "render/Renderer.h"

#include "raylib.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace tb;

namespace {

enum class AppState { Editor, Battle };

struct Session {
    SpellCatalog catalog = makeDefaultCatalog();
    std::vector<Entity> creatures = makeDefaultCreatures(); // bestiary (Summon effects)
    Ruleset ruleset = makeDefaultRuleset();                 // economy + ring + arena + format
    std::unique_ptr<BuildRepository> repo = std::make_unique<InMemoryBuildRepository>();
};

// Two starter presets so the editor and enemy picker have content (stands in for
// a pre-populated builds table).
CharacterBuild pyromancerBuild() {
    CharacterBuild b;
    b.name = "Pyromancer";
    b.stats.bonusAp = 1;
    b.spellIds = {spellid::Attack, spellid::Fireball, spellid::Poison};
    return b;
}
CharacterBuild bruiserBuild() {
    CharacterBuild b;
    b.name = "Bruiser";
    b.stats.hpPurchases = 4;
    b.stats.bonusMp = 1;
    b.spellIds = {spellid::Attack, spellid::Knockback, spellid::Harpoon};
    return b;
}

std::string spellLabel(const Entity& u, int slot) {
    if (slot < 0 || slot >= static_cast<int>(u.spells.size())) return "";
    const Spell& sp = u.spells[slot];
    const int cd = slot < static_cast<int>(u.spellCooldowns.size()) ? u.spellCooldowns[slot] : 0;
    if (cd > 0)
        return TextFormat("[%d] %s  %d AP  rng %d-%d  COOLDOWN %d", slot + 1, sp.name.c_str(),
                          sp.apCost, sp.minRange, sp.maxRange, cd);
    return TextFormat("[%d] %s  %d AP  rng %d-%d", slot + 1, sp.name.c_str(), sp.apCost,
                      sp.minRange, sp.maxRange);
}

} // namespace

int main() {
    Session session;

    // Load the spell catalog from data/catalog.json. Policy:
    //   - found & valid    -> use it
    //   - absent           -> fall back to the built-in default (with a notice)
    //   - present & invalid -> fail loudly before opening a window (never
    //                          silently fall back — that would hide corruption)
    if (std::optional<std::string> path = render::findContent("catalog.json")) {
        CatalogLoad load = loadCatalogFromFile(*path);
        if (!load.ok) {
            TraceLog(LOG_ERROR, "Catalog '%s' is invalid:", path->c_str());
            for (const std::string& e : load.errors) TraceLog(LOG_ERROR, "  - %s", e.c_str());
            return 1;
        }
        session.catalog = std::move(load.catalog);
        TraceLog(LOG_INFO, "Loaded catalog '%s' v%s (sha256 %.12s…)", path->c_str(),
                 load.version.c_str(), load.sha256.c_str());
    } else {
        TraceLog(LOG_WARNING, "No data/catalog.json found — using the built-in default catalog.");
    }

    // Same policy for the bestiary (data/creatures.json).
    if (std::optional<std::string> path = render::findContent("creatures.json")) {
        CreatureLoad load = loadCreaturesFromFile(*path);
        if (!load.ok) {
            TraceLog(LOG_ERROR, "Bestiary '%s' is invalid:", path->c_str());
            for (const std::string& e : load.errors) TraceLog(LOG_ERROR, "  - %s", e.c_str());
            return 1;
        }
        session.creatures = std::move(load.creatures);
        TraceLog(LOG_INFO, "Loaded bestiary '%s' v%s (%zu creatures)", path->c_str(),
                 load.version.c_str(), session.creatures.size());
    } else {
        TraceLog(LOG_WARNING, "No data/creatures.json found — using the built-in default bestiary.");
    }

    // Same policy for the match ruleset (data/rules.json) — economy, ring, arena, format.
    if (std::optional<std::string> path = render::findContent("rules.json")) {
        RulesetLoad load = loadRulesetFromFile(*path);
        if (!load.ok) {
            TraceLog(LOG_ERROR, "Ruleset '%s' is invalid:", path->c_str());
            for (const std::string& e : load.errors) TraceLog(LOG_ERROR, "  - %s", e.c_str());
            return 1;
        }
        session.ruleset = std::move(load.ruleset);
        TraceLog(LOG_INFO, "Loaded ruleset '%s' v%s (teamSize %d)", path->c_str(),
                 load.version.c_str(), session.ruleset.teamSize);
    } else {
        TraceLog(LOG_WARNING, "No data/rules.json found — using the built-in default ruleset.");
    }

    session.repo->save(pyromancerBuild()); // seed the store (stands in for the DB)
    session.repo->save(bruiserBuild());

    render::Layout layout;
    // Open large enough for both the arena (sized from the ruleset) and the
    // (responsive) build editor; the editor reads the live window size each frame,
    // so it adapts to resizes / tiling window managers (e.g. Sway).
    const int arenaW = layout.screenWidth(Grid(session.ruleset.arena.width, session.ruleset.arena.height));
    const int arenaH = layout.screenHeight(Grid(session.ruleset.arena.width, session.ruleset.arena.height));
    const int sw = std::max(arenaW, 1180);
    const int sh = std::max(arenaH, 720);

    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE);
    InitWindow(sw, sh, "Tactical Battler — POC");
    if (!IsWindowReady()) {
        // No display/GL context (e.g. headless, or Wayland without the Wayland
        // backend compiled in). Bail cleanly instead of crashing in the GL calls.
        TraceLog(LOG_ERROR, "Window/GL context unavailable — cannot run the GUI. "
                            "Run from a graphical session, or rebuild with the Wayland backend.");
        return 1;
    }
    SetTargetFPS(60);

    render::BuildEditorScreen editor(session.catalog, *session.repo, session.ruleset);

    AppState state = AppState::Editor;
    std::optional<Battle> battle;
    std::string status;
    int selectedSpell = 0;

    constexpr float kAiTick = 0.35f;
    float aiTimer = 0.0f;

    auto enterBattle = [&]() {
        battle.emplace(buildMatch(session.ruleset, editor.playerTeam(), editor.enemyTeam(),
                                  session.catalog, /*seed=*/0, session.creatures));
        selectedSpell = 0;
        aiTimer = 0.0f;
        status = "Player turn — left-click move, 1-9 pick spell, right-click cast, Tab=editor.";
        state = AppState::Battle;
    };

    while (!WindowShouldClose()) {
        const float dt = GetFrameTime();

        if (state == AppState::Editor) {
            BeginDrawing();
            if (editor.runFrame(GetScreenWidth(), GetScreenHeight()) ==
                render::BuildEditorScreen::Result::Fight) {
                enterBattle();
            }
            EndDrawing();
            continue;
        }

        // -------------------------- Battle state -----------------------------
        Vec2i hovered = render::screenToGrid(layout, GetMouseX(), GetMouseY());
        const bool hoveredValid = battle->grid().inBounds(hovered);

        if (IsKeyPressed(KEY_TAB)) { state = AppState::Editor; continue; }
        if (IsKeyPressed(KEY_R)) {
            battle.emplace(buildMatch(session.ruleset, editor.playerTeam(), editor.enemyTeam(),
                                      session.catalog, /*seed=*/0, session.creatures));
            selectedSpell = 0;
            status = "New arena. Player turn.";
        }

        const bool finished = battle->phase() == Phase::Finished;
        const EntityId active = finished ? 0 : battle->activeUnit();
        // Drive turns by control, not team: a player only inputs for their own
        // Champions; summons (either team) are AI, and objects (bombs) auto-pass.
        const Control ctrl = finished ? Control::AI : battle->controlOf(active);
        const bool playerControl = !finished && ctrl == Control::Player;

        if (playerControl) {
            const EntityId me = active;
            const int spellCount = static_cast<int>(battle->unit(me).spells.size());
            for (int k = 0; k < spellCount && k < 9; ++k)
                if (IsKeyPressed(KEY_ONE + k)) selectedSpell = k;
            if (selectedSpell >= spellCount) selectedSpell = 0;

            if (hoveredValid && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                int moved = battle->moveToward(me, hovered);
                status = moved > 0 ? TextFormat("Moved %d tile(s).", moved) : "Can't move there.";
            }
            if (hoveredValid && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
                if (battle->cast(me, selectedSpell, hovered))
                    status = TextFormat("Cast %s!", battle->unit(me).spells[selectedSpell].name.c_str());
                else
                    status = "Cast failed (check AP, range, LOS).";
            }
            if (IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_ENTER)) {
                battle->endTurn();
                aiTimer = 0.0f;
            }
        } else if (!finished) {
            aiTimer += dt;
            if (aiTimer >= kAiTick) {
                aiTimer = 0.0f;
                if (ctrl == Control::Inert) {
                    battle->endTurn(); // a bomb: its fuse/ignition ticked at turn start
                } else {
                    const std::string who = battle->unit(active).name;
                    AIAction act = enemyTakeOneAction(*battle);
                    if (act == AIAction::Attacked) status = who + " casts a spell.";
                    else if (act == AIAction::Moved) status = who + " moves.";
                    else battle->endTurn();
                }
            }
        }

        if (finished) {
            auto w = battle->winner();
            status = (w && *w == Faction::Player) ? "Victory! Tab=editor, R=rematch."
                                                  : "Defeat. Tab=editor, R=rematch.";
        }

        render::ViewState view;
        view.hoveredTile = hovered;
        view.hoveredValid = hoveredValid;
        view.statusLine = status;
        if (playerControl) {
            const EntityId me = active;
            const Entity& u = battle->unit(me);
            view.reachable = reachableWithin(battle->grid(), u.pos, u.mp, battle->occupancy(me));
            view.showLosToHover = true;
            view.spellLabel = spellLabel(u, selectedSpell);
            if (hoveredValid && selectedSpell < static_cast<int>(u.spells.size())) {
                view.spellCastable = battle->canCast(me, selectedSpell, hovered);
                view.spellZone = battle->affectedTiles(u.spells[selectedSpell], u.pos, hovered);
            }
        }

        BeginDrawing();
        render::drawFrame(layout, *battle, view);
        EndDrawing();
    }

    CloseWindow();
    return 0;
}
