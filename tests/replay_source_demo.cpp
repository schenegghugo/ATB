//
// replay_source_demo.cpp — Phase 5.1: the read-only replay playback source.
//
// Record a real match, then play it back through render::ReplayMatchSource by
// pumping update() — stepping the recorded intents through a fresh MatchRunner —
// and confirm the playback reproduces the exact final state (same winner) that
// replay::verify() computes. Headless (ReplayMatchSource is raylib-free). CI.
//
#include "core/AI.h"
#include "core/Creatures.h"
#include "core/Match.h"
#include "core/Ruleset.h"
#include "core/Spells.h"
#include "net/MatchRunner.h"
#include "net/Replay.h"
#include "render/ReplayMatchSource.h"

#include <cstdio>
#include <optional>
#include <vector>

using namespace tb;

static int g_fails = 0;
#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) std::printf("  [PASS] %s\n", msg);                                               \
        else { std::printf("  [FAIL] %s\n", msg); ++g_fails; }                                     \
    } while (0)

int main() {
    const SpellCatalog catalog = makeDefaultCatalog();
    const std::vector<Entity> creatures = makeDefaultCreatures();
    const Ruleset ruleset = makeDefaultRuleset();

    CharacterBuild pb;
    pb.name = "P";
    pb.spellIds = {spellid::Attack, spellid::Fireball};
    CharacterBuild eb;
    eb.name = "E";
    eb.stats.hpPurchases = 2;
    eb.spellIds = {spellid::Attack};

    // Record a full match: drive both seats with the Brain through a MatchRunner,
    // logging every submitted intent (exactly what the wire / a saved replay holds).
    const unsigned seed = 4242;
    replay::GameRecord rec;
    rec.catalogHash = replay::catalogHash(catalog);
    rec.rulesetHash = replay::rulesetHash(ruleset);
    rec.seed = seed;
    rec.player = pb;
    rec.enemy = eb;
    {
        net::MatchRunner runner(buildMatch(ruleset, {pb}, {eb}, catalog, seed, creatures),
                                net::Seat::Human, net::Seat::Human);
        for (int guard = 0; guard < 4000 && !runner.finished(); ++guard) {
            const std::optional<Faction> seat = runner.awaitingSeat();
            if (!seat) break;
            const EntityId me = runner.battle().activeUnit();
            bool acted = false;
            for (const PlannedAction& a : defaultBrain().planTurn(runner.battle(), me)) {
                if (runner.finished()) break;
                const net::Intent in = a.kind == PlannedAction::Kind::Cast
                                           ? net::Intent::cast(a.slot, a.target)
                                           : net::Intent::move(a.target);
                rec.intents.push_back(in);
                runner.submit(*seat, in);
                acted = true;
            }
            if (!runner.finished() && (!acted || runner.awaitingSeat() == seat)) {
                rec.intents.push_back(net::Intent::endTurn());
                runner.submit(*seat, net::Intent::endTurn());
            }
        }
    }
    CHECK(!rec.intents.empty(), "recorded a non-empty match");

    const replay::VerifyResult v = replay::verify(rec, ruleset, catalog, creatures,
                                                  /*requireCommitments=*/false);
    CHECK(v.ok, "the record verifies (re-simulates cleanly)");

    // Play it back through the source, pumping update() with a dt past the step
    // interval so each pump advances one intent.
    render::ReplayMatchSource src(rec, ruleset, catalog, creatures);
    CHECK(src.total() == rec.intents.size(), "the source holds every recorded intent");
    CHECK(!src.awaitingLocalInput(), "playback is read-only (never awaits input)");

    for (int guard = 0; guard < 5000 && !src.matchOver(); ++guard) src.update(1.0f);
    CHECK(src.matchOver(), "playback runs to the end");
    CHECK(src.battle().winner() == v.winner, "playback reproduces the recorded winner");
    CHECK(src.cursor() == rec.intents.size() || src.battle().phase() == Phase::Finished,
          "every recorded intent was applied (or the match ended early, deterministically)");

    std::printf("\n%s (%d failure%s)\n", g_fails == 0 ? "ALL PASS" : "FAILURES", g_fails,
                g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
