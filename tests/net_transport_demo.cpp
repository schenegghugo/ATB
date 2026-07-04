//
// net_transport_demo.cpp — Phase 4.4: an authoritative 1v1 over a REAL socket.
//
// Spins up the GameServer on an ephemeral localhost port and drives two
// GameClients (each a deterministic mirror, in its own thread) through a full
// match. Proves the transport + handshake + build admission + per-intent
// authority work end to end, and that each client's mirror stays byte-identical
// to the server's authoritative state. CI smoke test.
//
#include "core/Build.h"
#include "core/Creatures.h"
#include "core/Ruleset.h"
#include "core/Spells.h"
#include "net/GameClient.h"
#include "net/GameServer.h"
#include "net/Socket.h"

#include <cstdio>
#include <optional>
#include <string>
#include <thread>

using namespace tb;
using namespace tb::net;

static int g_fails = 0;
#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) std::printf("  [PASS] %s\n", msg);                                               \
        else { std::printf("  [FAIL] %s\n", msg); ++g_fails; }                                     \
    } while (0)

namespace {

CharacterBuild pyromancer() {
    CharacterBuild b;
    b.name = "Pyromancer";
    b.spellIds = {spellid::Attack, spellid::Fireball, spellid::Poison};
    return b;
}
CharacterBuild bruiser() {
    CharacterBuild b;
    b.name = "Bruiser";
    b.stats.hpPurchases = 2;
    b.spellIds = {spellid::Attack, spellid::Knockback};
    return b;
}

} // namespace

int main() {
    const SpellCatalog catalog = makeDefaultCatalog();
    const std::vector<Entity> creatures = makeDefaultCreatures();
    const Ruleset ruleset = makeDefaultRuleset();
    const std::string hash = contentHashOf(catalog);

    std::printf("Builds are admissible under the ruleset\n");
    {
        CHECK(validateBuild(pyromancer(), catalog, ruleset.economy, ruleset.bannedSpells).ok,
              "player build validates");
        CHECK(validateBuild(bruiser(), catalog, ruleset.economy, ruleset.bannedSpells).ok,
              "enemy build validates");
    }

    const MatchConfig cfg{ruleset, catalog, creatures, hash};

    std::printf("A full 1v1 plays over a real localhost socket\n");
    {
        std::optional<Listener> listener = Listener::bind(0);
        CHECK(listener.has_value(), "server binds an ephemeral port");
        const uint16_t port = listener->port();

        ServeResult server;
        std::thread srv([&] { server = serveOneMatch(*listener, cfg); });

        ClientResult ca, cb;
        std::thread t0([&] {
            ca = playClient("127.0.0.1", port, hash, pyromancer(), ruleset, catalog, creatures, chasePolicy);
        });
        std::thread t1([&] {
            cb = playClient("127.0.0.1", port, hash, bruiser(), ruleset, catalog, creatures, chasePolicy);
        });
        t0.join();
        t1.join();
        srv.join();

        CHECK(server.ok, "server ran the match to completion");
        CHECK(server.winner.has_value(), "the match produced a winner");
        CHECK(ca.ok && cb.ok, "both clients finished cleanly");
        CHECK(ca.seat.has_value() && cb.seat.has_value() && *ca.seat != *cb.seat,
              "clients were seated on opposite sides");
        CHECK(!server.finalSnapshot.empty() && ca.finalSnapshot == server.finalSnapshot &&
                  cb.finalSnapshot == server.finalSnapshot,
              "both client mirrors match the server's authoritative final state byte-for-byte");
    }

    std::printf("A content-hash mismatch is rejected at the handshake\n");
    {
        std::optional<Listener> listener = Listener::bind(0);
        CHECK(listener.has_value(), "server binds a port");
        const uint16_t port = listener->port();

        ServeResult server;
        std::thread srv([&] { server = serveOneMatch(*listener, cfg); });

        ClientResult good, bad;
        std::thread t0([&] {
            good = playClient("127.0.0.1", port, hash, pyromancer(), ruleset, catalog, creatures, chasePolicy);
        });
        std::thread t1([&] {
            bad = playClient("127.0.0.1", port, "deadbeef", bruiser(), ruleset, catalog, creatures, chasePolicy);
        });
        t0.join();
        t1.join();
        srv.join();

        CHECK(!server.ok, "server aborts the match on a bad handshake");
        CHECK(bad.error.find("content hash") != std::string::npos,
              "the mismatched client is told its content hash is wrong");
        CHECK(!good.ok, "the match does not proceed for the honest client either");
    }

    std::printf("\n%s (%d failure%s)\n", g_fails == 0 ? "ALL PASS" : "FAILURES", g_fails,
                g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
