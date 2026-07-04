//
// matchsource_demo.cpp — Phase 4.2: the LocalMatchSource seam, headless.
//
// LocalMatchSource is raylib-free (pure core + net), so the turn-driving logic
// the GUI now runs behind the seam is testable without a window: its contract
// (awaitingLocalInput / submit / update) and that a full match driven through it
// is deterministic. CI smoke test.
//
#include "core/AI.h"     // defaultBrain, PlannedAction
#include "core/Battle.h"
#include "core/Build.h"
#include "core/Grid.h"
#include "core/Spells.h"
#include "data/Net.h"    // Intent, snapshotOf, serializeSnapshot
#include "render/MatchSource.h"

#include <cstdio>
#include <memory>
#include <string>

using namespace tb;
using namespace tb::render;

static int g_fails = 0;
#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) std::printf("  [PASS] %s\n", msg);                                               \
        else { std::printf("  [FAIL] %s\n", msg); ++g_fails; }                                     \
    } while (0)

namespace {

// A fixed, file-free 1v1 (mirrors headless_demo): player Pyromancer vs AI Bruiser.
Battle makeFixture() {
    SpellCatalog catalog = makeDefaultCatalog();
    BuildRules rules{};

    CharacterBuild pyro;
    pyro.name = "Pyromancer";
    pyro.stats.bonusAp = 1;
    pyro.spellIds = {spellid::Attack, spellid::Fireball, spellid::Poison};

    CharacterBuild bruiser;
    bruiser.name = "Bruiser";
    bruiser.stats.hpPurchases = 4;
    bruiser.stats.bonusMp = 1;
    bruiser.spellIds = {spellid::Attack, spellid::Knockback, spellid::Harpoon};

    ArenaConfig cfg;
    cfg.seed = 1337;
    Grid grid = generateArena(cfg);

    std::vector<Entity> roster;
    roster.push_back(instantiate(pyro, catalog, Faction::Player, cfg.playerSpawn, rules));
    roster.push_back(instantiate(bruiser, catalog, Faction::Enemy, cfg.enemySpawn, rules));
    return Battle(std::move(grid), std::move(roster));
}

// Play a whole match through the seam: on the player's turn feed the default
// Brain's plan as Intents (what a human's clicks would produce); on other turns
// tick update() with dt past the AI pacing so it acts each call.
void driveToEnd(LocalMatchSource& s) {
    for (int guard = 0; guard < 4000 && s.battle().phase() != Phase::Finished; ++guard) {
        if (s.awaitingLocalInput()) {
            const EntityId me = s.battle().activeUnit();
            for (const PlannedAction& a : defaultBrain().planTurn(s.battle(), me)) {
                if (s.battle().phase() == Phase::Finished) break;
                s.submit(a.kind == PlannedAction::Kind::Cast ? net::Intent::cast(a.slot, a.target)
                                                             : net::Intent::move(a.target));
            }
            if (s.battle().phase() != Phase::Finished) s.submit(net::Intent::endTurn());
        } else {
            s.update(1.0f); // dt >= kAiTick → the AI takes one action this call
        }
    }
}

} // namespace

int main() {
    std::printf("Contract: awaitingLocalInput / submit / update\n");
    {
        LocalMatchSource s(makeFixture());
        CHECK(s.awaitingLocalInput(), "player Champion holds the first turn (input is live)");

        // update() must NOT drive anything while it's the player's turn.
        const std::string before = serializeSnapshot(net::snapshotOf(s.battle()));
        CHECK(!s.update(1.0f).has_value(), "update() is a no-op on the player's turn");
        CHECK(serializeSnapshot(net::snapshotOf(s.battle())) == before, "state unchanged by that update()");

        // A move Intent reports tiles moved (or refuses cleanly).
        const EntityId me = s.battle().activeUnit();
        const Vec2i foe = s.battle().unit(1).pos;
        auto moved = s.submit(net::Intent::move(foe));
        CHECK(moved.has_value(), "move Intent yields a status");

        // An illegal cast is refused without mutating (target far away / no AP path).
        const std::string preCast = serializeSnapshot(net::snapshotOf(s.battle()));
        auto bad = s.submit(net::Intent::cast(1, {-1, -1}));
        CHECK(bad.has_value() && bad->rfind("Cast failed", 0) == 0, "illegal cast reports failure");
        CHECK(serializeSnapshot(net::snapshotOf(s.battle())) == preCast, "illegal cast does not mutate state");

        // Ending the turn hands control to the AI side.
        s.submit(net::Intent::endTurn());
        CHECK(!s.awaitingLocalInput(), "after endTurn it is the AI's turn (no local input)");

        // Now update() drives the AI: paced (no-op below the tick, acts at/above it).
        LocalMatchSource s2(makeFixture());
        s2.submit(net::Intent::endTurn()); // hand off to the AI
        CHECK(!s2.update(0.01f).has_value(), "update() below the AI tick does nothing");
        const std::string preTick = serializeSnapshot(net::snapshotOf(s2.battle()));
        s2.update(1.0f);
        CHECK(serializeSnapshot(net::snapshotOf(s2.battle())) != preTick, "update() past the tick advances the AI");
    }

    std::printf("A full match through the seam is deterministic\n");
    {
        LocalMatchSource a(makeFixture());
        driveToEnd(a);
        const std::string sa = serializeSnapshot(net::snapshotOf(a.battle()));

        LocalMatchSource b(makeFixture());
        driveToEnd(b);
        const std::string sb = serializeSnapshot(net::snapshotOf(b.battle()));

        CHECK(a.battle().phase() == Phase::Finished, "the match reaches a conclusion");
        CHECK(sa == sb, "two seam-driven runs yield the identical final snapshot");
    }

    // The interface is usable polymorphically (the shape a RemoteMatchSource fills).
    {
        std::unique_ptr<MatchSource> src = std::make_unique<LocalMatchSource>(makeFixture());
        CHECK(src->awaitingLocalInput(), "drives through the MatchSource base interface");
    }

    std::printf("\n%s (%d failure%s)\n", g_fails == 0 ? "ALL PASS" : "FAILURES", g_fails,
                g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
