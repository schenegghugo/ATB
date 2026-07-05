//
// ranked_demo.cpp — Phase 4.5 (slice 2): a ranked match end to end.
//
// A ranked server (AccountStore set) authenticates two logging-in clients over a
// real socket, runs their match, and records the Elo result; a wrong-password
// client is rejected. CI smoke test.
//
#include "core/Build.h"
#include "core/Creatures.h"
#include "core/Ruleset.h"
#include "core/Spells.h"
#include "net/AccountStore.h"
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

    const std::string dbPath = "tb_ranked_test.json";
    std::remove(dbPath.c_str());

    std::printf("A ranked match authenticates both players and records Elo\n");
    {
        AccountStore accounts(dbPath);
        MatchConfig cfg{ruleset, catalog, creatures, hash, &accounts};

        std::optional<Listener> listener = Listener::bind(0);
        const uint16_t port = listener->port();
        int matchesRun = -1;
        std::thread srv([&] { matchesRun = serveMatches(*listener, cfg, /*maxMatches=*/1); });

        ClientResult p1, p2;
        std::thread t0([&] {
            p1 = playClient("127.0.0.1", port, hash, aggro(), ruleset, catalog, creatures, chasePolicy,
                            15, "p1", "pw1");
        });
        std::thread t1([&] {
            p2 = playClient("127.0.0.1", port, hash, aggro(), ruleset, catalog, creatures, chasePolicy,
                            15, "p2", "pw2");
        });
        t0.join();
        t1.join();
        srv.join();

        CHECK(matchesRun == 1 && p1.ok && p2.ok, "both authenticated clients played the match");
        CHECK(accounts.size() == 2, "both players were auto-registered");

        const int r1 = accounts.ratingOf("p1"), r2 = accounts.ratingOf("p2");
        CHECK(r1 + r2 == 2 * kDefaultRating, "ratings are zero-sum");
        const AccountView v1 = *accounts.get("p1");
        const AccountView v2 = *accounts.get("p2");
        const int games = v1.wins + v1.losses + v2.wins + v2.losses;
        CHECK(games == 0 || games == 2, "a decisive result records exactly one W and one L");
        if (games == 2)
            CHECK((r1 == kDefaultRating + 16 && r2 == kDefaultRating - 16) ||
                      (r1 == kDefaultRating - 16 && r2 == kDefaultRating + 16),
                  "Elo applied: winner +16, loser -16");
        else
            CHECK(r1 == kDefaultRating && r2 == kDefaultRating, "a draw leaves ratings unchanged");
    }

    std::printf("A wrong-password login is rejected; the queue keeps working\n");
    {
        AccountStore accounts(dbPath); // reloads p1/p2; pre-registers alice
        accounts.authenticate("alice", "correct-horse");
        MatchConfig cfg{ruleset, catalog, creatures, hash, &accounts};

        std::optional<Listener> listener = Listener::bind(0);
        const uint16_t port = listener->port();
        int matchesRun = -1;
        std::thread srv([&] { matchesRun = serveMatches(*listener, cfg, /*maxMatches=*/1); });

        // Bad login first (rejected + dropped), then two good players who still pair.
        ClientResult bad;
        std::thread tbad([&] {
            bad = playClient("127.0.0.1", port, hash, aggro(), ruleset, catalog, creatures, chasePolicy,
                             15, "alice", "wrong");
        });
        tbad.join();

        ClientResult g0, g1;
        std::thread t0([&] {
            g0 = playClient("127.0.0.1", port, hash, aggro(), ruleset, catalog, creatures, chasePolicy,
                            15, "p1", "pw1");
        });
        std::thread t1([&] {
            g1 = playClient("127.0.0.1", port, hash, aggro(), ruleset, catalog, creatures, chasePolicy,
                            15, "p2", "pw2");
        });
        t0.join();
        t1.join();
        srv.join();

        CHECK(!bad.ok && bad.error.find("password") != std::string::npos,
              "wrong password is rejected with a clear message");
        CHECK(matchesRun == 1 && g0.ok && g1.ok, "valid players still get matched after the rejection");
    }

    std::remove(dbPath.c_str());
    std::printf("\n%s (%d failure%s)\n", g_fails == 0 ? "ALL PASS" : "FAILURES", g_fails,
                g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
