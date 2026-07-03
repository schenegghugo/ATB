#pragma once
//
// AI.h — Enemy decision logic (headless).
//
// A turn-level planner driving one champion against the Battle API. The default
// planner is a beam search that clones the Battle to simulate candidate action
// sequences within the AP/MP budget and scores the end-of-turn state (see
// AI.cpp). Planning is addressed by EntityId, so it is roster-agnostic.
//
// The planner sits behind a small `Brain` strategy interface so community AIs can
// drop in without forking AI.cpp (see Phase 3 in MILESTONES.md). The default beam
// search is `defaultBrain()`; callers pass a `const Brain&` (or omit it to get the
// default). Summons are deliberately *not* Brain-driven — they run a fixed,
// single-purpose behaviour (`summonTakeOneAction` in AI.cpp).
//
// `enemyTakeOneAction` re-plans and executes a single step (movement one tile at
// a time, so the frontend can pace/animate on a timer); `runEnemyTurn` plans
// once and runs the whole sequence for headless / test use.
//
#include "Battle.h"

#include <string_view>
#include <vector>

namespace tb {

enum class AIAction : std::uint8_t {
    Attacked, // spent AP on a cast
    Moved,    // spent 1 MP stepping toward the target
    Done      // nothing left to do this turn
};

// One step a Brain wants a champion to take this turn. A Move's `target` is the
// tile to advance toward (the caller chooses whether to step once or move the
// whole MP budget); a Cast names a spell `slot` + its `target` tile.
struct PlannedAction {
    enum class Kind { Cast, Move } kind = Kind::Cast;
    int slot = -1;
    Vec2i target{};
};

// A pluggable turn-planner for one champion. `planTurn` returns the ordered
// action sequence to take this turn; the caller decides execution granularity
// (whole-turn for headless/sim, one step at a time for the GUI). Implementations
// must be deterministic given the Battle state — replays and the balance gauntlet
// depend on it — and must not mutate the passed Battle (they clone it to look
// ahead).
class Brain {
public:
    virtual ~Brain() = default;
    [[nodiscard]] virtual std::vector<PlannedAction> planTurn(const Battle& battle,
                                                              EntityId self) const = 0;
    [[nodiscard]] virtual std::string_view name() const = 0;
};

// The built-in beam-search planner — today's AI, and the default when a caller
// doesn't supply a Brain. A stable process-wide singleton (stateless).
[[nodiscard]] const Brain& defaultBrain();

// Performs at most one action for the given unit and returns what it did.
[[nodiscard]] AIAction enemyTakeOneAction(Battle& battle, EntityId self, const Brain& brain);
[[nodiscard]] inline AIAction enemyTakeOneAction(Battle& battle, EntityId self) {
    return enemyTakeOneAction(battle, self, defaultBrain());
}

// Convenience: act for whichever unit currently holds the turn.
[[nodiscard]] inline AIAction enemyTakeOneAction(Battle& battle, const Brain& brain) {
    return enemyTakeOneAction(battle, battle.activeUnit(), brain);
}
[[nodiscard]] inline AIAction enemyTakeOneAction(Battle& battle) {
    return enemyTakeOneAction(battle, battle.activeUnit(), defaultBrain());
}

// Runs the active unit's entire turn synchronously, then (optionally) ends it.
void runEnemyTurn(Battle& battle, bool autoEndTurn, const Brain& brain);
inline void runEnemyTurn(Battle& battle, bool autoEndTurn = true) {
    runEnemyTurn(battle, autoEndTurn, defaultBrain());
}

} // namespace tb
