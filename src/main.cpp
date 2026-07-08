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
#include "net/Lobby.h"
#include "net/MirrorSession.h"
#include "render/Animator.h"
#include "render/BuildEditorScreen.h"
#include "render/ConnectScreen.h"
#include "render/ContentPaths.h"
#include "render/CorrespondenceMatchSource.h"
#include "render/LobbyScreen.h"
#include "render/MainMenuScreen.h"
#include "render/MatchSource.h"
#include "render/ReadyCheckScreen.h"
#include "render/RemoteMatchSource.h"
#include "render/Renderer.h"
#include "render/SettingsScreen.h"
#include "render/SpritePack.h"
#include "render/Ui.h"

#include "raylib.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace tb;

namespace {

enum class AppState { Menu, Editor, Connect, Lobby, ReadyCheck, Battle, Settings };

struct Session {
    SpellCatalog catalog = makeDefaultCatalog();
    std::vector<Entity> creatures = makeDefaultCreatures(); // bestiary (Summon effects)
    Ruleset ruleset = makeDefaultRuleset();                 // economy + ring + arena + format
    std::optional<Ruleset> rankedRuleset;                   // data/rules.ranked.json (rated online)
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

    // The official ranked ruleset (data/rules.ranked.json) — needed to build an
    // identical mirror for a RATED online game. Optional: absent → rated online
    // play is simply unavailable (the lobby's rated toggle is disabled).
    if (std::optional<std::string> path = render::findContent("rules.ranked.json")) {
        RulesetLoad load = loadRulesetFromFile(*path);
        if (load.ok) {
            session.rankedRuleset = std::move(load.ruleset);
            TraceLog(LOG_INFO, "Loaded ranked ruleset '%s' (rated online enabled)", path->c_str());
        } else {
            TraceLog(LOG_WARNING, "data/rules.ranked.json is invalid — rated online disabled.");
        }
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
    render::MainMenuScreen menu;
    render::SettingsScreen settings;
    render::LobbyScreen lobbyScreen;
    render::ReadyCheckScreen readyScreen;
    std::unique_ptr<net::LobbySession> lobby; // live lobby connection (Online Home)
    std::string lobbyHost = "127.0.0.1";      // parsed from the connect form on join
    uint16_t lobbyPort = 5555;
    AppState editorReturn = AppState::Menu;   // where the build editor returns to

    AppState state = AppState::Menu;
    // Which action the build editor was entered for (set by the mode-first menu).
    render::BuildEditorScreen::Mode editorMode = render::BuildEditorScreen::Mode::Edit;
    bool quit = false;
    // The source of match truth behind a seam: LocalMatchSource drives the Battle
    // in-process (a future RemoteMatchSource would mirror a server). The UI feeds
    // it player Intents and reads source->battle() to render (Phase 4.2).
    std::unique_ptr<render::MatchSource> source;
    render::Animator animator; // per-entity event-clip playback (cast flashes, §2.4)
    std::string status;
    int selectedSpell = 0;
    int logScroll = 0;             // combat-log scrollback (0 = pinned to newest)
    bool onlineMatch = false;      // this battle came from the lobby (→ end screen returns there)
    float turnClock = 0.0f;        // seconds left in the active seat's move (timed matches)
    EntityId lastActive = 0;       // detect turn changes to reset turnClock
    std::string chatDraft;         // in-match chat being typed
    bool chatFocused = false;      // chat input has keyboard focus

    // The networking screen is seeded from the ATB_* env vars (so they still work
    // as defaults), then edited live in the GUI.
    render::ConnectScreen::Params netDefaults;
    if (const char* c = std::getenv("ATB_CONNECT"); c && *c) netDefaults.host = c;
    if (const char* u = std::getenv("ATB_USER"); u) netDefaults.user = u;
    if (const char* p = std::getenv("ATB_PASS"); p) netDefaults.pass = p;
    if (const char* l = std::getenv("ATB_LOBBY"); l) netDefaults.lobby = l;
    render::ConnectScreen connect(netDefaults);

    // A local, in-process match (LocalMatchSource drives the Battle directly).
    auto newLocalMatch = [&]() -> std::unique_ptr<render::MatchSource> {
        return std::make_unique<render::LocalMatchSource>(
            buildMatch(session.ruleset, editor.playerTeam(), editor.enemyTeam(), session.catalog,
                       /*seed=*/0, session.creatures,
                       session.staticArena ? &*session.staticArena : nullptr));
    };

    // Split "host:port" (default port 5555).
    auto parseHostPort = [](const std::string& s, std::string& host, uint16_t& port) {
        const auto colon = s.find(':');
        host = colon == std::string::npos ? s : s.substr(0, colon);
        port = static_cast<uint16_t>(colon == std::string::npos ? 5555
                                                                : std::atoi(s.substr(colon + 1).c_str()));
    };

    auto enterBattleWith = [&](std::unique_ptr<render::MatchSource> src) {
        source = std::move(src);
        animator.reset();
        selectedSpell = 0;
        logScroll = 0;
        status = "Player turn — left-click move, click a spell (or 1-9), right-click cast, Tab=editor.";
        state = AppState::Battle;
    };

    // Turn a lobby pairing into a playable MatchSource: a live token → a mirror
    // (RemoteMatchSource) over a fresh match conn; a correspondence handle → a
    // CorrespondenceSession over this lobby connection. A rated game mirrors under
    // the ranked ruleset (which must be loaded locally).
    auto routePairing = [&](const net::PairedInfo& pi) {
        onlineMatch = true; // → the end-of-match screen returns to the lobby
        const Ruleset& rs =
            (pi.rated && session.rankedRuleset) ? *session.rankedRuleset : session.ruleset;
        if (pi.live) {
            std::string err;
            std::unique_ptr<net::MirrorSession> ms = net::MirrorSession::joinToken(
                lobbyHost, lobbyPort, pi.token, rs, session.catalog, session.creatures, &err);
            if (ms) enterBattleWith(std::make_unique<render::RemoteMatchSource>(std::move(ms)));
            else lobbyScreen.setStatus("Join failed: " + err);
        } else {
            net::CorrespondenceSetup setup{rs, session.catalog, session.creatures, pi.seed, pi.player,
                                           pi.enemy};
            auto cs = std::make_unique<net::CorrespondenceSession>(
                std::make_unique<net::LobbyChannel>(lobby.get()), pi.game, setup, pi.seat,
                lobby->account().user);
            enterBattleWith(std::make_unique<render::CorrespondenceMatchSource>(
                std::move(cs), lobby.get(), pi.game, pi.seat, pi.rated));
        }
    };

    using EMode = render::BuildEditorScreen::Mode;

    while (!WindowShouldClose() && !quit) {
        const float dt = GetFrameTime();

        if (state == AppState::Menu) {
            BeginDrawing();
            const auto r = menu.runFrame(GetScreenWidth(), GetScreenHeight());
            EndDrawing();
            switch (r) {
                case render::MainMenuScreen::Result::LocalMatch:  editorMode = EMode::Local; editorReturn = AppState::Menu; state = AppState::Editor; break;
                case render::MainMenuScreen::Result::PlayOnline:  state = AppState::Connect; break; // → login → lobby
                case render::MainMenuScreen::Result::BuildEditor: editorMode = EMode::Edit; editorReturn = AppState::Menu; state = AppState::Editor; break;
                case render::MainMenuScreen::Result::Settings:    state = AppState::Settings; break;
                case render::MainMenuScreen::Result::Quit:        quit = true; break;
                case render::MainMenuScreen::Result::None: break;
            }
            continue;
        }

        if (state == AppState::Settings) {
            std::vector<std::pair<std::string, std::string>> rows = {
                {"Content dir", session.staticArena ? "data/ (static map)" : "data/"},
                {"Ruleset", TextFormat("teamSize %d, budget %d pts", session.ruleset.teamSize,
                                       session.ruleset.economy.pointBudget)},
                {"Sprite pack", pack ? pack->name() : std::string("(built-in primitives)")},
                {"Default server", connect.params().host},
            };
            BeginDrawing();
            const auto r = settings.runFrame(GetScreenWidth(), GetScreenHeight(), rows);
            EndDrawing();
            if (r == render::SettingsScreen::Result::Back) state = AppState::Menu;
            continue;
        }

        if (state == AppState::Editor) {
            BeginDrawing();
            const auto r = editor.runFrame(GetScreenWidth(), GetScreenHeight(), editorMode);
            EndDrawing();
            if (r == render::BuildEditorScreen::Result::Fight) { onlineMatch = false; enterBattleWith(newLocalMatch()); }
            // Both the primary button and "‹ Menu" return to wherever the editor was
            // opened from (the lobby or a ready check when editing an online build).
            else if (r == render::BuildEditorScreen::Result::PlayOnline) state = editorReturn;
            else if (r == render::BuildEditorScreen::Result::Menu) state = editorReturn;
            continue;
        }

        if (state == AppState::Connect) {
            BeginDrawing();
            const auto r = connect.runFrame(GetScreenWidth(), GetScreenHeight());
            EndDrawing();
            if (r == render::ConnectScreen::Result::Back) {
                state = AppState::Menu;
            } else if (r == render::ConnectScreen::Result::Connect) {
                // Connect to the lobby (the Online Home), then browse/challenge there.
                const render::ConnectScreen::Params& pr = connect.params();
                parseHostPort(pr.host, lobbyHost, lobbyPort);
                std::string err;
                lobby = net::LobbySession::connect(lobbyHost, lobbyPort,
                                                   net::contentHashOf(session.catalog), pr.user,
                                                   pr.pass, &err);
                if (lobby) {
                    lobbyScreen.setStatus(session.rankedRuleset ? ""
                                                                : "Rated online disabled (no ranked ruleset).");
                    state = AppState::Lobby;
                } else {
                    connect.setStatus("Lobby connect failed: " + err);
                }
            }
            continue;
        }

        if (state == AppState::Lobby) {
            BeginDrawing();
            const auto r = lobbyScreen.runFrame(GetScreenWidth(), GetScreenHeight(), *lobby,
                                                editor.playerTeam().front(),
                                                /*ratedAvailable=*/session.rankedRuleset.has_value());
            EndDrawing();
            if (r == render::LobbyScreen::Result::Back) {
                lobby.reset();
                state = AppState::Menu;
            } else if (r == render::LobbyScreen::Result::EditBuild) {
                editorMode = EMode::Online; // author/pick a build, then return to the lobby
                editorReturn = AppState::Lobby;
                state = AppState::Editor;
            } else if (r == render::LobbyScreen::Result::ReadyCheck) {
                readyScreen.begin(lobbyScreen.readyCheck());
                state = AppState::ReadyCheck;
            }
            continue;
        }

        if (state == AppState::ReadyCheck) {
            BeginDrawing();
            const auto r = readyScreen.runFrame(GetScreenWidth(), GetScreenHeight(), *lobby,
                                                editor.playerTeam().front());
            EndDrawing();
            if (r == render::ReadyCheckScreen::Result::Matched) {
                routePairing(readyScreen.pairing()); // → Battle
            } else if (r == render::ReadyCheckScreen::Result::Cancelled) {
                state = AppState::Lobby;
            } else if (r == render::ReadyCheckScreen::Result::EditBuild) {
                editorMode = EMode::Online;
                editorReturn = AppState::ReadyCheck; // resume the ready check afterwards
                state = AppState::Editor;
            }
            continue;
        }

        // -------------------------- Battle state -----------------------------
        Vec2i hovered = render::screenToGrid(layout, GetMouseX(), GetMouseY());
        const bool hoveredValid = source->battle().grid().inBounds(hovered);

        // Combat-log scrollback: wheel up = older, down = newer (clamped ≥ 0).
        logScroll = std::max(0, logScroll + static_cast<int>(GetMouseWheelMove()));

        if (!chatFocused && IsKeyPressed(KEY_TAB)) { state = AppState::Editor; continue; }
        // R starts a fresh LOCAL arena. (In a networked match the server owns the
        // arena, so this just drops you into a local rematch — Tab returns to editor.)
        if (!chatFocused && IsKeyPressed(KEY_R)) {
            source = newLocalMatch();
            onlineMatch = false;
            animator.reset();
            selectedSpell = 0;
            logScroll = 0;
            status = "New arena. Player turn.";
        }

        // Pump the source EVERY frame — before reading whose turn it is. A remote /
        // correspondence mirror advances only when this drains the server's echoes,
        // *including the confirmation of our own move*; without an unconditional pump
        // the mirror would stay stuck on our turn and the move would never appear. A
        // LocalMatchSource no-ops here while it's our turn or the match is finished,
        // and otherwise paces one AI/inert action.
        if (auto s = source->update(dt)) status = *s;

        const bool finished = source->matchOver();
        const EntityId active = finished ? 0 : source->battle().activeUnit();
        // The seam decides who drives: the local player inputs only for their own
        // Champions; summons (either team) are AI and objects (bombs) auto-pass —
        // all handled by source->update().
        const bool playerControl = source->awaitingLocalInput();

        // Turn clock (timed networked matches): reset to the per-move window whenever
        // the active unit changes, tick down otherwise. Approximate (client-local),
        // just a visible indicator of the server-enforced idle-forfeit window.
        const int clockSec = source->clockSeconds();
        if (clockSec > 0 && !finished) {
            if (active != lastActive) { turnClock = static_cast<float>(clockSec); lastActive = active; }
            else turnClock = std::max(0.0f, turnClock - dt);
        }

        // In-match chat focus + typing (live networked matches). Click the input box
        // to focus (click elsewhere unfocuses); while focused, keystrokes go to chat
        // and the board/spell hotkeys are suppressed.
        if (source->chatEnabled() && !finished) {
            render::ViewState geo;
            geo.windowW = GetScreenWidth();
            geo.windowH = GetScreenHeight();
            geo.showClock = clockSec > 0;
            geo.showChat = true;
            const render::Rect ci = render::chatInputRect(layout, source->battle().grid(), geo);
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
                chatFocused = ci.contains(GetMouseX(), GetMouseY());
            if (chatFocused) {
                int k = GetCharPressed();
                while (k > 0) {
                    if (k >= 32 && k < 127 && chatDraft.size() < 200)
                        chatDraft.push_back(static_cast<char>(k));
                    k = GetCharPressed();
                }
                if (IsKeyPressed(KEY_BACKSPACE) && !chatDraft.empty()) chatDraft.pop_back();
                if (IsKeyPressed(KEY_ENTER) && !chatDraft.empty()) {
                    source->sendChat(chatDraft);
                    chatDraft.clear();
                }
            }
        } else {
            chatFocused = false;
        }

        // Which spell button (if any) the cursor is over — set inside the player
        // block below; also read later when composing the view (hover tooltip).
        int hoveredSpell = -1;

        if (playerControl && !chatFocused) {
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
        }

        if (finished) {
            // update() (pumped above) let a correspondence source finalize + submit;
            // keep its result message, else show win/loss/draw from the local seat's
            // view (a remote player may be the Enemy seat; a forfeit sets the winner).
            const std::optional<Faction> w = source->winner();
            if (status.rfind("Game over", 0) != 0)
                status = !w ? "Draw. Tab=editor, R=rematch."
                        : (*w == source->localSeat()) ? "Victory! Tab=editor, R=rematch."
                                                       : "Defeat. Tab=editor, R=rematch.";
        }

        render::ViewState view;
        view.hoveredTile = hovered;
        view.hoveredValid = hoveredValid;
        view.statusLine = status;
        view.windowW = GetScreenWidth();
        view.windowH = GetScreenHeight();
        view.logScroll = logScroll;
        // Two-clock strip atop the log column (timed matches): my time ticks on my
        // turn, the opponent's on theirs; the idle side shows the full window.
        view.showClock = clockSec > 0 && !finished;
        if (view.showClock) {
            view.myTurnActive = playerControl;
            view.myClock = playerControl ? turnClock : static_cast<float>(clockSec);
            view.oppClock = playerControl ? static_cast<float>(clockSec) : turnClock;
        }
        view.showChat = source->chatEnabled();
        if (view.showChat) {
            view.chatLog = &source->chatLog();
            view.localSeat = source->localSeat();
            view.chatDraft = chatDraft;
            view.chatFocused = chatFocused;
        }
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

        bool returnToLobby = false;
        BeginDrawing();
        render::drawFrame(layout, source->battle(), view, pack ? &*pack : nullptr, &animator);

        // End-of-match screen for an ONLINE game: clear result + return to the lobby
        // (a local match keeps the Tab=editor / R=rematch status instead).
        if (finished && onlineMatch) {
            const int W = GetScreenWidth(), H = GetScreenHeight();
            DrawRectangle(0, 0, W, H, Color{0, 0, 0, 190});
            const std::optional<Faction> w = source->winner();
            const char* title = !w ? "DRAW" : (*w == source->localSeat()) ? "VICTORY" : "DEFEAT";
            const Color tc = !w ? render::ui::kMuted
                                : (*w == source->localSeat()) ? render::ui::kGood : render::ui::kBad;
            const int titleW = MeasureText(title, 64);
            DrawText(title, (W - titleW) / 2, H / 2 - 90, 64, tc);
            Rectangle btn{static_cast<float>(W) / 2 - 130, static_cast<float>(H) / 2 + 20, 260, 46};
            if (render::ui::button(btn, "Return to lobby", GetMousePosition(), render::ui::kAccent))
                returnToLobby = true;
        }
        EndDrawing();

        if (returnToLobby) {
            source.reset();
            onlineMatch = false;
            state = lobby ? AppState::Lobby : AppState::Menu;
        }
    }

    if (pack) pack->unload(); // free textures while the GL context is still alive
    CloseWindow();
    return 0;
}
