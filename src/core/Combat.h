#pragma once
//
// Combat.h — The spell/effect data model (the engine's content vocabulary).
//
// Pure data: a Spell is AP/range/shape + a list of typed Effects. No engine, no
// entities, no arena. The catalog (Spells.h) and the JSON layer (data/) map onto
// these types; the Battle engine resolves them. This is the security boundary —
// content can only express what these types allow.
//
#include <cstdint>
#include <string>
#include <vector>

namespace tb {

// What dealt a lethal blow — lets analysis attribute how matches end.
enum class DamageSource : std::uint8_t { Spell, Storm, Collision };

// Forced movement (Push/Pull) converts every *blocked* remaining tile into this
// much damage (see Battle::applyForcedMove). Lives here rather than inside the
// engine so the evaluator can price displacement threats with the same number
// the engine deals.
constexpr int kCollisionDamagePerCell = 5;

// --- Status effects ---------------------------------------------------------
// Carried per-entity, ticked at the owner's turn start. Buffs feed the AP/MP
// reset; DamageOverTime applies on tick; Shield absorbs incoming damage;
// RangeDebuff shortens the owner's spell reach (magnitude = percent off
// maxRange, clamped to minRange — see Battle::canCast).
struct StatusEffect {
    enum class Kind : std::uint8_t {
        DamageOverTime, Shield, ApBuff, MpBuff, Invisible, Rewind, RangeDebuff
    };
    Kind kind = Kind::DamageOverTime;
    int magnitude = 0;       // dmg/turn, absorb pool, +AP/+MP, or % range reduction
    int remainingTurns = 0;  // decremented after each of the owner's turns
    int delay = 0;           // >0: inert for that many of the owner's turns, THEN
                             // activates with its full remainingTurns (delayed
                             // payloads, e.g. a buff whose crash lands later)
};

// --- Ground effects (persistent battlefield features) -----------------------
// Spawned by spells onto tiles, with a turn duration. Walls block movement/LOS;
// Glyphs repel anyone entering; Portals teleport anyone entering — or standing
// on the entry when the portal is cast — to an exit traced from the caster.
enum class GroundKind : std::uint8_t { Wall, Glyph, Portal };

// Authoring payload carried by an Effect of type Spawn.
struct GroundSpec {
    GroundKind kind = GroundKind::Wall;
    int duration = 2;   // turns the feature persists
    int magnitude = 0;  // Glyph: repel distance; Portal: trace length (the exit
                        // lands this far past the entry along the caster's aim)
};

// --- Spell / effect data ----------------------------------------------------
struct Effect {
    // Decoy: spawn an identical twin of the caster on the target tile and cloak
    // the pair — damage to either member defers until the pair is revealed
    // (casting from a member declares it the real one; expiry defaults to the
    // original). See Battle's cloak-pair machinery.
    enum class Type : std::uint8_t { Damage, Heal, Push, Pull, ApplyStatus, Spawn, Summon, Decoy };
    Type type = Type::Damage;
    int amount = 0;            // damage / heal / forced-move distance / decoy duration
    StatusEffect status{};     // used when type == ApplyStatus
    GroundSpec ground{};       // used when type == Spawn
    std::string creature{};    // creature template key, used when type == Summon
    bool polarized = false;    // ApplyStatus only: negate the magnitude vs foes
                               // (one spell = buff on allies, debuff on enemies)
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

} // namespace tb
