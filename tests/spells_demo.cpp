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
        Battle b = makeArena({1, 3}, {6, 3}, {spellid::Attack, spellid::Fireball});
        int fb = slotOf(b, P, spellid::Fireball);
        const Vec2i tgt{6, 3}; // Manhattan 5, within Fireball's range 2-6
        check(b.canCast(P, fb, tgt), "fireball castable initially");
        b.cast(P, fb, tgt);
        check(!b.canCast(P, fb, tgt), "fireball blocked right after cast (cd 2)");
        b.endTurn(); b.endTurn(); // back to player: cd 2 -> 1
        check(!b.canCast(P, fb, tgt), "still on cooldown after 1 turn");
        b.endTurn(); b.endTurn(); // back to player: cd 1 -> 0
        check(b.canCast(P, fb, tgt), "castable again after 2 turns");
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

    // --- Portal: teleport on enter -----------------------------------------
    std::printf("Portal (teleport on enter):\n");
    {
        Battle b = makeArena({1, 3}, {12, 1}, {spellid::Portal}); // enemy out of the way
        int po = slotOf(b, P, spellid::Portal);
        b.cast(P, po, {8, 3});                // entry on (1,3), exit at (8,3); Manhattan 7 <= 8
        b.stepTo(P, {1, 2});                  // step off the entry
        b.stepTo(P, {1, 3});                  // step back on -> teleport
        check(b.unit(P).pos == Vec2i{8, 3}, "stepping onto entry teleports to exit");
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

    std::printf("\n%s (%d failure%s)\n", g_fails == 0 ? "ALL PASS" : "FAILURES", g_fails,
                g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
