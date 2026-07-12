// Cooldowns gate repeat-casting: a spell with cooldown N can't be recast by the
// same caster for N of its turns (see Battle: spellCooldowns / canCast).
#include "Spells.h"

#include <cctype>
#include <utility>

namespace tb {

void SpellCatalog::add(SpellDef def) { defs_.push_back(std::move(def)); }

const SpellDef* SpellCatalog::find(int id) const {
    for (const SpellDef& d : defs_)
        if (d.id == id) return &d;
    return nullptr;
}

const SpellDef* SpellCatalog::findByKey(const std::string& key) const {
    for (const SpellDef& d : defs_)
        if (d.key == key) return &d;
    return nullptr;
}

namespace {

// Small builders keep the catalog readable and mirror DB columns 1:1.
Effect damage(int amount) { return Effect{Effect::Type::Damage, amount, {}, {}}; }
Effect heal(int amount) { return Effect{Effect::Type::Heal, amount, {}, {}}; }
Effect push(int dist) { return Effect{Effect::Type::Push, dist, {}, {}}; }
Effect pull(int dist) { return Effect{Effect::Type::Pull, dist, {}, {}}; }
Effect dot(int perTurn, int turns) {
    return Effect{Effect::Type::ApplyStatus, 0,
                  StatusEffect{StatusEffect::Kind::DamageOverTime, perTurn, turns}, {}};
}
Effect shield(int pool, int turns) {
    return Effect{Effect::Type::ApplyStatus, 0,
                  StatusEffect{StatusEffect::Kind::Shield, pool, turns}, {}};
}
Effect invisibility(int turns) {
    return Effect{Effect::Type::ApplyStatus, 0,
                  StatusEffect{StatusEffect::Kind::Invisible, 0, turns}, {}};
}
Effect spawnWall(int duration) {
    return Effect{Effect::Type::Spawn, 0, {}, GroundSpec{GroundKind::Wall, duration, 0}};
}
Effect spawnGlyph(int duration, int repel) {
    return Effect{Effect::Type::Spawn, 0, {}, GroundSpec{GroundKind::Glyph, duration, repel}};
}
Effect spawnPortal(int duration, int trace) {
    return Effect{Effect::Type::Spawn, 0, {}, GroundSpec{GroundKind::Portal, duration, trace}};
}
Effect rewind(int turns) {
    return Effect{Effect::Type::ApplyStatus, 0,
                  StatusEffect{StatusEffect::Kind::Rewind, 0, turns}, {}, {}};
}
Effect summon(std::string creatureKey) {
    Effect e;
    e.type = Effect::Type::Summon;
    e.creature = std::move(creatureKey);
    return e;
}
Effect rangeDebuff(int percent, int turns) {
    return Effect{Effect::Type::ApplyStatus, 0,
                  StatusEffect{StatusEffect::Kind::RangeDebuff, percent, turns}, {}};
}
Effect apBuff(int amount, int turns, int delay = 0) {
    return Effect{Effect::Type::ApplyStatus, 0,
                  StatusEffect{StatusEffect::Kind::ApBuff, amount, turns, delay}, {}};
}
Effect mpFlux(int amount, int turns) { // polarized: +amount ally, -amount foe
    Effect e{Effect::Type::ApplyStatus, 0,
             StatusEffect{StatusEffect::Kind::MpBuff, amount, turns}, {}};
    e.polarized = true;
    return e;
}
Effect decoyFx(int duration) {
    Effect e;
    e.type = Effect::Type::Decoy;
    e.amount = duration;
    return e;
}

SpellDef def(int id, std::string key, int cost, Spell spell, std::vector<std::string> tags = {}) {
    spell.name = key;
    // Capitalise the display name from the slug.
    if (!spell.name.empty()) spell.name[0] = static_cast<char>(std::toupper(spell.name[0]));
    return SpellDef{id, std::move(key), cost, std::move(spell), std::move(tags)};
}

} // namespace

// Spell fields: name, apCost, minRange, maxRange, needsLOS, shape, radius, cooldown, effects
SpellCatalog makeDefaultCatalog() {
    SpellCatalog c;

    // Attack — the reliable single-target staple, castable every turn (short reach).
    c.add(def(spellid::Attack, "attack", 2,
              Spell{"", 3, 1, 3, true, TargetShape::Single, 0, 0, {damage(15)}},
              {"damage", "single"}));

    // Fireball — AoE burst; hits everything in a radius-1 diamond (friendly fire!).
    c.add(def(spellid::Fireball, "fireball", 2,
              Spell{"", 3, 3, 7, true, TargetShape::Circle, 1, 0, {damage(15)}},
              {"damage", "ranged", "aoe"}));

    // Poison — a hit now, damage-over-time later.
    c.add(def(spellid::Poison, "poison", 3,
              Spell{"", 3, 1, 5, true, TargetShape::Single, 0, 3, {damage(8), dot(7, 3)}},
              {"damage", "debuff", "dot", "single"}));

    // Knockback — short-range shove with collision damage potential.
    c.add(def(spellid::Knockback, "knockback", 2,
              Spell{"", 3, 1, 2, true, TargetShape::Single, 0, 1, {damage(4), push(4)}},
              {"damage", "debuff", "mobility", "melee", "single"}));

    // Harpoon — yank a target toward you, softening it up.
    c.add(def(spellid::Harpoon, "harpoon", 2,
              Spell{"", 3, 2, 6, true, TargetShape::Single, 0, 1, {pull(4), damage(4)}},
              {"damage", "debuff", "mobility", "ranged", "single"}));

    // Bulwark — self/ally shield (no LOS needed, range includes self).
    c.add(def(spellid::Bulwark, "bulwark", 2,
              Spell{"", 3, 0, 2, false, TargetShape::Single, 0, 2, {shield(10, 2)}},
              {"support", "buff"}));

    // Mend — self/ally heal.
    c.add(def(spellid::Mend, "mend", 3,
              Spell{"", 3, 0, 3, false, TargetShape::Single, 0, 3, {heal(15)}},
              {"support", "buff"}));

    // Shelter — conjure a line of 5 temporary walls (block movement + LOS).
    c.add(def(spellid::Shelter, "shelter", 3,
              Spell{"", 3, 1, 5, true, TargetShape::Line, 4, 4, {spawnWall(3)}},
              {"support", "terrain"}));

    // Invisible — conceal the caster from enemy AI for two turns (self, no LOS).
    c.add(def(spellid::Invisible, "invisible", 3,
              Spell{"", 3, 0, 0, false, TargetShape::Single, 0, 4, {invisibility(2)}},
              {"support", "buff", "mobility"}));

    // Portal — traced from the caster: the target tile is the ENTRY; the exit
    // lands 4 tiles further along the caster→entry ray (walls clamp the trace).
    // Whatever stands on the entry at cast — bomb, ally, enemy — is transported
    // immediately; walking onto the entry teleports for the portal's lifetime.
    c.add(def(spellid::Portal, "portal", 3,
              Spell{"", 3, 1, 8, false, TargetShape::Single, 0, 3, {spawnPortal(3, 4)}},
              {"support", "mobility", "terrain"}));

    // Glyph — lay a radius-3 trap zone; anyone entering is repelled 2 tiles.
    c.add(def(spellid::Glyph, "glyph", 3,
              Spell{"", 3, 1, 5, true, TargetShape::Circle, 3, 3, {spawnGlyph(3, 2)}},
              {"support", "terrain", "debuff"}));

    // Rewind — tag a unit (or self, range 0); after 2 of its turns it snaps back
    // to the position + HP + statuses + cooldowns it had when hit (fizzles if it
    // died first). High cooldown — a powerful escape/undo.
    c.add(def(spellid::Rewind, "rewind", 3,
              Spell{"", 3, 0, 6, true, TargetShape::Single, 0, 5, {rewind(2)}},
              {"support", "buff"}));

    // Bomb — lob an inert "bomb" creature onto a tile; it detonates on its 2nd
    // turn (or when destroyed) for a radius-1 blast. The bomb is an entity, so it
    // can be pushed / pulled / rewound. (Spawns the "bomb" creature template.)
    c.add(def(spellid::Bomb, "bomb", 3,
              Spell{"", 3, 1, 6, true, TargetShape::Single, 0, 3, {summon("bomb")}},
              {"summon"}));

    // Summons — place an AI-driven helper on a nearby tile (capped per team).
    c.add(def(spellid::Blocker, "blocker", 4,
              Spell{"", 4, 1, 4, true, TargetShape::Single, 0, 6, {summon("blocker")}},
              {"summon"}));
    c.add(def(spellid::Healer, "healer", 4,
              Spell{"", 4, 1, 4, true, TargetShape::Single, 0, 6, {summon("healer")}},
              {"summon", "support"}));
    c.add(def(spellid::Brute, "brute", 4,
              Spell{"", 4, 1, 4, true, TargetShape::Single, 0, 6, {summon("brute")}},
              {"summon"}));

    // Blind — cripple a caster's reach: -60% max cast range (never below the
    // spell's minRange) while the debuff holds. Ages at the victim's turn start,
    // so turns=3 restricts two of their full turns.
    c.add(def(spellid::Blind, "blind", 2,
              Spell{"", 2, 1, 6, true, TargetShape::Single, 0, 3, {rangeDebuff(60, 3)}},
              {"debuff", "ranged", "single"}));

    // Surge — overload an ally (or self): +2 AP for their next two turns, then
    // the crash lands: -6 AP for one turn (AP floors at 0).
    c.add(def(spellid::Surge, "surge", 2,
              Spell{"", 2, 0, 3, false, TargetShape::Single, 0, 5,
                    {apBuff(2, 2), apBuff(-6, 1, /*delay=*/2)}},
              {"support", "buff", "single"}));

    // Flux — polarized current: +2 MP to an ally, -2 MP to an enemy (one turn).
    c.add(def(spellid::Flux, "flux", 2,
              Spell{"", 2, 1, 5, true, TargetShape::Single, 0, 2, {mpFlux(2, 1)}},
              {"buff", "debuff", "single"}));

    // Decoy — conjure an identical twin on a nearby tile and cloak the pair:
    // damage to either defers until the reveal (acting from a member declares it
    // real; expiry defaults to the original). Perfect-information-safe stealth —
    // the ranked-legal replacement for Invisible (see MILESTONES CR.6).
    c.add(def(spellid::Decoy, "decoy", 3,
              Spell{"", 3, 1, 3, true, TargetShape::Single, 0, 5, {decoyFx(3)}},
              {"support", "mobility", "single"}));

    return c;
}

} // namespace tb
