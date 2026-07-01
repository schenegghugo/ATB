#pragma once
//
// Event.h — the structured combat event stream (§2.3).
//
// As `Battle` resolves a turn it appends typed, ordered `BattleEvent`s: pure
// data, deterministic, no I/O. It never affects resolution — it's a read-only
// narration of what happened, in the order it happened. One stream feeds three
// consumers: the GUI combat log (2.3), animation event-clips (2.4), and — being
// an ordered, replayable record — replays (Phase 5) and PvP deltas (Phase 4).
//
// Entities are addressed by stable `EntityId` (units_ is append-only), so a
// consumer can always resolve names/spells via `Battle::unit(id)`, even after a
// unit has died.
//
#include "Combat.h" // DamageSource, StatusEffect
#include "Entity.h" // EntityId
#include "Grid.h"   // Vec2i

#include <cstdint>

namespace tb {

enum class EventType : std::uint8_t {
    TurnStart, // `actor` begins its turn
    Move,      // `actor` stepped to `to` (one tile; runs coalesce in the log)
    Cast,      // `actor` cast spell slot `spellSlot`
    Damage,    // `target` lost `amount` HP (`source` = why)
    Heal,      // `target` regained `amount` HP
    Status,    // `target` gained status `status` (magnitude `amount`)
    Death,     // `target` reached 0 HP (`source` = the lethal blow)
};

// A single narration record. Only the fields relevant to `type` are meaningful
// (documented per member); the rest keep their defaults.
struct BattleEvent {
    EventType type = EventType::TurnStart;
    EntityId actor = 0;   // TurnStart / Move / Cast subject
    EntityId target = 0;  // Damage / Heal / Status / Death recipient
    int amount = 0;       // Damage/Heal magnitude; Status magnitude
    int spellSlot = -1;   // Cast: the caster's loadout slot
    DamageSource source = DamageSource::Spell;         // Damage / Death
    StatusEffect::Kind status = StatusEffect::Kind::DamageOverTime; // Status
    Vec2i to{0, 0};       // Move: destination tile
};

} // namespace tb
