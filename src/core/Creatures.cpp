#include "Creatures.h"

namespace tb {

namespace {

// "bomb" — an inert Object. Ignition (a self-DoT) chips its HP each turn, and a
// 2-turn fuse detonates it regardless; reaching 0 HP (ignition, an attack, or a
// shove into a wall) detonates it early. onDeath is a radius-1, 20-damage blast
// (friendly fire included). Being an entity, it's pushable / pullable / rewindable
// for free.
Entity makeBomb() {
    Entity e;
    e.name = "bomb";
    e.kind = EntityKind::Object;
    e.maxHp = e.hp = 12;
    e.maxAp = e.ap = 0;
    e.maxMp = e.mp = 0;
    e.initiative = 1; // acts late in the order
    e.fuse = 2;       // detonates on its 2nd turn
    e.statuses.push_back({StatusEffect::Kind::DamageOverTime, 4, 99}); // ignition chip
    e.onDeath = Spell{"detonation", 0, 0, 0, false, TargetShape::Circle, 1, 0,
                      {Effect{Effect::Type::Damage, 20, {}, {}, {}}}};
    return e;
}

// Shared scaffolding for the summon archetypes: a Summon with one innate spell,
// fewer stats than a champion.
Entity makeSummon(std::string name, int hp, int initiative, Spell ability) {
    Entity e;
    e.name = std::move(name);
    e.kind = EntityKind::Summon;
    e.maxHp = e.hp = hp;
    e.maxAp = e.ap = ability.apCost; // exactly enough to use its ability once
    e.maxMp = e.mp = 2;
    e.initiative = initiative;
    e.spells.push_back(std::move(ability));
    return e;
}

// "blocker" — a tanky body. Its one trick: a self-centred Cross pull (the 4
// cardinal lines, range 4) that yanks every foe on those lines toward it (they
// stop adjacent and take collision damage). A wall of a creature that drags the
// enemy into the open.
Entity makeBlocker() {
    Spell drag{"Drag", 3, 0, 0, false, TargetShape::Cross, 4, 1,
               {Effect{Effect::Type::Pull, 2, {}, {}, {}}}};
    return makeSummon("blocker", 45, 4, std::move(drag));
}

// "healer" — restores the most-wounded ally (self or friendly) within range 4.
Entity makeHealer() {
    Spell heal{"Mend", 3, 0, 4, false, TargetShape::Single, 0, 1,
               {Effect{Effect::Type::Heal, 12, {}, {}, {}}}};
    return makeSummon("healer", 20, 6, std::move(heal));
}

// "brute" — a bruiser-lite that strikes the nearest foe.
Entity makeBrute() {
    Spell strike{"Strike", 3, 1, 4, true, TargetShape::Single, 0, 0,
                 {Effect{Effect::Type::Damage, 10, {}, {}, {}}}};
    return makeSummon("brute", 30, 5, std::move(strike));
}

} // namespace

std::vector<Entity> makeDefaultCreatures() {
    return {makeBomb(), makeBlocker(), makeHealer(), makeBrute()};
}

} // namespace tb
