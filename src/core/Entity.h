#pragma once
//
// Entity.h — A unit in the roster: identity, team/kind, stats, loadout, status.
//
// A roster member addressed by a stable EntityId. Depends on the combat data
// model (its spells / statuses) and the grid (its position) — but not on the
// Battle engine, so content/data code can use entities without pulling in the
// state machine.
//
#include "Combat.h" // Spell, StatusEffect
#include "Grid.h"   // Vec2i

#include <cstdint>
#include <string>
#include <vector>

namespace tb {

using EntityId = std::uint16_t;

enum class Faction : std::uint8_t { Player, Enemy };

[[nodiscard]] constexpr Faction opposing(Faction f) {
    return f == Faction::Player ? Faction::Enemy : Faction::Player;
}

// Role in the roster. Only Champions count for victory; Summons are AI-driven
// helpers; Objects (e.g. bombs) are inert and just tick/auto-end their turn.
enum class EntityKind : std::uint8_t { Champion, Summon, Object };

// Who decides a unit's turn: the local player, the AI planner, or nobody (an
// inert object whose turn simply passes).
enum class Control : std::uint8_t { Player, AI, Inert };

struct Entity {
    std::string name;
    Faction team = Faction::Player;
    EntityKind kind = EntityKind::Champion; // Champions decide victory (see checkVictory)
    Vec2i pos;
    int hp = 0, maxHp = 0;
    int ap = 0, maxAp = 0; // action points (spells)
    int mp = 0, maxMp = 0; // movement points (tiles)
    int initiative = 0;    // higher acts earlier
    std::vector<StatusEffect> statuses;
    std::vector<Spell> spells;
    std::vector<int> spellCooldowns; // remaining turns per spell slot (parallel to spells)
    Spell onDeath;                   // resolved at the entity's tile when it dies (empty = none)
    int fuse = 0;                    // >0: detonates (dies -> onDeath) when it counts down to 0

    [[nodiscard]] bool alive() const { return hp > 0; }
    [[nodiscard]] bool isChampion() const { return kind == EntityKind::Champion; }
    [[nodiscard]] bool hasStatus(StatusEffect::Kind k) const {
        for (const StatusEffect& s : statuses)
            if (s.kind == k) return true;
        return false;
    }
    [[nodiscard]] bool invisible() const { return hasStatus(StatusEffect::Kind::Invisible); }
};

// Snapshot of a unit's restorable state, captured by the Rewind spell.
struct EntitySnapshot {
    Vec2i pos;
    int hp = 0;
    std::vector<StatusEffect> statuses;
    std::vector<int> spellCooldowns;
};

} // namespace tb
