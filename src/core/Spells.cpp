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
Effect spawnPortal(int duration) {
    return Effect{Effect::Type::Spawn, 0, {}, GroundSpec{GroundKind::Portal, duration, 0}};
}

SpellDef def(int id, std::string key, int cost, Spell spell) {
    spell.name = key;
    // Capitalise the display name from the slug.
    if (!spell.name.empty()) spell.name[0] = static_cast<char>(std::toupper(spell.name[0]));
    return SpellDef{id, std::move(key), cost, std::move(spell)};
}

} // namespace

// Spell fields: name, apCost, minRange, maxRange, needsLOS, shape, radius, cooldown, effects
SpellCatalog makeDefaultCatalog() {
    SpellCatalog c;

    // Attack — the reliable single-target staple, castable every turn.
    c.add(def(spellid::Attack, "attack", 1,
              Spell{"", 3, 1, 6, true, TargetShape::Single, 0, 0, {damage(15)}}));

    // Fireball — AoE burst; hits everything in a radius-1 diamond (friendly fire!).
    // Repriced 4 -> 3 pt after balance sims showed it underperforming its cost.
    c.add(def(spellid::Fireball, "fireball", 3,
              Spell{"", 4, 2, 6, true, TargetShape::Circle, 1, 2, {damage(14)}}));

    // Poison — light hit now, damage-over-time later.
    c.add(def(spellid::Poison, "poison", 3,
              Spell{"", 3, 1, 5, true, TargetShape::Single, 0, 2, {damage(4), dot(7, 3)}}));

    // Knockback — short-range shove with collision damage potential.
    c.add(def(spellid::Knockback, "knockback", 2,
              Spell{"", 2, 1, 2, true, TargetShape::Single, 0, 1, {damage(6), push(2)}}));

    // Harpoon — yank a target toward you, softening it up.
    c.add(def(spellid::Harpoon, "harpoon", 2,
              Spell{"", 3, 2, 6, true, TargetShape::Single, 0, 1, {pull(3), damage(4)}}));

    // Bulwark — self/ally shield (no LOS needed, range includes self).
    c.add(def(spellid::Bulwark, "bulwark", 2,
              Spell{"", 2, 0, 2, false, TargetShape::Single, 0, 3, {shield(20, 2)}}));

    // Mend — self/ally heal.
    c.add(def(spellid::Mend, "mend", 2,
              Spell{"", 2, 0, 3, false, TargetShape::Single, 0, 2, {heal(18)}}));

    // Shelter — conjure a line of 5 temporary walls (block movement + LOS).
    c.add(def(spellid::Shelter, "shelter", 3,
              Spell{"", 3, 1, 5, true, TargetShape::Line, 4, 4, {spawnWall(3)}}));

    // Invisible — conceal the caster from enemy AI for two turns (self, no LOS).
    c.add(def(spellid::Invisible, "invisible", 3,
              Spell{"", 2, 0, 0, false, TargetShape::Single, 0, 5, {invisibility(2)}}));

    // Portal — place an entry on the caster and an exit at the target; stepping
    // onto the entry teleports the unit to the exit.
    c.add(def(spellid::Portal, "portal", 3,
              Spell{"", 3, 2, 8, false, TargetShape::Single, 0, 4, {spawnPortal(3)}}));

    // Glyph — lay a radius-3 trap zone; anyone entering is repelled 2 tiles.
    c.add(def(spellid::Glyph, "glyph", 3,
              Spell{"", 3, 1, 5, true, TargetShape::Circle, 3, 3, {spawnGlyph(3, 2)}}));

    return c;
}

} // namespace tb
