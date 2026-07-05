//
// arbiter_demo.cpp — Phase 4.5 CR.4: submit-to-arbiter + ranked MMR.
//
// Two players submit their scoresheet; the arbiter double-checks they agree,
// re-simulates (verify), and records Elo. Covers the happy path plus the abuse
// cases (disagreeing sheets, wrong seat, illegal record, single submission,
// double recording). CI smoke test.
//
#include "core/AI.h"
#include "core/Build.h"
#include "core/Creatures.h"
#include "core/Match.h"
#include "core/Ruleset.h"
#include "core/Spells.h"
#include "data/Net.h"
#include "net/AccountStore.h"
#include "net/Arbiter.h"
#include "net/MatchRunner.h"
#include "net/Replay.h"

#include <algorithm>
#include <cstdio>
#include <string>

using namespace tb;
using namespace tb::net;

static int g_fails = 0;
#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) std::printf("  [PASS] %s\n", msg);                                               \
        else { std::printf("  [FAIL] %s\n", msg); ++g_fails; }                                     \
    } while (0)

namespace {

CharacterBuild pyro() {
    CharacterBuild b;
    b.name = "Pyromancer";
    b.stats.bonusAp = 1;
    b.spellIds = {spellid::Attack, spellid::Fireball, spellid::Poison};
    return b;
}
CharacterBuild bruiser() {
    CharacterBuild b;
    b.name = "Bruiser";
    b.stats.hpPurchases = 4;
    b.stats.bonusMp = 1;
    b.spellIds = {spellid::Attack, spellid::Knockback, spellid::Harpoon};
    return b;
}

// Record a real pyro(Player) vs bruiser(Enemy) match into a notation string.
std::string makeNotation(const Ruleset& r, const SpellCatalog& cat, const std::vector<Entity>& cre,
                         unsigned seed) {
    replay::GameRecord rec;
    rec.catalogHash = replay::catalogHash(cat);
    rec.seed = seed;
    rec.player = pyro();
    rec.enemy = bruiser();
    MatchRunner runner(buildMatch(r, {rec.player}, {rec.enemy}, cat, seed, cre), Seat::Human,
                       Seat::Human);
    for (int g = 0; g < 4000 && !runner.finished(); ++g) {
        const std::optional<Faction> seat = runner.awaitingSeat();
        if (!seat) break;
        const EntityId me = runner.battle().activeUnit();
        for (const PlannedAction& a : defaultBrain().planTurn(runner.battle(), me)) {
            if (runner.finished()) break;
            const net::Intent in = a.kind == PlannedAction::Kind::Cast
                                       ? net::Intent::cast(a.slot, a.target)
                                       : net::Intent::move(a.target);
            rec.intents.push_back(in);
            runner.submit(*seat, in);
        }
        if (!runner.finished()) {
            rec.intents.push_back(net::Intent::endTurn());
            runner.submit(*seat, net::Intent::endTurn());
        }
    }
    return replay::serializeRecord(rec);
}

} // namespace

int main() {
    const SpellCatalog catalog = makeDefaultCatalog();
    const std::vector<Entity> creatures = makeDefaultCreatures();
    const Ruleset ruleset = makeDefaultRuleset();

    const std::string dbPath = "tb_arbiter_test.json";
    std::remove(dbPath.c_str());
    AccountStore accounts(dbPath);
    Arbiter arb(accounts, ruleset, catalog, creatures);

    auto reg = [&](const std::string& u) { accounts.authenticate(u, "pw"); };

    std::printf("Double-submit of an agreeing scoresheet ranks the game\n");
    {
        reg("alice");
        reg("bob");
        const std::string game = makeNotation(ruleset, catalog, creatures, 1337);

        Arbiter::Result r1 = arb.submit({"alice", "bob", Faction::Player, game});
        CHECK(r1.status == Arbiter::Status::Pending, "first submission is pending");
        CHECK(accounts.ratingOf("alice") == kDefaultRating, "no rating change yet");

        Arbiter::Result r2 = arb.submit({"bob", "alice", Faction::Enemy, game});
        CHECK(r2.status == Arbiter::Status::Ranked, "second (agreeing) submission ranks the game");
        CHECK(r2.winner == "alice" || r2.winner == "bob" || r2.winner.empty(), "winner is one of the two (or draw)");

        const int ra = accounts.ratingOf("alice"), rb = accounts.ratingOf("bob");
        CHECK(ra + rb == 2 * kDefaultRating, "ratings zero-sum");
        if (!r2.winner.empty()) {
            CHECK(accounts.ratingOf(r2.winner) == kDefaultRating + 16, "winner +16");
            CHECK(std::min(ra, rb) == kDefaultRating - 16, "loser -16");
        }

        Arbiter::Result dup = arb.submit({"alice", "bob", Faction::Player, game});
        CHECK(dup.status == Arbiter::Status::Rejected && dup.error.find("already") != std::string::npos,
              "re-submitting a decided game is rejected");
    }

    std::printf("Disagreeing scoresheets are rejected (no rating change)\n");
    {
        reg("carol");
        reg("dave");
        const std::string good = makeNotation(ruleset, catalog, creatures, 2001);
        // Drop the last whole intent token: still parseable, but a different sheet.
        const std::string tampered = good.substr(0, good.rfind(' '));

        arb.submit({"carol", "dave", Faction::Player, good});
        Arbiter::Result r = arb.submit({"dave", "carol", Faction::Enemy, tampered});
        CHECK(r.status == Arbiter::Status::Rejected && r.error.find("disagree") != std::string::npos,
              "mismatched scoresheets are rejected");
        CHECK(accounts.ratingOf("carol") == kDefaultRating && accounts.ratingOf("dave") == kDefaultRating,
              "no rating change on a disputed game");
    }

    std::printf("Inconsistent seat claims + illegal records are rejected\n");
    {
        reg("erin");
        reg("finn");
        const std::string game = makeNotation(ruleset, catalog, creatures, 3001);
        arb.submit({"erin", "finn", Faction::Player, game});
        Arbiter::Result r = arb.submit({"finn", "erin", Faction::Player, game}); // both "Player"
        CHECK(r.status == Arbiter::Status::Rejected && r.error.find("seat") != std::string::npos,
              "both claiming the same seat is rejected");

        // An agreed-upon but illegal record (over-budget build) fails verification.
        replay::GameRecord bad;
        bad.catalogHash = replay::catalogHash(catalog);
        bad.seed = 4001;
        bad.player = pyro();
        bad.player.stats.hpPurchases = 999; // blows the budget
        bad.enemy = bruiser();
        const std::string badWire = replay::serializeRecord(bad);
        reg("gwen");
        reg("hank");
        arb.submit({"gwen", "hank", Faction::Player, badWire});
        Arbiter::Result rb = arb.submit({"hank", "gwen", Faction::Enemy, badWire});
        CHECK(rb.status == Arbiter::Status::Rejected && rb.error.find("verification") != std::string::npos,
              "an illegal (over-budget) record fails verification");
        CHECK(accounts.ratingOf("gwen") == kDefaultRating, "no rating change on an unverifiable game");
    }

    std::printf("A single submission never ranks (can't fabricate a win alone)\n");
    {
        reg("ivan");
        reg("jane");
        const std::string game = makeNotation(ruleset, catalog, creatures, 5001);
        Arbiter::Result r = arb.submit({"ivan", "jane", Faction::Player, game});
        CHECK(r.status == Arbiter::Status::Pending, "one side alone stays pending");
        CHECK(accounts.ratingOf("ivan") == kDefaultRating && accounts.ratingOf("jane") == kDefaultRating,
              "a lone submission changes no ratings");
        CHECK(arb.pendingCount() >= 1, "the game is tracked as pending");
    }

    std::remove(dbPath.c_str());
    std::printf("\n%s (%d failure%s)\n", g_fails == 0 ? "ALL PASS" : "FAILURES", g_fails,
                g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
