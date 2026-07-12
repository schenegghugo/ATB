//
// decoy_demo.cpp — CR.6 mechanics: decoy pairs (deferred damage + reveal), the
// Blind range debuff, delayed statuses (Surge's crash), and polarized statuses
// (Flux). The AI doesn't script these, so the Battle API is driven directly on a
// fixed open arena (same approach as spells_demo). CI smoke test.
//
#include "core/Battle.h"
#include "core/Spells.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace tb;

namespace {

int g_fails = 0;
void check(bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) ++g_fails;
}

SpellCatalog catalog = makeDefaultCatalog();

Entity makeUnit(std::string name, Faction team, Vec2i pos, std::vector<int> spellIds,
                int initiative) {
    Entity e;
    e.name = std::move(name);
    e.team = team;
    e.pos = pos;
    e.maxHp = e.hp = 60;
    e.maxAp = e.ap = 30; // generous so scripts can chain casts
    e.maxMp = e.mp = 30;
    e.initiative = initiative;
    for (int id : spellIds)
        if (const SpellDef* d = catalog.find(id)) e.spells.push_back(d->spell);
    return e;
}

int slotOf(const Battle& b, EntityId who, int spellId) {
    const auto& spells = b.unit(who).spells;
    const SpellDef* def = catalog.find(spellId);
    for (int i = 0; i < static_cast<int>(spells.size()); ++i)
        if (def && spells[i].name == def->spell.name) return i;
    return -1;
}

// Advance turns until `who` holds the turn (bounded).
void toTurnOf(Battle& b, EntityId who) {
    for (int i = 0; i < 32 && b.activeUnit() != who && b.phase() != Phase::Finished; ++i)
        b.endTurn();
}

} // namespace

int main() {
    constexpr EntityId P = 0, E = 1;

    // --- Blind: -60% max cast range, floored at minRange ---------------------
    std::printf("Blind (range debuff):\n");
    {
        Grid grid(14, 7);
        std::vector<Entity> roster;
        roster.push_back(makeUnit("P", Faction::Player, {1, 3}, {spellid::Blind}, 10));
        roster.push_back(makeUnit("E", Faction::Enemy, {4, 3}, {spellid::Attack}, 5));
        Battle b(std::move(grid), std::move(roster));

        const int atk = slotOf(b, E, spellid::Attack); // range 1-3
        check(b.canCast(E, atk, {1, 3}), "attack reaches range 3 before the debuff");

        b.cast(P, slotOf(b, P, spellid::Blind), {4, 3}); // distance 3, in blind's range
        // effMax = 3 - (3*60)/100 = 2.
        check(!b.canCast(E, atk, {1, 3}), "range 3 blocked while blinded");
        check(b.canCast(E, atk, {2, 3}), "range 2 (the reduced max) still castable");

        // Stacking past 100% clamps to minRange (attack minRange 1).
        b.unit(E).statuses.push_back({StatusEffect::Kind::RangeDebuff, 60, 3});
        check(!b.canCast(E, atk, {2, 3}), "range 2 blocked at a stacked 120% (capped 100%)");
        check(b.canCast(E, atk, {3, 3}), "range 1 (minRange floor) always castable");

        // Expiry: turns=3 ages at E's turn starts -> free again on its third turn.
        b.unit(E).statuses.pop_back(); // drop the injected stack, keep the cast one
        toTurnOf(b, E); // ages 3->2 (still active)
        check(!b.canCast(E, atk, {1, 3}), "still blinded on the victim's first turn");
        toTurnOf(b, P); toTurnOf(b, E); // ages 2->1 (still active)
        check(!b.canCast(E, atk, {1, 3}), "still blinded on the second turn");
        toTurnOf(b, P); toTurnOf(b, E); // ages 1->0 -> removed
        check(b.canCast(E, atk, {1, 3}), "debuff expired — full range restored");
    }

    // --- Surge: +2 AP for 2 turns, then the -6 crash for 1 turn --------------
    std::printf("Surge (delayed status crash):\n");
    {
        Grid grid(14, 7);
        std::vector<Entity> roster;
        roster.push_back(makeUnit("P", Faction::Player, {1, 3}, {spellid::Surge}, 10));
        roster.push_back(makeUnit("E", Faction::Enemy, {7, 3}, {spellid::Attack}, 5));
        Battle b(std::move(grid), std::move(roster));

        b.cast(P, slotOf(b, P, spellid::Surge), {1, 3}); // self-cast (range 0-3)
        toTurnOf(b, E); toTurnOf(b, P);
        check(b.unit(P).ap == 32, "turn 1 after cast: +2 AP (32)");
        toTurnOf(b, E); toTurnOf(b, P);
        check(b.unit(P).ap == 32, "turn 2 after cast: +2 AP (32)");
        toTurnOf(b, E); toTurnOf(b, P);
        check(b.unit(P).ap == 24, "turn 3: the delayed crash lands (-6 -> 24)");
        toTurnOf(b, E); toTurnOf(b, P);
        check(b.unit(P).ap == 30, "turn 4: back to normal");

        // The AP floor: a crash bigger than max AP can't go negative.
        b.unit(E).maxAp = 4;
        b.unit(E).statuses.push_back({StatusEffect::Kind::ApBuff, -6, 1});
        toTurnOf(b, E);
        check(b.unit(E).ap == 0, "AP floors at 0 under a heavy debuff");
    }

    // --- Flux: polarized — +2 MP to an ally, -2 MP to a foe ------------------
    std::printf("Flux (polarized status):\n");
    {
        constexpr EntityId A = 1, E2 = 2;
        Grid grid(14, 7);
        std::vector<Entity> roster;
        roster.push_back(makeUnit("P", Faction::Player, {1, 3}, {spellid::Flux}, 10));
        roster.push_back(makeUnit("A", Faction::Player, {2, 3}, {spellid::Attack}, 8));
        roster.push_back(makeUnit("E", Faction::Enemy, {6, 3}, {spellid::Attack}, 5));
        Battle b(std::move(grid), std::move(roster));

        const int flux = slotOf(b, P, spellid::Flux);
        check(b.cast(P, flux, {2, 3}), "flux lands on the ally");
        toTurnOf(b, A);
        check(b.unit(A).mp == 32, "ally gets +2 MP");

        toTurnOf(b, P); // cooldown 2 -> 1
        toTurnOf(b, E2); toTurnOf(b, P); // cooldown 1 -> 0
        check(b.cast(P, flux, {6, 3}), "flux lands on the enemy after cooldown");
        toTurnOf(b, E2);
        check(b.unit(E2).mp == 28, "enemy gets -2 MP from the same spell");
        toTurnOf(b, P); toTurnOf(b, E2);
        check(b.unit(E2).mp == 30, "one turn only — MP restored after");
    }

    // --- Decoy: cloaked pair, deferred damage, reveal ------------------------
    std::printf("Decoy (cloaked pair):\n");
    auto decoyArena = [&]() {
        Grid grid(14, 7);
        std::vector<Entity> roster;
        roster.push_back(makeUnit("P", Faction::Player, {2, 3},
                                  {spellid::Attack, spellid::Decoy}, 10));
        roster.push_back(makeUnit("E", Faction::Enemy, {5, 3}, {spellid::Attack}, 5));
        return Battle(std::move(grid), std::move(roster));
    };
    constexpr EntityId Twin = 2; // spawned third

    { // spawn + indistinguishability + deferred damage + reveal-by-acting
        Battle b = decoyArena();
        b.cast(P, slotOf(b, P, spellid::Decoy), {3, 3});
        check(b.unitCount() == 3, "the twin joins the roster");
        check(b.unit(Twin).name == "P" && b.unit(Twin).kind == EntityKind::Champion &&
                  b.unit(Twin).hp == b.unit(P).hp && b.unit(Twin).team == Faction::Player,
              "the twin is a full, publicly identical copy");
        check(b.isCloaked(P) && b.isCloaked(Twin) && b.cloakPairs().size() == 1,
              "both members are cloaked as one pair");

        toTurnOf(b, E);
        const int atk = slotOf(b, E, spellid::Attack);
        b.cast(E, atk, {2, 3}); // hit the original's tile
        b.cast(E, atk, {3, 3}); // hit the twin's tile
        check(b.unit(P).hp == 60 && b.unit(Twin).hp == 60,
              "hits on cloaked members defer — no visible HP change");

        toTurnOf(b, P);
        b.cast(P, slotOf(b, P, spellid::Attack), {5, 3}); // acting reveals: P is real
        check(!b.isCloaked(P) && b.cloakPairs().empty(), "casting reveals the pair");
        check(!b.unit(Twin).alive(), "the decoy vanishes at reveal");
        check(b.unit(P).hp == 45, "only the real member's deferred damage lands (60-15)");
        check(b.unit(E).hp == 45, "the revealing cast still resolves against the foe");
        check(b.phase() != Phase::Finished, "vanishing decoy is not a victory-relevant death");
    }

    { // acting from the TWIN swaps identity: the original becomes the decoy
        Battle b = decoyArena();
        b.cast(P, slotOf(b, P, spellid::Decoy), {3, 3});
        toTurnOf(b, Twin); // twin has its own initiative slot
        b.cast(Twin, slotOf(b, Twin, spellid::Attack), {5, 3});
        check(!b.unit(P).alive() && b.unit(Twin).alive(),
              "the twin was declared real — the original body fades");
        check(b.phase() != Phase::Finished, "the team lives on through the twin");
        check(b.controlledByPlayer(Twin), "the twin stays player-controlled");
    }

    { // expiry: unrevealed pairs default to the original
        Battle b = decoyArena();
        b.cast(P, slotOf(b, P, spellid::Decoy), {3, 3});
        for (int i = 0; i < 40 && b.isCloaked(P); ++i) b.endTurn();
        check(!b.isCloaked(P) && b.unit(P).alive() && !b.unit(Twin).alive(),
              "at expiry the original is real by rule; the twin fades");
    }

    { // lethal deferred damage: the reveal itself can kill (and ends the match)
        Battle b = decoyArena();
        b.unit(P).hp = 10;
        b.cast(P, slotOf(b, P, spellid::Decoy), {3, 3});
        toTurnOf(b, E);
        b.cast(E, slotOf(b, E, spellid::Attack), {2, 3}); // 15 pending > 10 HP
        check(b.unit(P).alive(), "still standing while cloaked (damage deferred)");
        toTurnOf(b, P);
        const int ehp = b.unit(E).hp;
        b.cast(P, slotOf(b, P, spellid::Attack), {5, 3}); // reveal -> lethal -> fizzle
        check(!b.unit(P).alive(), "the deferred damage lands lethally at reveal");
        check(b.unit(E).hp == ehp, "the fizzled cast never resolves");
        check(b.phase() == Phase::Finished && b.winner() == Faction::Enemy,
              "with both bodies down the match ends properly");
    }

    std::printf("\n%s (%d failure%s)\n", g_fails == 0 ? "ALL PASS" : "FAILURES", g_fails,
                g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
