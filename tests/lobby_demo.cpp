//
// lobby_demo.cpp — Phase 4.5 (slice 3): private lobbies via a shared code.
//
// Players presenting the same non-empty lobby code are paired into a private
// match; different codes never cross-pair. Verified over real sockets by tagging
// each room's champions and checking no foreign tag reaches a client's final
// snapshot. CI smoke test.
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

CharacterBuild named(const std::string& n) {
    CharacterBuild b;
    b.name = n; // shows up in the snapshot, so we can prove who was matched with whom
    b.spellIds = {spellid::Attack, spellid::Fireball, spellid::Poison};
    return b;
}

} // namespace

int main() {
    const SpellCatalog catalog = makeDefaultCatalog();
    const std::vector<Entity> creatures = makeDefaultCreatures();
    const Ruleset ruleset = makeDefaultRuleset();
    const std::string hash = contentHashOf(catalog);
    const MatchConfig cfg{ruleset, catalog, creatures, hash}; // unranked/custom (no accounts)

    std::printf("Two players sharing a code get a private match\n");
    {
        std::optional<Listener> listener = Listener::bind(0);
        const uint16_t port = listener->port();
        int matchesRun = -1;
        std::thread srv([&] { matchesRun = serveMatches(*listener, cfg, /*maxMatches=*/1); });

        ClientResult a, b;
        std::thread t0([&] { a = playClient("127.0.0.1", port, hash, named("Ax"), ruleset, catalog, creatures, chasePolicy, 15, "", "", "duel"); });
        std::thread t1([&] { b = playClient("127.0.0.1", port, hash, named("Bx"), ruleset, catalog, creatures, chasePolicy, 15, "", "", "duel"); });
        t0.join();
        t1.join();
        srv.join();
        CHECK(matchesRun == 1 && a.ok && b.ok, "the two code-sharers played a match");
    }

    std::printf("Two rooms in flight never cross-pair\n");
    {
        std::optional<Listener> listener = Listener::bind(0);
        const uint16_t port = listener->port();
        int matchesRun = -1;
        std::thread srv([&] { matchesRun = serveMatches(*listener, cfg, /*maxMatches=*/2); });

        // Room "r1": champions tagged R1*. Room "r2": tagged R2*.
        ClientResult a, b, c, d;
        std::thread t0([&] { a = playClient("127.0.0.1", port, hash, named("R1a"), ruleset, catalog, creatures, chasePolicy, 15, "", "", "r1"); });
        std::thread t1([&] { b = playClient("127.0.0.1", port, hash, named("R1b"), ruleset, catalog, creatures, chasePolicy, 15, "", "", "r1"); });
        std::thread t2([&] { c = playClient("127.0.0.1", port, hash, named("R2c"), ruleset, catalog, creatures, chasePolicy, 15, "", "", "r2"); });
        std::thread t3([&] { d = playClient("127.0.0.1", port, hash, named("R2d"), ruleset, catalog, creatures, chasePolicy, 15, "", "", "r2"); });
        t0.join(); t1.join(); t2.join(); t3.join();
        srv.join();

        CHECK(matchesRun == 2 && a.ok && b.ok && c.ok && d.ok, "both private rooms played to a finish");
        // A room-1 client must never see a room-2 champion's tag, and vice versa.
        const bool r1clean = a.finalSnapshot.find("R2") == std::string::npos &&
                             b.finalSnapshot.find("R2") == std::string::npos;
        const bool r2clean = c.finalSnapshot.find("R1") == std::string::npos &&
                             d.finalSnapshot.find("R1") == std::string::npos;
        CHECK(r1clean && r2clean, "no client was paired across room codes (isolation holds)");
        CHECK(a.finalSnapshot.find("R1") != std::string::npos &&
                  c.finalSnapshot.find("R2") != std::string::npos,
              "each client did play within its own room");
    }

    std::printf("\n%s (%d failure%s)\n", g_fails == 0 ? "ALL PASS" : "FAILURES", g_fails,
                g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
