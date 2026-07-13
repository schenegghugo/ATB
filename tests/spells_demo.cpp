//
// spells_demo.cpp — Deterministic checks for cooldowns + the four ground/status
// spells (Shelter, Glyph, Portal, Invisible). The combat AI never casts utility
// spells, so these mechanics are validated by scripting the Battle API directly
// on a fixed open arena.
//
#include "core/AI.h"
#include "core/Battle.h"
#include "core/Spells.h"

#include <cstdio>
#include <vector>

using namespace tb;

namespace {

int g_fails = 0;
void check(bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) ++g_fails;
}

SpellCatalog catalog = makeDefaultCatalog();

Entity makeUnit(std::string name, Faction team, Vec2i pos, std::vector<int> spellIds) {
    Entity e;
    e.name = std::move(name);
    e.team = team;
    e.pos = pos;
    e.maxHp = e.hp = 60;
    e.maxAp = e.ap = 30; // generous so scripts can chain casts
    e.maxMp = e.mp = 30;
    e.initiative = team == Faction::Player ? 10 : 5;
    for (int id : spellIds)
        if (const SpellDef* d = catalog.find(id)) e.spells.push_back(d->spell);
    return e;
}

// Open 14x7 arena, player vs enemy. Caller passes the player's spell loadout.
Battle makeArena(Vec2i playerPos, Vec2i enemyPos, std::vector<int> playerSpells) {
    Grid grid(14, 7); // all walkable
    std::vector<Entity> roster;
    roster.push_back(makeUnit("P", Faction::Player, playerPos, std::move(playerSpells)));
    roster.push_back(makeUnit("E", Faction::Enemy, enemyPos, {spellid::Attack}));
    Battle b(std::move(grid), std::move(roster));
    b.unit(1).ap = b.unit(1).mp = 30; // give the enemy resources for scripted moves
    return b;
}

int slotOf(const Battle& b, EntityId who, int spellId) {
    const auto& spells = b.unit(who).spells;
    const SpellDef* def = catalog.find(spellId);
    for (int i = 0; i < static_cast<int>(spells.size()); ++i)
        if (def && spells[i].name == def->spell.name) return i;
    return -1;
}

} // namespace

int main() {
    constexpr EntityId P = 0, E = 1;

    // --- Cooldowns ----------------------------------------------------------
    std::printf("Cooldowns:\n");
    {
        Battle b = makeArena({1, 3}, {6, 3}, {spellid::Attack, spellid::Bulwark});
        int bw = slotOf(b, P, spellid::Bulwark);
        const Vec2i tgt{1, 3}; // self-cast (Bulwark range 0-2, no LOS needed)
        check(b.canCast(P, bw, tgt), "bulwark castable initially");
        b.cast(P, bw, tgt);
        check(!b.canCast(P, bw, tgt), "bulwark blocked right after cast (cd 2)");
        b.endTurn(); b.endTurn(); // back to player: cd 2 -> 1
        check(!b.canCast(P, bw, tgt), "still on cooldown after 1 turn");
        b.endTurn(); b.endTurn(); // back to player: cd 1 -> 0
        check(b.canCast(P, bw, tgt), "castable again after 2 turns");
    }

    // --- Glyph: repel on enter ---------------------------------------------
    std::printf("Glyph (repel on enter):\n");
    {
        Battle b = makeArena({1, 3}, {8, 3}, {spellid::Glyph});
        int gl = slotOf(b, P, spellid::Glyph);
        b.cast(P, gl, {6, 3});               // radius-3 zone centred at (6,3)
        check(!b.groundEffects().empty(), "glyph ground effect spawned");
        b.stepTo(E, {7, 3});                  // enemy steps inward onto the zone
        check(b.unit(E).pos == Vec2i{9, 3}, "enemy repelled outward to (9,3)");
    }

    // --- Shelter: temporary walls ------------------------------------------
    std::printf("Shelter (temporary walls + LOS block):\n");
    {
        Battle b = makeArena({1, 3}, {8, 3}, {spellid::Shelter});
        int sh = slotOf(b, P, spellid::Shelter);
        b.cast(P, sh, {3, 3});                // line of walls extending east
        check(b.wallTiles().size() == 5, "5 wall tiles created");
        check(!b.clearLineOfSight({1, 3}, {8, 3}), "LOS blocked across the wall");
        b.stepTo(P, {2, 3});
        check(!b.stepTo(P, {3, 3}), "cannot step onto a wall tile");
    }

    // --- Portal: traced entry -> exit, teleport on enter ---------------------
    std::printf("Portal (traced entry -> exit):\n");
    {
        Battle b = makeArena({1, 3}, {12, 1}, {spellid::Portal}); // enemy out of the way
        int po = slotOf(b, P, spellid::Portal);
        b.cast(P, po, {4, 3});                // entry (4,3); trace continues east 4 -> exit (8,3)
        b.stepTo(P, {2, 3});
        b.stepTo(P, {3, 3});
        b.stepTo(P, {4, 3});                  // walk onto the entry -> teleport
        check(b.unit(P).pos == Vec2i{8, 3}, "entering the entry teleports to the traced exit");
    }
    std::printf("Portal (spawn-time transport):\n");
    {
        Battle b = makeArena({1, 3}, {5, 3}, {spellid::Portal});
        int po = slotOf(b, P, spellid::Portal);
        b.cast(P, po, {5, 3});                // entry under the enemy; trace east -> exit (9,3)
        check(b.unit(E).pos == Vec2i{9, 3}, "unit standing on the entry is transported at cast");
    }
    std::printf("Portal (player-placed exit, off the trace, within reach):\n");
    {
        Battle b = makeArena({1, 3}, {5, 3}, {spellid::Portal});
        int po = slotOf(b, P, spellid::Portal);
        // Exit (5,6): off the eastward caster->entry ray but Manhattan 3 ≤ the
        // portal's reach (4). The enemy on the entry teleports there at cast.
        b.cast(P, po, {5, 3}, Vec2i{5, 6});
        check(b.unit(E).pos == Vec2i{5, 6}, "in-range explicit exit overrides the traced one");
    }
    std::printf("Portal (out-of-range exit falls back to the trace):\n");
    {
        Battle b = makeArena({1, 3}, {5, 3}, {spellid::Portal});
        int po = slotOf(b, P, spellid::Portal);
        // Exit (5,7)... use (2,6): Manhattan 6 > reach 4 → ignored; the eastward
        // trace exit (9,3) is used instead, transporting the enemy there.
        b.cast(P, po, {5, 3}, Vec2i{2, 6});
        check(b.unit(E).pos == Vec2i{9, 3}, "an exit beyond reach reverts to the traced exit");
    }
    std::printf("Portal (trace clamps at the arena edge):\n");
    {
        Battle b = makeArena({8, 3}, {1, 1}, {spellid::Portal});
        int po = slotOf(b, P, spellid::Portal);
        b.cast(P, po, {12, 3});               // trace wants (16,3) — grid is only 14 wide
        b.stepTo(P, {9, 3});
        b.stepTo(P, {10, 3});
        b.stepTo(P, {11, 3});
        b.stepTo(P, {12, 3});
        check(b.unit(P).pos == Vec2i{13, 3}, "exit clamps to the last walkable tile");
    }

    // --- Invisible: hidden from AI target acquisition -----------------------
    std::printf("Invisible (hidden from AI):\n");
    {
        Battle b = makeArena({4, 3}, {8, 3}, {spellid::Invisible});
        int iv = slotOf(b, P, spellid::Invisible);
        b.cast(P, iv, {4, 3});                // self-cast
        check(b.unit(P).invisible(), "caster gained Invisible status");
        check(!b.nearestFoe(E).has_value(), "enemy AI cannot acquire the invisible player");
    }

    // --- Rewind: snap back to position + HP + statuses after 2 turns --------
    std::printf("Rewind (delayed state restore):\n");
    {
        Battle b = makeArena({4, 3}, {8, 3}, {spellid::Rewind});
        int rw = slotOf(b, P, spellid::Rewind);
        b.unit(P).hp = 50;                                   // known starting HP (max 60)
        b.unit(P).statuses.push_back({StatusEffect::Kind::Shield, 30, 9}); // a status to restore
        b.cast(P, rw, {4, 3});                               // self-cast: snapshot {(4,3), 50, [shield]}
        check(b.unit(P).hasStatus(StatusEffect::Kind::Rewind), "rewind marker applied");

        b.stepTo(P, {5, 3});                                 // mutate state after the snapshot
        b.unit(P).hp = 20;
        b.unit(P).statuses.push_back({StatusEffect::Kind::DamageOverTime, 3, 9});

        b.endTurn(); b.endTurn();                            // back to P: turnsLeft 2 -> 1
        check(b.unit(P).pos == Vec2i{5, 3} && b.unit(P).hp == 20, "not yet rewound after 1 turn");

        b.endTurn(); b.endTurn();                            // back to P: turnsLeft 1 -> 0, restore
        check(b.unit(P).pos == Vec2i{4, 3}, "rewound to original position");
        check(b.unit(P).hp == 50, "rewound to original HP");
        check(b.unit(P).hasStatus(StatusEffect::Kind::Shield), "restored the pre-cast Shield");
        check(!b.unit(P).hasStatus(StatusEffect::Kind::DamageOverTime), "dropped the post-cast DoT");
        check(!b.unit(P).hasStatus(StatusEffect::Kind::Rewind), "rewind marker cleared after firing");
    }

    // --- Rewind fizzles on a dead target (no revive) ------------------------
    std::printf("Rewind (no revive):\n");
    {
        // Two player champions so killing one doesn't end the battle.
        std::vector<Entity> roster;
        roster.push_back(makeUnit("P1", Faction::Player, {4, 3}, {spellid::Rewind}));
        roster.push_back(makeUnit("P2", Faction::Player, {2, 3}, {spellid::Attack}));
        roster.push_back(makeUnit("E2", Faction::Enemy, {10, 3}, {spellid::Attack}));
        Battle b(Grid(14, 7), std::move(roster));
        int rw = slotOf(b, 0, spellid::Rewind);
        b.unit(0).hp = 40;
        b.cast(0, rw, {4, 3});                               // P1 self-rewinds (snapshot HP 40)
        b.unit(0).hp = 0;                                    // P1 dies before the rewind fires
        for (int i = 0; i < 6; ++i) b.endTurn();             // turns pass; dead P1 is skipped
        check(!b.unit(0).alive() && b.unit(0).hp == 0, "dead target stays dead — rewind does not revive");
    }

    std::printf("\n%s (%d failure%s)\n", g_fails == 0 ? "ALL PASS" : "FAILURES", g_fails,
                g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
