//
// replay_demo.cpp — Phase 4.5 CR.2 / §5.1: game notation + verifier.
//
// Records a real match into the compact notation, round-trips the string, and
// proves verify() re-simulates the SAME game (winner + final state) from it —
// plus content-hash / illegal-build / incomplete-record rejections. CI smoke test.
//
#include "core/AI.h"
#include "core/Battle.h"
#include "core/Build.h"
#include "core/Creatures.h"
#include "core/Match.h"
#include "core/Ruleset.h"
#include "core/Spells.h"
#include "data/Net.h"
#include "net/MatchRunner.h"
#include "net/Replay.h"

#include <cstdio>
#include <string>

using namespace tb;

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

// Play a match with the default Brain on both seats, logging the human intents in
// applied order — the game notation's payload.
replay::GameRecord record(const Ruleset& r, const SpellCatalog& cat, const std::vector<Entity>& cre,
                          unsigned seed, std::string& finalSnap) {
    replay::GameRecord rec;
    rec.catalogHash = replay::catalogHash(cat);
    rec.rulesetHash = replay::rulesetHash(r);
    rec.seed = seed;
    rec.player = pyro();
    rec.enemy = bruiser();

    net::MatchRunner runner(buildMatch(r, {rec.player}, {rec.enemy}, cat, seed, cre), net::Seat::Human,
                            net::Seat::Human);
    for (int guard = 0; guard < 4000 && !runner.finished(); ++guard) {
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
    finalSnap = net::serializeSnapshot(net::snapshotOf(runner.battle()));
    return rec;
}

} // namespace

int main() {
    const SpellCatalog catalog = makeDefaultCatalog();
    const std::vector<Entity> creatures = makeDefaultCreatures();
    const Ruleset ruleset = makeDefaultRuleset();

    std::string liveFinal;
    const replay::GameRecord rec = record(ruleset, catalog, creatures, 1337, liveFinal);

    std::printf("Notation round-trips byte-for-byte\n");
    const std::string wire = replay::serializeRecord(rec);
    {
        replay::RecordParse p = replay::parseRecord(wire);
        CHECK(p.ok, "record parses");
        CHECK(p.ok && replay::serializeRecord(p.record) == wire, "serialize -> parse -> serialize identical");
        CHECK(p.ok && p.record.seed == rec.seed && p.record.intents.size() == rec.intents.size(),
              "seed + intent count preserved");
        std::printf("    notation is %zu chars, %zu intents\n", wire.size(), rec.intents.size());
    }

    std::printf("verify() reproduces the exact game\n");
    {
        replay::VerifyResult v = replay::verify(rec, ruleset, catalog, creatures);
        CHECK(v.ok, "record verifies");
        CHECK(v.ok && v.winner.has_value(), "the game has a winner");
        CHECK(v.ok && v.finalSnapshot == liveFinal, "replay's final state == the live match's");

        // Verify from the PARSED string (proves the notation fully captures the game).
        replay::VerifyResult v2 = replay::verify(replay::parseRecord(wire).record, ruleset, catalog, creatures);
        CHECK(v2.ok && v2.finalSnapshot == liveFinal, "verify from the parsed string reproduces it too");
        CHECK(v.finalSnapshot == v2.finalSnapshot, "verification is deterministic");
    }

    std::printf("Tampered / invalid records are rejected\n");
    {
        replay::GameRecord bad = rec;
        bad.catalogHash = "deadbeef";
        CHECK(!replay::verify(bad, ruleset, catalog, creatures).ok, "wrong catalog hash rejected");

        replay::GameRecord wrongRules = rec;
        wrongRules.rulesetHash = "deadbeef";
        CHECK(!replay::verify(wrongRules, ruleset, catalog, creatures).ok,
              "wrong ruleset hash rejected (a game under other rules can't be ranked here)");

        replay::GameRecord overBudget = rec;
        overBudget.player.stats.hpPurchases = 999; // blows the point budget
        CHECK(!replay::verify(overBudget, ruleset, catalog, creatures).ok, "illegal build rejected");

        replay::GameRecord truncated = rec;
        if (truncated.intents.size() > 6)
            truncated.intents.resize(truncated.intents.size() - 6); // stops before the finish
        CHECK(!replay::verify(truncated, ruleset, catalog, creatures).ok,
              "a record that doesn't reach a conclusion is rejected");

        CHECK(!replay::parseRecord("not a record").ok, "garbage string rejected");
        CHECK(!replay::parseRecord("ATB1 hash 5 !!! @@@").ok, "malformed builds rejected");
    }

    std::printf("\n%s (%d failure%s)\n", g_fails == 0 ? "ALL PASS" : "FAILURES", g_fails,
                g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
