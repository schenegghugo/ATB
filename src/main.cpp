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
#include "core/Grid.h"
#include "core/Spells.h"
#include "data/BuildRepository.h"
#include "render/BuildEditorScreen.h"
#include "render/Renderer.h"

#include "raylib.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace tb;

namespace {

enum class AppState { Editor, Battle };

struct Session {
    SpellCatalog catalog = makeDefaultCatalog();
    BuildRules rules{};
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

Battle makeBattle(Session& s, const ArenaConfig& cfg, const CharacterBuild& player,
                  const CharacterBuild& enemy) {
    Grid grid = generateArena(cfg);
    std::vector<Entity> roster;
    roster.push_back(instantiate(player, s.catalog, Faction::Player, cfg.playerSpawn, s.rules));
    roster.push_back(instantiate(enemy, s.catalog, Faction::Enemy, cfg.enemySpawn, s.rules));
    return Battle(std::move(grid), std::move(roster));
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
    session.repo->save(pyromancerBuild()); // seed the store (stands in for the DB)
    session.repo->save(bruiserBuild());

    render::Layout layout;
    ArenaConfig cfg;
    // Window is sized for the battle arena; the editor lays out within it.
    const int sw = layout.screenWidth(Grid(cfg.width, cfg.height));
    const int sh = layout.screenHeight(Grid(cfg.width, cfg.height));

    SetConfigFlags(FLAG_MSAA_4X_HINT);
    InitWindow(sw, sh, "Tactical Battler — POC");
    if (!IsWindowReady()) {
        // No display/GL context (e.g. headless, or Wayland without the Wayland
        // backend compiled in). Bail cleanly instead of crashing in the GL calls.
        TraceLog(LOG_ERROR, "Window/GL context unavailable — cannot run the GUI. "
                            "Run from a graphical session, or rebuild with the Wayland backend.");
        return 1;
    }
    SetTargetFPS(60);

    render::BuildEditorScreen editor(session.catalog, *session.repo, session.rules);

    AppState state = AppState::Editor;
    std::optional<Battle> battle;
    CharacterBuild playerBuild;
    std::string status;
    int selectedSpell = 0;

    constexpr float kAiTick = 0.35f;
    float aiTimer = 0.0f;

    auto enterBattle = [&]() {
        cfg.seed = 0;
        battle.emplace(makeBattle(session, cfg, playerBuild, editor.enemyBuild()));
        selectedSpell = 0;
        aiTimer = 0.0f;
        status = "Player turn — left-click move, 1-9 pick spell, right-click cast, Tab=editor.";
        state = AppState::Battle;
    };

    while (!WindowShouldClose()) {
        const float dt = GetFrameTime();

        if (state == AppState::Editor) {
            BeginDrawing();
            if (editor.runFrame(sw, sh) == render::BuildEditorScreen::Result::Fight) {
                playerBuild = editor.playerBuild();
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
            cfg.seed = 0;
            battle.emplace(makeBattle(session, cfg, playerBuild, editor.enemyBuild()));
            selectedSpell = 0;
            status = "New arena. Player turn.";
        }

        if (battle->phase() == Phase::PlayerTurn) {
            const EntityId me = battle->activeUnit();
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
                status = "Enemy turn...";
                aiTimer = 0.0f;
            }
        } else if (battle->phase() == Phase::EnemyTurn) {
            aiTimer += dt;
            if (aiTimer >= kAiTick) {
                aiTimer = 0.0f;
                AIAction act = enemyTakeOneAction(*battle);
                if (act == AIAction::Attacked) status = "Enemy casts a spell!";
                else if (act == AIAction::Moved) status = "Enemy advances...";
                else { battle->endTurn(); status = "Player turn."; }
            }
        }

        if (battle->phase() == Phase::Finished) {
            auto w = battle->winner();
            status = (w && *w == Faction::Player) ? "Victory! Tab=editor, R=rematch."
                                                  : "Defeat. Tab=editor, R=rematch.";
        }

        render::ViewState view;
        view.hoveredTile = hovered;
        view.hoveredValid = hoveredValid;
        view.statusLine = status;
        if (battle->phase() == Phase::PlayerTurn) {
            const EntityId me = battle->activeUnit();
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
