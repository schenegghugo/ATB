#pragma once
//
// SpellEnums.h — The single source of truth for enum <-> JSON string mappings.
//
// Every catalog/pack enum that crosses the JSON boundary has one table here.
// **To support a new core enum value, add one row to its table** — both
// toString() and fromString() pick it up, and the compile-time checks at the
// bottom of this file enforce that each table stays internally consistent.
//
// Adding string values is backward-compatible (old files still parse); only a
// structural change bumps the catalog `schema`.
//
#include "../core/Battle.h" // TargetShape, Effect::Type, StatusEffect::Kind, GroundKind

#include <cstddef>
#include <iterator>
#include <optional>
#include <string_view>
#include <utility>

namespace tb::enums {

template <class E>
using Row = std::pair<E, std::string_view>;

// --- The tables (lowercased from the core enums) ----------------------------

inline constexpr Row<TargetShape> kTargetShapes[] = {
    {TargetShape::Single, "single"},
    {TargetShape::Line, "line"},
    {TargetShape::Cross, "cross"},
    {TargetShape::Circle, "circle"},
};

inline constexpr Row<Effect::Type> kEffectTypes[] = {
    {Effect::Type::Damage, "damage"},
    {Effect::Type::Heal, "heal"},
    {Effect::Type::Push, "push"},
    {Effect::Type::Pull, "pull"},
    {Effect::Type::ApplyStatus, "applyStatus"},
    {Effect::Type::Spawn, "spawn"},
    {Effect::Type::Summon, "summon"},
};

inline constexpr Row<StatusEffect::Kind> kStatusKinds[] = {
    {StatusEffect::Kind::DamageOverTime, "damageOverTime"},
    {StatusEffect::Kind::Shield, "shield"},
    {StatusEffect::Kind::ApBuff, "apBuff"},
    {StatusEffect::Kind::MpBuff, "mpBuff"},
    {StatusEffect::Kind::Invisible, "invisible"},
    {StatusEffect::Kind::Rewind, "rewind"},
};

inline constexpr Row<GroundKind> kGroundKinds[] = {
    {GroundKind::Wall, "wall"},
    {GroundKind::Glyph, "glyph"},
    {GroundKind::Portal, "portal"},
};

inline constexpr Row<EntityKind> kEntityKinds[] = {
    {EntityKind::Champion, "champion"},
    {EntityKind::Summon, "summon"},
    {EntityKind::Object, "object"},
};

// --- Generic lookups over a table -------------------------------------------

// enum -> string; empty string_view if unmapped (the checks below forbid that).
template <class E, std::size_t N>
[[nodiscard]] constexpr std::string_view toString(const Row<E> (&table)[N], E value) {
    for (std::size_t i = 0; i < N; ++i)
        if (table[i].first == value) return table[i].second;
    return {};
}

// string -> enum; nullopt if the string isn't in the table.
template <class E, std::size_t N>
[[nodiscard]] constexpr std::optional<E> fromString(const Row<E> (&table)[N], std::string_view s) {
    for (std::size_t i = 0; i < N; ++i)
        if (table[i].second == s) return table[i].first;
    return std::nullopt;
}

// --- Compile-time integrity: round-trips + no duplicate keys/values ---------

template <class E, std::size_t N>
[[nodiscard]] constexpr bool tableConsistent(const Row<E> (&table)[N]) {
    for (std::size_t i = 0; i < N; ++i) {
        if (table[i].second.empty()) return false;                 // every row named
        if (toString(table, table[i].first) != table[i].second) return false;
        const std::optional<E> back = fromString(table, table[i].second);
        if (!back || *back != table[i].first) return false;        // round-trips
        for (std::size_t j = i + 1; j < N; ++j)
            if (table[i].first == table[j].first || table[i].second == table[j].second)
                return false;                                       // no duplicates
    }
    return true;
}

static_assert(tableConsistent(kTargetShapes), "kTargetShapes inconsistent");
static_assert(tableConsistent(kEffectTypes), "kEffectTypes inconsistent");
static_assert(tableConsistent(kStatusKinds), "kStatusKinds inconsistent");
static_assert(tableConsistent(kGroundKinds), "kGroundKinds inconsistent");
static_assert(tableConsistent(kEntityKinds), "kEntityKinds inconsistent");

// Expected counts — bump these when a core enum gains a value, as a reminder to
// add the matching row above. (C++ can't enumerate enum values, so any *new*
// core value is also caught in practice by the catalog round-trip test once the
// default catalog uses it — serialization of an unmapped value is empty.)
static_assert(std::size(kTargetShapes) == 4, "TargetShape changed — update kTargetShapes");
static_assert(std::size(kEffectTypes) == 7, "Effect::Type changed — update kEffectTypes");
static_assert(std::size(kStatusKinds) == 6, "StatusEffect::Kind changed — update kStatusKinds");
static_assert(std::size(kGroundKinds) == 3, "GroundKind changed — update kGroundKinds");
static_assert(std::size(kEntityKinds) == 3, "EntityKind changed — update kEntityKinds");

} // namespace tb::enums
