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
#include "Combat.h" // DamageSource, StatusEffect, GroundKind/Spec, Effect, Spell
#include "Entity.h" // EntityId, Faction, EntityKind, Control, Entity, EntitySnapshot
#include "Event.h"  // BattleEvent — the structured combat event stream
#include "Grid.h"
#include "Storm.h"  // StormConfig (match config, also embedded in the Ruleset)

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tb {

// A pending Rewind: when `turnsLeft` hits 0 at the target's turn start, restore
// `snap` (unless the target is dead by then — Rewind does not revive).
struct PendingRewind {
    EntityId target = 0;
    EntitySnapshot snap;
    int turnsLeft = 0;
};

// A cloaked decoy pair (Effect::Type::Decoy): entity `a` (the original caster)
// and `b` (its spawned twin) are publicly indistinguishable — nothing in shared
// state says which is real. Damage to either member DEFERS into its pending pool
// (HP doesn't move, nobody dies) until the pair reveals: casting from a member
// declares THAT member real (the choice rides in the ordinary intent stream, so
// replays/verification need no new format); if the duration expires unrevealed,
// the original `a` is real by rule. At reveal the decoy quietly vanishes and only
// the real member's pending damage lands — hits the opponent "wasted" on the
// decoy evaporate.
struct CloakPair {
    EntityId a = 0;    // the original caster (real by default at expiry)
    EntityId b = 0;    // the spawned twin
    int turnsLeft = 0; // ticked at a's turn start; 0 = auto-reveal (real = a)
    int pendingA = 0;  // deferred damage accumulated per member
    int pendingB = 0;
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
    Element element = Element::None; // elemental surface (None = neutral floor)
    bool blocksLos = false;     // Steam / clouds block line of sight like walls
};

enum class Phase : std::uint8_t { PlayerTurn, EnemyTurn, Finished };

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
        return controlOf(id) == Control::Player;
    }
    // Who decides this unit's turn: the local player drives only their own
    // Champions; Summons (either team) are AI; Objects are inert (auto-end).
    [[nodiscard]] Control controlOf(EntityId id) const {
        const Entity& e = units_[id];
        if (e.kind == EntityKind::Object) return Control::Inert;
        if (e.kind == EntityKind::Summon) return Control::AI;
        return e.team == Faction::Player ? Control::Player : Control::AI;
    }
    [[nodiscard]] Phase phase() const;
    [[nodiscard]] std::optional<Faction> winner() const;

    // --- Combat event stream (see Event.h) -----------------------------------
    // Ordered narration of everything that has happened, appended during
    // resolution. A consumer tracks its own read cursor (the vector only grows).
    [[nodiscard]] const std::vector<BattleEvent>& events() const { return events_; }
    // The AI clones the Battle to simulate candidate turns; it disables recording
    // on its throwaway copies so they neither grow nor copy a log (also clears
    // any inherited events). The real match keeps recording.
    void setEventRecording(bool on) {
        recordEvents_ = on;
        if (!on) events_.clear();
    }

    // Add an entity to the live roster mid-battle (summon/bomb). Inserts into the
    // initiative order by initiative (ties by EntityId) without shifting the unit
    // whose turn it currently is. Returns the new stable EntityId.
    EntityId spawnEntity(Entity e);

    // Register the prototypes a Summon effect can spawn (keyed by Entity::name).
    // Content lives outside core (makeDefaultCreatures / creatures.json); the
    // engine just spawns whatever it was given.
    void setCreatures(std::vector<Entity> prototypes) { creatures_ = std::move(prototypes); }

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

    // --- Cloaked decoy pairs (see CloakPair) ----------------------------------
    [[nodiscard]] const std::vector<CloakPair>& cloakPairs() const { return cloaks_; }
    [[nodiscard]] bool isCloaked(EntityId id) const { return pairIndexOf(id).has_value(); }

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
    // the cast is illegal. `portalExit`, when set, places a Portal spell's exit
    // explicitly (a walkable tile) instead of tracing it from the caster's aim —
    // ignored by every other spell. `rotation` turns a Line spell's heading in
    // 90° steps (Shelter walls; ignored by every other shape).
    bool cast(EntityId caster, int spellIdx, Vec2i target,
              std::optional<Vec2i> portalExit = std::nullopt, int rotation = 0);

    // --- Targeting helpers (pure; reused by AI preview + renderer) -----------
    // `rotation` rotates a Line shape's heading by that many 90° steps (0 = the
    // default caster→target ray); other shapes ignore it.
    [[nodiscard]] std::vector<Vec2i> affectedTiles(const Spell& spell, Vec2i casterPos,
                                                   Vec2i target, int rotation = 0) const;
    [[nodiscard]] std::vector<EntityId> unitsAt(const std::vector<Vec2i>& tiles) const;

    // --- Forced movement (out-of-turn; hook for push/pull spells) ------------
    // Slides `who` `distance` tiles along `dir`, stopping at wall/obstacle/edge/
    // unit; each blocked remaining tile becomes collision damage.
    void applyForcedMove(EntityId who, Vec2i dir, int distance);

private:
    void emit(const BattleEvent& ev) { if (recordEvents_) events_.push_back(ev); }
    void startTurnFor(EntityId id); // tick statuses + cooldowns, reset AP/MP (+buffs)
    // honours Shield, records the source of any lethal blow, checks victory
    void applyDamage(EntityId id, int amount, DamageSource src = DamageSource::Spell);
    void checkVictory();
    void spawnGround(const GroundSpec& spec, Faction owner, Vec2i casterPos, Vec2i target,
                     const std::vector<Vec2i>& zone, std::optional<Vec2i> portalExit = std::nullopt);
    // Resolve a spell's effects over its zone (shared by cast() and death-
    // triggered detonations). `casterPos` anchors directional/shape geometry.
    void applySpellEffects(const Spell& sp, Faction casterTeam, Vec2i casterPos, Vec2i target,
                           std::optional<Vec2i> portalExit = std::nullopt, int rotation = 0);
    // Spawn a registered creature prototype (by key) for `team` at `at`, if the
    // tile is free. No-op if the key is unknown.
    void spawnCreature(const std::string& key, Faction team, Vec2i at);
    // Decoy machinery (see CloakPair). spawnDecoy twins `caster` onto `at` (must
    // be free + walkable, else a silent no-op like Summon); revealPair resolves a
    // pair, keeping `realId` (applies its pending damage) and vanishing the other.
    void spawnDecoy(EntityId caster, Vec2i at, int duration);
    void revealPair(std::size_t pairIdx, EntityId realId);
    [[nodiscard]] std::optional<std::size_t> pairIndexOf(EntityId id) const;
    void tickGround();              // age ground effects, drop the expired
    // Fire any ground effect on the unit's current tile (repel / teleport).
    // Only voluntary steps trigger; the resulting forced move / teleport does
    // not re-trigger, so chains can't loop.
    void onEnterTile(EntityId who);
    // If `who` stands on a live portal's entry, send it to the exit (when free).
    // Shared by walk-in (onEnterTile), displacement (push/pull/repel), and spawning
    // — so a bomb dropped or shoved onto a portal rides it like a walking unit.
    // Returns true if it teleported. Does not chain into further ground effects.
    bool teleportIfOnPortal(EntityId who);
    // Tick this unit's pending Rewind (if any); restore the snapshot when it
    // elapses, or fizzle if the unit is dead. Called at its turn start.
    void rewindTick(EntityId id);

    // --- Elemental-surface passives (E.2, docs/elements.md §3) ---------------
    [[nodiscard]] static bool hasStatus(const Entity& e, StatusEffect::Kind k);
    // Add a status, or refresh an existing one of the same kind to the longer
    // duration / larger magnitude (keeps Wet/Burning/… from stacking unboundedly).
    void refreshStatus(EntityId who, StatusEffect::Kind k, int magnitude, int turns);
    void clearStatus(EntityId who, StatusEffect::Kind k);
    // Element `el`'s effect when a unit first steps onto the surface.
    void surfaceEnter(EntityId who, Element el);
    // Element `el`'s effect on a unit that begins its turn standing on the surface.
    void surfaceTick(EntityId who, Element el);
    // The elemental surface (if any) currently under `at`, else Element::None.
    [[nodiscard]] Element surfaceElementAt(Vec2i at) const;

    // --- Reaction engine (E.3, docs/elements.md §4) --------------------------
    // Paint `incoming` across `zone`, resolving the reaction matrix per tile in
    // zone order (deterministic; one reaction per tile per cast) and applying any
    // burst. Elemental surfaces are stored one-tile-per-GroundEffect so reactions
    // stay tile-local. Called by spawnGround for an elemental Glyph spawn.
    void paintSurface(Element incoming, int duration, const std::vector<Vec2i>& zone, Faction team);
    // Strip any elemental surface (element != None) covering tile `t`.
    void removeSurfaceTileAt(Vec2i t);

    Grid grid_;
    std::vector<Entity> units_;
    std::vector<EntityId> order_; // initiative order (indices into units_)
    std::vector<GroundEffect> ground_;
    std::vector<CloakPair> cloaks_;
    std::vector<PendingRewind> rewinds_;
    std::vector<Entity> creatures_; // spawnable prototypes (keyed by name)
    std::vector<BattleEvent> events_;
    bool recordEvents_ = true;
    std::size_t turnIdx_ = 0;
    bool finished_ = false;

    StormConfig storm_;
    Vec2i stormCenter_;
    int stormMaxRadius_ = 0;
    int round_ = 0;
    DamageSource lastDeathSource_ = DamageSource::Spell;
};

} // namespace tb
