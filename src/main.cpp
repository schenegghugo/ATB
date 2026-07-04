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
#include "data/MapJson.h"
#include "data/Net.h"
#include "data/RulesetJson.h"
#include "net/GameServer.h"   // contentHashOf
#include "net/MirrorSession.h"
#include "render/Animator.h"
#include "render/BuildEditorScreen.h"
#include "render/ContentPaths.h"
#include "render/MatchSource.h"
#include "render/RemoteMatchSource.h"
#include "render/Renderer.h"
#include "render/SpritePack.h"

#include "raylib.h"

#include <algorithm>
#include <cstdlib>
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
    std::optional<Grid> staticArena;                        // set when rules.arena.map is used
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

// The stable catalog slug for a runtime spell (used to resolve future 2.2 icons).
// No core change: the runtime Spell carries no key, so we match it by name — spell
// names are unique in the catalog. Empty if unknown (e.g. a summon's innate).
std::string catalogKey(const SpellCatalog& catalog, const std::string& spellName) {
    for (const SpellDef& d : catalog.all())
        if (d.spell.name == spellName) return d.key;
    return "";
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

    // Static map (if the ruleset names one): load data/maps/<map>.json once.
    if (!session.ruleset.arena.map.empty()) {
        const std::string rel = "maps/" + session.ruleset.arena.map + ".json";
        std::optional<std::string> path = render::findContent(rel);
        MapLoad load = path ? loadMapFromFile(*path) : MapLoad{};
        if (!path) load.errors.push_back("map file not found: " + rel);
        if (!load.ok) {
            TraceLog(LOG_ERROR, "Arena map '%s' is invalid:", rel.c_str());
            for (const std::string& e : load.errors) TraceLog(LOG_ERROR, "  - %s", e.c_str());
            return 1;
        }
        session.staticArena = std::move(load.grid);
        TraceLog(LOG_INFO, "Loaded static map '%s' (%dx%d)", rel.c_str(),
                 session.staticArena->width(), session.staticArena->height());
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

    // Optional presentation pack (art/palette). ATB_PACK=<dir> points at a folder
    // holding pack.json; absent or unloadable → the built-in primitives (identical
    // to before). A pack is client-side cosmetic only — it never touches rules.
    std::optional<render::SpritePack> pack;
    if (const char* pk = std::getenv("ATB_PACK"); pk && *pk) {
        pack.emplace();
        std::vector<std::string> errs;
        if (pack->load(pk, errs)) {
            TraceLog(LOG_INFO, "Loaded sprite pack '%s' (%s)", pk, pack->name().c_str());
        } else {
            TraceLog(LOG_WARNING, "Sprite pack '%s' failed to load — using primitives:", pk);
            for (const std::string& e : errs) TraceLog(LOG_WARNING, "  - %s", e.c_str());
            pack.reset();
        }
    }

    render::BuildEditorScreen editor(session.catalog, *session.repo, session.ruleset);

    AppState state = AppState::Editor;
    // The source of match truth behind a seam: LocalMatchSource drives the Battle
    // in-process (a future RemoteMatchSource would mirror a server). The UI feeds
    // it player Intents and reads source->battle() to render (Phase 4.2).
    std::unique_ptr<render::MatchSource> source;
    render::Animator animator; // per-entity event-clip playback (cast flashes, §2.4)
    std::string status;
    int selectedSpell = 0;
    int logScroll = 0; // combat-log scrollback (0 = pinned to newest)

    // Build the match source. With ATB_CONNECT=host[:port] set, join a networked
    // match on that server (RemoteMatchSource — same render path, authoritative
    // server); otherwise drive an in-process Battle (LocalMatchSource). A failed
    // connect logs and falls back to local so the game is still playable.
    auto newMatch = [&]() -> std::unique_ptr<render::MatchSource> {
        if (const char* conn = std::getenv("ATB_CONNECT"); conn && *conn) {
            std::string hp = conn;
            const auto colon = hp.find(':');
            const std::string host = colon == std::string::npos ? hp : hp.substr(0, colon);
            const uint16_t port = static_cast<uint16_t>(
                colon == std::string::npos ? 5555 : std::atoi(hp.substr(colon + 1).c_str()));
            std::string err;
            std::unique_ptr<net::MirrorSession> ms = net::MirrorSession::connect(
                host, port, net::contentHashOf(session.catalog), editor.playerTeam().front(),
                session.ruleset, session.catalog, session.creatures, &err);
            if (ms) {
                TraceLog(LOG_INFO, "Connected to %s — playing as %s.", conn,
                         ms->seat() == Faction::Player ? "player" : "enemy");
                return std::make_unique<render::RemoteMatchSource>(std::move(ms));
            }
            TraceLog(LOG_ERROR, "Remote connect to %s failed: %s — using a local match.", conn,
                     err.c_str());
        }
        return std::make_unique<render::LocalMatchSource>(
            buildMatch(session.ruleset, editor.playerTeam(), editor.enemyTeam(), session.catalog,
                       /*seed=*/0, session.creatures,
                       session.staticArena ? &*session.staticArena : nullptr));
    };

    auto enterBattle = [&]() {
        source = newMatch();
        animator.reset();
        selectedSpell = 0;
        logScroll = 0;
        status = "Player turn — left-click move, click a spell (or 1-9), right-click cast, Tab=editor.";
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
        const bool hoveredValid = source->battle().grid().inBounds(hovered);

        // Combat-log scrollback: wheel up = older, down = newer (clamped ≥ 0).
        logScroll = std::max(0, logScroll + static_cast<int>(GetMouseWheelMove()));

        if (IsKeyPressed(KEY_TAB)) { state = AppState::Editor; continue; }
        if (IsKeyPressed(KEY_R)) {
            source = newMatch();
            animator.reset();
            selectedSpell = 0;
            logScroll = 0;
            status = "New arena. Player turn.";
        }

        const bool finished = source->battle().phase() == Phase::Finished;
        const EntityId active = finished ? 0 : source->battle().activeUnit();
        // The seam decides who drives: the local player inputs only for their own
        // Champions; summons (either team) are AI and objects (bombs) auto-pass —
        // all handled by source->update().
        const bool playerControl = source->awaitingLocalInput();

        // Which spell button (if any) the cursor is over — set inside the player
        // block below; also read later when composing the view (hover tooltip).
        int hoveredSpell = -1;

        if (playerControl) {
            const EntityId me = active;
            const int spellCount = static_cast<int>(source->battle().unit(me).spells.size());
            for (int k = 0; k < spellCount && k < 9; ++k)
                if (IsKeyPressed(KEY_ONE + k)) selectedSpell = k;
            if (selectedSpell >= spellCount) selectedSpell = 0;

            // Hit-test the spell buttons against the same rects the renderer draws.
            for (int s = 0; s < spellCount; ++s)
                if (render::spellSlotRect(layout, source->battle().grid(), s, spellCount)
                        .contains(GetMouseX(), GetMouseY())) {
                    hoveredSpell = s;
                    break;
                }

            // Spell bar is hit-tested *before* the board so a HUD click selects a
            // spell instead of being read as a move. Selection is pure UI state; the
            // move/cast/endTurn actions go to the seam as Intents.
            if (hoveredSpell >= 0 && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                selectedSpell = hoveredSpell;
                status = TextFormat("Selected %s.",
                                    source->battle().unit(me).spells[selectedSpell].name.c_str());
            } else if (hoveredValid && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                if (auto s = source->submit(net::Intent::move(hovered))) status = *s;
            }
            if (hoveredValid && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
                if (auto s = source->submit(net::Intent::cast(selectedSpell, hovered))) status = *s;
            }
            if (IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_ENTER)) {
                source->submit(net::Intent::endTurn());
            }
        } else if (!finished) {
            if (auto s = source->update(dt)) status = *s;
        }

        if (finished) {
            auto w = source->battle().winner();
            status = (w && *w == Faction::Player) ? "Victory! Tab=editor, R=rematch."
                                                  : "Defeat. Tab=editor, R=rematch.";
        }

        render::ViewState view;
        view.hoveredTile = hovered;
        view.hoveredValid = hoveredValid;
        view.statusLine = status;
        view.windowW = GetScreenWidth();
        view.windowH = GetScreenHeight();
        view.logScroll = logScroll;
        if (playerControl) {
            const EntityId me = active;
            const Entity& u = source->battle().unit(me);
            view.reachable =
                reachableWithin(source->battle().grid(), u.pos, u.mp, source->battle().occupancy(me));
            view.showLosToHover = true;
            view.showSpellBar = true;
            view.selectedSpell = selectedSpell;
            // Hovering a button previews *that* spell; otherwise show the selected one.
            view.spellLabel = spellLabel(u, hoveredSpell >= 0 ? hoveredSpell : selectedSpell);
            for (const Spell& sp : u.spells) view.spellIconKeys.push_back(catalogKey(session.catalog, sp.name));
            if (hoveredValid && selectedSpell < static_cast<int>(u.spells.size())) {
                view.spellCastable = source->battle().canCast(me, selectedSpell, hovered);
                view.spellZone = source->battle().affectedTiles(u.spells[selectedSpell], u.pos, hovered);
            }
        }

        // Trigger cast-clip animations off the same event stream the log reads.
        animator.sync(source->battle(), GetTime());

        BeginDrawing();
        render::drawFrame(layout, source->battle(), view, pack ? &*pack : nullptr, &animator);
        EndDrawing();
    }

    if (pack) pack->unload(); // free textures while the GL context is still alive
    CloseWindow();
    return 0;
}
