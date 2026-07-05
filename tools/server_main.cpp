//
// server_main.cpp — tb_server: a self-hosted authoritative match server.
//
// Loads the shared content from ./data (same files the GUI uses, so their content
// hashes match), pins that hash, and runs a persistent matchmaking loop: it pairs
// connecting players FIFO and runs each match concurrently (Phase 4.5, slice 1).
//
//   Usage: tb_server [port] [bind-addr] [rules-file]
//     port       listen port                    (default 5555)
//     bind-addr  interface to bind              (default 127.0.0.1)
//                127.0.0.1 = local only (safe default); 0.0.0.0 = all interfaces
//                (LAN/internet — only behind a firewall/VPN); or a specific IP
//                (e.g. a Tailscale address) to expose it to just that network.
//     rules-file ruleset to serve               (default data/rules.json;
//                pass data/rules.ranked.json to host the official ranked format)
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
#include "net/GameServer.h"
#include "net/Socket.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

using namespace tb;
using namespace tb::net;

int main(int argc, char** argv) {
    // Line-buffer stdout so "listening on ..." reaches logs/journald immediately
    // (a killed daemon would otherwise take its unflushed startup lines with it).
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    const uint16_t port = static_cast<uint16_t>(argc > 1 ? std::atoi(argv[1]) : 5555);
    const std::string bindAddr = argc > 2 ? argv[2] : "127.0.0.1";
    const std::string rulesFile = argc > 3 ? argv[3] : "data/rules.json";

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
    if (std::ifstream(rulesFile).good()) {
        RulesetLoad l = loadRulesetFromFile(rulesFile);
        if (!l.ok) { std::fprintf(stderr, "tb_server: %s invalid\n", rulesFile.c_str()); return 1; }
        cfg.ruleset = std::move(l.ruleset);
        std::printf("tb_server: serving ruleset '%s' (v%s)\n", rulesFile.c_str(), l.version.c_str());
    } else if (argc > 3) {
        std::fprintf(stderr, "tb_server: rules file '%s' not found\n", rulesFile.c_str());
        return 1; // an explicitly requested ruleset must exist — don't silently serve defaults
    }
    cfg.contentHash = contentHashOf(cfg.catalog);

    // Ranked: persistent accounts + Elo in ./accounts.json. Clients must send a
    // login (auto-registered on first use). Put passwords behind TLS/VPN before
    // exposing this publicly — the transport is not yet encrypted.
    AccountStore accounts("accounts.json");
    cfg.accounts = &accounts;

    std::optional<Listener> listener = Listener::bind(port, bindAddr);
    if (!listener) {
        std::fprintf(stderr, "tb_server: could not bind %s:%u\n", bindAddr.c_str(), port);
        return 1;
    }
    std::printf("tb_server: listening on %s:%u (content %.12s…, %zu accounts)\n", bindAddr.c_str(),
                listener->port(), cfg.contentHash.c_str(), accounts.size());
    if (bindAddr == "0.0.0.0")
        std::printf("tb_server: bound to ALL interfaces — ensure a firewall/VPN guards this port.\n");
    std::printf("tb_server: matchmaking — pairing players as they connect (Ctrl-C to stop).\n");

    // Persistent daemon: pair players forever. A generous per-move read timeout
    // doubles as a turn clock (a player idle past it forfeits their match).
    serveMatches(*listener, cfg, /*maxMatches=*/-1, /*readTimeoutSec=*/300);
    return 0;
}
