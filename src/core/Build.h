#pragma once
//
// Build.h — Classless point-buy character builds.
//
// A CharacterBuild is the persisted definition of a character: a point budget
// spent across (a) skills chosen from the SpellCatalog and (b) stat upgrades.
// No classes — any mix is legal as long as it fits the budget. This struct maps
// directly to the `builds` + `build_spells` tables (see data/schema.sql).
//
// `instantiate()` turns a build + catalog into a runtime Entity for a Battle.
//
#include "Entity.h" // Entity, Faction (instantiate's output)
#include "Spells.h"

#include <optional>
#include <string>
#include <vector>

namespace tb {

// Economy / baseline knobs. In production these live in config or a `rulesets`
// table; here they are defaulted so a build is meaningful on its own.
struct BuildRules {
    int pointBudget = 20;

    int baseHp = 30;
    int baseAp = 4;
    int baseMp = 3;
    int baseInitiative = 5;

    // Stat upgrade economy (the creative tradeoff vs. buying more spells).
    int hpStep = 5;   // HP granted per purchase
    int hpCost = 3;   // points per +hpStep HP
    int apCost = 3;   // points per +1 AP
    int mpCost = 2;   // points per +1 MP
    int initCost = 1; // points per +1 initiative (act earlier)
};

// Point-bought stat upgrades on top of the baseline.
struct StatAllocation {
    int hpPurchases = 0; // each grants BuildRules::hpStep HP
    int bonusAp = 0;
    int bonusMp = 0;
    int bonusInitiative = 0; // each grants +1 initiative
};

struct CharacterBuild {
    std::string name;
    StatAllocation stats;
    std::vector<int> spellIds; // ids into the SpellCatalog
};

struct BuildValidation {
    bool ok = false;
    int spent = 0;
    int budget = 0;
    std::vector<std::string> errors;
};

// Verifies the build references real spells, has no duplicates / negatives, fits
// the point budget, and uses no banned spell (by catalog key — the ruleset's
// `bannedSpells`). Pure — safe to call from UI on every edit, and the server's
// build-admission check.
[[nodiscard]] BuildValidation validateBuild(const CharacterBuild& build, const SpellCatalog& catalog,
                                            const BuildRules& rules = {},
                                            const std::vector<std::string>& bannedSpells = {});

// Materialises a runtime Entity. Assumes the build is valid (call validateBuild
// first in UI); unknown spell ids are skipped defensively.
[[nodiscard]] Entity instantiate(const CharacterBuild& build, const SpellCatalog& catalog,
                                 Faction team, Vec2i pos, const BuildRules& rules = {});

// --- Persistence helpers (text round-trip; the FileBuildRepository uses these,
//     and they document exactly what a DB row must capture). -----------------
[[nodiscard]] std::string serializeBuild(const CharacterBuild& build);
[[nodiscard]] std::optional<CharacterBuild> deserializeBuild(const std::string& text);

} // namespace tb
