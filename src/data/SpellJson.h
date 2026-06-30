#pragma once
//
// SpellJson.h — Shared Spell/Effect <-> JSON mapping.
//
// The catalog (a SpellDef = id/key/buildCost + Spell) and the bestiary (a
// creature's innate `spells` + `onDeath`) both serialize the same Spell/Effect
// shapes, so the mapping lives here once. Strict + contextual error reporting.
//
#include "../core/Combat.h" // Spell, Effect, StatusEffect
#include "Json.h"

#include <string>
#include <vector>

namespace tb::spelljson {

using Errors = std::vector<std::string>;

// Effect <-> JSON.
[[nodiscard]] Effect parseEffect(const json::Value& ev, const std::string& ctx, Errors& e);
[[nodiscard]] json::Value effectToJson(const Effect& fx);

// StatusEffect <-> JSON ({kind, magnitude, turns}). Used by spell effects and by
// creature `statuses` (e.g. a bomb's ignition).
void parseStatus(const json::Value& sv, const std::string& ctx, StatusEffect& out, Errors& e);
[[nodiscard]] json::Value statusToJson(const StatusEffect& s);

// A Spell's gameplay fields (name, apCost, ranges, LOS, shape, radius, cooldown,
// effects) <-> a JSON object. readSpellFields does NOT reject unknown keys — the
// caller owns the full allowed-key set (the catalog flattens these next to
// id/key/buildCost). Optional fields default to `out`'s current values, so pass a
// default-constructed Spell (or one with a preset name).
void readSpellFields(const json::Value& obj, const std::string& ctx, Spell& out, Errors& e);
void writeSpellFields(json::Value& obj, const Spell& sp);

} // namespace tb::spelljson
