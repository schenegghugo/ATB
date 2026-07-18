//
// spells_demo.cpp — Deterministic checks for cooldowns + the four ground/status
// spells (Shelter, Glyph, Portal, Invisible). The combat AI never casts utility
// spells, so these mechanics are validated by scripting the Battle API directly
// on a fixed open arena.
//
#include "core/AI.h"
#include "core/Battle.h"
#include "core/Creatures.h"
#include "core/Spells.h"

#include <algorithm>
#include <cstdio>
#include <optional>
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

    // --- Shelter: wheel-rotated wall heading (90° steps) --------------------
    std::printf("Shelter (rotated footprint):\n");
    {
        auto hasWall = [](const Battle& b, Vec2i t) {
            const std::vector<Vec2i>& w = b.wallTiles();
            return std::find(w.begin(), w.end(), t) != w.end();
        };
        // Caster west of the target: rotation 0 runs the wall EAST from (3,3).
        Battle b0 = makeArena({1, 3}, {8, 5}, {spellid::Shelter});
        int s0 = slotOf(b0, P, spellid::Shelter);
        b0.cast(P, s0, {3, 3});
        check(hasWall(b0, {5, 3}) && !hasWall(b0, {3, 5}), "rotation 0: wall runs east");

        // One wheel step (rotation 1 = 90° CW) turns the same aim to run SOUTH.
        Battle b1 = makeArena({1, 3}, {8, 5}, {spellid::Shelter});
        int s1 = slotOf(b1, P, spellid::Shelter);
        b1.cast(P, s1, {3, 3}, std::nullopt, /*rotation=*/1);
        check(hasWall(b1, {3, 5}) && !hasWall(b1, {5, 3}), "rotation 1: wall runs south");

        // Rotation wraps mod 4: 5 ≡ 1, so the footprint is identical to rotation 1.
        Battle b5 = makeArena({1, 3}, {8, 5}, {spellid::Shelter});
        int s5 = slotOf(b5, P, spellid::Shelter);
        b5.cast(P, s5, {3, 3}, std::nullopt, /*rotation=*/5);
        check(hasWall(b5, {3, 5}) && !hasWall(b5, {5, 3}), "rotation wraps mod 4 (5 == 1)");
    }

    // --- Elemental surfaces: passive behaviour (E.2) -----------------------
    std::printf("Elemental surfaces (passive on-enter / on-tick):\n");
    {
        // A bare paint spell: spawn an elemental surface on one tile (no LOS need).
        auto paint = [](Element el) {
            Spell s;
            s.name = "paint";
            s.apCost = 1;
            s.minRange = 1;
            s.maxRange = 6;
            s.needsLineOfSight = false;
            s.shape = TargetShape::Single;
            s.effects.push_back(
                Effect{Effect::Type::Spawn, 0, {}, GroundSpec{GroundKind::Glyph, 5, 0, el}});
            return s;
        };
        auto has = [](const Battle& b, EntityId who, StatusEffect::Kind k) {
            for (const StatusEffect& s : b.unit(who).statuses)
                if (s.kind == k) return true;
            return false;
        };

        { // Fire: stepping onto it ignites (Burning)
            Battle b = makeArena({1, 3}, {12, 1}, {spellid::Attack});
            b.unit(P).spells.push_back(paint(Element::Fire));
            const int fp = static_cast<int>(b.unit(P).spells.size()) - 1;
            b.cast(P, fp, {3, 3});
            b.stepTo(P, {2, 3});
            b.stepTo(P, {3, 3}); // enter the fire
            check(has(b, P, StatusEffect::Kind::Burning), "walking into Fire applies Burning");
        }
        { // Wet (from Water) blocks Burning on a later Fire tile
            Battle b = makeArena({1, 3}, {12, 1}, {spellid::Attack});
            b.unit(P).spells.push_back(paint(Element::Water));
            b.unit(P).spells.push_back(paint(Element::Fire));
            const int wp = static_cast<int>(b.unit(P).spells.size()) - 2;
            const int fp = static_cast<int>(b.unit(P).spells.size()) - 1;
            b.cast(P, wp, {3, 3});
            b.cast(P, fp, {4, 3});
            b.stepTo(P, {2, 3});
            b.stepTo(P, {3, 3}); // Wet
            b.stepTo(P, {4, 3}); // Fire, but Wet
            check(has(b, P, StatusEffect::Kind::Wet), "Water soaks the unit (Wet)");
            check(!has(b, P, StatusEffect::Kind::Burning), "Wet blocks Burning on Fire");
        }
        { // Electric: shock + stun, and the next turn is skipped
            Battle b = makeArena({1, 3}, {12, 1}, {spellid::Attack});
            b.unit(P).spells.push_back(paint(Element::Electric));
            const int ep = static_cast<int>(b.unit(P).spells.size()) - 1;
            const int hp0 = b.unit(P).hp;
            b.cast(P, ep, {3, 3});
            b.stepTo(P, {2, 3});
            b.stepTo(P, {3, 3}); // enter the electric field
            check(b.unit(P).hp < hp0, "Electric shocks on enter");
            check(has(b, P, StatusEffect::Kind::Stunned), "Electric stuns on enter");
            b.endTurn();          // P -> E
            b.endTurn();          // E -> P: stun consumes the turn
            check(b.unit(P).ap == 0 && b.unit(P).mp == 0, "Stunned turn is skipped (no AP/MP)");
        }
        { // Frozen roots: a frozen unit cannot voluntarily step
            Battle b = makeArena({1, 3}, {12, 1}, {spellid::Attack});
            b.unit(P).statuses.push_back(StatusEffect{StatusEffect::Kind::Frozen, 0, 2, 0});
            check(!b.stepTo(P, {2, 3}), "Frozen unit cannot move");
        }
        { // Heal pool mends a hurt unit that stands in it
            Battle b = makeArena({1, 3}, {12, 1}, {spellid::Attack});
            b.unit(P).spells.push_back(paint(Element::Heal));
            const int hp2 = b.unit(P).hp = 20;
            const int slot = static_cast<int>(b.unit(P).spells.size()) - 1;
            b.cast(P, slot, {3, 3});
            b.stepTo(P, {2, 3});
            b.stepTo(P, {3, 3}); // step into the pool
            check(b.unit(P).hp > hp2, "Heal pool restores HP on enter");
        }
    }

    // --- Elemental surfaces: reaction engine (E.3) -------------------------
    std::printf("Elemental surfaces (reaction matrix):\n");
    {
        auto paint = [](Element el) {
            Spell s;
            s.name = "paint";
            s.apCost = 1;
            s.minRange = 1;
            s.maxRange = 8;
            s.needsLineOfSight = false;
            s.shape = TargetShape::Single;
            s.effects.push_back(
                Effect{Effect::Type::Spawn, 0, {}, GroundSpec{GroundKind::Glyph, 5, 0, el}});
            return s;
        };
        auto surfaceAt = [](const Battle& b, Vec2i t) {
            for (const GroundEffect& g : b.groundEffects())
                if (g.kind == GroundKind::Glyph && g.element != Element::None)
                    for (Vec2i x : g.tiles)
                        if (x == t) return g.element;
            return Element::None;
        };
        auto has = [](const Battle& b, EntityId who, StatusEffect::Kind k) {
            for (const StatusEffect& s : b.unit(who).statuses)
                if (s.kind == k) return true;
            return false;
        };
        // Caster P at (1,3) paints onto (5,3); a victim V sits on (5,3).
        auto arena = [&](std::vector<Element> paints) {
            Battle b = makeArena({1, 3}, {5, 3}, {spellid::Attack});
            for (Element el : paints) b.unit(P).spells.push_back(paint(el));
            return b;
        };
        auto castPaints = [&](Battle& b, std::vector<Element> paints, Vec2i at) {
            for (int i = 0; i < static_cast<int>(paints.size()); ++i) b.cast(P, 1 + i, at);
        };

        { // Water + Fire -> Steam (blocks LOS)
            Battle b = arena({Element::Water, Element::Fire});
            castPaints(b, {Element::Water, Element::Fire}, {8, 3}); // empty tile, no victim
            check(surfaceAt(b, {8, 3}) == Element::Steam, "Water + Fire -> Steam");
            check(!b.clearLineOfSight({7, 3}, {9, 3}), "Steam blocks line of sight");
        }
        { // Fire + Water -> doused (no surface)
            Battle b = arena({Element::Fire, Element::Water});
            castPaints(b, {Element::Fire, Element::Water}, {8, 3});
            check(surfaceAt(b, {8, 3}) == Element::None, "Fire + Water -> doused (bare)");
        }
        { // Ice on Fire -> Water (melt)
            Battle b = arena({Element::Fire, Element::Ice});
            castPaints(b, {Element::Fire, Element::Ice}, {8, 3});
            check(surfaceAt(b, {8, 3}) == Element::Water, "Fire + Ice -> Water (melt)");
        }
        { // Water + Electric -> Electric, shocking + stunning the unit on it
            Battle b = arena({Element::Water, Element::Electric});
            const int hp0 = b.unit(E).hp;
            castPaints(b, {Element::Water, Element::Electric}, {5, 3}); // V = E on (5,3)
            check(surfaceAt(b, {5, 3}) == Element::Electric, "Water + Electric -> Electric field");
            check(b.unit(E).hp < hp0, "electrified water shocks the unit on it");
            check(has(b, E, StatusEffect::Kind::Stunned), "electrified water stuns the unit on it");
        }
        { // Water + Ice -> Ice, freezing the unit on it
            Battle b = arena({Element::Water, Element::Ice});
            castPaints(b, {Element::Water, Element::Ice}, {5, 3});
            check(surfaceAt(b, {5, 3}) == Element::Ice, "Water + Ice -> Ice");
            check(has(b, E, StatusEffect::Kind::Frozen), "freezing water roots the unit on it");
        }
        { // Poison + Fire -> explosion (surface consumed, r1 fire burst)
            Battle b = arena({Element::Poison, Element::Fire});
            const int hp0 = b.unit(E).hp;
            castPaints(b, {Element::Poison, Element::Fire}, {5, 3});
            check(surfaceAt(b, {5, 3}) == Element::None, "Poison + Fire -> explosion consumes surface");
            check(b.unit(E).hp <= hp0 - 15, "explosion deals r1 fire damage");
        }
    }

    // --- Elemental surfaces: PaintSurface effect + Cone shape (E.4) --------
    std::printf("Elemental surfaces (PaintSurface effect + Cone):\n");
    {
        auto surfaceAt = [](const Battle& b, Vec2i t) {
            for (const GroundEffect& g : b.groundEffects())
                if (g.kind == GroundKind::Glyph && g.element != Element::None)
                    for (Vec2i x : g.tiles)
                        if (x == t) return g.element;
            return Element::None;
        };
        { // A PaintSurface effect lays an elemental surface across its zone.
            Battle b = makeArena({1, 3}, {12, 1}, {spellid::Attack});
            Spell s;
            s.name = "ignite";
            s.apCost = 1;
            s.minRange = 1;
            s.maxRange = 8;
            s.needsLineOfSight = false;
            s.shape = TargetShape::Single;
            s.effects.push_back(Effect{});
            s.effects[0].type = Effect::Type::PaintSurface;
            s.effects[0].amount = 3; // duration
            s.effects[0].element = Element::Fire;
            b.unit(P).spells.push_back(s);
            const int slot = static_cast<int>(b.unit(P).spells.size()) - 1;
            b.cast(P, slot, {4, 3});
            check(surfaceAt(b, {4, 3}) == Element::Fire, "PaintSurface(Fire) creates a Fire surface");
        }
        { // A Cone fans out from the caster along its facing (rotatable).
            Battle b = makeArena({3, 3}, {12, 1}, {spellid::Attack});
            Spell s;
            s.name = "blizzard";
            s.apCost = 1;
            s.minRange = 1;
            s.maxRange = 8;
            s.needsLineOfSight = false;
            s.shape = TargetShape::Cone;
            s.radius = 3;
            s.effects.push_back(Effect{});
            s.effects[0].type = Effect::Type::PaintSurface;
            s.effects[0].amount = 3;
            s.effects[0].element = Element::Ice;
            b.unit(P).spells.push_back(s);
            const int slot = static_cast<int>(b.unit(P).spells.size()) - 1;
            b.cast(P, slot, {6, 3}); // aim east → cone points +x
            // Apex row (k=1) is one tile ahead and 1 wide; the far row (k=3) is 5 wide.
            check(surfaceAt(b, {4, 3}) == Element::Ice, "Cone covers the tile ahead of the caster");
            check(surfaceAt(b, {6, 3}) == Element::Ice, "Cone reaches its full length");
            check(surfaceAt(b, {6, 1}) == Element::Ice && surfaceAt(b, {6, 5}) == Element::Ice,
                  "Cone widens with distance");
            check(surfaceAt(b, {4, 1}) == Element::None, "Cone is narrow at the apex");
        }
    }

    // --- Storm + Blizzard: the shipped elemental spells (E.5) ---------------
    std::printf("Storm (rain -> lightning on the wet zone):\n");
    {
        auto surfaceAt = [](const Battle& b, Vec2i t) {
            for (const GroundEffect& g : b.groundEffects())
                if (g.kind == GroundKind::Glyph && g.element != Element::None)
                    for (Vec2i x : g.tiles)
                        if (x == t) return g.element;
            return Element::None;
        };
        Battle b = makeArena({1, 3}, {6, 3}, {spellid::Storm});
        b.setCreatures(makeDefaultCreatures()); // Storm summons the "stormcloud"
        const int st = slotOf(b, P, spellid::Storm);
        const int hp0 = b.unit(E).hp;
        b.cast(P, st, {5, 3});                       // cloud lands on the empty tile by the enemy
        check(surfaceAt(b, {6, 3}) == Element::Water, "Storm rains a Water surface over the zone");
        // Cycle turns until the fused stormcloud strikes (its onDeath lightning).
        for (int i = 0; i < 6 && surfaceAt(b, {6, 3}) == Element::Water; ++i) b.endTurn();
        check(surfaceAt(b, {6, 3}) == Element::Electric,
              "the bolt electrifies the wet zone (Water + Electric)");
        check(b.unit(E).hp < hp0, "the enemy caught in the storm takes damage");
    }
    std::printf("Blizzard (cone freeze):\n");
    {
        Battle b = makeArena({3, 3}, {5, 3}, {spellid::Blizzard});
        const int bz = slotOf(b, P, spellid::Blizzard);
        const int hp0 = b.unit(E).hp;
        b.cast(P, bz, {5, 3}); // cone points east, sweeping the enemy at (5,3)
        bool frozen = false;
        for (const StatusEffect& s : b.unit(E).statuses)
            if (s.kind == StatusEffect::Kind::Frozen) frozen = true;
        check(b.unit(E).hp < hp0, "Blizzard damages units in the cone");
        check(frozen, "Blizzard freezes (roots) units in the cone");
    }

    // --- Elemental determinism guard (verify-don't-host, E.8) --------------
    // Reactions are pure (no RNG), so the same cast sequence must reproduce the
    // exact board. This guards the re-sim contract against future regressions.
    std::printf("Elemental reactions are deterministic:\n");
    {
        auto fingerprint = [](Battle& b) {
            std::string s;
            for (EntityId i = 0; i < b.unitCount(); ++i) {
                const Entity& u = b.unit(i);
                s += std::to_string(u.pos.x) + "," + std::to_string(u.pos.y) + ":" +
                     std::to_string(u.hp) + ";";
            }
            for (const GroundEffect& g : b.groundEffects())
                for (Vec2i t : g.tiles)
                    s += "g" + std::to_string(static_cast<int>(g.element)) + "@" +
                         std::to_string(t.x) + "," + std::to_string(t.y) + ";";
            return s;
        };
        auto run = [&]() {
            Battle b = makeArena({1, 3}, {6, 3}, {spellid::Storm, spellid::Blizzard});
            b.setCreatures(makeDefaultCreatures());
            b.cast(P, slotOf(b, P, spellid::Storm), {5, 3});
            b.cast(P, slotOf(b, P, spellid::Blizzard), {3, 3});
            for (int i = 0; i < 6; ++i) b.endTurn();
            return fingerprint(b);
        };
        check(run() == run(), "identical elemental cast sequences reproduce the exact board");
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

    // --- Bomb rides a portal: spawned onto the entry ------------------------
    std::printf("Bomb through portal (summoned onto the entry):\n");
    {
        Battle b = makeArena({1, 3}, {1, 6}, {spellid::Portal, spellid::Bomb});
        b.setCreatures(makeDefaultCreatures()); // register the "bomb" prototype
        b.cast(P, slotOf(b, P, spellid::Portal), {4, 3}); // entry (4,3), traced exit (8,3)
        b.cast(P, slotOf(b, P, spellid::Bomb), {4, 3});   // summon a bomb ONTO the entry
        const EntityId bomb = 2;                          // spawned after P(0), E(1)
        check(b.unit(bomb).name == "bomb", "the bomb spawned");
        check(b.unit(bomb).pos == Vec2i{8, 3},
              "a bomb summoned onto the portal entry is teleported to the exit");
    }

    // --- Bomb rides a portal: shoved onto the entry -------------------------
    std::printf("Bomb through portal (displaced onto the entry):\n");
    {
        Battle b = makeArena({2, 3}, {1, 6}, {spellid::Portal, spellid::Bomb, spellid::Knockback});
        b.setCreatures(makeDefaultCreatures()); // register the "bomb" prototype
        b.cast(P, slotOf(b, P, spellid::Portal), {8, 3}); // entry (8,3), traced exit (12,3)
        b.cast(P, slotOf(b, P, spellid::Bomb), {4, 3});   // bomb lands off the portal, at (4,3)
        const EntityId bomb = 2;
        check(b.unit(bomb).pos == Vec2i{4, 3}, "bomb starts off the portal");
        // Knockback shoves it 4 east: (4,3) -> lands on the entry (8,3) -> rides through.
        b.cast(P, slotOf(b, P, spellid::Knockback), {4, 3});
        check(b.unit(bomb).pos == Vec2i{12, 3},
              "a bomb shoved onto the portal entry is teleported to the exit");
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

    // --- Ground lifetime is independent of spawned non-champions ------------
    // Regression: ground effects used to age on EVERY unit's turn, so a bomb or
    // summon (which joins the initiative order) silently accelerated the decay of
    // portals, glyphs and walls. They now age on champion turns only, so a portal
    // survives the same number of rounds no matter how many non-champions share
    // the field.
    std::printf("Ground lifetime unaffected by a spawned non-champion:\n");
    {
        // A freshly-cast portal (duration 3) must still be standing when its caster
        // comes back around one full initiative cycle later. The number of extra
        // non-champions in the order must NOT change that — the player-facing bug was
        // that a bomb/summon burned the portal away before the caster could reuse it.
        auto portalUpNextPlayerTurn = [](Battle& b) {
            int guard = 0;
            do { b.endTurn(); } while (b.activeUnit() != P && guard++ < 500);
            for (const GroundEffect& g : b.groundEffects())
                if (g.kind == GroundKind::Portal) return true;
            return false;
        };

        // Baseline: a plain 1v1 (two champions).
        Battle base = makeArena({1, 3}, {12, 3}, {spellid::Portal});
        base.cast(P, slotOf(base, P, spellid::Portal), {3, 3});
        const bool baseUp = portalUpNextPlayerTurn(base);

        // Same, but with an extra non-champion (an inert Object) parked in the
        // initiative order, far from the champions and with no fuse so it persists.
        std::vector<Entity> roster;
        roster.push_back(makeUnit("P", Faction::Player, {1, 3}, {spellid::Portal}));
        roster.push_back(makeUnit("E", Faction::Enemy, {12, 3}, {spellid::Attack}));
        Entity obj;
        obj.name = "dummy";
        obj.kind = EntityKind::Object;
        obj.team = Faction::Player;
        obj.pos = {6, 6};
        obj.maxHp = obj.hp = 999; // long-lived, no fuse/ignition → stays in the order
        obj.initiative = 1;       // acts after the champions
        roster.push_back(std::move(obj));
        Battle withObj(Grid(14, 7), std::move(roster));
        withObj.cast(P, slotOf(withObj, P, spellid::Portal), {3, 3});
        const bool objUp = portalUpNextPlayerTurn(withObj);

        check(baseUp, "baseline portal survives to the caster's next turn");
        check(objUp == baseUp,
              "a non-champion in initiative does not shorten the portal's lifetime");
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
