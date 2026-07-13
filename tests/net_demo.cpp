//
// net_demo.cpp — Phase 4.1: wire-format round-trip + determinism, no sockets.
//
// Proves the three PvP payloads are round-trippable and that driving a match by
// Intents is deterministic and identical to the engine verbs — the foundation
// the authoritative server (Phase 4) rests on. CI smoke test.
//
#include "core/AI.h"     // defaultBrain, PlannedAction, runEnemyTurn
#include "core/Battle.h"
#include "core/Build.h"  // CharacterBuild, instantiate, serializeBuild/deserializeBuild
#include "core/Grid.h"
#include "core/Spells.h" // makeDefaultCatalog, spellid
#include "data/Net.h"

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

// A fixed, file-free 1v1 (mirrors headless_demo): two champions, no summons, so
// controlOf is irrelevant and the whole match is deterministic.
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

// Play the active unit's whole turn by translating the default Brain's plan into
// Intents (the exact vocabulary the wire carries), then endTurn.
void playTurnViaIntents(Battle& b) {
    const EntityId self = b.activeUnit();
    for (const PlannedAction& a : defaultBrain().planTurn(b, self)) {
        if (b.phase() == Phase::Finished) break;
        const Intent in = a.kind == PlannedAction::Kind::Cast ? Intent::cast(a.slot, a.target)
                                                              : Intent::move(a.target);
        applyIntent(b, self, in);
    }
    if (b.phase() != Phase::Finished) applyIntent(b, self, Intent::endTurn());
}

void playViaIntents(Battle& b) {
    for (int guard = 0; guard < 200 && b.phase() != Phase::Finished; ++guard) playTurnViaIntents(b);
}

void printErrors(const std::vector<std::string>& errs) {
    for (const std::string& e : errs) std::printf("         · %s\n", e.c_str());
}

} // namespace

int main() {
    std::printf("Intent round-trip (byte-identical) + equality\n");
    {
        const Intent samples[] = {Intent::move({3, 4}), Intent::cast(2, {5, 7}),
                                  Intent::cast(8, {5, 7}, /*rotation=*/3),
                                  Intent::castTo(1, {5, 7}, {2, 9}), Intent::endTurn()};
        for (const Intent& in : samples) {
            const std::string j = serializeIntent(in);
            Parse<Intent> p = parseIntent(j);
            if (!p.ok) printErrors(p.errors);
            CHECK(p.ok, "intent parses");
            CHECK(p.ok && p.value == in, "intent value survives the round-trip");
            CHECK(p.ok && serializeIntent(p.value) == j, "intent serialize->parse->serialize identical");
        }
        // A plain cast and a two-tile cast to the same entry are distinct intents.
        CHECK(Intent::cast(1, {5, 7}) != Intent::castTo(1, {5, 7}, {2, 9}),
              "portal exit is part of intent identity");
        // Rotation is part of intent identity too (Shelter wall heading).
        CHECK(Intent::cast(8, {5, 7}, 1) != Intent::cast(8, {5, 7}, 2),
              "wall rotation is part of intent identity");
    }

    std::printf("Intent malformed inputs are rejected\n");
    {
        CHECK(!parseIntent(R"({"kind":"fly"})").ok, "unknown kind rejected");
        CHECK(!parseIntent(R"({"kind":"move"})").ok, "move without target rejected");
        CHECK(!parseIntent(R"({"kind":"cast","target":[1,2]})").ok, "cast without spellIdx rejected");
        CHECK(!parseIntent(R"({"kind":"cast","spellIdx":-1,"target":[1,2]})").ok,
              "negative spellIdx rejected");
        CHECK(!parseIntent(R"({"kind":"endTurn","target":[1,2]})").ok, "endTurn with extra field rejected");
        CHECK(!parseIntent(R"({"kind":"move","target":[1]})").ok, "malformed target rejected");
        CHECK(!parseIntent("not json").ok, "garbage rejected");
    }

    std::printf("Build payload reuses serializeBuild (wire build format)\n");
    {
        CharacterBuild bd;
        bd.name = "Wire Champion";
        bd.stats.hpPurchases = 2;
        bd.stats.bonusAp = 1;
        bd.spellIds = {spellid::Attack, spellid::Fireball, spellid::Poison};
        const std::string wire = serializeBuild(bd);
        std::optional<CharacterBuild> back = deserializeBuild(wire);
        CHECK(back.has_value(), "build payload deserializes");
        CHECK(back && back->name == bd.name && back->spellIds == bd.spellIds &&
                  back->stats.bonusAp == 1,
              "build payload fields preserved");
        CHECK(back && serializeBuild(*back) == wire, "build payload round-trips byte-identical");
    }

    std::printf("Snapshot round-trip (byte-identical) + field fidelity\n");
    {
        Battle x = makeFixture();
        for (int i = 0; i < 4 && x.phase() != Phase::Finished; ++i) runEnemyTurn(x, /*autoEndTurn=*/true);
        const Snapshot s = snapshotOf(x);
        const std::string j = serializeSnapshot(s);
        Parse<Snapshot> p = parseSnapshot(j);
        if (!p.ok) printErrors(p.errors);
        CHECK(p.ok, "snapshot parses");
        CHECK(p.ok && serializeSnapshot(p.value) == j, "snapshot serialize->parse->serialize identical");
        CHECK(p.ok && p.value.units.size() == s.units.size(), "unit count preserved");
        CHECK(p.ok && p.value.active == s.active && p.value.phase == s.phase, "active/phase preserved");
        CHECK(p.ok && !s.units.empty() && p.value.units[0].hp == s.units[0].hp &&
                  p.value.units[0].name == s.units[0].name && p.value.units[0].pos.x == s.units[0].pos.x,
              "unit fields preserved");
    }

    std::printf("Snapshot malformed inputs are rejected\n");
    {
        CHECK(!parseSnapshot(R"({"phase":"playerTurn"})").ok, "snapshot missing units/storm rejected");
        CHECK(!parseSnapshot(R"({"phase":"nope","active":0,"round":0,"storm":{"center":[0,0]},"units":[],"ground":[]})").ok,
              "snapshot bad phase rejected");
        CHECK(!parseSnapshot("[]").ok, "non-object snapshot rejected");
    }

    std::printf("Determinism: intent-driven play is reproducible and matches the verbs\n");
    {
        Battle a = makeFixture();
        playViaIntents(a);
        const std::string sa = serializeSnapshot(snapshotOf(a));

        Battle b = makeFixture();
        playViaIntents(b);
        const std::string sb = serializeSnapshot(snapshotOf(b));
        CHECK(sa == sb, "two intent-driven runs yield the identical final snapshot");

        Battle c = makeFixture();
        for (int guard = 0; guard < 200 && c.phase() != Phase::Finished; ++guard)
            runEnemyTurn(c, /*autoEndTurn=*/true);
        CHECK(serializeSnapshot(snapshotOf(c)) == sa,
              "intents reproduce the engine verbs (matches runEnemyTurn play)");
    }

    std::printf("\n%s (%d failure%s)\n", g_fails == 0 ? "ALL PASS" : "FAILURES", g_fails,
                g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
