//
// ranked_rules_demo.cpp — Phase 4.5 CR.5: the perfect-information ranked ruleset.
//
// data/rules.ranked.json bans invisibility (P2P/correspondence clients must hold
// the full state to simulate, so a hidden unit is only hidden from honest
// clients — ranked is perfect-information until commit-reveal lands, see
// MILESTONES). Proves: the ban loads + rejects invisibility builds; the ruleset
// hash is content-addressed (a URL-fetched copy verifies by hash); scoresheets
// pin their ruleset so games can't be cross-submitted; and a legal ranked game
// still ranks end-to-end through the arbiter. CI smoke test.
//
// Usage: tb_ranked_rules_demo [path-to-rules.ranked.json]   (default: data/…)
//
#include "core/AI.h"
#include "core/Build.h"
#include "core/Creatures.h"
#include "core/Match.h"
#include "core/Ruleset.h"
#include "core/Spells.h"
#include "data/Net.h"
#include "data/RulesetJson.h"
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

CharacterBuild sneak() { // invisibility user — legal casually, banned in ranked
    CharacterBuild b;
    b.name = "Sneak";
    b.spellIds = {spellid::Attack, spellid::Invisible};
    return b;
}
// Note: builds here must fit the CANONICAL data-file economy (data/rules.json,
// e.g. hpCost 2), which is stricter than the compiled fallback defaults.
CharacterBuild pyro() {
    CharacterBuild b;
    b.name = "Pyromancer";
    b.stats.bonusAp = 1; // 2+2+3 spells + 3 = 10 pts
    b.spellIds = {spellid::Attack, spellid::Fireball, spellid::Poison};
    return b;
}
CharacterBuild tank() {
    CharacterBuild b;
    b.name = "Tank";
    b.stats.hpPurchases = 2; // 2+2 spells + 2*2 hp + 2 mp = 10 pts
    b.stats.bonusMp = 1;
    b.spellIds = {spellid::Attack, spellid::Knockback};
    return b;
}

// Record a legal pyro-vs-bruiser game under `rules` into a notation string.
std::string makeNotation(const Ruleset& rules, const SpellCatalog& cat,
                         const std::vector<Entity>& cre, unsigned seed) {
    replay::GameRecord rec;
    rec.catalogHash = replay::catalogHash(cat);
    rec.rulesetHash = replay::rulesetHash(rules);
    rec.seed = seed;
    rec.player = pyro();
    rec.enemy = tank();
    MatchRunner runner(buildMatch(rules, {rec.player}, {rec.enemy}, cat, seed, cre), Seat::Human,
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

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : "data/rules.ranked.json";
    const SpellCatalog catalog = makeDefaultCatalog();
    const std::vector<Entity> creatures = makeDefaultCreatures();

    // Casual = the canonical data file (hand-tuned economy), not the compiled
    // fallback — the ranked file is authored as "that + the invisibility ban".
    RulesetLoad casualLoad = loadRulesetFromFile("data/rules.json");
    if (!casualLoad.ok) {
        std::printf("FATAL: data/rules.json failed to load (run from the repo root)\n");
        return 1;
    }
    const Ruleset casual = casualLoad.ruleset;

    std::printf("The official ranked ruleset loads and bans invisibility\n");
    Ruleset ranked;
    {
        RulesetLoad load = loadRulesetFromFile(path);
        if (!load.ok)
            for (const std::string& e : load.errors) std::printf("         · %s\n", e.c_str());
        CHECK(load.ok, "data/rules.ranked.json loads + validates");
        ranked = load.ruleset;
        CHECK(std::find(ranked.bannedSpells.begin(), ranked.bannedSpells.end(), "invisible") !=
                  ranked.bannedSpells.end(),
              "ranked bans the 'invisible' spell key");
    }

    std::printf("The ruleset hash is content-addressed (URL-fetch verifiable)\n");
    {
        // An equivalent built in memory (casual + the ban) must hash the same as
        // the loaded file — the hash pins content, not bytes or origin.
        Ruleset rebuilt = casual;
        rebuilt.bannedSpells = {"invisible"};
        CHECK(replay::rulesetHash(rebuilt) == replay::rulesetHash(ranked),
              "loaded file and equivalent in-memory ruleset hash identically");
        CHECK(replay::rulesetHash(ranked) != replay::rulesetHash(casual),
              "ranked and casual rulesets hash differently");
    }

    std::printf("Invisibility builds: fine casually, rejected in ranked\n");
    {
        CHECK(validateBuild(sneak(), catalog, casual.economy, casual.bannedSpells).ok,
              "the invisibility build is legal under casual rules");
        CHECK(!validateBuild(sneak(), catalog, ranked.economy, ranked.bannedSpells).ok,
              "the same build is rejected under ranked rules");
    }

    std::printf("A scoresheet with an invisibility build cannot rank\n");
    {
        replay::GameRecord rec;
        rec.catalogHash = replay::catalogHash(catalog);
        rec.rulesetHash = replay::rulesetHash(ranked); // claims to be a ranked game
        rec.seed = 42;
        rec.player = sneak();
        rec.enemy = tank();
        const replay::VerifyResult v = replay::verify(rec, ranked, catalog, creatures);
        CHECK(!v.ok && v.error.find("illegal player build") != std::string::npos,
              "verify() under ranked rejects it as an illegal build");
    }

    std::printf("Scoresheets pin their ruleset — no cross-submitting\n");
    const std::string rankedGame = makeNotation(ranked, catalog, creatures, 7331);
    {
        const replay::GameRecord rec = replay::parseRecord(rankedGame).record;
        CHECK(replay::verify(rec, ranked, catalog, creatures).ok, "a legal ranked game verifies");
        const replay::VerifyResult cross = replay::verify(rec, casual, catalog, creatures);
        CHECK(!cross.ok && cross.error.find("ruleset hash") != std::string::npos,
              "the same record is rejected under a different ruleset (hash mismatch)");
    }

    std::printf("End to end: a legal ranked game ranks through the arbiter\n");
    {
        const std::string dbPath = "tb_ranked_rules_test.json";
        std::remove(dbPath.c_str());
        AccountStore accounts(dbPath);
        accounts.authenticate("alice", "pw");
        accounts.authenticate("bob", "pw");
        Arbiter arb(accounts, ranked, catalog, creatures);

        arb.submit({"alice", "bob", Faction::Player, rankedGame});
        const Arbiter::Result r = arb.submit({"bob", "alice", Faction::Enemy, rankedGame});
        CHECK(r.status == Arbiter::Status::Ranked, "double-submit under the ranked ruleset ranks");
        CHECK(accounts.ratingOf("alice") + accounts.ratingOf("bob") == 2 * kDefaultRating,
              "Elo recorded zero-sum");
        std::remove(dbPath.c_str());
    }

    std::printf("\n%s (%d failure%s)\n", g_fails == 0 ? "ALL PASS" : "FAILURES", g_fails,
                g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
