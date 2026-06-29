#pragma once
//
// Battle.h — Headless combat state machine (roster edition).
//
// Owns the grid + a roster of entities addressed by stable EntityId, plus the
// initiative order, turn bookkeeping, and the data-driven spell/effect pipeline.
// No rendering, no input — a frontend (Raylib) or a headless harness drives it
// identically through this API.
//
// Roster invariant: units_ is append-only. Entities are never erased, so a raw
// index *is* a permanent, stable EntityId. (When summons land, swap the index
// for a free-list behind this same typedef — no public API churn.)
//
#include "Grid.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tb {

using EntityId = std::uint16_t;

enum class Faction : std::uint8_t { Player, Enemy };

[[nodiscard]] constexpr Faction opposing(Faction f) {
    return f == Faction::Player ? Faction::Enemy : Faction::Player;
}

// What dealt a lethal blow — lets analysis attribute how matches end.
enum class DamageSource : std::uint8_t { Spell, Storm, Collision };

// --- Status effects ---------------------------------------------------------
// Carried per-entity, ticked at the owner's turn start. Buffs feed the AP/MP
// reset; DamageOverTime applies on tick; Shield absorbs incoming damage.
struct StatusEffect {
    enum class Kind : std::uint8_t { DamageOverTime, Shield, ApBuff, MpBuff, Invisible };
    Kind kind = Kind::DamageOverTime;
    int magnitude = 0;       // dmg/turn, absorb pool, or +AP/+MP (unused for Invisible)
    int remainingTurns = 0;  // decremented after each of the owner's turns
};

// --- Ground effects (persistent battlefield features) -----------------------
// Spawned by spells onto tiles, with a turn duration. Walls block movement/LOS;
// Glyphs repel anyone entering; Portals teleport anyone entering to an exit.
enum class GroundKind : std::uint8_t { Wall, Glyph, Portal };

// Authoring payload carried by an Effect of type Spawn.
struct GroundSpec {
    GroundKind kind = GroundKind::Wall;
    int duration = 2;   // turns the feature persists
    int magnitude = 0;  // Glyph: repel distance
};

// --- Spell / effect data ----------------------------------------------------
struct Effect {
    enum class Type : std::uint8_t { Damage, Heal, Push, Pull, ApplyStatus, Spawn };
    Type type = Type::Damage;
    int amount = 0;            // damage / heal / forced-move distance
    StatusEffect status{};     // used when type == ApplyStatus
    GroundSpec ground{};       // used when type == Spawn
};

enum class TargetShape : std::uint8_t { Single, Line, Cross, Circle };

struct Spell {
    std::string name;
    int apCost = 0;
    int minRange = 1;
    int maxRange = 1;          // Manhattan range
    bool needsLineOfSight = true;
    TargetShape shape = TargetShape::Single;
    int radius = 0;            // for Cross / Circle / Line length
    int cooldown = 0;          // turns before the caster may reuse it (0 = every turn)
    std::vector<Effect> effects;
};

// Spell construction lives in the catalog (Spells.h) — Battle is pure mechanics.

// --- Entity -----------------------------------------------------------------
struct Entity {
    std::string name;
    Faction team = Faction::Player;
    Vec2i pos;
    int hp = 0, maxHp = 0;
    int ap = 0, maxAp = 0; // action points (spells)
    int mp = 0, maxMp = 0; // movement points (tiles)
    int initiative = 0;    // higher acts earlier
    std::vector<StatusEffect> statuses;
    std::vector<Spell> spells;
    std::vector<int> spellCooldowns; // remaining turns per spell slot (parallel to spells)

    [[nodiscard]] bool alive() const { return hp > 0; }
    [[nodiscard]] bool hasStatus(StatusEffect::Kind k) const {
        for (const StatusEffect& s : statuses)
            if (s.kind == k) return true;
        return false;
    }
    [[nodiscard]] bool invisible() const { return hasStatus(StatusEffect::Kind::Invisible); }
};

// A live ground feature on the battlefield (see GroundKind).
struct GroundEffect {
    GroundKind kind = GroundKind::Wall;
    Faction owner = Faction::Player;
    std::vector<Vec2i> tiles;   // footprint
    int remainingTurns = 0;
    int magnitude = 0;          // Glyph repel distance
    Vec2i center;               // Glyph repel origin
    Vec2i exit;                 // Portal destination
};

enum class Phase : std::uint8_t { PlayerTurn, EnemyTurn, Finished };

// Closing-ring ("storm"): from `startRound`, the safe square around the arena
// centre shrinks one ring per round; units outside it take `damage` at their
// turn start. Forces lingering opponents into conflict.
struct StormConfig {
    bool enabled = true;
    int startRound = 5;
    int damage = 8;
};

// ---------------------------------------------------------------------------
class Battle {
public:
    explicit Battle(Grid grid, std::vector<Entity> units, StormConfig storm = {});

    // --- State accessors -----------------------------------------------------
    [[nodiscard]] const Grid& grid() const { return grid_; }
    [[nodiscard]] const std::vector<Entity>& units() const { return units_; }
    [[nodiscard]] std::size_t unitCount() const { return units_.size(); }

    [[nodiscard]] Entity& unit(EntityId id) { return units_[id]; }
    [[nodiscard]] const Entity& unit(EntityId id) const { return units_[id]; }

    [[nodiscard]] EntityId activeUnit() const { return order_[turnIdx_]; }
    [[nodiscard]] bool controlledByPlayer(EntityId id) const {
        return units_[id].team == Faction::Player;
    }
    [[nodiscard]] Phase phase() const;
    [[nodiscard]] std::optional<Faction> winner() const;

    // --- Closing ring --------------------------------------------------------
    [[nodiscard]] int round() const { return round_; }
    [[nodiscard]] int stormDamage() const { return storm_.damage; }
    [[nodiscard]] Vec2i stormCenter() const { return stormCenter_; }
    [[nodiscard]] int safeRadius() const;          // Chebyshev radius of the safe square
    [[nodiscard]] bool inStorm(Vec2i tile) const;  // tile currently outside the safe square
    [[nodiscard]] DamageSource lastDeathSource() const { return lastDeathSource_; } // valid once finished

    // Stable lookups shared by gameplay, AI and rendering.
    [[nodiscard]] std::optional<EntityId> unitAt(Vec2i tile) const;
    [[nodiscard]] std::optional<EntityId> nearestFoe(EntityId of) const;
    [[nodiscard]] std::vector<Vec2i> occupancy(std::optional<EntityId> exclude = std::nullopt) const;

    // --- Ground effects ------------------------------------------------------
    [[nodiscard]] const std::vector<GroundEffect>& groundEffects() const { return ground_; }
    // Tiles that block movement (units + terrain are handled separately): the
    // footprints of Wall ground effects. Feed into pathfinding `blocked` lists.
    [[nodiscard]] std::vector<Vec2i> wallTiles() const;
    // Combined movement blockers for pathing: other units + Wall ground tiles.
    [[nodiscard]] std::vector<Vec2i> pathBlockers(EntityId mover) const;
    // LOS that also accounts for temporary Shelter walls.
    [[nodiscard]] bool clearLineOfSight(Vec2i a, Vec2i b) const;

    // --- Turn lifecycle ------------------------------------------------------
    void endTurn(); // advance initiative to the next living unit & start its turn

    // --- Movement (return tiles/steps actually taken; never mutate on failure)
    int moveToward(EntityId who, Vec2i dest);
    bool stepTo(EntityId who, Vec2i adjacent);

    // --- Spellcasting --------------------------------------------------------
    // Geometry validity only: AP, Manhattan range, and LOS to the target tile.
    [[nodiscard]] bool canCast(EntityId caster, int spellIdx, Vec2i target) const;

    // Resolves the spell: spends AP, computes the affected tiles, applies every
    // effect to each unit in the zone (friendly fire included). Returns false if
    // the cast is illegal.
    bool cast(EntityId caster, int spellIdx, Vec2i target);

    // --- Targeting helpers (pure; reused by AI preview + renderer) -----------
    [[nodiscard]] std::vector<Vec2i> affectedTiles(const Spell& spell, Vec2i casterPos,
                                                   Vec2i target) const;
    [[nodiscard]] std::vector<EntityId> unitsAt(const std::vector<Vec2i>& tiles) const;

    // --- Forced movement (out-of-turn; hook for push/pull spells) ------------
    // Slides `who` `distance` tiles along `dir`, stopping at wall/obstacle/edge/
    // unit; each blocked remaining tile becomes collision damage.
    void applyForcedMove(EntityId who, Vec2i dir, int distance);

private:
    void startTurnFor(EntityId id); // tick statuses + cooldowns, reset AP/MP (+buffs)
    // honours Shield, records the source of any lethal blow, checks victory
    void applyDamage(EntityId id, int amount, DamageSource src = DamageSource::Spell);
    void checkVictory();
    void spawnGround(const GroundSpec& spec, Faction owner, Vec2i casterPos, Vec2i target,
                     const std::vector<Vec2i>& zone);
    void tickGround();              // age ground effects, drop the expired
    // Fire any ground effect on the unit's current tile (repel / teleport).
    // Only voluntary steps trigger; the resulting forced move / teleport does
    // not re-trigger, so chains can't loop.
    void onEnterTile(EntityId who);

    Grid grid_;
    std::vector<Entity> units_;
    std::vector<EntityId> order_; // initiative order (indices into units_)
    std::vector<GroundEffect> ground_;
    std::size_t turnIdx_ = 0;
    bool finished_ = false;

    StormConfig storm_;
    Vec2i stormCenter_;
    int stormMaxRadius_ = 0;
    int round_ = 0;
    DamageSource lastDeathSource_ = DamageSource::Spell;
};

} // namespace tb
