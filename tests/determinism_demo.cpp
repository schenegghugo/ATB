//
// determinism_demo.cpp — Phase 4.5 CR.1: cross-platform determinism lock-down.
//
// Known-answer tests that pin (seed -> arena) and (seed + builds + intents ->
// final state) to fixed fingerprints. If a compiler / standard library / platform
// ever produces a different result from the same inputs, these fail — which is the
// whole point: the correspondence-ranked arbiter (and the live mirror) must
// reproduce a game bit-for-bit. The expected hashes were captured on x86-64 /
// libstdc++; a conforming platform must match them.
//
#include "core/AI.h"
#include "core/Battle.h"
#include "core/Build.h"
#include "core/Grid.h"
#include "core/Spells.h"
#include "data/Net.h"
#include "data/Sha256.h"

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

// Locked fingerprints (x86-64 / libstdc++). See header note.
constexpr const char* kArenaFp = "6fd60888391ca8402dee80d2435596ac2d797e6f531c4e09a5aaf73811c83025";
constexpr const char* kMatchFp = "ec39f5c1ef00944ed5133a7e36dd9b2d766e2d53753106279a20e7ff2aaa158e";

std::string gridString(const Grid& g) {
    std::string s;
    for (int y = 0; y < g.height(); ++y)
        for (int x = 0; x < g.width(); ++x) {
            const TileType t = g.at({x, y});
            s.push_back(t == TileType::Wall ? '#' : t == TileType::Obstacle ? 'o' : '.');
        }
    return s;
}

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

} // namespace

int main() {
    std::printf("Arena generation is deterministic + platform-stable\n");
    std::string arenaFp;
    {
        ArenaConfig cfg;
        cfg.seed = 1337;
        const Grid a = generateArena(cfg);
        const Grid b = generateArena(cfg);
        CHECK(gridString(a) == gridString(b), "same seed -> identical arena (within run)");

        ArenaConfig other = cfg;
        other.seed = 9001;
        CHECK(gridString(generateArena(other)) != gridString(a), "different seed -> different arena");

        arenaFp = sha256Hex(gridString(a));
        std::printf("    arena fingerprint = %s\n", arenaFp.c_str());
        CHECK(arenaFp == kArenaFp, "arena fingerprint matches the locked known answer");
    }

    std::printf("A full match is deterministic + platform-stable\n");
    std::string matchFp;
    {
        Battle x = makeFixture();
        for (int g = 0; g < 400 && x.phase() != Phase::Finished; ++g) runEnemyTurn(x, true);
        matchFp = sha256Hex(net::serializeSnapshot(net::snapshotOf(x)));
        std::printf("    match fingerprint = %s\n", matchFp.c_str());

        Battle y = makeFixture();
        for (int g = 0; g < 400 && y.phase() != Phase::Finished; ++g) runEnemyTurn(y, true);
        CHECK(net::serializeSnapshot(net::snapshotOf(y)) == net::serializeSnapshot(net::snapshotOf(x)),
              "same inputs -> identical final state (within run)");
        CHECK(matchFp == kMatchFp, "match fingerprint matches the locked known answer");
    }

    std::printf("\n%s (%d failure%s)\n", g_fails == 0 ? "ALL PASS" : "FAILURES", g_fails,
                g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
