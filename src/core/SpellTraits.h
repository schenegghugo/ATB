#pragma once
//
// SpellTraits.h — tiny read-only spell/status inspection helpers.
//
// Shared vocabulary between the planner (AI.cpp) and the evaluator
// (Evaluator.cpp): "how much damage does this spell represent", "does it carry
// effect X / status kind K". Pure functions over the combat data model — no
// engine, no entities.
//
#include "Combat.h"

namespace tb {

// Nominal damage a single cast represents: direct hits plus the first tick-worth
// of any DamageOverTime it applies (a deliberate one-turn view, not the full DoT).
[[nodiscard]] inline int spellDamage(const Spell& s) {
    int d = 0;
    for (const Effect& fx : s.effects) {
        if (fx.type == Effect::Type::Damage) d += fx.amount;
        else if (fx.type == Effect::Type::ApplyStatus &&
                 fx.status.kind == StatusEffect::Kind::DamageOverTime)
            d += fx.status.magnitude;
    }
    return d;
}

// Worst-case collision damage a single cast's forced movement (Push/Pull) can
// convert into: the fully-blocked slam, amount × kCollisionDamagePerCell. For a
// pull this is realised whenever the victim starts adjacent — the caster's own
// body is the backstop; a push needs terrain behind the victim. Threat-side
// pricing only (Evaluator's expectedIncoming is a worst-case model by
// contract); the acting side needs no proxy — simulation deals the real thing.
[[nodiscard]] inline int spellForcedMoveThreat(const Spell& s) {
    int d = 0;
    for (const Effect& fx : s.effects)
        if (fx.type == Effect::Type::Push || fx.type == Effect::Type::Pull)
            d += fx.amount * kCollisionDamagePerCell;
    return d;
}

[[nodiscard]] inline bool hasEffect(const Spell& s, Effect::Type t) {
    for (const Effect& fx : s.effects)
        if (fx.type == t) return true;
    return false;
}

[[nodiscard]] inline bool hasStatusEffect(const Spell& s, StatusEffect::Kind k) {
    for (const Effect& fx : s.effects)
        if (fx.type == Effect::Type::ApplyStatus && fx.status.kind == k) return true;
    return false;
}

} // namespace tb
