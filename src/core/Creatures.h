#pragma once
//
// Creatures.h — The bestiary: prototypes a Summon effect can spawn.
//
// Mirrors the spell catalog: code-defined seed data today (makeDefaultCreatures),
// the migration target for a future data/creatures.json loaded by the same JSON +
// enum + validation machinery as the spell catalog. Each prototype is an Entity
// template keyed by its `name`; Battle::spawnCreature copies it, sets team + pos,
// and inserts it into the live roster.
//
#include "Battle.h"

#include <vector>

namespace tb {

// The POC bestiary. Currently: "bomb" — an inert Object that ticks ignition
// damage and detonates (radius-1 blast) on its 2nd turn or when destroyed.
[[nodiscard]] std::vector<Entity> makeDefaultCreatures();

} // namespace tb
