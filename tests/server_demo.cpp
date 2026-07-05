//
// server_demo.cpp — Phase 4.5 (slice 1): the persistent matchmaking server.
//
// Starts serveMatches() on an ephemeral port and connects four clients; they must
// be paired FIFO into two concurrent authoritative matches that both run to
// completion over real sockets. Also checks a rejected (bad-hash) player is
// dropped without wedging the queue. CI smoke test.
//
#include "core/Build.h"
#include "core/Creatures.h"
#include "core/Ruleset.h"
#include "core/Spells.h"
#include "net/GameClient.h"
#include "net/GameServer.h"
#include "net/Socket.h"

#include <atomic>
#include <cstdio>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace tb;
using namespace tb::net;

static int g_fails = 0;
#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) std::printf("  [PASS] %s\n", msg);                                               \
        else { std::printf("  [FAIL] %s\n", msg); ++g_fails; }                                     \
    } while (0)

namespace {

CharacterBuild aggro() {
    CharacterBuild b;
    b.name = "Aggro";
    b.spellIds = {spellid::Attack, spellid::Fireball, spellid::Poison};
    return b;
}

} // namespace

int main() {
    const SpellCatalog catalog = makeDefaultCatalog();
    const std::vector<Entity> creatures = makeDefaultCreatures();
    const Ruleset ruleset = makeDefaultRuleset();
    const std::string hash = contentHashOf(catalog);
    const MatchConfig cfg{ruleset, catalog, creatures, hash};

    std::printf("Four players are matched into two concurrent matches\n");
    {
        std::optional<Listener> listener = Listener::bind(0);
        CHECK(listener.has_value(), "server binds an ephemeral port");
        const uint16_t port = listener->port();

        int matchesRun = -1;
        std::thread srv([&] { matchesRun = serveMatches(*listener, cfg, /*maxMatches=*/2); });

        std::atomic<int> okCount{0};
        std::vector<std::thread> clients;
        for (int i = 0; i < 4; ++i)
            clients.emplace_back([&] {
                ClientResult r =
                    playClient("127.0.0.1", port, hash, aggro(), ruleset, catalog, creatures, chasePolicy);
                if (r.ok && !r.finalSnapshot.empty()) ++okCount;
            });
        for (std::thread& t : clients) t.join();
        srv.join();

        CHECK(matchesRun == 2, "the server started exactly two matches");
        CHECK(okCount.load() == 4, "all four clients played a full match to a finish");
    }

    std::printf("A rejected player is dropped without wedging the queue\n");
    {
        std::optional<Listener> listener = Listener::bind(0);
        const uint16_t port = listener->port();

        // One match's worth of good players, plus a bad-hash client the server must
        // drop (not pair). The two good players should still be matched.
        int matchesRun = -1;
        std::thread srv([&] { matchesRun = serveMatches(*listener, cfg, /*maxMatches=*/1); });

        ClientResult bad, g0, g1;
        std::thread tb([&] { bad = playClient("127.0.0.1", port, "deadbeef", aggro(), ruleset, catalog, creatures, chasePolicy); });
        tb.join(); // let the bad client be rejected first
        std::thread t0([&] { g0 = playClient("127.0.0.1", port, hash, aggro(), ruleset, catalog, creatures, chasePolicy); });
        std::thread t1([&] { g1 = playClient("127.0.0.1", port, hash, aggro(), ruleset, catalog, creatures, chasePolicy); });
        t0.join();
        t1.join();
        srv.join();

        CHECK(!bad.ok && bad.error.find("content hash") != std::string::npos,
              "the bad-hash player is rejected");
        CHECK(matchesRun == 1 && g0.ok && g1.ok, "the two valid players are still matched and play");
    }

    std::printf("\n%s (%d failure%s)\n", g_fails == 0 ? "ALL PASS" : "FAILURES", g_fails,
                g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
