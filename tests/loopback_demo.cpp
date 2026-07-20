//
// loopback_demo.cpp — Phase 4.3: the authoritative MatchRunner, in-process.
//
// Proves the intent -> apply -> snapshot loop through the server-side runner is
// deterministic and byte-identical to the raw engine, and that the trust checks
// (seat ownership + intent legality) hold — all with no socket. CI smoke test.
//
#include "core/AI.h" // defaultBrain, PlannedAction, runEnemyTurn
#include "core/Battle.h"
#include "core/Build.h"
#include "core/Grid.h"
#include "core/Spells.h"
#include "data/Net.h"
#include "net/MatchRunner.h"
#include "net/Replay.h" // intentToken / parseIntentToken (correspondence notation)

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

// A fixed, file-free 1v1 (mirrors headless_demo): two champions, no summons.
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

std::string snap(const MatchRunner& r) { return serializeSnapshot(r.snapshot()); }

// A tiny, file-free open arena with a Player champion holding Shelter at a KNOWN
// tile, so a cast's wall footprint is deterministic. The enemy is a static
// sparring dummy off to the side (never in the wall's path).
Battle makeShelterFixture() {
    SpellCatalog catalog = makeDefaultCatalog();
    Grid grid(14, 7); // all walkable
    auto mk = [&](std::string name, Faction team, Vec2i pos, std::vector<int> ids, int init) {
        Entity e;
        e.name = std::move(name);
        e.team = team;
        e.pos = pos;
        e.maxHp = e.hp = 60;
        e.maxAp = e.ap = 30; // generous so the cast never fails on AP
        e.maxMp = e.mp = 30;
        e.initiative = init; // Player acts first
        for (int id : ids)
            if (const SpellDef* d = catalog.find(id)) e.spells.push_back(d->spell);
        return e;
    };
    std::vector<Entity> roster;
    roster.push_back(mk("P", Faction::Player, {2, 1}, {spellid::Attack, spellid::Shelter}, 10));
    roster.push_back(mk("E", Faction::Enemy, {11, 5}, {spellid::Attack}, 5));
    return Battle(std::move(grid), std::move(roster));
}

bool hasWall(const Battle& b, Vec2i t) {
    for (Vec2i w : b.wallTiles())
        if (w.x == t.x && w.y == t.y) return true;
    return false;
}

// A scripted "human": on its seat's turn, play the default Brain's plan as
// Intents (exactly the mutations the engine would apply), then endTurn.
void playHumans(MatchRunner& r) {
    for (int guard = 0; guard < 4000 && !r.finished(); ++guard) {
        const std::optional<Faction> seat = r.awaitingSeat();
        if (!seat) break;
        const EntityId me = r.battle().activeUnit();
        for (const PlannedAction& a : defaultBrain().planTurn(r.battle(), me)) {
            if (r.finished()) break;
            r.submit(*seat, a.kind == PlannedAction::Kind::Cast ? Intent::cast(a.slot, a.target)
                                                               : Intent::move(a.target));
        }
        if (!r.finished()) r.submit(*seat, Intent::endTurn());
    }
}

} // namespace

int main() {
    // The reference: the raw engine driving every turn.
    std::string engineFinal;
    {
        Battle ref = makeFixture();
        for (int g = 0; g < 4000 && ref.phase() != Phase::Finished; ++g)
            runEnemyTurn(ref, /*autoEndTurn=*/true);
        engineFinal = serializeSnapshot(snapshotOf(ref));
    }

    std::printf("A fully-AI runner reproduces the raw engine\n");
    {
        MatchRunner r(makeFixture(), Seat::AI, Seat::AI);
        CHECK(r.finished(), "all-AI match runs to completion inside the runner");
        CHECK(!r.awaitingSeat().has_value(), "a finished match awaits no seat");
        CHECK(snap(r) == engineFinal, "runner's final snapshot is byte-identical to the engine");
    }

    std::printf("The intent loop reproduces the engine, and is deterministic\n");
    {
        MatchRunner a(makeFixture(), Seat::Human, Seat::Human);
        playHumans(a);
        MatchRunner b(makeFixture(), Seat::Human, Seat::Human);
        playHumans(b);
        CHECK(a.finished() && b.finished(), "both human-driven matches finish");
        CHECK(snap(a) == snap(b), "two intent-driven runs yield the identical final snapshot");
        CHECK(snap(a) == engineFinal, "intent-driven play matches the raw engine byte-for-byte");
    }

    std::printf("Authority: seat ownership + intent legality are enforced\n");
    {
        MatchRunner r(makeFixture(), Seat::Human, Seat::Human);
        const std::optional<Faction> seat = r.awaitingSeat();
        CHECK(seat.has_value(), "a human seat is awaited at the start");

        // A different seat cannot move this seat's unit.
        const std::string before = snap(r);
        const bool wrong = r.submit(opposing(*seat), Intent::move(r.battle().unit(1).pos));
        CHECK(!wrong, "an intent from the wrong seat is rejected");
        CHECK(snap(r) == before, "the rejected wrong-seat intent does not mutate state");

        // An illegal cast from the correct seat is refused, no mutation.
        const bool illegal = r.submit(*seat, Intent::cast(1, {-1, -1}));
        CHECK(!illegal, "an illegal cast is rejected");
        CHECK(snap(r) == before, "the rejected illegal cast does not mutate state");

        // A legal move from the correct seat is accepted.
        const bool ok = r.submit(*seat, Intent::move(r.battle().unit(1).pos));
        CHECK(ok, "a legal move from the owning seat is accepted");
    }

    std::printf("PvE: the server plays the AI seat; only the human is awaited\n");
    {
        MatchRunner r(makeFixture(), Seat::Human, Seat::AI);
        bool sawEnemyAwaited = false;
        for (int g = 0; g < 4000 && !r.finished(); ++g) {
            const std::optional<Faction> seat = r.awaitingSeat();
            if (!seat) break;
            if (*seat == Faction::Enemy) sawEnemyAwaited = true;
            const EntityId me = r.battle().activeUnit();
            for (const PlannedAction& a : defaultBrain().planTurn(r.battle(), me)) {
                if (r.finished()) break;
                r.submit(*seat, a.kind == PlannedAction::Kind::Cast ? Intent::cast(a.slot, a.target)
                                                                   : Intent::move(a.target));
            }
            if (!r.finished()) r.submit(*seat, Intent::endTurn());
        }
        CHECK(!sawEnemyAwaited, "the AI-filled enemy seat is never awaited (server drives it)");
        CHECK(r.finished(), "the PvE match completes");
    }

    // A Line spell's wheel rotation (Shelter walls today; more spells later) must
    // survive the wire AND actually rotate the cast on the authoritative side —
    // both the JSON wire (live online) and the notation token (correspondence)
    // carry `rotation`, and every server path applies it through applyIntent.
    // Regression guard for the dropped-rotation bug (LocalMatchSource skipped it).
    std::printf("A Line spell's rotation survives the wire and rotates the cast\n");
    {
        const int shelterSlot = 1; // fixture loadout is {Attack, Shelter}
        // Caster (2,1); target (5,1) is due EAST, so one wheel notch (rotation 1)
        // turns the heading SOUTH — the wall must run DOWN from (5,1), not east.
        const Intent original = Intent::cast(shelterSlot, {5, 1}, /*rotation=*/1);

        // (a) JSON wire — RemoteMatchSource -> GameServer / MirrorSession.
        const Parse<Intent> j = parseIntent(serializeIntent(original));
        CHECK(j.ok, "cast intent JSON round-trips");
        CHECK(j.ok && j.value == original, "JSON preserves rotation (decoded == original)");

        // (b) Notation token — correspondence (intentToken -> parseIntentToken).
        Intent tok;
        CHECK(replay::parseIntentToken(replay::intentToken(original), tok),
              "cast intent notation token round-trips");
        CHECK(tok == original, "notation preserves rotation (decoded == original)");

        // Apply the WIRE-DECODED intent through the authoritative runner, which is
        // exactly what the server does (parseIntent -> runner.submit -> applyIntent).
        MatchRunner r(makeShelterFixture(), Seat::Human, Seat::Human);
        const std::optional<Faction> seat = r.awaitingSeat();
        CHECK(seat == Faction::Player, "the player champion is awaited first");
        CHECK(r.submit(*seat, j.value), "the rotated Shelter cast is accepted");

        const Battle& b = r.battle();
        CHECK(b.wallTiles().size() == 5, "5 wall tiles created");
        CHECK(hasWall(b, {5, 2}) && hasWall(b, {5, 5}), "walls run SOUTH (the rotated footprint)");
        CHECK(!hasWall(b, {6, 1}) && !hasWall(b, {8, 1}),
              "walls do NOT run straight east from the caster (dropped-rotation regression)");
    }

    std::printf("\n%s (%d failure%s)\n", g_fails == 0 ? "ALL PASS" : "FAILURES", g_fails,
                g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
