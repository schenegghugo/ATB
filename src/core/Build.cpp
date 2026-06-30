#include "Build.h"

#include <algorithm>
#include <sstream>
#include <unordered_set>

namespace tb {

namespace {
int statPointCost(const StatAllocation& s, const BuildRules& r) {
    return s.hpPurchases * r.hpCost + s.bonusAp * r.apCost + s.bonusMp * r.mpCost +
           s.bonusInitiative * r.initCost;
}
} // namespace

BuildValidation validateBuild(const CharacterBuild& build, const SpellCatalog& catalog,
                              const BuildRules& rules, const std::vector<std::string>& bannedSpells) {
    BuildValidation v;
    v.budget = rules.pointBudget;

    if (build.stats.hpPurchases < 0 || build.stats.bonusAp < 0 || build.stats.bonusMp < 0 ||
        build.stats.bonusInitiative < 0)
        v.errors.push_back("Stat allocations cannot be negative.");

    auto isBanned = [&](const std::string& key) {
        return std::find(bannedSpells.begin(), bannedSpells.end(), key) != bannedSpells.end();
    };

    std::unordered_set<int> seen;
    int spellPoints = 0;
    for (int id : build.spellIds) {
        if (!seen.insert(id).second) {
            v.errors.push_back("Duplicate spell id " + std::to_string(id) + ".");
            continue;
        }
        const SpellDef* def = catalog.find(id);
        if (!def) {
            v.errors.push_back("Unknown spell id " + std::to_string(id) + ".");
            continue;
        }
        if (isBanned(def->key))
            v.errors.push_back("Spell '" + def->key + "' is banned by the ruleset.");
        spellPoints += def->buildCost;
    }

    v.spent = spellPoints + statPointCost(build.stats, rules);
    if (v.spent > v.budget)
        v.errors.push_back("Over budget: spent " + std::to_string(v.spent) + " / " +
                           std::to_string(v.budget) + ".");

    v.ok = v.errors.empty();
    return v;
}

Entity instantiate(const CharacterBuild& build, const SpellCatalog& catalog, Faction team,
                   Vec2i pos, const BuildRules& rules) {
    Entity e;
    e.name = build.name;
    e.team = team;
    e.pos = pos;
    e.maxHp = e.hp = rules.baseHp + build.stats.hpPurchases * rules.hpStep;
    e.maxAp = e.ap = rules.baseAp + build.stats.bonusAp;
    e.maxMp = e.mp = rules.baseMp + build.stats.bonusMp;
    e.initiative = rules.baseInitiative + build.stats.bonusInitiative;

    for (int id : build.spellIds) {
        if (const SpellDef* def = catalog.find(id)) e.spells.push_back(def->spell);
    }
    return e;
}

// ---------------------------------------------------------------------------
// Persistence — a deliberately trivial line format. Every field here is a column
// the DB must store; FileBuildRepository writes one of these per character.
// ---------------------------------------------------------------------------
std::string serializeBuild(const CharacterBuild& b) {
    std::ostringstream os;
    os << "name=" << b.name << '\n';
    os << "hp=" << b.stats.hpPurchases << '\n';
    os << "ap=" << b.stats.bonusAp << '\n';
    os << "mp=" << b.stats.bonusMp << '\n';
    os << "init=" << b.stats.bonusInitiative << '\n';
    os << "spells=";
    for (std::size_t i = 0; i < b.spellIds.size(); ++i) {
        if (i) os << ',';
        os << b.spellIds[i];
    }
    os << '\n';
    return os.str();
}

std::optional<CharacterBuild> deserializeBuild(const std::string& text) {
    CharacterBuild b;
    bool sawName = false;
    std::istringstream is(text);
    std::string line;
    while (std::getline(is, line)) {
        if (line.empty()) continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string val = line.substr(eq + 1);

        if (key == "name") { b.name = val; sawName = true; }
        else if (key == "hp") b.stats.hpPurchases = std::atoi(val.c_str());
        else if (key == "ap") b.stats.bonusAp = std::atoi(val.c_str());
        else if (key == "mp") b.stats.bonusMp = std::atoi(val.c_str());
        else if (key == "init") b.stats.bonusInitiative = std::atoi(val.c_str());
        else if (key == "spells") {
            std::stringstream ss(val);
            std::string tok;
            while (std::getline(ss, tok, ',')) {
                if (!tok.empty()) b.spellIds.push_back(std::atoi(tok.c_str()));
            }
        }
    }
    if (!sawName) return std::nullopt;
    return b;
}

} // namespace tb
