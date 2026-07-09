//
// queue_demo.cpp — the quick-match QUEUE: auto-pairing on a widening Elo band.
//
// Queued players in the same format pair automatically once their rating gap fits
// inside the wider of their two bands (band = queueBandStart + queueBandPerSec ×
// seconds queued) — no manual accept. Close ratings pair instantly; a wide gap
// pairs only after the band has widened; the resulting ready check lands on both
// sides via poll, exactly like an accepted seek. Also covers queueLeave and the
// guest-rated refusal. CI smoke test.
//
#include "core/Creatures.h"
#include "core/Ruleset.h"
#include "core/Spells.h"
#include "net/AccountStore.h"
#include "net/GameServer.h" // contentHashOf
#include "net/Lobby.h"
#include "net/Socket.h"

#include <chrono>
#include <cstdio>
#include <memory>
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

// Poll until a ready check arrives (bounded); nullopt on timeout.
std::optional<ReadyCheckInfo> awaitReadyCheck(LobbySession& s, int maxMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(maxMs);
    while (std::chrono::steady_clock::now() < deadline) {
        const LobbyEvent ev = s.poll();
        if (ev.kind == LobbyEvent::Kind::ReadyCheck) return ev.readyCheck;
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    return std::nullopt;
}

} // namespace

int main() {
    const SpellCatalog catalog = makeDefaultCatalog();

    const std::string dbPath = "tb_queue_test.json";
    std::remove(dbPath.c_str());
    AccountStore accounts(dbPath);
    // Manufacture a rating spread before anyone connects: carol beats dave four
    // times, separating them well past the starting band.
    accounts.authenticate("carol", "pw-c");
    accounts.authenticate("dave", "pw-d");
    for (int i = 0; i < 4; ++i) accounts.recordResult("carol", "dave");
    const int carolElo = accounts.get("carol")->rating;
    const int daveElo = accounts.get("dave")->rating;

    LobbyConfig cfg;
    cfg.catalog = catalog;
    cfg.creatures = makeDefaultCreatures();
    cfg.casualRules = makeDefaultRuleset();
    cfg.rankedRules = makeDefaultRuleset();
    cfg.contentHash = contentHashOf(catalog);
    cfg.accounts = &accounts;
    cfg.queueBandStart = 40;    // narrower than carol↔dave's manufactured gap
    cfg.queueBandPerSec = 150;  // …but widening fast, so the test stays quick

    std::optional<Listener> listener = Listener::bind(0);
    const uint16_t port = listener->port();
    constexpr int kConns = 5; // alice, bob, carol, dave + one guest
    std::thread lobby([&] { serveLobby(*listener, cfg, /*maxConns=*/kConns, /*readTimeoutSec=*/30); });

    MatchFormat fmt;
    fmt.time = MatchFormat::Time::PerMove;
    fmt.perMoveSec = 30;
    fmt.rated = true;

    {
        std::string e;
        std::unique_ptr<LobbySession> alice =
            LobbySession::connect("127.0.0.1", port, cfg.contentHash, "alice", "pw-a", &e);
        std::unique_ptr<LobbySession> bob =
            LobbySession::connect("127.0.0.1", port, cfg.contentHash, "bob", "pw-b", &e);
        std::unique_ptr<LobbySession> carol =
            LobbySession::connect("127.0.0.1", port, cfg.contentHash, "carol", "pw-c", &e);
        std::unique_ptr<LobbySession> dave =
            LobbySession::connect("127.0.0.1", port, cfg.contentHash, "dave", "pw-d", &e);
        CHECK(alice && bob && carol && dave, "four players log in");
        if (!alice || !bob || !carol || !dave) { lobby.join(); return 1; }

        std::printf("A guest can't queue rated\n");
        {
            std::unique_ptr<LobbySession> guest =
                LobbySession::connect("127.0.0.1", port, cfg.contentHash, "", "", &e);
            std::string ge;
            CHECK(guest && !guest->queueJoin(fmt, &ge) && ge.find("login") != std::string::npos,
                  "a guest's rated queue join is refused");
        }

        std::printf("Equal ratings pair instantly\n");
        CHECK(alice->queueJoin(fmt, &e), "alice queues (1000)");
        CHECK(bob->queueJoin(fmt, &e), "bob queues (1000) — gap 0, inside the band");
        std::optional<ReadyCheckInfo> aRc = awaitReadyCheck(*alice, 2000);
        std::optional<ReadyCheckInfo> bRc = awaitReadyCheck(*bob, 2000);
        CHECK(aRc && bRc, "both get a ready check via poll");
        CHECK(aRc && aRc->opponent == "bob" && bRc && bRc->opponent == "alice",
              "…against each other");
        CHECK(aRc && aRc->seat == Faction::Player && bRc && bRc->seat == Faction::Enemy,
              "the earlier-queued player takes the Player seat");
        if (aRc) alice->cancelReady(aRc->id); // clean up — this demo only tests pairing

        std::printf("A wide gap pairs only once the band widens (%d vs %d Elo)\n", carolElo, daveElo);
        CHECK(carolElo - daveElo > cfg.queueBandStart,
              "the manufactured gap exceeds the starting band");
        CHECK(carol->queueJoin(fmt, &e) && dave->queueJoin(fmt, &e), "carol and dave queue");
        const LobbyEvent immediate = carol->poll();
        CHECK(immediate.kind != LobbyEvent::Kind::ReadyCheck,
              "no instant match — the gap is outside the starting band");
        std::optional<ReadyCheckInfo> cRc = awaitReadyCheck(*carol, 4000);
        std::optional<ReadyCheckInfo> dRc = awaitReadyCheck(*dave, 4000);
        CHECK(cRc && dRc && cRc->opponent == "dave" && dRc->opponent == "carol",
              "the widening band eventually pairs them");
        if (cRc) carol->cancelReady(cRc->id);

        std::printf("queueLeave removes the slot\n");
        CHECK(alice->queueJoin(fmt, &e) && alice->queueLeave(), "alice queues, then leaves");
        CHECK(bob->queueJoin(fmt, &e), "bob queues after her exit");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const LobbyEvent none = bob->poll();
        CHECK(none.kind != LobbyEvent::Kind::ReadyCheck, "bob stays unmatched — alice truly left");
    }

    lobby.join();
    std::remove(dbPath.c_str());

    std::printf("\n%s (%d failure%s)\n", g_fails == 0 ? "ALL PASS" : "FAILURES", g_fails,
                g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
