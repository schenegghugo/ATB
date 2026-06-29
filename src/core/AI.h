#pragma once
//
// AI.h — Enemy decision logic (headless).
//
// A turn-level beam-search planner driving one enemy unit against the Battle
// API. It clones the Battle to simulate candidate action sequences within the
// AP/MP budget and scores the end-of-turn state (see AI.cpp). It addresses units
// by EntityId, so it is roster-agnostic.
//
// `enemyTakeOneAction` re-plans and executes a single step (movement one tile at
// a time, so the frontend can pace/animate on a timer); `runEnemyTurn` plans
// once and runs the whole sequence for headless / test use.
//
#include "Battle.h"

namespace tb {

enum class AIAction : std::uint8_t {
    Attacked, // spent AP on a cast
    Moved,    // spent 1 MP stepping toward the target
    Done      // nothing left to do this turn
};

// Performs at most one action for the given unit and returns what it did.
[[nodiscard]] AIAction enemyTakeOneAction(Battle& battle, EntityId self);

// Convenience: act for whichever unit currently holds the turn.
[[nodiscard]] inline AIAction enemyTakeOneAction(Battle& battle) {
    return enemyTakeOneAction(battle, battle.activeUnit());
}

// Runs the active unit's entire turn synchronously, then (optionally) ends it.
void runEnemyTurn(Battle& battle, bool autoEndTurn = true);

} // namespace tb
