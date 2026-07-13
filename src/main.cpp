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
//     Esc          : open the pause menu (Resume / Settings / GitHub / Quit);
//                    in battle it first cancels a half-placed portal or decoy
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
#include "data/Prefs.h"
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
#include "render/Theme.h"
#include "render/MatchSource.h"
#include "render/ReadyCheckScreen.h"
#include "render/RemoteMatchSource.h"
#include "render/Renderer.h"
#include "render/ReplayMatchSource.h"
#include "net/Replay.h"
#include "render/SettingsScreen.h"
#include "render/SpectateMatchSource.h"
#include "render/SpritePack.h"
#include "render/Ui.h"

#include "raylib.h"

// Build version, compiled in from the git tag by CMake (see the tactical_battler
// target). Defaults to "dev" for ad-hoc builds without the define.
#ifndef ATB_VERSION
#define ATB_VERSION "dev"
#endif

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace tb;

namespace {

enum class AppState { Menu, Editor, Connect, Lobby, ReadyCheck, Waiting, Battle, Settings, Paused };

// The in-game pause menu (Esc anywhere) links out to the project page.
inline constexpr const char* kGithubUrl = "https://github.com/schenegghugo/ATB";

// A blocking network call moved off the render thread (async connect / join). The
// worker fills the result under `mu` and flips `done`; the UI polls each frame.
// Cancel = drop the owning shared_ptr — the detached worker keeps its own ref and
// its late result is simply discarded.
struct PendingConnect {
    std::mutex mu;
    bool done = false;
    std::unique_ptr<tb::net::LobbySession> session;
    std::string error;
};
struct PendingJoin {
    std::mutex mu;
    bool done = false;
    std::unique_ptr<tb::net::MirrorSession> mirror;
    std::string error;
};

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

// A portal-type spell places a Portal ground effect — the GUI casts it with an
// explicit two-click entry+exit (see the battle input loop) rather than one tile.
// `portalReach` is how far (Manhattan) the exit may sit from the entry; 0 = not a
// portal. It mirrors the core cap in Battle::spawnGround (GroundSpec::magnitude).
int portalReach(const Spell& sp) {
    for (const Effect& fx : sp.effects)
        if (fx.type == Effect::Type::Spawn && fx.ground.kind == GroundKind::Portal)
            return fx.ground.magnitude;
    return 0;
}
bool isPortalSpell(const Spell& sp) { return portalReach(sp) > 0; }

} // namespace

int main() {
    TraceLog(LOG_INFO, "Tactical Battler %s", ATB_VERSION);
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

    // Player preferences (settings.json beside the app, hand-editable): the
    // picked UI theme + sprite pack. Absent file = defaults; a malformed one is
    // reported and ignored (never guessed at).
    Prefs prefs;
    if (PrefsLoad pl = loadPrefsFromFile("settings.json"); pl.ok) {
        prefs = pl.prefs;
    } else {
        TraceLog(LOG_WARNING, "settings.json is invalid — using defaults:");
        for (const std::string& e : pl.errors) TraceLog(LOG_WARNING, "  - %s", e.c_str());
    }
#ifdef __EMSCRIPTEN__
    // First run in a browser: no settings.json in the bundle — default to the
    // baked-in pack so the itch.io build ships with art on.
    if (prefs.pack.empty() && prefs.theme.empty() && !std::getenv("ATB_PACK"))
        prefs.pack = "default";
#endif

    // Apply the picked theme (themes/<name>.json) to the chrome + battle
    // palettes; "" or a broken file → the built-in defaults. Returns false
    // (with a status message) so the Settings screen can surface a bad pick.
    std::string settingsStatus;
    // Battle-board metrics from the active theme (resizable-UI extension); the
    // player's prefs.uiScale multiplies these. applyThemeByName refreshes them.
    int themeTileSize = render::Theme{}.metrics.tileSize;
    double themeScale = render::Theme{}.metrics.uiScale;
    auto applyThemeByName = [&](const std::string& name) -> bool {
        if (name.empty()) {
            const render::Theme defaults;
            render::ui::applyTheme(defaults);
            render::applyBattleTheme(defaults);
            themeTileSize = defaults.metrics.tileSize;
            themeScale = defaults.metrics.uiScale;
            return true;
        }
        const std::string dir = render::siblingDir("themes");
        const std::string path = dir + "/" + name + ".json";
        const render::ThemeLoad tl =
            dir.empty() ? render::ThemeLoad{} : render::loadThemeFromFile(path);
        if (!tl.ok) {
            settingsStatus = "Theme '" + name + "' failed to load — see the log.";
            TraceLog(LOG_WARNING, "Theme '%s' is invalid — palette unchanged:", path.c_str());
            for (const std::string& e : tl.errors) TraceLog(LOG_WARNING, "  - %s", e.c_str());
            return false;
        }
        render::ui::applyTheme(tl.theme);
        render::applyBattleTheme(tl.theme);
        themeTileSize = tl.theme.metrics.tileSize;
        themeScale = tl.theme.metrics.uiScale;
        TraceLog(LOG_INFO, "Applied theme '%s' (%s)", name.c_str(), tl.name.c_str());
        return true;
    };
    if (!applyThemeByName(prefs.theme)) prefs.theme.clear(); // bad pref → defaults

    // Effective board layout = theme density × theme scale × the player's prefs
    // scale, with the remaining metrics derived from the tile size. Recomputed
    // wherever the scale can change so a resize applies live.
    auto makeLayout = [&]() {
        render::Layout l;
        const double f = std::clamp(themeScale * static_cast<double>(prefs.uiScale), 0.6, 1.8);
        l.tileSize = std::clamp(static_cast<int>(std::lround(themeTileSize * f)), 16, 120);
        const double s = l.tileSize / 36.0;
        l.originX = static_cast<int>(std::lround(16 * s));
        l.originY = static_cast<int>(std::lround(28 * s));
        l.spellBarHeight = static_cast<int>(std::lround(46 * s));
        l.hudHeight = static_cast<int>(std::lround(96 * s));
        return l;
    };
    render::Layout layout = makeLayout();
    // Open large enough for both the arena (sized from the ruleset) and the
    // (responsive) build editor; the editor reads the live window size each frame,
    // so it adapts to resizes / tiling window managers (e.g. Sway).
    const int arenaW = layout.screenWidth(Grid(session.ruleset.arena.width, session.ruleset.arena.height));
    const int arenaH = layout.screenHeight(Grid(session.ruleset.arena.width, session.ruleset.arena.height));
    const int sw = std::max(arenaW, 1180);
    const int sh = std::max(arenaH, 720);

    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE);
    InitWindow(sw, sh, "Tactical Battler — POC");
    SetExitKey(KEY_NULL); // Esc opens the pause menu instead of closing the window
    if (!IsWindowReady()) {
        // No display/GL context (e.g. headless, or Wayland without the Wayland
        // backend compiled in). Bail cleanly instead of crashing in the GL calls.
        TraceLog(LOG_ERROR, "Window/GL context unavailable — cannot run the GUI. "
                            "Run from a graphical session, or rebuild with the Wayland backend.");
        return 1;
    }
    SetTargetFPS(60);

    // Optional presentation pack (art/palette). A pack is client-side cosmetic
    // only — it never touches rules. Sources, first hit wins:
    //   1. ATB_PACK=<dir> (dev override: a directory path holding pack.json)
    //   2. settings.json's "pack" (a name under packs/, picked in Settings)
    //   3. none → the built-in primitives.
    std::optional<render::SpritePack> pack;
    auto loadPackFromDir = [&pack](const std::string& dir) -> bool {
        if (pack) pack->unload(); // needs the GL context — swapped in-frame only
        pack.emplace();
        std::vector<std::string> errs;
        if (pack->load(dir, errs)) {
            TraceLog(LOG_INFO, "Loaded sprite pack '%s' (%s)", dir.c_str(), pack->name().c_str());
            return true;
        }
        TraceLog(LOG_WARNING, "Sprite pack '%s' failed to load — using primitives:", dir.c_str());
        for (const std::string& e : errs) TraceLog(LOG_WARNING, "  - %s", e.c_str());
        pack.reset();
        return false;
    };
    // Swap to packs/<name> ("" = drop to primitives); false + status on failure.
    auto loadPackByName = [&](const std::string& name) -> bool {
        if (name.empty()) {
            if (pack) { pack->unload(); pack.reset(); }
            return true;
        }
        const std::string dir = render::siblingDir("packs");
        if (dir.empty() || !loadPackFromDir(dir + "/" + name)) {
            settingsStatus = "Pack '" + name + "' failed to load — see the log.";
            return false;
        }
        return true;
    };
    if (const char* pk = std::getenv("ATB_PACK"); pk && *pk) {
        loadPackFromDir(pk);
    } else if (!loadPackByName(prefs.pack)) {
        prefs.pack.clear(); // bad pref → primitives
    }

    render::BuildEditorScreen editor(session.catalog, *session.repo, session.ruleset);
    render::MainMenuScreen menu;
    render::SettingsScreen settings;
    std::vector<std::string> themesList, packsList; // rescanned on entering Settings
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
    // Which battle-layout grip is being dragged (board corner / column dividers).
    enum class LayoutDrag { None, Board, Clock, Chat } layoutDrag = LayoutDrag::None;
    // Two-click portal: the placed entry tile awaiting an exit (nullopt = not placing).
    std::optional<Vec2i> portalPending;
    AppState pauseReturn = AppState::Menu;    // where Esc's pause menu returns to
    AppState settingsReturn = AppState::Menu; // where the Settings "Back" returns to
    int logScroll = 0;             // combat-log scrollback (0 = pinned to newest)
    bool onlineMatch = false;      // this battle came from the lobby (→ end screen returns there)
    float turnClock = 0.0f;        // seconds left in the active seat's move (timed matches)
    EntityId lastActive = 0;       // detect turn changes to reset turnClock
    std::string chatDraft;         // in-match chat being typed
    bool chatFocused = false;      // chat input has keyboard focus
    render::ReplayMatchSource* replay = nullptr; // non-null when `source` is a replay
    bool spectating = false;       // `source` watches someone else's live match (5.2)
    std::optional<net::Intent> pendingDecoy; // a decoy cast awaiting its 'a'/'b' commitment
    std::shared_ptr<PendingConnect> pendingConnect; // async lobby login in flight
    std::shared_ptr<PendingJoin> pendingJoin;       // async match join in flight (Waiting)

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
        replay = nullptr;    // cleared for live/local; the replay boot sets it after
        spectating = false;  // likewise set after by the lobby Watch route
        pendingDecoy.reset();
        animator.reset();
        selectedSpell = 0;
        logScroll = 0;
        status = "Player turn — left-click move, click a spell (or 1-9), right-click cast, Tab=editor.";
        state = AppState::Battle;
    };

    // Turn a lobby pairing into a playable MatchSource: a live token → a mirror
    // (RemoteMatchSource) over a fresh match conn; a correspondence handle → a
    // CorrespondenceSession over this lobby connection. A rated game mirrors under
    // the ranked ruleset (which must be loaded locally). `resume` = a cold resume of
    // an existing correspondence game: replay the server's log (and my persisted
    // decoy secrets) instead of starting fresh.
    auto routePairing = [&](const net::PairedInfo& pi, bool resume = false) {
        onlineMatch = true; // → the end-of-match screen returns to the lobby
        const Ruleset& rs =
            (pi.rated && session.rankedRuleset) ? *session.rankedRuleset : session.ruleset;
        if (pi.live) {
            // joinToken blocks until BOTH players' match conns arrive (the first is
            // parked server-side) — run it off-thread and show the Waiting screen.
            auto pj = std::make_shared<PendingJoin>();
            std::thread([pj, host = lobbyHost, port = lobbyPort, token = pi.token, rules = rs,
                         cat = session.catalog, cre = session.creatures] {
                std::string err;
                std::unique_ptr<net::MirrorSession> ms =
                    net::MirrorSession::joinToken(host, port, token, rules, cat, cre, &err);
                std::lock_guard<std::mutex> lk(pj->mu);
                pj->mirror = std::move(ms);
                pj->error = err;
                pj->done = true;
            }).detach();
            pendingJoin = std::move(pj);
            state = AppState::Waiting;
        } else {
            net::CorrespondenceSetup setup{rs, session.catalog, session.creatures, pi.seed, pi.player,
                                           pi.enemy};
            auto cs = std::make_unique<net::CorrespondenceSession>(
                std::make_unique<net::LobbyChannel>(lobby.get()), pi.game, setup, pi.seat,
                lobby->account().user);
            // My decoy commitment secrets live beside the app, per game — written on
            // every local move, read back on a cold resume.
            const std::string secretsPath = ".atb-secrets/" + pi.game + ".txt";
            if (resume) {
                std::ifstream f(secretsPath);
                const std::string text((std::istreambuf_iterator<char>(f)),
                                       std::istreambuf_iterator<char>());
                std::string err;
                if (!cs->resume(net::parseDecoySecrets(text), &err)) {
                    lobbyScreen.setStatus("Resume failed: " + err);
                    onlineMatch = false;
                    return;
                }
            }
            enterBattleWith(std::make_unique<render::CorrespondenceMatchSource>(
                std::move(cs), lobby.get(), pi.game, pi.seat, pi.rated, lobby->account().user,
                secretsPath));
        }
    };

    using EMode = render::BuildEditorScreen::Mode;

    // A replay to watch? ATB_REPLAY=<file> holds a game notation (net/Replay); boot
    // straight into read-only playback. Controls live in the Battle loop below.
    if (const char* rp = std::getenv("ATB_REPLAY"); rp && *rp) {
        std::ifstream f(rp);
        const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        const replay::RecordParse pr = replay::parseRecord(text);
        if (!pr.ok) {
            TraceLog(LOG_ERROR, "ATB_REPLAY '%s' unparseable: %s", rp, pr.error.c_str());
        } else {
            const Ruleset& rs = replay::rulesetHash(session.ruleset) == pr.record.rulesetHash
                                    ? session.ruleset
                                    : (session.rankedRuleset ? *session.rankedRuleset : session.ruleset);
            auto rsrc = std::make_unique<render::ReplayMatchSource>(pr.record, rs, session.catalog,
                                                                    session.creatures);
            render::ReplayMatchSource* raw = rsrc.get();
            enterBattleWith(std::move(rsrc));
            replay = raw;
            TraceLog(LOG_INFO, "Replaying '%s' (%zu intents).", rp, replay->total());
        }
    }

    while (!WindowShouldClose() && !quit) {
        const float dt = GetFrameTime();
        layout = makeLayout(); // pick up a live UI-scale change from Settings

        // Esc, everywhere: sub-modal actions cancel first (portal placement, the
        // decoy prompt), otherwise Esc toggles the pause menu.
        if (IsKeyPressed(KEY_ESCAPE)) {
            if (state == AppState::Battle && portalPending) {
                portalPending.reset();
                status = "Portal cancelled.";
            } else if (state == AppState::Battle && pendingDecoy) {
                pendingDecoy.reset();
                status = "Decoy cast cancelled.";
            } else if (state == AppState::Paused) {
                state = pauseReturn; // Esc closes the pause menu
            } else if (state == AppState::Settings) {
                state = settingsReturn; // Esc = Back on the settings screen
            } else {
                pauseReturn = state; // open the pause menu from anywhere else
                state = AppState::Paused;
            }
        }

        if (state == AppState::Paused) {
            BeginDrawing();
            const int W = GetScreenWidth(), H = GetScreenHeight();
            DrawRectangle(0, 0, W, H, Color{0, 0, 0, 200});
            const char* title = "PAUSED";
            DrawText(title, (W - MeasureText(title, 40)) / 2, static_cast<int>(H * 0.24f), 40,
                     render::ui::kText);
            const Vector2 mp = GetMousePosition();
            const float bw = 280.0f, bh = 46.0f, gap = 12.0f, bx = (W - bw) / 2.0f;
            float by = H * 0.24f + 80.0f;
            if (render::ui::button({bx, by, bw, bh}, "Resume", mp, render::ui::kAccent))
                state = pauseReturn;
            by += bh + gap;
            if (render::ui::button({bx, by, bw, bh}, "Settings", mp, render::ui::kPanel)) {
                settingsReturn = AppState::Paused;
                state = AppState::Settings;
            }
            by += bh + gap;
            if (render::ui::button({bx, by, bw, bh}, "Open GitHub page", mp, render::ui::kPanel))
                OpenURL(kGithubUrl);
            by += bh + gap;
            if (render::ui::button({bx, by, bw, bh}, "Quit", mp, render::ui::kPanel)) quit = true;
            DrawText("Esc to resume", (W - MeasureText("Esc to resume", 14)) / 2,
                     static_cast<int>(by) + bh + 18, 14, render::ui::kMuted);
            EndDrawing();
            continue;
        }

        if (state == AppState::Menu) {
            BeginDrawing();
            const auto r = menu.runFrame(GetScreenWidth(), GetScreenHeight(), ATB_VERSION);
            EndDrawing();
            switch (r) {
                case render::MainMenuScreen::Result::LocalMatch:  editorMode = EMode::Local; editorReturn = AppState::Menu; state = AppState::Editor; break;
                case render::MainMenuScreen::Result::PlayOnline:  state = AppState::Connect; break; // → login → lobby
                case render::MainMenuScreen::Result::BuildEditor: editorMode = EMode::Edit; editorReturn = AppState::Menu; state = AppState::Editor; break;
                case render::MainMenuScreen::Result::Settings:
                    // Rescan on entry so a pack/theme dropped in while the game
                    // runs shows up without a restart (ricing-friendly).
                    themesList = listThemes(render::siblingDir("themes"));
                    packsList = listPacks(render::siblingDir("packs"));
                    settingsReturn = AppState::Menu;
                    state = AppState::Settings;
                    break;
                case render::MainMenuScreen::Result::Quit:        quit = true; break;
                case render::MainMenuScreen::Result::None: break;
            }
            continue;
        }

        if (state == AppState::Settings) {
            render::SettingsScreen::View sv;
            sv.rows = {
                {"Content dir", session.staticArena ? "data/ (static map)" : "data/"},
                {"Ruleset", TextFormat("teamSize %d, budget %d pts", session.ruleset.teamSize,
                                       session.ruleset.economy.pointBudget)},
                {"Sprite pack", pack ? pack->name() : std::string("(built-in primitives)")},
                {"Default server", connect.params().host},
            };
            sv.themes = themesList;
            sv.packs = packsList;
            sv.curTheme = prefs.theme;
            sv.curPack = prefs.pack;
            sv.uiScale = prefs.uiScale;
            sv.status = settingsStatus;
            BeginDrawing();
            const auto r = settings.runFrame(GetScreenWidth(), GetScreenHeight(), sv);
            EndDrawing();
            // Apply + persist a pick. A failed load keeps the previous pref (and
            // palette/pack) and reports via settingsStatus.
            using SR = render::SettingsScreen::Result;
            if (r == SR::Back) {
                state = settingsReturn; // back to the menu, or the pause menu that opened it
            } else if (r == SR::SetTheme) {
                if (applyThemeByName(settings.picked())) {
                    prefs.theme = settings.picked();
                    settingsStatus = savePrefsToFile(prefs, "settings.json")
                                         ? "Theme applied + saved."
                                         : "Theme applied (couldn't write settings.json).";
                }
            } else if (r == SR::ReloadTheme) {
                if (applyThemeByName(prefs.theme)) settingsStatus = "Theme file reloaded.";
            } else if (r == SR::SetPack) {
                if (loadPackByName(settings.picked())) {
                    prefs.pack = settings.picked();
                    settingsStatus = savePrefsToFile(prefs, "settings.json")
                                         ? "Pack applied + saved."
                                         : "Pack applied (couldn't write settings.json).";
                }
            } else if (r == SR::ScaleUp || r == SR::ScaleDown) {
                prefs.uiScale = std::clamp(prefs.uiScale + (r == SR::ScaleUp ? 0.1f : -0.1f),
                                           0.7f, 1.6f);
                layout = makeLayout();
                // Refit the window so the resized board stays fully visible.
                const Grid ag(session.ruleset.arena.width, session.ruleset.arena.height);
                SetWindowSize(std::max(layout.screenWidth(ag), 1180),
                              std::max(layout.screenHeight(ag), 720));
                settingsStatus = savePrefsToFile(prefs, "settings.json")
                                     ? TextFormat("UI scale %.0f%% — saved.", prefs.uiScale * 100.0f)
                                     : "UI scale set (couldn't write settings.json).";
            }
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
            // An async login finished? Adopt (or report) it before drawing.
            if (pendingConnect) {
                std::unique_ptr<net::LobbySession> got;
                std::string err;
                bool done = false;
                {
                    std::lock_guard<std::mutex> lk(pendingConnect->mu);
                    if (pendingConnect->done) {
                        done = true;
                        got = std::move(pendingConnect->session);
                        err = pendingConnect->error;
                    }
                }
                if (done) {
                    pendingConnect.reset();
                    if (got) {
                        lobby = std::move(got);
                        lobbyScreen.setStatus(session.rankedRuleset
                                                  ? ""
                                                  : "Rated online disabled (no ranked ruleset).");
                        state = AppState::Lobby;
                        continue;
                    }
                    connect.setStatus("Lobby connect failed: " + err);
                }
            }
            BeginDrawing();
            const auto r = connect.runFrame(GetScreenWidth(), GetScreenHeight());
            EndDrawing();
            if (r == render::ConnectScreen::Result::Back) {
                pendingConnect.reset(); // abandon any in-flight attempt
                state = AppState::Menu;
            } else if (r == render::ConnectScreen::Result::Connect && !pendingConnect) {
                // Connect to the lobby (the Online Home) OFF-THREAD — a dead host
                // would otherwise freeze the UI for the whole TCP timeout.
                const render::ConnectScreen::Params& pr = connect.params();
                parseHostPort(pr.host, lobbyHost, lobbyPort);
                connect.setStatus("Connecting…");
                auto pc = std::make_shared<PendingConnect>();
                std::thread([pc, host = lobbyHost, port = lobbyPort,
                             hash = net::contentHashOf(session.catalog), user = pr.user,
                             pass = pr.pass] {
                    std::string err;
                    std::unique_ptr<net::LobbySession> s =
                        net::LobbySession::connect(host, port, hash, user, pass, &err);
                    std::lock_guard<std::mutex> lk(pc->mu);
                    pc->session = std::move(s);
                    pc->error = err;
                    pc->done = true;
                }).detach();
                pendingConnect = std::move(pc);
            }
            continue;
        }

        if (state == AppState::Waiting) {
            // The opponent's match conn arrived (or the join failed)?
            std::unique_ptr<net::MirrorSession> got;
            std::string err;
            bool done = false;
            if (pendingJoin) {
                std::lock_guard<std::mutex> lk(pendingJoin->mu);
                if (pendingJoin->done) {
                    done = true;
                    got = std::move(pendingJoin->mirror);
                    err = pendingJoin->error;
                }
            }
            if (done || !pendingJoin) {
                pendingJoin.reset();
                if (got) {
                    onlineMatch = true;
                    enterBattleWith(std::make_unique<render::RemoteMatchSource>(std::move(got)));
                } else {
                    lobbyScreen.setStatus("Join failed: " + (err.empty() ? "abandoned" : err));
                    state = lobby ? AppState::Lobby : AppState::Menu;
                }
                continue;
            }
            const int W = GetScreenWidth(), H = GetScreenHeight();
            BeginDrawing();
            ClearBackground(render::ui::kBg);
            const char* title = "WAITING FOR OPPONENT";
            const int tw = MeasureText(title, 34);
            DrawText(title, (W - tw) / 2, H / 2 - 80, 34, render::ui::kText);
            const int dots = 1 + static_cast<int>(GetTime() * 2) % 3;
            DrawText(std::string(static_cast<std::size_t>(dots), '.').c_str(), (W + tw) / 2 + 8,
                     H / 2 - 80, 34, render::ui::kMuted);
            const char* sub = "Your match starts as soon as they connect.";
            DrawText(sub, (W - MeasureText(sub, 16)) / 2, H / 2 - 34, 16, render::ui::kMuted);
            Rectangle c{static_cast<float>(W) / 2 - 90, static_cast<float>(H) / 2 + 30, 180, 40};
            const bool cancel = render::ui::button(c, "Cancel", GetMousePosition(), render::ui::kPanel);
            EndDrawing();
            if (cancel) {
                pendingJoin.reset(); // the worker's late result is discarded
                lobbyScreen.setStatus("Join abandoned.");
                state = lobby ? AppState::Lobby : AppState::Menu;
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
            } else if (r == render::LobbyScreen::Result::Resume) {
                routePairing(lobbyScreen.resumeGame(), /*resume=*/true);
            } else if (r == render::LobbyScreen::Result::Watch) {
                // Spectate a live game (5.2): prime a mirror from the match's logged
                // broadcast stream (the welcome is entry 0), then keep polling it.
                const net::LiveGameInfo g = lobbyScreen.watchGame();
                if (g.rated && !session.rankedRuleset) {
                    lobbyScreen.setStatus("Can't watch a rated game without the ranked ruleset.");
                } else {
                    const Ruleset& rs =
                        (g.rated && session.rankedRuleset) ? *session.rankedRuleset : session.ruleset;
                    auto mirror = std::make_unique<net::SpectatorMirror>(rs, session.catalog,
                                                                         session.creatures);
                    std::size_t cursor = 0;
                    if (std::optional<net::ChannelPoll> cp = lobby->watchPoll(g.id, 0)) {
                        for (const net::MailEntry& e : cp->entries) mirror->feed(e.msg);
                        cursor = cp->next;
                    }
                    if (mirror->ready()) {
                        onlineMatch = true;
                        enterBattleWith(std::make_unique<render::SpectateMatchSource>(
                            std::move(mirror), lobby.get(), g.id, cursor));
                        spectating = true;
                        status = "SPECTATING " + g.userP + " vs " + g.userE + " — Tab returns to the lobby.";
                    } else {
                        lobbyScreen.setStatus("Can't watch that game (its log is unavailable).");
                    }
                }
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

        // Replay playback controls (read-only viewer): Space pause, →/. step, ↑↓ speed.
        if (replay) {
            if (IsKeyPressed(KEY_TAB)) { source.reset(); replay = nullptr; state = AppState::Menu; continue; }
            if (IsKeyPressed(KEY_SPACE)) replay->togglePause();
            if (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_PERIOD)) replay->step();
            if (IsKeyPressed(KEY_UP)) replay->faster();
            if (IsKeyPressed(KEY_DOWN)) replay->slower();
            status = TextFormat("REPLAY %s  %zu/%zu   Space=pause  ->=step  up/down=speed  Tab=menu",
                                replay->matchOver() ? "ended"
                                : replay->paused()  ? "paused"
                                                    : "playing",
                                replay->cursor(), replay->total());
        }

        // Spectating: read-only; Tab leaves the stream and returns to the lobby.
        if (spectating && IsKeyPressed(KEY_TAB)) {
            source.reset();
            spectating = false;
            onlineMatch = false;
            state = lobby ? AppState::Lobby : AppState::Menu;
            continue;
        }

        if (!replay && !spectating && !chatFocused && IsKeyPressed(KEY_TAB)) { state = AppState::Editor; continue; }
        // R starts a fresh LOCAL arena. (In a networked match the server owns the
        // arena, so this just drops you into a local rematch — Tab returns to editor.)
        if (!replay && !spectating && !chatFocused && IsKeyPressed(KEY_R)) {
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
        // A chess-clock match instead reads the authoritative banks off the source.
        const int clockSec = source->clockSeconds();
        const bool chessClock = source->chessClock();
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
            geo.showClock = clockSec > 0 || chessClock;
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

        // --- Interactive layout: drag the board's corner or a column divider to
        // resize the battle panels; the new ratios persist to settings.json on
        // release. A live drag suppresses the board's move/cast handling below.
        {
            render::ViewState lg;
            lg.windowW = GetScreenWidth();
            lg.windowH = GetScreenHeight();
            lg.showClock = (clockSec > 0 || chessClock) && !finished;
            lg.showChat = source->chatEnabled();
            lg.clockHeight = prefs.clockHeight;
            lg.chatFraction = prefs.chatFraction;
            const Grid& bg = source->battle().grid();
            const int mx = GetMouseX(), my = GetMouseY();
            if (layoutDrag == LayoutDrag::None && !pendingDecoy && !chatFocused &&
                IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                if (render::boardResizeHandle(layout, bg).contains(mx, my))
                    layoutDrag = LayoutDrag::Board;
                else if (render::clockDivider(layout, bg, lg).contains(mx, my))
                    layoutDrag = LayoutDrag::Clock;
                else if (render::chatDivider(layout, bg, lg).contains(mx, my))
                    layoutDrag = LayoutDrag::Chat;
            }
            if (layoutDrag != LayoutDrag::None) {
                if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                    if (layoutDrag == LayoutDrag::Board) {
                        const double baseTile = std::max(1.0, themeTileSize * themeScale);
                        const double wantTile =
                            static_cast<double>(mx - layout.originX) / std::max(1, bg.width());
                        prefs.uiScale = std::clamp(static_cast<float>(wantTile / baseTile), 0.7f, 1.6f);
                        layout = makeLayout(); // board follows the cursor this frame
                    } else if (layoutDrag == LayoutDrag::Clock) {
                        prefs.clockHeight = std::clamp(my - layout.originY, 66, 240);
                    } else if (layoutDrag == LayoutDrag::Chat) {
                        const int top = layout.originY + prefs.clockHeight + 6;
                        const int avail = lg.windowH - 8 - top;
                        if (avail > 40)
                            prefs.chatFraction =
                                std::clamp(static_cast<float>(my - top) / avail, 0.15f, 0.85f);
                    }
                } else { // released — persist the layout ratios
                    layoutDrag = LayoutDrag::None;
                    savePrefsToFile(prefs, "settings.json");
                    status = "Layout saved.";
                }
            }
        }
        const bool draggingLayout = layoutDrag != LayoutDrag::None;

        // Which spell button (if any) the cursor is over — set inside the player
        // block below; also read later when composing the view (hover tooltip).
        int hoveredSpell = -1;

        if (playerControl && !chatFocused && !pendingDecoy && !draggingLayout) {
            const EntityId me = active;
            const int spellCount = static_cast<int>(source->battle().unit(me).spells.size());
            for (int k = 0; k < spellCount && k < 9; ++k)
                if (IsKeyPressed(KEY_ONE + k)) selectedSpell = k;
            if (selectedSpell >= spellCount) selectedSpell = 0;
            // Switching off the portal spell abandons a half-placed portal.
            if (portalPending && !isPortalSpell(source->battle().unit(me).spells[selectedSpell]))
                portalPending.reset();

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
                portalPending.reset(); // moving abandons a half-placed portal
                if (auto s = source->submit(net::Intent::move(hovered))) status = *s;
            }
            if (hoveredValid && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
                const Spell& sel = source->battle().unit(me).spells[selectedSpell];
                if (isPortalSpell(sel)) {
                    // Two clicks: the first right-click places the ENTRY (a legal
                    // cast tile), the second places the EXIT (any walkable tile) and
                    // fires the cast — the core teleports whoever stands on the entry.
                    if (!portalPending) {
                        if (source->battle().canCast(me, selectedSpell, hovered)) {
                            portalPending = hovered;
                            status = "Portal entry placed — right-click the exit tile (Esc cancels).";
                        } else {
                            status = "Can't open a portal there.";
                        }
                    } else if (source->battle().grid().isWalkable(hovered) &&
                               hovered != *portalPending &&
                               manhattan(*portalPending, hovered) <= portalReach(sel)) {
                        if (auto s = source->submit(
                                net::Intent::castTo(selectedSpell, *portalPending, hovered)))
                            status = *s;
                        portalPending.reset();
                    } else {
                        status = "Pick a walkable exit within the portal's reach (Esc cancels).";
                    }
                } else {
                    const net::Intent in = net::Intent::cast(selectedSpell, hovered);
                    // A decoy cast commits to a hidden choice up-front (CR.6) — prompt
                    // for it instead of submitting straight away.
                    if (source->needsDecoyChoice(in)) {
                        pendingDecoy = in;
                        status = "Decoy: commit your secret — stay the ORIGINAL or swap to the TWIN.";
                    } else if (auto s = source->submit(in)) {
                        status = *s;
                    }
                }
            }
            if (IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_ENTER)) {
                portalPending.reset();
                source->submit(net::Intent::endTurn());
            }
        }

        if (finished && !replay && !spectating) {
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
        view.clockHeight = prefs.clockHeight;
        view.chatFraction = prefs.chatFraction;
        view.showLayoutHandles = true; // draggable grips on the board + column
        // Two-clock strip atop the log column (timed matches): per-move, my time
        // ticks on my turn and the idle side shows the full window; a chess clock
        // shows both seats' authoritative remaining banks.
        view.showClock = (clockSec > 0 || chessClock) && !finished;
        if (view.showClock) {
            view.myTurnActive = playerControl;
            if (chessClock) {
                const Faction mySeat = source->localSeat();
                view.myClock = source->bankSeconds(mySeat);
                view.oppClock = source->bankSeconds(opposing(mySeat));
            } else {
                view.myClock = playerControl ? turnClock : static_cast<float>(clockSec);
                view.oppClock = playerControl ? static_cast<float>(clockSec) : turnClock;
            }
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
            if (selectedSpell < static_cast<int>(u.spells.size())) {
                const Grid& g = source->battle().grid();
                if (portalPending) {
                    // Exit placement: green = every walkable tile within the portal's
                    // reach of the entry (matching the core cap); mark the entry.
                    const int reach = portalReach(u.spells[selectedSpell]);
                    for (int y = 0; y < g.height(); ++y)
                        for (int x = 0; x < g.width(); ++x)
                            if (g.isWalkable({x, y}) && Vec2i{x, y} != *portalPending &&
                                manhattan(*portalPending, {x, y}) <= reach)
                                view.castable.push_back({x, y});
                    view.portalEntry = *portalPending;
                    view.portalEntrySet = true;
                } else {
                    // Green field: every tile the selected spell may legally target.
                    for (int y = 0; y < g.height(); ++y)
                        for (int x = 0; x < g.width(); ++x)
                            if (source->battle().canCast(me, selectedSpell, {x, y}))
                                view.castable.push_back({x, y});
                }
            }
            if (!portalPending && hoveredValid && selectedSpell < static_cast<int>(u.spells.size())) {
                view.spellCastable = source->battle().canCast(me, selectedSpell, hovered);
                view.spellZone = source->battle().affectedTiles(u.spells[selectedSpell], u.pos, hovered);
            }
        }

        // Trigger cast-clip animations off the same event stream the log reads.
        animator.sync(source->battle(), GetTime());

        bool returnToLobby = false;
        BeginDrawing();
        render::drawFrame(layout, source->battle(), view, pack ? &*pack : nullptr, &animator);

        // Decoy commitment prompt (correspondence, CR.6): pick the hidden member.
        // The choice is hashed into the move — the opponent sees only the commit.
        if (pendingDecoy) {
            const int W = GetScreenWidth(), H = GetScreenHeight();
            DrawRectangle(0, 0, W, H, Color{0, 0, 0, 170});
            const char* title = "DECOY COMMITMENT";
            const int titleW = MeasureText(title, 34);
            DrawText(title, (W - titleW) / 2, H / 2 - 110, 34, render::ui::kAccent);
            const char* sub = "Commit (secretly) to which member will be the real one.";
            const int subW = MeasureText(sub, 16);
            DrawText(sub, (W - subW) / 2, H / 2 - 62, 16, render::ui::kText);
            const Vector2 mp = GetMousePosition();
            Rectangle a{static_cast<float>(W) / 2 - 290, static_cast<float>(H) / 2 - 20, 280, 46};
            Rectangle b{static_cast<float>(W) / 2 + 10, static_cast<float>(H) / 2 - 20, 280, 46};
            Rectangle c{static_cast<float>(W) / 2 - 90, static_cast<float>(H) / 2 + 44, 180, 36};
            std::optional<char> choice;
            if (render::ui::button(a, "A — stay the ORIGINAL", mp, render::ui::kPanel)) choice = 'a';
            if (render::ui::button(b, "B — swap to the TWIN", mp, render::ui::kPanel)) choice = 'b';
            if (choice) {
                if (auto s = source->submitWithChoice(*pendingDecoy, *choice)) status = *s;
                else status = "Decoy committed — your choice stays hidden until the reveal.";
                pendingDecoy.reset();
            } else if (render::ui::button(c, "Cancel", mp, render::ui::kPanel)) {
                pendingDecoy.reset();
                status = "Decoy cast cancelled.";
            }
        }

        // End-of-match screen for an ONLINE game: clear result + return to the lobby
        // (a local match keeps the Tab=editor / R=rematch status instead).
        if (finished && onlineMatch) {
            const int W = GetScreenWidth(), H = GetScreenHeight();
            DrawRectangle(0, 0, W, H, Color{0, 0, 0, 190});
            const std::optional<Faction> w = source->winner();
            // A spectator has no seat — name the winner neutrally instead.
            const char* title = !w                       ? "DRAW"
                                : spectating             ? (*w == Faction::Player ? "PLAYER WINS" : "ENEMY WINS")
                                : (*w == source->localSeat()) ? "VICTORY"
                                                              : "DEFEAT";
            const Color tc = !w ? render::ui::kMuted
                             : spectating ? render::ui::kAccent
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
            spectating = false;
            state = lobby ? AppState::Lobby : AppState::Menu;
        }
    }

    if (pack) pack->unload(); // free textures while the GL context is still alive
    CloseWindow();
    return 0;
}
