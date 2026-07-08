//
// lobby_forfeit_demo.cpp — Phase 4.5 slice 6.3: idle-clock forfeit.
//
// A live match with a short per-move window: the seeker (Player) sits idle and
// never moves. The server forfeits the idle seat once the read-timeout elapses,
// tells BOTH clients the winner (a forfeit has no death to infer it from), and
// records Elo for the winner. CI smoke test (bounded wall-clock).
//
#include "core/Build.h"
#include "core/Creatures.h"
#include "core/Ruleset.h"
#include "core/Spells.h"
#include "net/AccountStore.h"
#include "net/GameServer.h" // contentHashOf
#include "net/Lobby.h"
#include "net/MirrorSession.h"
#include "net/Socket.h"

#include <chrono>
#include <cstdio>
#include <memory>
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
CharacterBuild makeBuild(const char* name) {
    CharacterBuild b;
    b.name = name;
    b.stats.hpPurchases = 2;
    b.spellIds = {spellid::Attack};
    return b;
}
} // namespace

int main() {
    const SpellCatalog catalog = makeDefaultCatalog();
    const std::vector<Entity> creatures = makeDefaultCreatures();
    const Ruleset ruleset = makeDefaultRuleset();

    const std::string dbPath = "tb_lobby_forfeit_test.json";
    std::remove(dbPath.c_str());
    AccountStore accounts(dbPath);

    LobbyConfig cfg;
    cfg.catalog = catalog;
    cfg.creatures = creatures;
    cfg.casualRules = ruleset;
    cfg.rankedRules = ruleset;
    cfg.contentHash = contentHashOf(catalog);
    cfg.accounts = &accounts;

    std::optional<Listener> listener = Listener::bind(0);
    const uint16_t port = listener->port();
    constexpr int kConns = 4; // 2 sessions + 2 match conns
    std::thread lobby([&] { serveLobby(*listener, cfg, /*maxConns=*/kConns, /*readTimeoutSec=*/30); });

    MatchFormat fmt;
    fmt.time = MatchFormat::Time::PerMove;
    fmt.perMoveSec = 1; // a 1-second idle window → quick forfeit
    fmt.rated = true;

    std::optional<Faction> aliceForfeitWinner, bobForfeitWinner;
    bool bobFinished = false;
    {
        std::string e;
        std::unique_ptr<LobbySession> alice =
            LobbySession::connect("127.0.0.1", port, cfg.contentHash, "alice", "pw-a", &e);
        std::unique_ptr<LobbySession> bob =
            LobbySession::connect("127.0.0.1", port, cfg.contentHash, "bob", "pw-b", &e);
        CHECK(alice && bob, "both players log in");

        CHECK(alice->seek(fmt, makeBuild("Alice"), &e), "alice seeks a 1s/move rated game");
        std::optional<std::vector<SeekInfo>> seeks = bob->listSeeks();
        std::optional<PairedInfo> bobPair =
            (seeks && !seeks->empty()) ? bob->acceptSeek((*seeks)[0].id, makeBuild("Bob"), &e)
                                       : std::nullopt;
        std::optional<PairedInfo> alicePair = alice->poll();
        CHECK(bobPair && alicePair && bobPair->live, "paired into a live match");
        if (!bobPair || !alicePair) { lobby.join(); return 1; }

        std::printf("Alice (Player, moves first) sits idle and forfeits\n");
        // Concurrent join; alice then does NOTHING while bob waits for the end.
        std::unique_ptr<MirrorSession> aMs, bMs;
        std::thread ta([&] { aMs = MirrorSession::joinToken("127.0.0.1", port, alicePair->token, ruleset, catalog, creatures, &e); });
        std::thread tb([&] { bMs = MirrorSession::joinToken("127.0.0.1", port, bobPair->token, ruleset, catalog, creatures, &e); });
        ta.join();
        tb.join();
        CHECK(aMs && bMs, "both clients join the match");
        if (!aMs || !bMs) { lobby.join(); return 1; }

        // Bob pumps and waits; alice never sends an intent (idle). Bound the wait so
        // the test can't hang if the forfeit never fires.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (!bMs->finished() && std::chrono::steady_clock::now() < deadline) {
            bMs->pump(100);
        }
        aMs->pump(200); // let alice see the end too
        bobFinished = bMs->finished();
        aliceForfeitWinner = aMs->forfeitWinner();
        bobForfeitWinner = bMs->forfeitWinner();

        CHECK(bobFinished, "the match ends (bob's mirror sees the forfeit) without a full game");
        CHECK(bobForfeitWinner && *bobForfeitWinner == Faction::Enemy,
              "both are told the winner: Enemy (bob), since Player (alice) sat idle");
        CHECK(aliceForfeitWinner && *aliceForfeitWinner == Faction::Enemy,
              "alice's client also learns she forfeited");
    }

    lobby.join();

    std::printf("The forfeit recorded Elo for the winner\n");
    const std::optional<AccountView> a = accounts.get("alice");
    const std::optional<AccountView> b = accounts.get("bob");
    CHECK(a && b, "both accounts exist");
    if (a && b) {
        CHECK(b->rating > kDefaultRating && a->rating < kDefaultRating,
              "bob (won by forfeit) gained rating, alice lost");
        CHECK(a->rating + b->rating == 2 * kDefaultRating, "zero-sum");
        CHECK(b->wins == 1 && a->losses == 1, "the forfeit counts as a win/loss");
    }
    std::remove(dbPath.c_str());

    std::printf("\n%s (%d failure%s)\n", g_fails == 0 ? "ALL PASS" : "FAILURES", g_fails,
                g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
