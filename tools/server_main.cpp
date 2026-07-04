//
// server_main.cpp — tb_server: a self-hosted 1v1 custom-match server (Phase 4.4).
//
// Loads the shared content from ./data (same files the GUI uses, so their content
// hashes match), pins that hash, listens on a port (default 5555 or argv[1]), and
// serves ONE authoritative match, then exits. A lobby / multi-match server is
// Phase 4.5. Usage: tb_server [port]  (run from the repo root so ./data resolves).
//
#include "core/Creatures.h"
#include "core/Ruleset.h"
#include "core/Spells.h"
#include "data/CatalogJson.h"
#include "data/CreatureJson.h"
#include "data/RulesetJson.h"
#include "net/GameServer.h"
#include "net/Socket.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

using namespace tb;
using namespace tb::net;

int main(int argc, char** argv) {
    const uint16_t port = static_cast<uint16_t>(argc > 1 ? std::atoi(argv[1]) : 5555);

    MatchConfig cfg;
    cfg.catalog = makeDefaultCatalog();
    cfg.creatures = makeDefaultCreatures();
    cfg.ruleset = makeDefaultRuleset();

    // Load ./data if present (matching the GUI). Absent -> the compiled defaults;
    // present-but-invalid -> fail loudly rather than serve mismatched content.
    if (std::ifstream("data/catalog.json").good()) {
        CatalogLoad l = loadCatalogFromFile("data/catalog.json");
        if (!l.ok) { std::fprintf(stderr, "tb_server: data/catalog.json invalid\n"); return 1; }
        cfg.catalog = std::move(l.catalog);
    }
    if (std::ifstream("data/creatures.json").good()) {
        CreatureLoad l = loadCreaturesFromFile("data/creatures.json");
        if (!l.ok) { std::fprintf(stderr, "tb_server: data/creatures.json invalid\n"); return 1; }
        cfg.creatures = std::move(l.creatures);
    }
    if (std::ifstream("data/rules.json").good()) {
        RulesetLoad l = loadRulesetFromFile("data/rules.json");
        if (!l.ok) { std::fprintf(stderr, "tb_server: data/rules.json invalid\n"); return 1; }
        cfg.ruleset = std::move(l.ruleset);
    }
    cfg.contentHash = contentHashOf(cfg.catalog);

    std::optional<Listener> listener = Listener::bind(port);
    if (!listener) {
        std::fprintf(stderr, "tb_server: could not bind port %u\n", port);
        return 1;
    }
    std::printf("tb_server: listening on 127.0.0.1:%u (content %.12s…)\n", listener->port(),
                cfg.contentHash.c_str());
    std::printf("tb_server: waiting for 2 clients…\n");

    const ServeResult r = serveOneMatch(*listener, cfg);
    if (!r.ok) {
        std::fprintf(stderr, "tb_server: match aborted — %s\n", r.error.c_str());
        return 1;
    }
    std::printf("tb_server: match finished — %s wins\n",
                r.winner && *r.winner == Faction::Player ? "player" : "enemy");
    return 0;
}
