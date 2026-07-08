//
// ready_check_demo.cpp — Phase 4.5 slice 6.2: the per-pairing ready check.
//
// After a pairing, BOTH players must submit a build + READY within the window, or
// the game is cancelled — no rating change. Covers: both ready → paired; an illegal
// build is rejected (re-pick, server-authoritative); a decline cancels for both; and
// a timeout cancels for both. Uses a short window so the timeout case is fast. CI.
//
#include "core/Creatures.h"
#include "core/Ruleset.h"
#include "core/Spells.h"
#include "net/AccountStore.h"
#include "net/GameServer.h" // contentHashOf
#include "net/Lobby.h"
#include "net/Socket.h"

#include "lobby_test_util.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <thread>

using namespace tb;
using namespace tb::net;
using tbtest::makeBuild;

static int g_fails = 0;
#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) std::printf("  [PASS] %s\n", msg);                                               \
        else { std::printf("  [FAIL] %s\n", msg); ++g_fails; }                                     \
    } while (0)

namespace {
// alice challenges bob (casual per-move); bob accepts → returns both ready checks.
bool challenge(LobbySession& alice, LobbySession& bob, ReadyCheckInfo& aliceRc,
               ReadyCheckInfo& bobRc) {
    MatchFormat fmt;
    fmt.time = MatchFormat::Time::PerMove;
    fmt.perMoveSec = 20;
    if (!alice.challenge("bob", fmt)) return false;
    std::optional<std::vector<ChallengeInfo>> inc = bob.listChallenges();
    if (!inc || inc->empty()) return false;
    std::optional<ReadyCheckInfo> brc = bob.acceptChallenge((*inc)[0].id);
    LobbyEvent ev = alice.poll();
    if (!brc || ev.kind != LobbyEvent::Kind::ReadyCheck) return false;
    bobRc = *brc;
    aliceRc = ev.readyCheck;
    return true;
}
} // namespace

int main() {
    const SpellCatalog catalog = makeDefaultCatalog();
    const std::vector<Entity> creatures = makeDefaultCreatures();
    const Ruleset ruleset = makeDefaultRuleset();

    const std::string dbPath = "tb_ready_check_test.json";
    std::remove(dbPath.c_str());
    AccountStore accounts(dbPath);

    LobbyConfig cfg;
    cfg.catalog = catalog;
    cfg.creatures = creatures;
    cfg.casualRules = ruleset;
    cfg.rankedRules = ruleset;
    cfg.contentHash = contentHashOf(catalog);
    cfg.accounts = &accounts;
    cfg.readyCheckSec = 1; // short window so the timeout case is fast

    std::optional<Listener> listener = Listener::bind(0);
    const uint16_t port = listener->port();
    std::thread lobby([&] { serveLobby(*listener, cfg, /*maxConns=*/2, /*readTimeoutSec=*/30); });

    {
        std::string e;
        std::unique_ptr<LobbySession> alice =
            LobbySession::connect("127.0.0.1", port, cfg.contentHash, "alice", "pw-a", &e);
        std::unique_ptr<LobbySession> bob =
            LobbySession::connect("127.0.0.1", port, cfg.contentHash, "bob", "pw-b", &e);
        CHECK(alice && bob, "both players log in");

        std::printf("An illegal build is rejected; then both ready → paired\n");
        ReadyCheckInfo aRc, bRc;
        CHECK(challenge(*alice, *bob, aRc, bRc), "challenge → ready check for both");

        CharacterBuild illegal;
        illegal.name = "TooMuch";
        illegal.stats.hpPurchases = 999; // busts the budget
        illegal.spellIds = {spellid::Attack};
        const ReadyResult bad = bob->ready(bRc.id, illegal);
        CHECK(bad.status == ReadyResult::Status::Rejected, "an over-budget build is rejected");

        const ReadyResult w = bob->ready(bRc.id, makeBuild("Bob")); // re-pick a legal one
        CHECK(w.status == ReadyResult::Status::Waiting, "a legal build → waiting for the opponent");
        const ReadyResult m = alice->ready(aRc.id, makeBuild("Alice"));
        CHECK(m.status == ReadyResult::Status::Matched, "the second READY → matched");
        LobbyEvent bobEv = bob->poll();
        CHECK(bobEv.kind == LobbyEvent::Kind::Paired, "the first-ready player learns Matched via poll");

        std::printf("Declining cancels the game for both\n");
        CHECK(challenge(*alice, *bob, aRc, bRc), "a second challenge → ready check");
        CHECK(bob->cancelReady(bRc.id), "bob declines");
        LobbyEvent aEv = alice->poll();
        CHECK(aEv.kind == LobbyEvent::Kind::Cancelled, "alice is told the ready check was cancelled");
        const ReadyResult after = alice->ready(aRc.id, makeBuild("Alice"));
        CHECK(after.status == ReadyResult::Status::Cancelled, "readying a cancelled check is refused");

        std::printf("A ready check times out if not both ready\n");
        CHECK(challenge(*alice, *bob, aRc, bRc), "a third challenge → ready check");
        CHECK(alice->ready(aRc.id, makeBuild("Alice")).status == ReadyResult::Status::Waiting,
              "only alice readies");
        std::this_thread::sleep_for(std::chrono::milliseconds(1300)); // past the 1s window
        LobbyEvent timeoutEv = bob->poll(); // poll reaps expired checks
        CHECK(timeoutEv.kind == LobbyEvent::Kind::Cancelled, "the stale ready check is cancelled");
    }

    lobby.join();

    std::printf("No game was ranked (no match completed)\n");
    const std::optional<AccountView> a = accounts.get("alice");
    CHECK(!a || (a->wins == 0 && a->losses == 0), "no wins/losses recorded from cancelled checks");
    std::remove(dbPath.c_str());

    std::printf("\n%s (%d failure%s)\n", g_fails == 0 ? "ALL PASS" : "FAILURES", g_fails,
                g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
