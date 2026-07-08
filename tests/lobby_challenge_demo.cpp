//
// lobby_challenge_demo.cpp — Phase 4.5 slice 5: the seek board + directed
// challenges, routed to a live authoritative match.
//
// Over real sockets: two logged-in players use ONE lobby each — one posts an open
// seek, the other accepts it; then one directs a challenge at the other, who
// accepts. Each accepted pairing hands both sides a token; they open a match conn
// (MirrorSession::joinToken) and play the live match to a finish, and a rated
// result moves Elo (zero-sum). Also covers the lobby's refusals: accept-your-own,
// unknown ids, decline, and a guest barred from rated. CI smoke test.
//
#include "core/AI.h"
#include "core/Build.h"
#include "core/Creatures.h"
#include "core/Ruleset.h"
#include "core/Spells.h"
#include "data/Net.h"
#include "net/AccountStore.h"
#include "net/GameServer.h" // contentHashOf
#include "net/Lobby.h"
#include "net/MirrorSession.h"
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

CharacterBuild makeBuild(const char* name) {
    CharacterBuild b;
    b.name = name;
    b.stats.hpPurchases = 2;
    b.spellIds = {spellid::Attack};
    return b;
}

// Drive both mirrors of one live match to a finish (single thread — the match
// alternates, so only one side ever awaits input). One Brain action per step, then
// pump both so the authoritative echo keeps them in lockstep.
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

// Join a paired live match and play it out; returns whether it finished.
bool playPairing(const std::string& host, uint16_t port, const PairedInfo& pa, const PairedInfo& pb,
                 const Ruleset& rules, const SpellCatalog& cat, const std::vector<Entity>& cre) {
    // Both sides must connect CONCURRENTLY: the server sends `welcome` only once
    // both match conns have arrived (the first is parked), so a sequential join
    // would deadlock waiting for a welcome that can't come yet.
    std::string ea, eb;
    std::unique_ptr<MirrorSession> a, b;
    std::thread ta([&] { a = MirrorSession::joinToken(host, port, pa.token, rules, cat, cre, &ea); });
    std::thread tbb([&] { b = MirrorSession::joinToken(host, port, pb.token, rules, cat, cre, &eb); });
    ta.join();
    tbb.join();
    if (!a || !b) {
        std::printf("         · join failed: %s / %s\n", ea.c_str(), eb.c_str());
        return false;
    }
    return playBoth(*a, *b);
}

} // namespace

int main() {
    const SpellCatalog catalog = makeDefaultCatalog();
    const std::vector<Entity> creatures = makeDefaultCreatures();
    const Ruleset ruleset = makeDefaultRuleset();

    const std::string dbPath = "tb_lobby_test.json";
    std::remove(dbPath.c_str());
    AccountStore accounts(dbPath);

    LobbyConfig cfg;
    cfg.catalog = catalog;
    cfg.creatures = creatures;
    cfg.casualRules = ruleset;
    cfg.rankedRules = ruleset; // slice-1 test: rated just means Elo is recorded
    cfg.contentHash = contentHashOf(catalog);
    cfg.accounts = &accounts;

    std::optional<Listener> listener = Listener::bind(0);
    const uint16_t port = listener->port();
    // Connection tally: 2 sessions + 1 guest + 2 games × 2 match conns = 7.
    constexpr int kConns = 7;
    std::thread lobby([&] { serveLobby(*listener, cfg, /*maxConns=*/kConns, /*readTimeoutSec=*/30); });

    MatchFormat rated;
    rated.time = MatchFormat::Time::PerMove;
    rated.perMoveSec = 20;
    rated.rated = true;

    { // clients close before we join the lobby thread
        std::string e;
        std::unique_ptr<LobbySession> alice =
            LobbySession::connect("127.0.0.1", port, cfg.contentHash, "alice", "pw-a", &e);
        std::unique_ptr<LobbySession> bob =
            LobbySession::connect("127.0.0.1", port, cfg.contentHash, "bob", "pw-b", &e);
        CHECK(alice && bob, "both players log in to the lobby");

        std::printf("Guests can't post a rated seek\n");
        {
            std::unique_ptr<LobbySession> guest =
                LobbySession::connect("127.0.0.1", port, cfg.contentHash, "", "", &e);
            std::string ge;
            CHECK(guest && !guest->seek(rated, makeBuild("G"), &ge),
                  "a guest's rated seek is refused");
            CHECK(ge.find("login") != std::string::npos, "…with a 'needs login' reason");
            // guest disconnects here (scope end) — its session thread exits.
        }

        std::printf("Open seek → accepted → a live rated match\n");
        CHECK(alice->seek(rated, makeBuild("Alice"), &e), "alice posts an open rated seek");
        std::optional<std::vector<SeekInfo>> seeks = bob->listSeeks();
        CHECK(seeks && seeks->size() == 1 && (*seeks)[0].user == "alice", "bob sees alice's seek");

        std::string ownErr;
        CHECK(!alice->acceptSeek(seeks ? (*seeks)[0].id : 0, makeBuild("Alice"), &ownErr).has_value(),
              "you can't accept your own seek");
        CHECK(!bob->acceptSeek(999999, makeBuild("Bob")).has_value(), "accepting a bad id fails");

        std::optional<PairedInfo> bobPair = bob->acceptSeek((*seeks)[0].id, makeBuild("Bob"), &e);
        CHECK(bobPair && bobPair->seat == Faction::Enemy, "bob accepts → paired as Enemy");
        std::optional<PairedInfo> alicePair = alice->poll();
        CHECK(alicePair && alicePair->seat == Faction::Player,
              "alice learns via poll → paired as Player");
        CHECK(alicePair && bobPair && alicePair->rated && bobPair->rated, "the pairing is rated");

        const bool g1 = alicePair && bobPair &&
                        playPairing("127.0.0.1", port, *alicePair, *bobPair, ruleset, catalog, creatures);
        CHECK(g1, "the seek match plays to a finish over the network");

        std::printf("Directed challenge → declined, then accepted → a live match\n");
        // A casual challenge alice declines-tests first.
        MatchFormat casual;
        casual.time = MatchFormat::Time::PerMove;
        casual.perMoveSec = 20;
        casual.rated = false;
        CHECK(bob->challenge("alice", casual, makeBuild("BobC"), &e), "bob challenges alice (casual)");
        std::optional<std::vector<ChallengeInfo>> inc = alice->listChallenges();
        CHECK(inc && inc->size() == 1 && (*inc)[0].from == "bob", "alice sees the incoming challenge");
        CHECK(alice->declineChallenge((*inc)[0].id), "alice declines it");
        std::optional<std::vector<ChallengeInfo>> gone = alice->listChallenges();
        CHECK(gone && gone->empty(), "the declined challenge is gone");

        // Now a rated challenge alice accepts and plays.
        CHECK(alice->challenge("bob", rated, makeBuild("AliceC"), &e), "alice challenges bob (rated)");
        inc = bob->listChallenges();
        CHECK(inc && inc->size() == 1 && (*inc)[0].from == "alice", "bob sees alice's challenge");
        std::optional<PairedInfo> bobPair2 = bob->acceptChallenge((*inc)[0].id, makeBuild("Bob2"), &e);
        CHECK(bobPair2 && bobPair2->seat == Faction::Enemy, "bob accepts the challenge → Enemy");
        std::optional<PairedInfo> alicePair2 = alice->poll();
        CHECK(alicePair2 && alicePair2->seat == Faction::Player, "alice is paired as Player (challenger)");

        const bool g2 = alicePair2 && bobPair2 &&
                        playPairing("127.0.0.1", port, *alicePair2, *bobPair2, ruleset, catalog, creatures);
        CHECK(g2, "the challenge match plays to a finish over the network");
    }

    lobby.join();

    std::printf("Rated results moved Elo (zero-sum)\n");
    const std::optional<AccountView> a = accounts.get("alice");
    const std::optional<AccountView> b = accounts.get("bob");
    CHECK(a && b, "both accounts exist");
    if (a && b) {
        CHECK(a->rating + b->rating == 2 * kDefaultRating, "ratings stay zero-sum");
        CHECK(a->wins + a->losses + b->wins + b->losses >= 2, "at least the two rated games recorded");
    }
    std::remove(dbPath.c_str());

    std::printf("\n%s (%d failure%s)\n", g_fails == 0 ? "ALL PASS" : "FAILURES", g_fails,
                g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
