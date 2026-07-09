//
// lobby_main.cpp — tb_lobby: the self-hosted Online Home (Phase 4.5 slice 5).
//
// The daemon behind the GUI's lobby screen: it holds authenticated sessions and a
// board of open seeks + directed challenges, and routes each accepted pairing to a
// live authoritative match (per-move/chess) or a correspondence game (Unlimited,
// played over the built-in Mailbox and ranked by the built-in arbiter). Loads the
// shared content from ./data so its content hash matches the GUI's.
//
//   Usage: tb_lobby [port] [bind-addr] [casual-rules] [ranked-rules] [persist-dir]
//     port         listen port                (default 5556)
//     bind-addr    interface to bind          (default 127.0.0.1; 0.0.0.0 = all
//                  interfaces — only behind a firewall/VPN; or a specific IP)
//     casual-rules ruleset for casual play    (default data/rules.json)
//     ranked-rules ruleset for rated play     (default data/rules.ranked.json)
//     persist-dir  correspondence state dir   (default lobby-state; "-" = in-memory
//                  only). Open correspondence games + their move logs survive a
//                  restart; clients cold-resume them from the lobby.
//
// Run from the repo root so ./data resolves. Ctrl-C (or a service stop) to quit.
//
#include "core/Creatures.h"
#include "core/Ruleset.h"
#include "core/Spells.h"
#include "data/CatalogJson.h"
#include "data/CreatureJson.h"
#include "data/RulesetJson.h"
#include "net/AccountStore.h"
#include "net/GameServer.h" // contentHashOf
#include "net/Lobby.h"
#include "net/Socket.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

using namespace tb;
using namespace tb::net;

namespace {
// Load a ruleset file into `out`; false (with a message) only if present-but-invalid.
// Absent → keep the compiled default. Returns true on ok/absent.
bool loadRules(const std::string& path, Ruleset& out, bool required) {
    if (std::ifstream(path).good()) {
        RulesetLoad l = loadRulesetFromFile(path);
        if (!l.ok) { std::fprintf(stderr, "tb_lobby: %s invalid\n", path.c_str()); return false; }
        out = std::move(l.ruleset);
        std::printf("tb_lobby: %s (v%s)\n", path.c_str(), l.version.c_str());
    } else if (required) {
        std::fprintf(stderr, "tb_lobby: rules file '%s' not found\n", path.c_str());
        return false;
    }
    return true;
}
} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0); // line-buffer for journald

    const uint16_t port = static_cast<uint16_t>(argc > 1 ? std::atoi(argv[1]) : 5556);
    const std::string bindAddr = argc > 2 ? argv[2] : "127.0.0.1";
    const std::string casualFile = argc > 3 ? argv[3] : "data/rules.json";
    const std::string rankedFile = argc > 4 ? argv[4] : "data/rules.ranked.json";
    const std::string persistDir = argc > 5 ? argv[5] : "lobby-state";

    LobbyConfig cfg;
    cfg.catalog = makeDefaultCatalog();
    cfg.creatures = makeDefaultCreatures();
    cfg.casualRules = makeDefaultRuleset();
    cfg.rankedRules = makeDefaultRuleset();

    // Load ./data if present (matching the GUI). Present-but-invalid → fail loudly.
    if (std::ifstream("data/catalog.json").good()) {
        CatalogLoad l = loadCatalogFromFile("data/catalog.json");
        if (!l.ok) { std::fprintf(stderr, "tb_lobby: data/catalog.json invalid\n"); return 1; }
        cfg.catalog = std::move(l.catalog);
    } else {
        // No ./data → compiled defaults, whose hash cannot match a GUI that loaded
        // data/. This is the #1 "content hash mismatch" cause — say so loudly.
        std::fprintf(stderr,
                     "tb_lobby: WARNING — no ./data/catalog.json found; using compiled defaults.\n"
                     "          A GUI client that loaded data/ will FAIL the content-hash check.\n"
                     "          Run tb_lobby from the repo root (where ./data lives).\n");
    }
    if (std::ifstream("data/creatures.json").good()) {
        CreatureLoad l = loadCreaturesFromFile("data/creatures.json");
        if (!l.ok) { std::fprintf(stderr, "tb_lobby: data/creatures.json invalid\n"); return 1; }
        cfg.creatures = std::move(l.creatures);
    }
    // Casual defaults silently; an explicitly-passed ranked file must exist.
    if (!loadRules(casualFile, cfg.casualRules, /*required=*/argc > 3)) return 1;
    if (!loadRules(rankedFile, cfg.rankedRules, /*required=*/argc > 4)) return 1;
    cfg.contentHash = contentHashOf(cfg.catalog);

    // Persistent accounts + Elo (ranked play + non-guest login). Passwords are in
    // the clear on the wire — put this behind TLS/VPN before exposing it publicly.
    AccountStore accounts("accounts.json");
    cfg.accounts = &accounts;
    if (persistDir != "-") {
        cfg.persistDir = persistDir;
        std::printf("tb_lobby: correspondence state persists in %s/\n", persistDir.c_str());
    }

    std::optional<Listener> listener = Listener::bind(port, bindAddr);
    if (!listener) {
        std::fprintf(stderr, "tb_lobby: could not bind %s:%u\n", bindAddr.c_str(), port);
        return 1;
    }
    std::printf("tb_lobby: listening on %s:%u (content %.12s…, %zu accounts)\n", bindAddr.c_str(),
                listener->port(), cfg.contentHash.c_str(), accounts.size());
    if (bindAddr == "0.0.0.0")
        std::printf("tb_lobby: bound to ALL interfaces — ensure a firewall/VPN guards this port.\n");
    std::printf("tb_lobby: Online Home up — seeks, challenges, live + correspondence (Ctrl-C to stop).\n");

    serveLobby(*listener, cfg, /*maxConns=*/-1, /*readTimeoutSec=*/300);
    return 0;
}
