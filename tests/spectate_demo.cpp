//
// spectate_demo.cpp — Phase 5.2: watch a live lobby match as a read-only mirror.
//
// Alice and Bob play a live match through the lobby while Carol (a guest) lists the
// games in progress, subscribes to the match's logged broadcast stream (`watch`),
// and feeds it to a SpectatorMirror. Because the core is deterministic, Carol's
// mirror ends bit-identical to the players' mirrors. Also covers: the game is
// delisted when it ends (the log stays drainable), and watching a bogus id fails.
// CI smoke test.
//
#include "core/AI.h"
#include "core/Build.h"
#include "core/Creatures.h"
#include "core/Ruleset.h"
#include "core/Spells.h"
#include "data/Net.h" // snapshotOf, serializeSnapshot
#include "net/GameServer.h" // contentHashOf
#include "net/Lobby.h"
#include "net/MirrorSession.h"
#include "net/Socket.h"
#include "net/Spectate.h"

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

std::string snapText(const Battle& b) { return serializeSnapshot(snapshotOf(b)); }

// Drive both players' mirrors to a finish (one Brain action per step — see
// lobby_challenge_demo). The spectator never participates here; she only polls.
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
    // No account store: casual/guest-only server — spectate needs no accounts.

    std::optional<Listener> listener = Listener::bind(0);
    const uint16_t port = listener->port();
    constexpr int kConns = 5; // 3 sessions (alice, bob, carol) + 2 match conns
    std::thread lobby([&] { serveLobby(*listener, cfg, /*maxConns=*/kConns, /*readTimeoutSec=*/30); });

    MatchFormat fmt;
    fmt.time = MatchFormat::Time::PerMove;
    fmt.perMoveSec = 20;

    {
        std::string e;
        std::unique_ptr<LobbySession> alice =
            LobbySession::connect("127.0.0.1", port, cfg.contentHash, "", "", &e);
        std::unique_ptr<LobbySession> bob =
            LobbySession::connect("127.0.0.1", port, cfg.contentHash, "", "", &e);
        std::unique_ptr<LobbySession> carol =
            LobbySession::connect("127.0.0.1", port, cfg.contentHash, "", "", &e);
        CHECK(alice && bob && carol, "two players + a spectator connect");
        if (!alice || !bob || !carol) { lobby.join(); return 1; }

        std::optional<std::vector<LiveGameInfo>> before = carol->listGames();
        CHECK(before && before->empty(), "no live games before the match starts");
        CHECK(!carol->watchPoll("bogus-id", 0).has_value(), "watching a bogus game id fails");

        std::printf("Alice and Bob pair into a live match\n");
        CHECK(alice->seek(fmt, &e), "alice posts an open seek");
        std::optional<std::vector<SeekInfo>> seeks = bob->listSeeks();
        std::optional<ReadyCheckInfo> bobRc =
            (seeks && !seeks->empty()) ? bob->acceptSeek((*seeks)[0].id, &e) : std::nullopt;
        LobbyEvent aliceEv = alice->poll();
        PairedInfo alicePair, bobPair;
        const bool paired = bobRc && aliceEv.kind == LobbyEvent::Kind::ReadyCheck &&
                            readyUp(*alice, aliceEv.readyCheck, makeBuild("Alice"), *bob, *bobRc,
                                    makeBuild("Bob"), alicePair, bobPair);
        CHECK(paired && alicePair.live, "ready up → paired into a live match");
        if (!paired) { lobby.join(); return 1; }

        // Both sides join concurrently (the first match conn is parked until the
        // second arrives); joinToken returns once the welcome lands, and the welcome
        // is only sent AFTER the lobby registers + logs the game — so from here the
        // game is listed and its log holds the setup.
        std::unique_ptr<MirrorSession> aMs, bMs;
        std::thread ta([&] { aMs = MirrorSession::joinToken("127.0.0.1", port, alicePair.token, ruleset, catalog, creatures, &e); });
        std::thread tbb([&] { bMs = MirrorSession::joinToken("127.0.0.1", port, bobPair.token, ruleset, catalog, creatures, &e); });
        ta.join();
        tbb.join();
        CHECK(aMs && bMs, "both players join the match");
        if (!aMs || !bMs) { lobby.join(); return 1; }

        std::printf("Carol finds the game and subscribes\n");
        std::optional<std::vector<LiveGameInfo>> games = carol->listGames();
        CHECK(games && games->size() == 1, "carol sees exactly one live game");
        const std::string gid = games && !games->empty() ? (*games)[0].id : "";
        CHECK(games && !games->empty() && !(*games)[0].userP.empty() && !(*games)[0].userE.empty(),
              "the listing names both players");

        SpectatorMirror mirror(ruleset, catalog, creatures);
        std::size_t cursor = 0;
        if (std::optional<ChannelPoll> cp = carol->watchPoll(gid, cursor)) {
            for (const MailEntry& en : cp->entries) mirror.feed(en.msg);
            cursor = cp->next;
        }
        CHECK(mirror.ready(), "the logged welcome builds carol's mirror");
        CHECK(mirror.ready() && snapText(mirror.battle()) == snapText(aMs->battle()),
              "her initial battle is bit-identical to alice's mirror");

        std::printf("The match plays out; carol stays in lockstep\n");
        CHECK(playBoth(*aMs, *bMs), "the match plays to a finish");

        // Drain the log until the end message lands (bounded wall-clock).
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (!mirror.finished() && std::chrono::steady_clock::now() < deadline) {
            if (std::optional<ChannelPoll> cp = carol->watchPoll(gid, cursor)) {
                for (const MailEntry& en : cp->entries) mirror.feed(en.msg);
                cursor = cp->next;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        CHECK(mirror.finished(), "carol's mirror sees the match end");
        CHECK(mirror.ready() && snapText(mirror.battle()) == snapText(aMs->battle()),
              "her final battle is bit-identical to alice's mirror");
        CHECK(mirror.battle().winner() == aMs->battle().winner(),
              "she reads the same winner the players saw");

        std::optional<std::vector<LiveGameInfo>> after = carol->listGames();
        CHECK(after && after->empty(), "the finished game is delisted");
        std::optional<ChannelPoll> late = carol->watchPoll(gid, 0);
        CHECK(late && !late->entries.empty(), "…but its log is still drainable (late watcher)");
    }

    lobby.join();

    std::printf("\n%s (%d failure%s)\n", g_fails == 0 ? "ALL PASS" : "FAILURES", g_fails,
                g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
