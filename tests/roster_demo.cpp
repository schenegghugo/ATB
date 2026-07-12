//
// roster_demo.cpp — Foundation for mid-battle roster entities (summons & bombs):
// entity roles, control classification, mid-battle spawn + initiative insertion,
// death-triggered effects (detonation), and victory-by-Champion. Non-zero exit
// on failure.
//
#include "core/AI.h"
#include "core/Battle.h"
#include "core/Creatures.h"
#include "core/Spells.h"

#include <cstdio>
#include <utility>
#include <vector>

using namespace tb;

static int g_fails = 0;
static void check(bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) ++g_fails;
}

namespace {
SpellCatalog catalog = makeDefaultCatalog();
Spell attackSpell() { return catalog.find(spellid::Attack)->spell; }

Entity mk(std::string name, Faction team, EntityKind kind, Vec2i pos, int hp, int init,
          std::vector<Spell> spells = {}) {
    Entity e;
    e.name = std::move(name);
    e.team = team;
    e.kind = kind;
    e.pos = pos;
    e.maxHp = e.hp = hp;
    e.maxAp = e.ap = 30;
    e.maxMp = e.mp = 30;
    e.initiative = init;
    e.spells = std::move(spells);
    return e;
}
} // namespace

int main() {
    // --- Control classification --------------------------------------------
    std::printf("Control classification:\n");
    {
        std::vector<Entity> roster;
        roster.push_back(mk("P", Faction::Player, EntityKind::Champion, {1, 3}, 30, 10));
        roster.push_back(mk("E", Faction::Enemy, EntityKind::Champion, {6, 3}, 30, 5));
        roster.push_back(mk("S", Faction::Player, EntityKind::Summon, {1, 4}, 20, 6));
        roster.push_back(mk("B", Faction::Player, EntityKind::Object, {2, 4}, 10, 1));
        Battle b(Grid(14, 7), std::move(roster));
        check(b.controlOf(0) == Control::Player, "player champion -> Player");
        check(b.controlOf(1) == Control::AI, "enemy champion -> AI");
        check(b.controlOf(2) == Control::AI, "summon (either team) -> AI");
        check(b.controlOf(3) == Control::Inert, "object -> Inert");
    }

    // --- Mid-battle spawn + initiative insertion ---------------------------
    std::printf("spawnEntity (initiative insertion, active unit stable):\n");
    {
        std::vector<Entity> roster;
        roster.push_back(mk("P", Faction::Player, EntityKind::Champion, {1, 3}, 30, 10));
        roster.push_back(mk("E", Faction::Enemy, EntityKind::Champion, {6, 3}, 30, 5));
        Battle b(Grid(14, 7), std::move(roster));
        const EntityId active = b.activeUnit(); // P (initiative 10) leads

        const EntityId s = b.spawnEntity(mk("S", Faction::Player, EntityKind::Summon, {1, 4}, 20, 7));
        check(b.unitCount() == 3, "roster grew to 3");
        check(b.activeUnit() == active, "active unit unchanged by the spawn");

        // Order should now be P(10), S(7), E(5): after P ends, S acts next.
        b.endTurn();
        check(b.activeUnit() == s, "spawned summon takes its initiative slot (acts after P)");
    }

    // --- Death-triggered detonation (onDeath) ------------------------------
    std::printf("onDeath detonation (bomb AoE, friendly fire):\n");
    {
        std::vector<Entity> roster;
        roster.push_back(mk("P", Faction::Player, EntityKind::Champion, {1, 3}, 50, 10, {attackSpell()}));
        roster.push_back(mk("T", Faction::Enemy, EntityKind::Champion, {4, 3}, 60, 5));
        Battle b(Grid(14, 7), std::move(roster));

        Entity bomb = mk("Bomb", Faction::Player, EntityKind::Object, {3, 3}, 10, 1);
        bomb.onDeath = Spell{"detonate", 0, 0, 0, false, TargetShape::Circle, 2, 0,
                             {Effect{Effect::Type::Damage, 20, {}, {}}}};
        b.spawnEntity(std::move(bomb)); // id 2

        check(b.cast(0, 0, {3, 3}), "P attacks the bomb (15 dmg, bomb has 10 HP)");
        check(!b.unit(2).alive(), "bomb destroyed");
        check(b.unit(1).hp == 40, "enemy champion in blast took 20 (60 -> 40)");
        check(b.unit(0).hp == 30, "caster took 20 too — detonation is friendly fire (50 -> 30)");
        check(!b.winner().has_value(), "both champions alive -> match not decided");
    }

    // --- Victory counts only Champions -------------------------------------
    std::printf("Victory by Champion (summons don't keep a team alive):\n");
    {
        std::vector<Entity> roster;
        roster.push_back(mk("P", Faction::Player, EntityKind::Champion, {1, 3}, 30, 10));
        roster.push_back(mk("S", Faction::Player, EntityKind::Summon, {1, 4}, 40, 8));
        roster.push_back(mk("E", Faction::Enemy, EntityKind::Champion, {4, 3}, 30, 5, {attackSpell()}));
        Battle b(Grid(14, 7), std::move(roster));

        b.cast(2, 0, {1, 3}); // E attacks P (15) — casting doesn't require it to be E's turn
        b.cast(2, 0, {1, 3}); // again -> P (champion) dies
        check(!b.unit(0).alive(), "player champion is dead");
        check(b.unit(1).alive(), "player summon still alive");
        check(b.phase() == Phase::Finished, "match ends despite the surviving summon");
        check(b.winner() == Faction::Enemy, "enemy wins — player has no living Champion");
    }

    // --- Bomb via the catalog spell: summon -> fuse -> detonation -----------
    std::printf("Bomb spell (summon + 2-turn fuse + blast):\n");
    {
        std::vector<Entity> roster;
        roster.push_back(mk("P", Faction::Player, EntityKind::Champion, {1, 3}, 50, 10,
                            {catalog.find(spellid::Bomb)->spell}));
        roster.push_back(mk("V", Faction::Enemy, EntityKind::Champion, {4, 3}, 60, 5));
        Battle b(Grid(14, 7), std::move(roster));
        b.setCreatures(makeDefaultCreatures()); // register the "bomb" prototype

        check(b.cast(0, 0, {3, 3}), "cast Bomb at (3,3)");
        check(b.unitCount() == 3, "a bomb entity was spawned");
        check(b.unit(2).kind == EntityKind::Object && b.unit(2).pos == Vec2i{3, 3},
              "spawned an Object at the target tile");

        b.endTurn(); // -> V
        b.endTurn(); // -> bomb's 1st turn (ignition chips, fuse 2 -> 1)
        check(b.unit(2).alive() && b.unit(1).hp == 60, "after 1 turn: bomb armed, no blast yet");

        b.endTurn(); // -> P
        b.endTurn(); // -> V
        b.endTurn(); // -> bomb's 2nd turn: fuse -> 0, detonates
        check(!b.unit(2).alive(), "bomb detonated on its 2nd turn");
        check(b.unit(1).hp == 35, "adjacent enemy took the 25 blast (60 -> 35)");
    }

    // --- Summons: cap + AI behaviour ---------------------------------------
    std::printf("Summon cap (max 2 per team):\n");
    {
        std::vector<Entity> roster;
        roster.push_back(mk("P", Faction::Player, EntityKind::Champion, {1, 3}, 50, 10,
                            {catalog.find(spellid::Brute)->spell}));
        roster.push_back(mk("E", Faction::Enemy, EntityKind::Champion, {12, 3}, 50, 5));
        Battle b(Grid(14, 7), std::move(roster));
        b.setCreatures(makeDefaultCreatures());
        // Reset AP + cooldown between casts so only the cap (not the spell's own
        // cooldown) limits spawning.
        b.unit(0).ap = 30;
        b.cast(0, 0, {1, 4});
        b.unit(0).ap = 30;
        b.unit(0).spellCooldowns[0] = 0;
        b.cast(0, 0, {1, 5});
        b.unit(0).ap = 30;
        b.unit(0).spellCooldowns[0] = 0;
        b.cast(0, 0, {1, 6}); // third should be refused by the cap
        int summons = 0;
        for (std::size_t i = 0; i < b.unitCount(); ++i)
            if (b.unit(static_cast<EntityId>(i)).kind == EntityKind::Summon) ++summons;
        check(summons == 2, "no more than 2 living summons per team");
    }

    std::printf("Blocker: self-centred Cross pull (radius 1 — slams an adjacent foe):\n");
    {
        // P (champion) far west; a foe already adjacent (east) to where the blocker
        // sits. The short cross-pull can't drag it past the blocker's body, so it
        // stays put and eats the collision.
        std::vector<Entity> roster;
        roster.push_back(mk("P", Faction::Player, EntityKind::Champion, {1, 6}, 50, 10));
        roster.push_back(mk("Foe", Faction::Enemy, EntityKind::Champion, {6, 3}, 60, 5));
        Battle b(Grid(14, 7), std::move(roster));

        // Spawn the real blocker prototype (with its Drag ability) at (5,3).
        Entity blocker;
        for (const Entity& proto : makeDefaultCreatures())
            if (proto.name == "blocker") { blocker = proto; break; }
        blocker.team = Faction::Player;
        blocker.pos = {5, 3};
        const EntityId bid = b.spawnEntity(std::move(blocker));

        const int foeBefore = b.unit(1).hp;
        (void)enemyTakeOneAction(b, bid); // blocker acts -> self-casts Drag
        check(b.unit(1).pos == Vec2i{6, 3}, "adjacent foe pulled into the blocker, not past it");
        check(b.unit(1).hp < foeBefore, "foe took collision damage stopping against the blocker");
    }

    // --- AI treats bombs as hazards, not targets ---------------------------
    std::printf("AI ignores bombs as targets (no suicidal harpoon-pull):\n");
    {
        std::vector<Entity> roster;
        roster.push_back(mk("P", Faction::Player, EntityKind::Champion, {1, 1}, 50, 10)); // keeps match live
        roster.push_back(mk("E", Faction::Enemy, EntityKind::Champion, {5, 3}, 50, 5,
                            {catalog.find(spellid::Harpoon)->spell}));
        Battle b(Grid(14, 7), std::move(roster));
        Entity bomb;
        for (const Entity& proto : makeDefaultCreatures())
            if (proto.name == "bomb") { bomb = proto; break; }
        bomb.team = Faction::Player; // the player's bomb is a "foe" to the enemy AI
        bomb.pos = {8, 3};           // 3 tiles east of E — within Harpoon's 2-6 range
        const EntityId bid = b.spawnEntity(std::move(bomb));
        const Vec2i before = b.unit(bid).pos;
        for (int i = 0; i < 4; ++i) (void)enemyTakeOneAction(b, 1); // let E plan + act
        check(b.unit(bid).pos == before, "enemy AI never pulls the bomb toward itself");
    }

    std::printf("\n%s (%d failure%s)\n", g_fails == 0 ? "ALL PASS" : "FAILURES", g_fails,
                g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
