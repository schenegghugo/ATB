//
// lobby_correspondence_demo.cpp — Phase 4.5 slice 5 + CR.6: an Unlimited challenge
// routed to a correspondence game, played over the lobby and ranked.
//
// Over real sockets: two logged-in players accept an Unlimited (rated) challenge;
// the lobby mints a correspondence game (seed + both builds) and relays their
// move-strings through its server-side Mailbox. Each side plays a CorrespondenceSession
// over a LobbyChannel on its own session. Mid-game, ONE player drops the transport
// and reconnects — the server keeps the game + log, so a rebind resumes play. Both
// finish, submit their scoresheets, and the embedded arbiter ranks the game (Elo).
// CI smoke test.
//
#include "core/AI.h"
#include "core/Build.h"
#include "core/Creatures.h"
#include "core/Ruleset.h"
#include "core/Spells.h"
#include "data/Net.h"
#include "net/AccountStore.h"
#include "net/Correspondence.h"
#include "net/GameServer.h" // contentHashOf
#include "net/Lobby.h"
#include "net/Socket.h"

#include "lobby_test_util.h"

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

using tbtest::makeBuild;
using tbtest::readyUp;

// Build a CorrespondenceSession from a lobby correspondence pairing, over a channel
// on `session`.
std::unique_ptr<CorrespondenceSession>
buildCorr(LobbySession* session, const PairedInfo& pi, const Ruleset& rules,
          const SpellCatalog& cat, const std::vector<Entity>& cre, const std::string& user) {
    CorrespondenceSetup setup{rules, cat, cre, pi.seed, pi.player, pi.enemy};
    return std::make_unique<CorrespondenceSession>(std::make_unique<LobbyChannel>(session), pi.game,
                                                   setup, pi.seat, user);
}

// Play one full turn for whichever side `s` awaits (no decoy in these builds).
void driveCorr(CorrespondenceSession& s) {
    if (!s.awaitingMe() || s.finished()) return;
    const EntityId me = s.battle().activeUnit();
    for (const PlannedAction& a : defaultBrain().planTurn(s.battle(), me)) {
        if (!s.awaitingMe() || s.finished()) break;
        s.submitLocal(a.kind == PlannedAction::Kind::Cast ? net::Intent::cast(a.slot, a.target)
                                                          : net::Intent::move(a.target));
    }
    if (s.awaitingMe() && !s.finished()) s.submitLocal(net::Intent::endTurn());
}

} // namespace

int main() {
    const SpellCatalog catalog = makeDefaultCatalog();
    const std::vector<Entity> creatures = makeDefaultCreatures();
    const Ruleset ruleset = makeDefaultRuleset();

    const std::string dbPath = "tb_lobby_corr_test.json";
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
    // Tally: alice + bob + bob's reconnect = 3 (correspondence uses the session conns,
    // so there are no separate match conns).
    constexpr int kConns = 3;
    std::thread lobby([&] { serveLobby(*listener, cfg, /*maxConns=*/kConns, /*readTimeoutSec=*/30); });

    MatchFormat corr;
    corr.time = MatchFormat::Time::Unlimited;
    corr.rated = true;

    std::string aliceNote, bobNote, game;
    Faction aliceSeat = Faction::Player, bobSeat = Faction::Enemy;
    {
        std::string e;
        std::unique_ptr<LobbySession> alice =
            LobbySession::connect("127.0.0.1", port, cfg.contentHash, "alice", "pw-a", &e);
        std::unique_ptr<LobbySession> bob =
            LobbySession::connect("127.0.0.1", port, cfg.contentHash, "bob", "pw-b", &e);
        CHECK(alice && bob, "both players log in");

        std::printf("An Unlimited challenge → ready check → a correspondence game\n");
        CHECK(alice->challenge("bob", corr, &e), "alice sends an Unlimited challenge (no build yet)");
        std::optional<std::vector<ChallengeInfo>> inc = bob->listChallenges();
        CHECK(inc && inc->size() == 1, "bob sees the challenge");
        std::optional<ReadyCheckInfo> bobRc = bob->acceptChallenge((*inc)[0].id, &e);
        CHECK(bobRc.has_value(), "bob accepts → a ready check");
        LobbyEvent aliceEv = alice->poll();
        CHECK(aliceEv.kind == LobbyEvent::Kind::ReadyCheck, "alice gets the ready check via poll");

        PairedInfo alicePair, bobPair;
        const bool ok = bobRc && aliceEv.kind == LobbyEvent::Kind::ReadyCheck &&
                        readyUp(*alice, aliceEv.readyCheck, makeBuild("Alice"), *bob, *bobRc,
                                makeBuild("Bob"), alicePair, bobPair);
        CHECK(ok, "both ready up → paired");
        CHECK(ok && !alicePair.live && !bobPair.live,
              "a CORRESPONDENCE pairing (setup, not a live token)");
        CHECK(ok && alicePair.game == bobPair.game && !alicePair.game.empty(),
              "both sides share the same game id");
        CHECK(ok && alicePair.seed == bobPair.seed && alicePair.seed != 0,
              "both sides share the same non-zero seed");
        if (!ok) { std::printf("no pairing\n"); lobby.join(); return 1; }
        game = alicePair.game;
        aliceSeat = alicePair.seat;
        bobSeat = bobPair.seat;

        std::unique_ptr<CorrespondenceSession> ca =
            buildCorr(alice.get(), alicePair, ruleset, catalog, creatures, "alice");
        std::unique_ptr<CorrespondenceSession> cb =
            buildCorr(bob.get(), bobPair, ruleset, catalog, creatures, "bob");

        std::printf("Play a few turns, then bob drops and reconnects\n");
        for (int i = 0; i < 3; ++i) {
            if (ca->awaitingMe()) driveCorr(*ca);
            if (cb->awaitingMe()) driveCorr(*cb);
            ca->sync();
            cb->sync();
        }
        CHECK(!(ca->finished() && cb->finished()), "the game is still in progress (good — we can resume)");

        // Bob's transport drops and reconnects; the lobby keeps the game + move log.
        std::unique_ptr<LobbySession> bob2 =
            LobbySession::connect("127.0.0.1", port, cfg.contentHash, "bob", "pw-b", &e);
        CHECK(bob2, "bob reconnects with a fresh session");
        cb->rebind(std::make_unique<LobbyChannel>(bob2.get()));
        bob = std::move(bob2); // old session closes → its server thread exits

        std::printf("Play resumes to a finish over the new connection\n");
        for (int guard = 0; guard < 20000 && !(ca->finished() && cb->finished()); ++guard) {
            if (ca->awaitingMe()) driveCorr(*ca);
            if (cb->awaitingMe()) driveCorr(*cb);
            ca->sync();
            cb->sync();
        }
        CHECK(ca->finished() && cb->finished(), "the correspondence game finishes after the reconnect");

        bool fa = false, fb = false;
        for (int k = 0; k < 20 && !(fa && fb); ++k) { fa = ca->finalize(); fb = cb->finalize(); }
        CHECK(fa && fb, "both finalize");
        aliceNote = ca->notation();
        bobNote = cb->notation();
        CHECK(aliceNote == bobNote, "both peers derive a byte-identical scoresheet");

        std::printf("Double-submit ranks the correspondence game\n");
        const SubmitResult r1 = alice->submitScore(game, aliceSeat, aliceNote);
        CHECK(r1.status == SubmitResult::Status::Pending, "alice's submission is pending");
        const SubmitResult r2 = bob->submitScore(game, bobSeat, bobNote);
        if (r2.status == SubmitResult::Status::Rejected)
            std::printf("         · %s\n", r2.error.c_str());
        CHECK(r2.status == SubmitResult::Status::Ranked, "bob's agreeing submission ranks the game");
    }

    lobby.join();

    std::printf("The result moved Elo (zero-sum)\n");
    const std::optional<AccountView> a = accounts.get("alice");
    const std::optional<AccountView> b = accounts.get("bob");
    CHECK(a && b, "both accounts exist");
    if (a && b) {
        CHECK(a->rating + b->rating == 2 * kDefaultRating, "ratings stay zero-sum");
        CHECK(a->rating != kDefaultRating || b->rating != kDefaultRating,
              "a decisive correspondence game changed ratings");
    }
    std::remove(dbPath.c_str());

    std::printf("\n%s (%d failure%s)\n", g_fails == 0 ? "ALL PASS" : "FAILURES", g_fails,
                g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
