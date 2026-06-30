#pragma once
//
// Storm.h — Closing-ring ("storm") configuration.
//
// From `startRound`, the safe square around the arena centre shrinks one ring per
// round; units outside it take `damage` at their turn start, forcing lingering
// opponents into conflict. This is match *config* (not engine state): it's
// embedded in the Ruleset and handed to the Battle constructor.
//
namespace tb {

struct StormConfig {
    bool enabled = true;
    int startRound = 5;
    int damage = 8;
};

} // namespace tb
