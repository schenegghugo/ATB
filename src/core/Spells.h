#pragma once
//
// Spells.h — The skill *dictionary* (catalog).
//
// A SpellDef is one catalog entry: stable id + slug + build-point cost wrapped
// around the gameplay `Spell` data. The SpellCatalog is the in-memory mirror of
// the `spells` / `spell_effects` database tables (see data/schema.sql) — in
// production a repository hydrates it from the DB; `makeDefaultCatalog()` is the
// code-defined seed used for the POC and as migration data.
//
// Classless by design: characters are built by spending points from this
// dictionary, never by picking a class.
//
#include "Battle.h"

#include <optional>
#include <string>
#include <vector>

namespace tb {

// Stable catalog ids (== primary keys in the `spells` table).
namespace spellid {
inline constexpr int Attack = 1;
inline constexpr int Fireball = 2;
inline constexpr int Poison = 3;
inline constexpr int Knockback = 4;
inline constexpr int Harpoon = 5;
inline constexpr int Bulwark = 6;  // shield buff
inline constexpr int Mend = 7;     // heal
inline constexpr int Shelter = 8;  // temporary blocking walls
inline constexpr int Invisible = 9; // self-conceal from the AI
inline constexpr int Portal = 10;  // teleport-on-enter
inline constexpr int Glyph = 11;   // repel-on-enter trap zone
inline constexpr int Rewind = 12;  // restore a unit's earlier state after a delay
inline constexpr int Bomb = 13;    // lob an inert bomb that detonates after a fuse
inline constexpr int Blocker = 14; // summon a tanky puller
inline constexpr int Healer = 15;  // summon a healer
inline constexpr int Brute = 16;   // summon a bruiser-lite
} // namespace spellid

struct SpellDef {
    int id = 0;            // catalog primary key
    std::string key;       // stable slug, e.g. "fireball"
    int buildCost = 0;     // points to include this spell in a build
    Spell spell;           // the gameplay data handed to an Entity
};

class SpellCatalog {
public:
    void add(SpellDef def);

    [[nodiscard]] const SpellDef* find(int id) const;
    [[nodiscard]] const SpellDef* findByKey(const std::string& key) const;
    [[nodiscard]] const std::vector<SpellDef>& all() const { return defs_; }
    [[nodiscard]] bool contains(int id) const { return find(id) != nullptr; }

private:
    std::vector<SpellDef> defs_; // small, fixed dictionary — linear scan is fine
};

// The POC skill dictionary: a varied set exercising every Effect type
// (damage, AoE, push, pull, DoT, shield, heal).
[[nodiscard]] SpellCatalog makeDefaultCatalog();

} // namespace tb
