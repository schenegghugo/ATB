//
// correspondence_demo.cpp — CR.6 slice 3: peer-to-peer correspondence play with
// in-game decoy commitments, end-to-end over a real relay socket.
//
// Two CorrespondenceSessions (one per seat) share a match setup and a relay game
// id. They exchange intents through the mailbox relay (both connect OUT — NAT
// immune); the Player casts a decoy, so the session mints a commitment and ships
// its HASH with the move, then reveals choice+nonce after the game. The test
// proves the flow closes the loop: both peers derive a BYTE-IDENTICAL scoresheet,
// replay::verify() reproduces the winner, and the arbiter ranks the game from the
// two agreeing submissions. CI smoke test.
//
#include "core/AI.h"
#include "core/Battle.h"
#include "core/Build.h"
#include "core/Creatures.h"
#include "core/Ruleset.h"
#include "core/Spells.h"
#include "data/Net.h"
#include "net/AccountStore.h"
#include "net/Arbiter.h"
#include "net/Correspondence.h"
#include "net/MailboxRelay.h"
#include "net/MoveChannel.h"
#include "net/Replay.h"
#include "net/Socket.h"

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

constexpr EntityId kPlayer = 0, kTwin = 2; // buildMatch: Player=0, Enemy=1, decoy twin=2
constexpr int kDecoySlot = 1;              // Player build {Attack, Decoy}

// First tile (deterministic scan order) the actor can legally cast `slot` at.
std::optional<Vec2i> castableTile(const Battle& b, EntityId actor, int slot) {
    const Grid& g = b.grid();
    for (int y = 0; y < g.height(); ++y)
        for (int x = 0; x < g.width(); ++x)
            if (b.canCast(actor, slot, {x, y})) return Vec2i{x, y};
    return std::nullopt;
}

// Plays exactly one full turn for whichever unit `s` currently awaits. To keep the
// Player's "a" commitment honest we never let the twin act (it passes) and never
// re-cast Decoy; the scripted first decoy is cast from the original with choice a.
struct Driver {
    bool decoyPending = true;

    void driveTurn(CorrespondenceSession& s, bool isPlayer) {
        if (!s.awaitingMe() || s.finished()) return;
        const EntityId me = s.battle().activeUnit();

        if (isPlayer && me == kTwin) { // never reveal via the twin — stays "a"
            s.submitLocal(net::Intent::endTurn());
            return;
        }
        if (isPlayer && me == kPlayer && decoyPending) {
            const std::optional<Vec2i> t = castableTile(s.battle(), me, kDecoySlot);
            if (t) {
                std::string err;
                if (!s.submitLocal(net::Intent::cast(kDecoySlot, *t), 'a', &err))
                    std::printf("         · decoy cast rejected: %s\n", err.c_str());
                decoyPending = false;
                if (s.awaitingMe() && !s.finished()) s.submitLocal(net::Intent::endTurn());
                return;
            }
            decoyPending = false; // not castable this turn — fall through to normal play
        }

        for (const PlannedAction& a : defaultBrain().planTurn(s.battle(), me)) {
            if (s.finished() || !s.awaitingMe()) break;
            if (a.kind == PlannedAction::Kind::Cast) {
                const auto& spells = s.battle().unit(me).spells;
                if (a.slot >= 0 && a.slot < static_cast<int>(spells.size()) &&
                    spells[a.slot].name == "Decoy")
                    continue; // one scripted decoy only
                s.submitLocal(net::Intent::cast(a.slot, a.target));
            } else {
                s.submitLocal(net::Intent::move(a.target));
            }
        }
        if (s.awaitingMe() && !s.finished()) s.submitLocal(net::Intent::endTurn());
    }
};

} // namespace

int main() {
    const SpellCatalog catalog = makeDefaultCatalog();
    const std::vector<Entity> creatures = makeDefaultCreatures();
    const Ruleset ruleset = makeDefaultRuleset();

    CharacterBuild pb; // {Attack, Decoy}
    pb.name = "Feint";
    pb.spellIds = {spellid::Attack, spellid::Decoy};
    CharacterBuild eb;
    eb.name = "Basic";
    eb.stats.hpPurchases = 2;
    eb.spellIds = {spellid::Attack};

    CorrespondenceSetup setup{ruleset, catalog, creatures, /*seed=*/9001u, pb, eb};

    Mailbox box;
    std::optional<Listener> listener = Listener::bind(0);
    const uint16_t port = listener->port();
    std::thread relay([&] { serveRelay(*listener, box, /*maxConns=*/2); });

    std::string playerNotation, enemyNotation;
    std::optional<Faction> replayedWinner;
    { // sessions live in this scope so their sockets close before we stop the relay
        std::optional<RelayClient> ra = RelayClient::connect("127.0.0.1", port);
        std::optional<RelayClient> rb = RelayClient::connect("127.0.0.1", port);
        CHECK(ra && rb, "both peers connect to the relay");

        const std::string game = "corr-1";
        CorrespondenceSession player(std::make_unique<RelayChannel>(std::move(*ra)), game, setup,
                                     Faction::Player, "alice");
        CorrespondenceSession enemy(std::make_unique<RelayChannel>(std::move(*rb)), game, setup,
                                    Faction::Enemy, "bob");

        std::printf("Two peers play a decoy game entirely through the relay\n");
        Driver drv;
        int guard = 0;
        while (!(player.finished() && enemy.finished()) && guard++ < 20000) {
            if (player.awaitingMe()) drv.driveTurn(player, /*isPlayer=*/true);
            if (enemy.awaitingMe()) drv.driveTurn(enemy, /*isPlayer=*/false);
            player.sync();
            enemy.sync();
        }
        CHECK(player.finished() && enemy.finished(), "the game reaches a conclusion on both peers");
        CHECK(!drv.decoyPending, "the Player cast the scripted decoy");

        std::printf("The commitment reveal reconciles both scoresheets\n");
        bool pf = false, ef = false;
        for (int k = 0; k < 100 && !(pf && ef); ++k) {
            pf = player.finalize();
            ef = enemy.finalize();
        }
        CHECK(pf && ef, "both peers finalize (every commitment resolved)");

        playerNotation = player.notation();
        enemyNotation = enemy.notation();
        CHECK(playerNotation == enemyNotation, "both peers derive a byte-identical scoresheet");

        const replay::GameRecord& rec = player.record();
        CHECK(rec.commits.size() == 1, "exactly one decoy commitment was recorded");
        CHECK(rec.commits.size() == 1 && rec.commits[0].choice == "a",
              "the Player's committed choice is \"a\"");
        CHECK(rec.commits.size() == 1 &&
                  replay::makeCommitment("a", rec.commits[0].nonce) == rec.commits[0].commit,
              "the shipped hash matches choice+nonce (the wire carried a real commitment)");

        std::printf("The scoresheet verifies and ranks\n");
        const replay::VerifyResult v = replay::verify(rec, ruleset, catalog, creatures);
        if (!v.ok) std::printf("         · %s\n", v.error.c_str());
        CHECK(v.ok, "replay::verify() reproduces the match");
        replayedWinner = v.winner;
    }

    relay.join();

    // The two agreeing submissions rank through the arbiter (as the mailbox would
    // relay them to a self-hosted arbiter after the game).
    const std::string dbPath = "tb_correspondence_test.json";
    std::remove(dbPath.c_str());
    AccountStore accounts(dbPath);
    Arbiter arb(accounts, ruleset, catalog, creatures);
    const Arbiter::Result r1 = arb.submit({"alice", "bob", Faction::Player, playerNotation});
    CHECK(r1.status == Arbiter::Status::Pending, "the first submission is pending");
    const Arbiter::Result r2 = arb.submit({"bob", "alice", Faction::Enemy, enemyNotation});
    CHECK(r2.status == Arbiter::Status::Ranked, "the second (agreeing) submission ranks the game");
    if (replayedWinner)
        CHECK(!r2.winner.empty(), "a decisive game records a winner");
    std::remove(dbPath.c_str());

    std::printf("\n%s (%d failure%s)\n", g_fails == 0 ? "ALL PASS" : "FAILURES", g_fails,
                g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
