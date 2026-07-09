//
// chess_clock_demo.cpp — Phase 4.5 slice 6.3 (completion): the TRUE chess clock.
//
// A Chess-format lobby match runs an accumulating per-seat time bank (main +
// increment) enforced by the server: the bank ticks only on your own decisions,
// grows by the increment when your turn passes, and an empty bank forfeits (flag
// fall). Both banks ride on every `applied` broadcast, so the mirrors' clocks are
// authoritative. Covers: the welcome advertises the clock, the increment lands,
// a full game plays under the bank, and an idle player flags. CI smoke test.
//
#include "core/AI.h"
#include "core/Build.h"
#include "core/Creatures.h"
#include "core/Ruleset.h"
#include "core/Spells.h"
#include "data/Net.h"
#include "net/GameServer.h" // contentHashOf
#include "net/Lobby.h"
#include "net/MirrorSession.h"
#include "net/Socket.h"

#include "lobby_test_util.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace tb;
using namespace tb::net;
using tbtest::makeBuild;
using tbtest::readyUp;

static int g_fails = 0;
#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) std::printf("  [PASS] %s\n", msg);                                               \
        else { std::printf("  [FAIL] %s\n", msg); ++g_fails; }                                     \
    } while (0)

namespace {

// Pair two sessions via seek/accept + ready-up, then join both match conns
// concurrently (the first is parked until the second arrives).
bool pairAndJoin(LobbySession& alice, LobbySession& bob, const MatchFormat& fmt,
                 const std::string& host, uint16_t port, const Ruleset& rules,
                 const SpellCatalog& cat, const std::vector<Entity>& cre,
                 std::unique_ptr<MirrorSession>& aMs, std::unique_ptr<MirrorSession>& bMs) {
    std::string e;
    if (!alice.seek(fmt, &e)) return false;
    std::optional<std::vector<SeekInfo>> seeks = bob.listSeeks();
    std::optional<ReadyCheckInfo> bobRc =
        (seeks && !seeks->empty()) ? bob.acceptSeek(seeks->back().id, &e) : std::nullopt;
    LobbyEvent aliceEv = alice.poll();
    PairedInfo aPair, bPair;
    if (!bobRc || aliceEv.kind != LobbyEvent::Kind::ReadyCheck ||
        !readyUp(alice, aliceEv.readyCheck, makeBuild("Alice"), bob, *bobRc, makeBuild("Bob"),
                 aPair, bPair))
        return false;
    std::thread ta([&] { aMs = MirrorSession::joinToken(host, port, aPair.token, rules, cat, cre, &e); });
    std::thread tbb([&] { bMs = MirrorSession::joinToken(host, port, bPair.token, rules, cat, cre, &e); });
    ta.join();
    tbb.join();
    return aMs && bMs;
}

// Drive both mirrors to a finish with the default Brain (see lobby_challenge_demo).
bool playBoth(MirrorSession& a, MirrorSession& b) {
    for (int guard = 0; guard < 400000 && !(a.finished() && b.finished()); ++guard) {
        a.pump(16);
        b.pump(16);
        MirrorSession* mv = a.awaitingMe() ? &a : (b.awaitingMe() ? &b : nullptr);
        if (!mv) continue;
        const EntityId me = mv->battle().activeUnit();
        const std::vector<PlannedAction> plan = defaultBrain().planTurn(mv->battle(), me);
        if (plan.empty()) {
            mv->send(net::Intent::endTurn());
        } else {
            const PlannedAction& act = plan.front();
            mv->send(act.kind == PlannedAction::Kind::Cast ? net::Intent::cast(act.slot, act.target)
                                                           : net::Intent::move(act.target));
        }
        a.pump(2000);
        b.pump(2000);
    }
    return a.finished() && b.finished();
}

} // namespace

int main() {
    const SpellCatalog catalog = makeDefaultCatalog();
    const std::vector<Entity> creatures = makeDefaultCreatures();
    const Ruleset ruleset = makeDefaultRuleset();

    LobbyConfig cfg;
    cfg.catalog = catalog;
    cfg.creatures = creatures;
    cfg.casualRules = ruleset;
    cfg.rankedRules = ruleset;
    cfg.contentHash = contentHashOf(catalog);

    std::optional<Listener> listener = Listener::bind(0);
    const uint16_t port = listener->port();
    constexpr int kConns = 6; // 2 sessions + 2 matches × 2 match conns
    std::thread lobby([&] { serveLobby(*listener, cfg, /*maxConns=*/kConns, /*readTimeoutSec=*/30); });

    {
        std::string e;
        std::unique_ptr<LobbySession> alice =
            LobbySession::connect("127.0.0.1", port, cfg.contentHash, "", "", &e);
        std::unique_ptr<LobbySession> bob =
            LobbySession::connect("127.0.0.1", port, cfg.contentHash, "", "", &e);
        CHECK(alice && bob, "both players connect");
        if (!alice || !bob) { lobby.join(); return 1; }

        std::printf("A 60s + 5s chess game: banks + increment\n");
        MatchFormat chess;
        chess.time = MatchFormat::Time::Chess;
        chess.mainSec = 60;
        chess.incSec = 5;
        std::unique_ptr<MirrorSession> aMs, bMs;
        CHECK(pairAndJoin(*alice, *bob, chess, "127.0.0.1", port, ruleset, catalog, creatures, aMs, bMs),
              "paired + joined a chess-clock match");
        if (!aMs || !bMs) { lobby.join(); return 1; }

        CHECK(aMs->chessClock() && bMs->chessClock(), "the welcome advertises a chess clock");
        CHECK(aMs->clockSec() == 0, "…and no per-move window (the bank IS the clock)");
        CHECK(aMs->bankSeconds(Faction::Player) > 59.0f && aMs->bankSeconds(Faction::Enemy) <= 60.0f,
              "both banks start at the main time");

        // Alice (Player, moves first) passes immediately: her bank should gain the
        // +5s increment (minus the instant she spent) once the turn moves on.
        CHECK(aMs->awaitingMe(), "alice to move first");
        aMs->send(net::Intent::endTurn());
        aMs->pump(2000);
        bMs->pump(2000);
        const float aliceBank = aMs->bankSeconds(Faction::Player);
        CHECK(aliceBank > 62.0f && aliceBank <= 65.0f,
              "her bank gained the increment when the turn passed (~65s)");
        CHECK(playBoth(*aMs, *bMs), "the match plays to a finish under the bank");

        std::printf("A 2s + 0 chess game: an idle player flags\n");
        MatchFormat blitz;
        blitz.time = MatchFormat::Time::Chess;
        blitz.mainSec = 2;
        blitz.incSec = 0;
        std::unique_ptr<MirrorSession> aMs2, bMs2;
        CHECK(pairAndJoin(*alice, *bob, blitz, "127.0.0.1", port, ruleset, catalog, creatures, aMs2, bMs2),
              "paired + joined a 2s blitz match");
        if (!aMs2 || !bMs2) { lobby.join(); return 1; }

        // Alice sits idle; her whole bank burns in ~2s and the server forfeits her.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!bMs2->finished() && std::chrono::steady_clock::now() < deadline) bMs2->pump(100);
        aMs2->pump(200);
        CHECK(bMs2->finished(), "the match ends on the flag without a full game");
        CHECK(bMs2->forfeitWinner() && *bMs2->forfeitWinner() == Faction::Enemy,
              "the idle seat (alice) forfeits — bob wins on time");
        CHECK(aMs2->forfeitWinner() && *aMs2->forfeitWinner() == Faction::Enemy,
              "alice's client learns she flagged");
    }

    lobby.join();

    std::printf("\n%s (%d failure%s)\n", g_fails == 0 ? "ALL PASS" : "FAILURES", g_fails,
                g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
