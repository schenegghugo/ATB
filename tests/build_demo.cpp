//
// build_demo.cpp — Exercises the classless build system end-to-end:
//   catalog (dictionary) -> point-buy build -> validation -> persistence
//   round-trip -> instantiate a runtime Entity.
//
// Demonstrates the database seam without a DB dependency (FileBuildRepository
// stands in for SqliteBuildRepository against data/schema.sql).
//
#include "core/Build.h"
#include "core/Spells.h"
#include "data/BuildRepository.h"

#include <cstdio>
#include <filesystem>

using namespace tb;

namespace {

void printValidation(const char* label, const BuildValidation& v) {
    std::printf("  %-22s -> %s  (spent %d / %d)\n", label, v.ok ? "VALID" : "INVALID", v.spent,
                v.budget);
    for (const std::string& e : v.errors) std::printf("      - %s\n", e.c_str());
}

} // namespace

int main() {
    SpellCatalog catalog = makeDefaultCatalog();
    BuildRules rules{};

    std::printf("Skill dictionary (%zu entries):\n", catalog.all().size());
    for (const SpellDef& d : catalog.all()) {
        std::printf("  #%d %-10s cost %d | %d AP, range %d-%d, %zu effect(s)\n", d.id, d.key.c_str(),
                    d.buildCost, d.spell.apCost, d.spell.minRange, d.spell.maxRange,
                    d.spell.effects.size());
    }

    std::printf("\nValidation:\n");

    CharacterBuild pyro;
    pyro.name = "Pyromancer";
    pyro.stats.bonusAp = 1;
    pyro.stats.bonusInitiative = 2; // +2 initiative @ 1 pt each
    pyro.spellIds = {spellid::Attack, spellid::Fireball, spellid::Poison};
    printValidation("Pyromancer", validateBuild(pyro, catalog, rules));

    CharacterBuild greedy; // intentionally over budget
    greedy.name = "Overbudget";
    greedy.spellIds = {spellid::Attack, spellid::Fireball, spellid::Poison, spellid::Harpoon,
                       spellid::Knockback, spellid::Bulwark, spellid::Mend};
    printValidation("Overbudget (too many)", validateBuild(greedy, catalog, rules));

    CharacterBuild bogus;
    bogus.name = "Bogus";
    bogus.spellIds = {spellid::Attack, 999}; // unknown id
    printValidation("Bogus (unknown spell)", validateBuild(bogus, catalog, rules));

    // Ban enforcement: pyro is valid normally, invalid when "fireball" is banned.
    const BuildValidation banned = validateBuild(pyro, catalog, rules, {"fireball"});
    printValidation("Pyromancer (fireball banned)", banned);
    const bool banWorks = validateBuild(pyro, catalog, rules).ok && !banned.ok;

    // --- Persistence round-trip ---------------------------------------------
    const std::string dir = (std::filesystem::temp_directory_path() / "tb_builds").string();
    FileBuildRepository repo(dir);
    repo.save(pyro);

    std::printf("\nPersisted '%s' to %s\n", pyro.name.c_str(), dir.c_str());
    auto loaded = repo.load("Pyromancer");
    if (!loaded) { std::printf("  FAILED to reload!\n"); return 1; }

    const bool same = loaded->name == pyro.name && loaded->spellIds == pyro.spellIds &&
                      loaded->stats.bonusAp == pyro.stats.bonusAp &&
                      loaded->stats.bonusInitiative == pyro.stats.bonusInitiative;
    std::printf("  Reloaded build round-trips identically: %s\n", same ? "yes" : "NO");

    // --- Instantiate ---------------------------------------------------------
    Entity e = instantiate(*loaded, catalog, Faction::Player, Vec2i{1, 7}, rules);
    std::printf("\nInstantiated '%s': HP %d, AP %d, MP %d, initiative %d, %zu spells\n",
                e.name.c_str(), e.maxHp, e.maxAp, e.maxMp, e.initiative, e.spells.size());
    const bool initOk = e.initiative == rules.baseInitiative + pyro.stats.bonusInitiative;
    std::printf("  Initiative buy applied (%d = base %d + bought %d): %s\n", e.initiative,
                rules.baseInitiative, pyro.stats.bonusInitiative, initOk ? "yes" : "NO");

    std::printf("  Ban enforcement (valid normally, rejected when banned): %s\n",
                banWorks ? "yes" : "NO");

    repo.remove("Pyromancer");
    return (same && initOk && banWorks) ? 0 : 1;
}
