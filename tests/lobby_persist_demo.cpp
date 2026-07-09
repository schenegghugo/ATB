//
// lobby_persist_demo.cpp — Persistence: correspondence games survive a lobby
// restart, and clients COLD-RESUME them.
//
// Round 1: alice (whose build carries Decoy) and bob pair into a rated Unlimited
// game over a persistDir-backed lobby; alice's first turn casts a decoy (secret
// choice 'a', persisted client-side as DecoySecrets), both play a turn, then
// everything is torn down — clients AND server. Round 2: a brand-new serveLobby on
// the same persistDir reloads the game registry + Mailbox journal; both players
// find the game via myCorrGames, rebuild fresh sessions, resume() them (replaying
// the whole log, restoring alice's secrets), play to a finish, exchange reveals,
// and the arbiter ranks the result. CI smoke test.
//
#include "core/AI.h"
#include "core/Build.h"
#include "core/Creatures.h"
#include "core/Ruleset.h"
#include "core/Spells.h"
#include "data/Net.h" // snapshotOf, serializeSnapshot
#include "net/AccountStore.h"
#include "net/Correspondence.h"
#include "net/GameServer.h" // contentHashOf
#include "net/Lobby.h"
#include "net/Socket.h"

#include "lobby_test_util.h"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace tb;
using namespace tb::net;
using tbtest::readyUp;

static int g_fails = 0;
#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) std::printf("  [PASS] %s\n", msg);                                               \
        else { std::printf("  [FAIL] %s\n", msg); ++g_fails; }                                     \
    } while (0)

namespace {

constexpr EntityId kPlayer = 0, kTwin = 2; // buildMatch: Player=0, Enemy=1, decoy twin=2
constexpr int kDecoySlot = 1;              // alice's build {Attack, Decoy}

std::string snapText(const Battle& b) { return serializeSnapshot(snapshotOf(b)); }

// First tile (deterministic scan order) the actor can legally cast `slot` at.
std::optional<Vec2i> castableTile(const Battle& b, EntityId actor, int slot) {
    const Grid& g = b.grid();
    for (int y = 0; y < g.height(); ++y)
        for (int x = 0; x < g.width(); ++x)
            if (b.canCast(actor, slot, {x, y})) return Vec2i{x, y};
    return std::nullopt;
}

// One full turn for whichever unit `s` awaits (see correspondence_demo): alice's
// first decoy is scripted with choice 'a'; her twin only ever passes (stays honest).
struct Driver {
    bool decoyPending = true;

    void driveTurn(CorrespondenceSession& s, bool isAlice) {
        if (!s.awaitingMe() || s.finished()) return;
        const EntityId me = s.battle().activeUnit();

        if (isAlice && me == kTwin) {
            s.submitLocal(net::Intent::endTurn());
            return;
        }
        if (isAlice && me == kPlayer && decoyPending) {
            if (const std::optional<Vec2i> t = castableTile(s.battle(), me, kDecoySlot)) {
                std::string err;
                if (!s.submitLocal(net::Intent::cast(kDecoySlot, *t), 'a', &err))
                    std::printf("         · decoy cast rejected: %s\n", err.c_str());
                decoyPending = false;
                if (s.awaitingMe() && !s.finished()) s.submitLocal(net::Intent::endTurn());
                return;
            }
            decoyPending = false;
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

std::unique_ptr<CorrespondenceSession>
buildCorr(LobbySession* session, const PairedInfo& pi, const Ruleset& rules,
          const SpellCatalog& cat, const std::vector<Entity>& cre, const std::string& user) {
    CorrespondenceSetup setup{rules, cat, cre, pi.seed, pi.player, pi.enemy};
    return std::make_unique<CorrespondenceSession>(std::make_unique<LobbyChannel>(session), pi.game,
                                                   setup, pi.seat, user);
}

} // namespace

int main() {
    const SpellCatalog catalog = makeDefaultCatalog();
    const std::vector<Entity> creatures = makeDefaultCreatures();
    const Ruleset ruleset = makeDefaultRuleset();

    const std::string dbPath = "tb_persist_accounts.json";
    const std::string stateDir = "tb_persist_state";
    std::remove(dbPath.c_str());
    std::filesystem::remove_all(stateDir);
    AccountStore accounts(dbPath);

    LobbyConfig cfg;
    cfg.catalog = catalog;
    cfg.creatures = creatures;
    cfg.casualRules = ruleset;
    cfg.rankedRules = ruleset;
    cfg.contentHash = contentHashOf(catalog);
    cfg.accounts = &accounts;
    cfg.persistDir = stateDir;

    CharacterBuild aliceBuild; // {Attack, Decoy} — the hidden-info case
    aliceBuild.name = "Feint";
    aliceBuild.spellIds = {spellid::Attack, spellid::Decoy};
    CharacterBuild bobBuild;
    bobBuild.name = "Basic";
    bobBuild.stats.hpPurchases = 2;
    bobBuild.spellIds = {spellid::Attack};

    MatchFormat corr;
    corr.time = MatchFormat::Time::Unlimited;
    corr.rated = true;

    std::string gameId, preRestartSnap;
    std::vector<DecoySecret> aliceSecrets;
    Driver drvA; // carries decoyPending across… no — round 2 uses a fresh one (decoy already cast)

    std::printf("Round 1 — play a decoy opening, then tear EVERYTHING down\n");
    {
        std::optional<Listener> listener = Listener::bind(0);
        const uint16_t port = listener->port();
        std::thread lobby([&] { serveLobby(*listener, cfg, /*maxConns=*/2, /*readTimeoutSec=*/30); });

        std::string e;
        std::unique_ptr<LobbySession> alice =
            LobbySession::connect("127.0.0.1", port, cfg.contentHash, "alice", "pw-a", &e);
        std::unique_ptr<LobbySession> bob =
            LobbySession::connect("127.0.0.1", port, cfg.contentHash, "bob", "pw-b", &e);
        CHECK(alice && bob, "both players log in");
        if (!alice || !bob) { lobby.join(); return 1; }

        CHECK(alice->challenge("bob", corr, &e), "alice sends a rated Unlimited challenge");
        std::optional<std::vector<ChallengeInfo>> inc = bob->listChallenges();
        std::optional<ReadyCheckInfo> bobRc =
            (inc && !inc->empty()) ? bob->acceptChallenge((*inc)[0].id, &e) : std::nullopt;
        LobbyEvent aliceEv = alice->poll();
        PairedInfo aPair, bPair;
        const bool paired = bobRc && aliceEv.kind == LobbyEvent::Kind::ReadyCheck &&
                            readyUp(*alice, aliceEv.readyCheck, aliceBuild, *bob, *bobRc, bobBuild,
                                    aPair, bPair);
        CHECK(paired && !aPair.live, "ready up → a correspondence game");
        if (!paired) { lobby.join(); return 1; }
        gameId = aPair.game;

        std::unique_ptr<CorrespondenceSession> aCs =
            buildCorr(alice.get(), aPair, ruleset, catalog, creatures, "alice");
        std::unique_ptr<CorrespondenceSession> bCs =
            buildCorr(bob.get(), bPair, ruleset, catalog, creatures, "bob");

        // Alice's opening turn (with the scripted decoy), then one bob turn.
        drvA.driveTurn(*aCs, true);
        CHECK(!drvA.decoyPending, "alice cast the scripted decoy on her opening turn");
        bCs->sync();
        Driver drvB;
        drvB.driveTurn(*bCs, false);
        aCs->sync();

        aliceSecrets = aCs->mySecrets();
        CHECK(aliceSecrets.size() == 1 && !aliceSecrets[0].nonce.empty(),
              "her commitment secret (choice+nonce) is exportable for client-side persistence");
        const std::string round =
            serializeDecoySecrets(aliceSecrets); // the client-side file round-trip
        CHECK(parseDecoySecrets(round).size() == 1 &&
                  parseDecoySecrets(round)[0].nonce == aliceSecrets[0].nonce,
              "secrets round-trip through their text format");
        preRestartSnap = snapText(aCs->battle());

        // Clients disconnect; the lobby thread drains (maxConns reached) — a full stop.
        alice.reset();
        bob.reset();
        lobby.join();
    }

    CHECK(std::filesystem::exists(stateDir + "/corrgames.json"), "the game registry is on disk");
    CHECK(std::filesystem::exists(stateDir + "/mailbox.jsonl"), "the move-log journal is on disk");

    std::printf("Round 2 — a NEW server on the same state dir; both cold-resume\n");
    {
        std::optional<Listener> listener = Listener::bind(0);
        const uint16_t port = listener->port();
        std::thread lobby([&] { serveLobby(*listener, cfg, /*maxConns=*/2, /*readTimeoutSec=*/30); });

        std::string e;
        std::unique_ptr<LobbySession> alice =
            LobbySession::connect("127.0.0.1", port, cfg.contentHash, "alice", "pw-a", &e);
        std::unique_ptr<LobbySession> bob =
            LobbySession::connect("127.0.0.1", port, cfg.contentHash, "bob", "pw-b", &e);
        CHECK(alice && bob, "both players log back in after the restart");
        if (!alice || !bob) { lobby.join(); return 1; }

        std::optional<std::vector<PairedInfo>> aGames = alice->myCorrGames();
        std::optional<std::vector<PairedInfo>> bGames = bob->myCorrGames();
        CHECK(aGames && aGames->size() == 1 && (*aGames)[0].game == gameId &&
                  (*aGames)[0].seat == Faction::Player && (*aGames)[0].rated,
              "alice finds her open game (seat + rated intact)");
        CHECK(bGames && bGames->size() == 1 && (*bGames)[0].opponent == "alice",
              "bob finds it too, listed against his opponent");
        if (!aGames || aGames->empty() || !bGames || bGames->empty()) { lobby.join(); return 1; }

        std::unique_ptr<CorrespondenceSession> aCs =
            buildCorr(alice.get(), (*aGames)[0], ruleset, catalog, creatures, "alice");
        std::unique_ptr<CorrespondenceSession> bCs =
            buildCorr(bob.get(), (*bGames)[0], ruleset, catalog, creatures, "bob");
        CHECK(aCs->resume(aliceSecrets, &e), "alice resumes (log replay + her secrets)");
        CHECK(bCs->resume({}, &e), "bob resumes (log replay)");

        CHECK(snapText(aCs->battle()) == preRestartSnap,
              "her resumed battle is bit-identical to the pre-restart state");
        CHECK(snapText(bCs->battle()) == preRestartSnap, "bob's resumed mirror matches it too");
        CHECK(aCs->record().commits.size() == 1 && !aCs->record().commits[0].nonce.empty(),
              "her decoy commitment carries its restored secret");

        // Play out the rest of the game over the restarted lobby.
        Driver a2; // the decoy is already cast — resume-aware
        a2.decoyPending = false;
        Driver b2;
        for (int g = 0; g < 100000 && !(aCs->finished() && bCs->finished()); ++g) {
            aCs->sync();
            bCs->sync();
            a2.driveTurn(*aCs, true);
            b2.driveTurn(*bCs, false);
        }
        CHECK(aCs->finished() && bCs->finished(), "the resumed game plays to a finish");

        bool aDone = false, bDone = false;
        for (int g = 0; g < 100 && !(aDone && bDone); ++g) {
            aDone = aCs->finalize();
            bDone = bCs->finalize();
        }
        CHECK(aDone && bDone, "reveals exchange (her restored secret opens the commitment)");
        CHECK(aCs->notation() == bCs->notation(), "both scoresheets are byte-identical");

        const SubmitResult r1 = alice->submitScore(gameId, Faction::Player, aCs->notation());
        const SubmitResult r2 = bob->submitScore(gameId, Faction::Enemy, bCs->notation());
        CHECK(r1.status == SubmitResult::Status::Pending && r2.status == SubmitResult::Status::Ranked,
              "the arbiter ranks the double-submitted result after the restart");

        alice.reset();
        bob.reset();
        lobby.join();
    }

    const std::optional<AccountView> a = accounts.get("alice");
    const std::optional<AccountView> b = accounts.get("bob");
    CHECK(a && b && a->rating + b->rating == 2 * kDefaultRating &&
              (a->rating != kDefaultRating || b->rating != kDefaultRating),
          "Elo moved (zero-sum) for the ranked result");

    std::remove(dbPath.c_str());
    std::filesystem::remove_all(stateDir);

    std::printf("\n%s (%d failure%s)\n", g_fails == 0 ? "ALL PASS" : "FAILURES", g_fails,
                g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
