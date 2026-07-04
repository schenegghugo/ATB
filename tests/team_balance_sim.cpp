//
// team_balance_sim.cpp — team-composition balance harness for NvN.
//
// Sibling to balance_sim.cpp. It reuses the same organic random build generator,
// but instead of per-spell stats it classifies every build into an archetype
// (by its spell effects) and reports balance at the *team* level:
//
//   * per-archetype win rate  (how each role does, pooled across comps)
//   * per-composition win rate (which team make-ups win — e.g. Support+Summoner)
//   * a composition-vs-composition matchup matrix (does comp A beat comp B?)
//
// Archetype is assigned by a fixed precedence over the build's spell effects:
//   Summoner (2+ Summon spells) > Support (Heal/Shield) > Control (Push/Pull) >
//   Evasion (Invisible/Rewind) > Aggro (everything else — pure damage).
// One build = one role; the rarest niche present wins. The 2-summon threshold
// keeps a build that merely rolled one summon out of the Summoner bucket.
//
// Outputs (siblings of <outfile>, .txt stripped for the base):
//   <outfile>          plain-text report (also stdout)
//   <base>.html        charts: role bars, composition bars, matchup heatmap
//   <base>.roles.csv   <base>.comps.csv   <base>.matchups.csv (long format)
//
//   usage: tb_team_balance [matches] [seed] [outfile]
//          defaults: 4000  12345  output/team_report.txt
//   env:   ATB_DATA_DIR, ATB_MAP, ATB_TEAM (team size; default from rules.json,
//          but this tool is pointless at 1 — use >= 2)
//
#include "core/AI.h"
#include "core/Battle.h"
#include "core/Build.h"
#include "core/Creatures.h"
#include "core/Grid.h"
#include "core/Match.h"
#include "core/Ruleset.h"
#include "core/Spells.h"
#include "data/CatalogJson.h"
#include "data/CreatureJson.h"
#include "data/MapJson.h"
#include "data/RulesetJson.h"

#include <optional>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace tb;

namespace {

constexpr int kHalfTurnCap = 200; // higher than 1v1: NvN games run longer
constexpr long kMinN = 30;        // below this, a cell/row is too small to judge

bool isOffensive(int id) {
    return id == spellid::Attack || id == spellid::Fireball || id == spellid::Poison ||
           id == spellid::Knockback || id == spellid::Harpoon;
}

// ---- Build generation (identical policy to balance_sim.cpp) -----------------
struct Build {
    CharacterBuild def;
    int role = 0;
};

CharacterBuild randomBuild(std::mt19937& rng, const SpellCatalog& catalog, const BuildRules& rules,
                           const std::vector<std::string>& banned) {
    CharacterBuild b;
    b.name = "rnd";
    std::vector<int> ids;
    for (const SpellDef& d : catalog.all())
        if (std::find(banned.begin(), banned.end(), d.key) == banned.end()) ids.push_back(d.id);
    std::shuffle(ids.begin(), ids.end(), rng);
    for (int id : ids) {
        b.spellIds.push_back(id);
        if (!validateBuild(b, catalog, rules, banned).ok) b.spellIds.pop_back();
    }
    if (!std::any_of(b.spellIds.begin(), b.spellIds.end(), isOffensive)) {
        b.spellIds.clear();
        b.spellIds.push_back(spellid::Attack);
    }
    std::uniform_int_distribution<int> pick(0, 3);
    for (int guard = 0; guard < 24; ++guard) {
        StatAllocation trial = b.stats;
        switch (pick(rng)) {
            case 0: ++trial.hpPurchases; break;
            case 1: ++trial.bonusAp; break;
            case 2: ++trial.bonusMp; break;
            default: ++trial.bonusInitiative; break;
        }
        CharacterBuild probe = b;
        probe.stats = trial;
        if (validateBuild(probe, catalog, rules, banned).ok) b.stats = trial;
    }
    return b;
}

// ---- Archetype classification ----------------------------------------------
enum Role { Aggro, Control, Summoner, Support, Evasion, RoleCount };
constexpr const char* kRoleName[RoleCount] = {"Aggro", "Control", "Summoner", "Support", "Evasion"};
constexpr const char* kRoleDesc[RoleCount] = {
    "pure damage, no specialty", "displacement (push/pull)", "2+ creature summons",
    "heals / shields allies", "invisibility / rewind"};

bool spellHas(const Spell& sp, Effect::Type t) {
    for (const Effect& fx : sp.effects)
        if (fx.type == t) return true;
    return false;
}
bool spellHasStatus(const Spell& sp, StatusEffect::Kind k) {
    for (const Effect& fx : sp.effects)
        if (fx.type == Effect::Type::ApplyStatus && fx.status.kind == k) return true;
    return false;
}

constexpr int kSummonerMin = 2; // a build is "Summoner" only at >= this many summon spells

int classifyRole(const CharacterBuild& b, const SpellCatalog& cat) {
    int summons = 0;
    bool support = false, control = false, evade = false;
    for (int id : b.spellIds) {
        const SpellDef* d = cat.find(id);
        if (!d) continue;
        const Spell& sp = d->spell;
        if (spellHas(sp, Effect::Type::Summon)) ++summons;
        if (spellHas(sp, Effect::Type::Heal) || spellHasStatus(sp, StatusEffect::Kind::Shield))
            support = true;
        if (spellHas(sp, Effect::Type::Push) || spellHas(sp, Effect::Type::Pull)) control = true;
        if (spellHasStatus(sp, StatusEffect::Kind::Invisible) ||
            spellHasStatus(sp, StatusEffect::Kind::Rewind))
            evade = true;
    }
    // A lone summon doesn't define the build (it'd swallow ~60% of them); only a
    // summon-focused build (>= kSummonerMin) is a Summoner. Otherwise it falls to
    // its next specialty, and a single-summon build reads by its other spells.
    if (summons >= kSummonerMin) return Summoner;
    if (support) return Support;
    if (control) return Control;
    if (evade) return Evasion;
    return Aggro;
}

// ---- Composition indexing (unordered multiset of roles) ---------------------
struct Comps {
    std::vector<std::vector<int>> list;       // each: sorted role indices, length teamSize
    std::map<std::string, int> index;         // canonical key -> position in list

    static std::string key(const std::vector<int>& roles) {
        std::string s;
        for (int r : roles) s += char('0' + r);
        return s;
    }
    int of(std::vector<int> roles) const { // roles need not be sorted
        std::sort(roles.begin(), roles.end());
        auto it = index.find(key(roles));
        return it == index.end() ? -1 : it->second;
    }
    std::string label(int i) const {
        std::string s;
        for (int r : list[i]) s += (s.empty() ? "" : "+") + std::string(kRoleName[r]);
        return s;
    }
};

void genComps(int start, int remaining, std::vector<int>& cur, Comps& out) {
    if (remaining == 0) {
        out.index[Comps::key(cur)] = static_cast<int>(out.list.size());
        out.list.push_back(cur);
        return;
    }
    for (int r = start; r < RoleCount; ++r) {
        cur.push_back(r);
        genComps(r, remaining - 1, cur, out);
        cur.pop_back();
    }
}

// ---- Match simulation (identical to balance_sim.cpp) ------------------------
struct Outcome {
    int result = 0;
    int length = 0;
};

Outcome runMatch(const Ruleset& ruleset, const SpellCatalog& catalog,
                 const std::vector<CharacterBuild>& A, const std::vector<CharacterBuild>& B,
                 unsigned seed, const Grid* staticArena, const std::vector<Entity>& creatures) {
    Battle battle = buildMatch(ruleset, A, B, catalog, seed, creatures, staticArena);
    Outcome o;
    for (; o.length < kHalfTurnCap && battle.phase() != Phase::Finished; ++o.length)
        runEnemyTurn(battle, /*autoEndTurn=*/true);
    if (auto w = battle.winner()) o.result = (*w == Faction::Player) ? 1 : -1;
    return o;
}

// ---- Stats helpers ----------------------------------------------------------
struct CI { double mean, lo, hi; };
CI wilson(long wins, long n) {
    if (n == 0) return {0, 0, 0};
    const double z = 1.96, p = static_cast<double>(wins) / n;
    const double d = 1.0 + z * z / n;
    const double c = (p + z * z / (2.0 * n)) / d;
    const double m = z * std::sqrt(p * (1 - p) / n + z * z / (4.0 * n * n)) / d;
    return {p, std::max(0.0, c - m), std::min(1.0, c + m)};
}
double pct(double f) { return 100.0 * f; }
std::string bar(double frac, int width = 22) {
    int n = static_cast<int>(std::lround(frac * width));
    return std::string(std::max(0, n), '#') + std::string(std::max(0, width - n), '.');
}
std::string f1(double v) { char b[32]; std::snprintf(b, sizeof b, "%.1f", v); return std::string(b); }

} // namespace

int main(int argc, char** argv) {
    const int matches = argc > 1 ? std::atoi(argv[1]) : 4000;
    const unsigned seed = argc > 2 ? static_cast<unsigned>(std::atoi(argv[2])) : 12345u;
    const std::string outfile = argc > 3 ? argv[3] : "output/team_report.txt";
    // Make sure the report's directory exists (e.g. the default output/).
    if (std::filesystem::path parent = std::filesystem::path(outfile).parent_path();
        !parent.empty())
        std::filesystem::create_directories(parent);

    std::string dataDir = "data";
    if (const char* dir = std::getenv("ATB_DATA_DIR"); dir && *dir) dataDir = dir;

    // Catalog (absent → compiled default; invalid → hard error).
    SpellCatalog catalog;
    std::string catalogSource, catalogVersion;
    if (const std::string p = dataDir + "/catalog.json"; std::ifstream(p).good()) {
        CatalogLoad load = loadCatalogFromFile(p);
        if (!load.ok) {
            std::fprintf(stderr, "team_balance: catalog '%s' invalid:\n", p.c_str());
            for (const std::string& e : load.errors) std::fprintf(stderr, "  - %s\n", e.c_str());
            return 1;
        }
        catalog = std::move(load.catalog);
        catalogSource = p;
        catalogVersion = load.version;
    } else {
        catalog = makeDefaultCatalog();
        catalogSource = "built-in";
    }

    // Ruleset.
    Ruleset ruleset;
    std::string rulesetSource;
    if (const std::string p = dataDir + "/rules.json"; std::ifstream(p).good()) {
        RulesetLoad load = loadRulesetFromFile(p);
        if (!load.ok) {
            std::fprintf(stderr, "team_balance: ruleset '%s' invalid:\n", p.c_str());
            for (const std::string& e : load.errors) std::fprintf(stderr, "  - %s\n", e.c_str());
            return 1;
        }
        ruleset = std::move(load.ruleset);
        rulesetSource = p;
    } else {
        ruleset = makeDefaultRuleset();
        rulesetSource = "built-in";
    }
    const BuildRules& rules = ruleset.economy;

    // Bestiary (for Summon effects — essential here, Summoner is an archetype).
    std::vector<Entity> creatures;
    if (const std::string p = dataDir + "/creatures.json"; std::ifstream(p).good()) {
        CreatureLoad load = loadCreaturesFromFile(p);
        if (!load.ok) {
            std::fprintf(stderr, "team_balance: creatures '%s' invalid:\n", p.c_str());
            for (const std::string& e : load.errors) std::fprintf(stderr, "  - %s\n", e.c_str());
            return 1;
        }
        creatures = std::move(load.creatures);
    } else {
        creatures = makeDefaultCreatures();
    }

    if (const char* mp = std::getenv("ATB_MAP")) ruleset.arena.map = mp;
    if (const char* tv = std::getenv("ATB_TEAM"); tv && *tv) ruleset.teamSize = std::max(1, std::atoi(tv));
    const int teamSize = ruleset.teamSize;

    std::optional<Grid> staticArena;
    if (!ruleset.arena.map.empty()) {
        const std::string& m = ruleset.arena.map;
        const bool isPath = m.size() >= 5 && m.compare(m.size() - 5, 5, ".json") == 0;
        const std::string mapPath = isPath ? m : dataDir + "/maps/" + m + ".json";
        MapLoad mload = loadMapFromFile(mapPath);
        if (!mload.ok) {
            std::fprintf(stderr, "team_balance: arena map '%s' invalid:\n", mapPath.c_str());
            for (const std::string& e : mload.errors) std::fprintf(stderr, "  - %s\n", e.c_str());
            return 1;
        }
        staticArena = std::move(mload.grid);
    }
    const Grid* arenaPtr = staticArena ? &*staticArena : nullptr;

    if (teamSize < 2)
        std::fprintf(stderr, "team_balance: note: teamSize=%d — composition analysis needs >= 2 "
                             "(set ATB_TEAM).\n", teamSize);

    // Enumerate every possible composition (multiset of roles, size teamSize).
    Comps comps;
    { std::vector<int> cur; genComps(0, teamSize, cur, comps); }
    const int C = static_cast<int>(comps.list.size());

    // Aggregates.
    std::array<long, RoleCount> roleN{}, roleW{};
    std::vector<long> compN(C, 0), compW(C, 0);
    std::vector<std::vector<long>> muN(C, std::vector<long>(C, 0)), muW(C, std::vector<long>(C, 0));
    long aWins = 0, bWins = 0, draws = 0, totalLen = 0;
    std::vector<int> lengths;
    lengths.reserve(matches);

    std::mt19937 rng(seed);
    const auto t0 = std::chrono::steady_clock::now();
    for (int m = 0; m < matches; ++m) {
        std::vector<CharacterBuild> A(teamSize), B(teamSize);
        std::vector<int> rolesA(teamSize), rolesB(teamSize);
        for (int t = 0; t < teamSize; ++t) {
            A[t] = randomBuild(rng, catalog, rules, ruleset.bannedSpells);
            B[t] = randomBuild(rng, catalog, rules, ruleset.bannedSpells);
            rolesA[t] = classifyRole(A[t], catalog);
            rolesB[t] = classifyRole(B[t], catalog);
        }
        const int ca = comps.of(rolesA), cb = comps.of(rolesB);
        Outcome o = runMatch(ruleset, catalog, A, B, rng(), arenaPtr, creatures);

        lengths.push_back(o.length);
        totalLen += o.length;
        const bool aWon = o.result > 0, bWon = o.result < 0;
        if (aWon) ++aWins; else if (bWon) ++bWins; else ++draws;

        for (int r : rolesA) { roleN[r]++; if (aWon) roleW[r]++; }
        for (int r : rolesB) { roleN[r]++; if (bWon) roleW[r]++; }
        if (ca >= 0) { compN[ca]++; if (aWon) compW[ca]++; }
        if (cb >= 0) { compN[cb]++; if (bWon) compW[cb]++; }
        if (ca >= 0 && cb >= 0) {
            muN[ca][cb]++; if (aWon) muW[ca][cb]++;
            muN[cb][ca]++; if (bWon) muW[cb][ca]++;
        }
    }
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::sort(lengths.begin(), lengths.end());

    // ---- Ranked composition rows (shared by report + CSV + HTML) -----------
    struct CompRow { int id; double wr, lo, hi, pick; long n; };
    std::vector<CompRow> crows;
    for (int i = 0; i < C; ++i) {
        CI c = wilson(compW[i], compN[i]);
        crows.push_back({i, c.mean, c.lo, c.hi, double(compN[i]) / (2.0 * matches), compN[i]});
    }
    std::sort(crows.begin(), crows.end(), [](const CompRow& a, const CompRow& b) {
        if ((a.n >= kMinN) != (b.n >= kMinN)) return a.n >= kMinN; // judged rows first
        return a.wr > b.wr;
    });

    // ---- Compose text report -----------------------------------------------
    std::ostringstream r;
    auto line = [&](const std::string& s = "") { r << s << "\n"; };
    auto rule = [&]() { r << std::string(74, '-') << "\n"; };

    r << "============ TACTICAL BATTLER — TEAM COMPOSITION REPORT ============\n\n";

    // Best/worst archetype + composition for the summary.
    int bestRole = 0, worstRole = 0;
    for (int i = 1; i < RoleCount; ++i) {
        if (double(roleW[i]) / std::max<long>(1, roleN[i]) >
            double(roleW[bestRole]) / std::max<long>(1, roleN[bestRole])) bestRole = i;
        if (double(roleW[i]) / std::max<long>(1, roleN[i]) <
            double(roleW[worstRole]) / std::max<long>(1, roleN[worstRole])) worstRole = i;
    }
    const CompRow* bestComp = nullptr;
    const CompRow* worstComp = nullptr;
    for (const CompRow& cr : crows)
        if (cr.n >= kMinN) {
            if (!bestComp || cr.wr > bestComp->wr) bestComp = &cr;
            if (!worstComp || cr.wr < worstComp->wr) worstComp = &cr;
        }

    line("SUMMARY  (plain English)");
    r << "  * Format: " << teamSize << "v" << teamSize << " over " << matches << " games.\n";
    r << "  * Strongest role: " << kRoleName[bestRole] << " ("
      << f1(pct(double(roleW[bestRole]) / std::max<long>(1, roleN[bestRole]))) << "% win) — weakest: "
      << kRoleName[worstRole] << " ("
      << f1(pct(double(roleW[worstRole]) / std::max<long>(1, roleN[worstRole]))) << "%).\n";
    if (bestComp && worstComp) {
        r << "  * Best team make-up: " << comps.label(bestComp->id) << " (" << f1(pct(bestComp->wr))
          << "%) — worst: " << comps.label(worstComp->id) << " (" << f1(pct(worstComp->wr)) << "%).\n";
    }
    r << "  * Roles are auto-tagged from a build's spells (precedence: Summoner [2+ summons] > "
         "Support > Control > Evasion > Aggro).\n";
    line("  * Charts: open the .html file. Raw tables: the .csv files.");
    line();
    rule();
    line("RUN");
    r << "  matches      " << matches << "     seed " << seed << "     " << teamSize << "v" << teamSize
      << "\n";
    r << "  wall time    " << elapsed << " s  (" << (elapsed > 0 ? matches / elapsed : 0)
      << " matches/s)\n";
    r << "  ruleset      " << rulesetSource << "     catalog " << catalogSource;
    if (!catalogVersion.empty()) r << " (v" << catalogVersion << ")";
    r << "\n";
    if (staticArena) r << "  arena        static map '" << ruleset.arena.map << "'\n";
    else r << "  arena        random " << ruleset.arena.width << "x" << ruleset.arena.height << "\n";
    {
        char buf[200];
        std::snprintf(buf, sizeof buf, "  outcomes     A(1st) %.1f%%   B(2nd) %.1f%%   draws %.1f%%",
                      pct(double(aWins) / matches), pct(double(bWins) / matches),
                      pct(double(draws) / matches));
        r << buf << "   (draws = hit the " << kHalfTurnCap << "-half-turn cap)\n";
    }
    line();
    rule();
    line("ARCHETYPE WIN RATES  (per build carrying that role; 95% Wilson CI)");
    line("  role        pick%   winrate  [   95% CI   ]   what it is");
    {
        std::array<int, RoleCount> order{};
        for (int i = 0; i < RoleCount; ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            return double(roleW[a]) / std::max<long>(1, roleN[a]) >
                   double(roleW[b]) / std::max<long>(1, roleN[b]);
        });
        for (int i : order) {
            CI c = wilson(roleW[i], roleN[i]);
            double pickPct = pct(double(roleN[i]) / (2.0 * matches * teamSize));
            char buf[200];
            std::snprintf(buf, sizeof buf, "  %-9s  %5.1f%%  %6.1f%%  [%5.1f–%5.1f]   %s",
                          kRoleName[i], pickPct, pct(c.mean), pct(c.lo), pct(c.hi), kRoleDesc[i]);
            r << buf << (roleN[i] < kMinN ? "   (too few)" : "") << "\n";
        }
    }
    line();
    rule();
    line("COMPOSITION WIN RATES  (per team fielding that make-up; rare comps last)");
    line("  composition                    pick%   winrate  [   95% CI   ]     n");
    for (const CompRow& cr : crows) {
        char buf[220];
        if (cr.n < kMinN)
            std::snprintf(buf, sizeof buf, "  %-28s  %5.1f%%     -      (too few)     n=%ld",
                          comps.label(cr.id).c_str(), pct(cr.pick), cr.n);
        else
            std::snprintf(buf, sizeof buf, "  %-28s  %5.1f%%  %6.1f%%  [%5.1f–%5.1f]   n=%ld",
                          comps.label(cr.id).c_str(), pct(cr.pick), pct(cr.wr), pct(cr.lo),
                          pct(cr.hi), cr.n);
        r << buf << "\n";
    }
    line();
    rule();
    line("TOP MATCHUPS  (comp on the left beats comp on the right; both >= min sample)");
    struct MU { int i, j; double wr; long n; };
    std::vector<MU> mus;
    for (int i = 0; i < C; ++i)
        for (int j = 0; j < C; ++j)
            if (i != j && muN[i][j] >= kMinN)
                mus.push_back({i, j, double(muW[i][j]) / muN[i][j], muN[i][j]});
    std::sort(mus.begin(), mus.end(), [](const MU& a, const MU& b) { return a.wr > b.wr; });
    for (size_t k = 0; k < mus.size() && k < 10; ++k) {
        const MU& mu = mus[k];
        char buf[220];
        std::snprintf(buf, sizeof buf, "  %-24s beats %-24s  %5.1f%%  n=%ld",
                      comps.label(mu.i).c_str(), comps.label(mu.j).c_str(), pct(mu.wr), mu.n);
        r << buf << "\n";
    }
    line();
    rule();
    line("READING IT");
    line("  - Builds are fully random (organic), then tagged into one archetype by their spells,");
    line("    so common comps (lots of Aggro) have big samples and exotic comps may be '(too few)'.");
    line("  - Composition win rate averages over both going first and second, so it isn't skewed");
    line("    by turn order (unlike a fixed-map first-move edge).");
    line("  - The matchup matrix cell [row,col] = how often a 'row' team beats a 'col' team.");

    // ---- CSV ---------------------------------------------------------------
    std::string base = outfile;
    if (base.size() > 4 && base.compare(base.size() - 4, 4, ".txt") == 0) base.resize(base.size() - 4);
    {
        std::ofstream c(base + ".roles.csv");
        c << "role,pick_pct,winrate_pct,ci_lo_pct,ci_hi_pct,n\n";
        for (int i = 0; i < RoleCount; ++i) {
            CI ci = wilson(roleW[i], roleN[i]);
            c << kRoleName[i] << ',' << f1(pct(double(roleN[i]) / (2.0 * matches * teamSize))) << ','
              << f1(pct(ci.mean)) << ',' << f1(pct(ci.lo)) << ',' << f1(pct(ci.hi)) << ',' << roleN[i]
              << "\n";
        }
    }
    {
        std::ofstream c(base + ".comps.csv");
        c << "composition,pick_pct,winrate_pct,ci_lo_pct,ci_hi_pct,n\n";
        for (const CompRow& cr : crows)
            c << comps.label(cr.id) << ',' << f1(pct(cr.pick)) << ',' << f1(pct(cr.wr)) << ','
              << f1(pct(cr.lo)) << ',' << f1(pct(cr.hi)) << ',' << cr.n << "\n";
    }
    {
        std::ofstream c(base + ".matchups.csv");
        c << "row_comp,col_comp,winrate_pct,n\n";
        for (int i = 0; i < C; ++i)
            for (int j = 0; j < C; ++j)
                if (muN[i][j] > 0)
                    c << comps.label(i) << ',' << comps.label(j) << ','
                      << f1(pct(double(muW[i][j]) / muN[i][j])) << ',' << muN[i][j] << "\n";
    }

    // ---- HTML --------------------------------------------------------------
    {
        auto esc = [](const std::string& s) {
            std::string o;
            for (char ch : s) o += ch == '&' ? "&amp;" : ch == '<' ? "&lt;" : ch == '>' ? "&gt;"
                                                                                        : std::string(1, ch);
            return o;
        };
        // Diverging red→neutral→green fill for a win-rate cell/bar.
        auto wrColor = [](double wrPct) {
            if (wrPct < 42) return "#c0392b";
            if (wrPct < 47) return "#e67e22";
            if (wrPct <= 53) return "#f0eee9";
            if (wrPct <= 58) return "#7fb069";
            return "#27ae60";
        };
        std::ostringstream h;
        h << "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
             "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
             "<title>Team Composition Report</title><style>"
             "body{font:14px/1.5 system-ui,sans-serif;margin:24px;color:#222;max-width:1000px}"
             "h1{font-size:20px}h2{font-size:16px;margin-top:28px;border-bottom:1px solid #ddd;padding-bottom:4px}"
             ".meta{color:#555;font-size:13px}.sum{background:#f6f8fa;border:1px solid #e1e4e8;"
             "border-radius:6px;padding:8px 16px}svg{max-width:100%}"
             "table.mx{border-collapse:collapse;font-size:11px}table.mx td,table.mx th{border:1px solid #fff;"
             "padding:4px 6px;text-align:center}table.mx th{background:#f2f2f2}"
             "table.mx td.rh{background:#f2f2f2;text-align:right;font-weight:600}"
             "</style></head><body>\n";
        h << "<h1>Tactical Battler — Team Composition Report</h1>\n";
        h << "<p class=\"meta\"><b>" << teamSize << "v" << teamSize << "</b> &middot; <b>" << matches
          << "</b> games &middot; seed <b>" << seed << "</b> &middot; ruleset <b>" << esc(rulesetSource)
          << "</b></p>\n";
        h << "<div class=\"sum\"><ul>";
        h << "<li>Strongest role: <b>" << kRoleName[bestRole] << "</b>; weakest: <b>"
          << kRoleName[worstRole] << "</b>.</li>";
        if (bestComp && worstComp)
            h << "<li>Best team make-up: <b>" << comps.label(bestComp->id) << "</b> ("
              << f1(pct(bestComp->wr)) << "%); worst: <b>" << comps.label(worstComp->id) << "</b> ("
              << f1(pct(worstComp->wr)) << "%).</li>";
        h << "<li>Roles auto-tagged from spells (precedence Summoner [2+ summons] &gt; Support &gt; "
             "Control &gt; Evasion &gt; Aggro). Rare comps may read <i>too few</i>.</li>";
        h << "</ul></div>\n";

        // Reusable win-rate bar chart.
        struct WR { std::string name; double wr, lo, hi; long n; };
        auto winrateChart = [&](const std::vector<WR>& items) {
            const int n = static_cast<int>(items.size());
            const int x0 = 230, barW = 460, rh = 22, top = 26, W = x0 + barW + 130, H = top + n * rh + 10;
            auto X = [&](double p) { return x0 + (p / 100.0) * barW; };
            h << "<svg width=\"" << W << "\" height=\"" << H << "\" viewBox=\"0 0 " << W << " " << H << "\">\n";
            for (int p = 0; p <= 100; p += 25) {
                double x = X(p);
                h << "<line x1=\"" << x << "\" y1=\"" << top - 6 << "\" x2=\"" << x << "\" y2=\"" << H - 4
                  << "\" stroke=\"" << (p == 50 ? "#888" : "#e8e8e8") << "\""
                  << (p == 50 ? " stroke-dasharray=\"4 3\"" : "") << "/>"
                  << "<text x=\"" << x << "\" y=\"" << top - 10
                  << "\" font-size=\"10\" fill=\"#888\" text-anchor=\"middle\">" << p << "%</text>\n";
            }
            for (int i = 0; i < n; ++i) {
                double yc = top + i * rh + rh / 2.0;
                h << "<text x=\"8\" y=\"" << yc + 4 << "\" font-size=\"12\">" << esc(items[i].name)
                  << "</text>";
                if (items[i].n < kMinN) {
                    h << "<text x=\"" << x0 + 4 << "\" y=\"" << yc + 4
                      << "\" font-size=\"11\" fill=\"#888\">(too few: n=" << items[i].n << ")</text>\n";
                    continue;
                }
                double bx = X(pct(items[i].wr)), xl = X(pct(items[i].lo)), xh = X(pct(items[i].hi));
                h << "<rect x=\"" << x0 << "\" y=\"" << top + i * rh + 4 << "\" width=\""
                  << std::max(0.0, bx - x0) << "\" height=\"" << rh - 8 << "\" fill=\""
                  << wrColor(pct(items[i].wr)) << "\" rx=\"2\"/>"
                  << "<line x1=\"" << xl << "\" y1=\"" << yc << "\" x2=\"" << xh << "\" y2=\"" << yc
                  << "\" stroke=\"#333\"/>"
                  << "<text x=\"" << x0 + barW + 6 << "\" y=\"" << yc + 4 << "\" font-size=\"11\">"
                  << f1(pct(items[i].wr)) << "% (n=" << items[i].n << ")</text>\n";
            }
            h << "</svg>\n";
        };

        h << "<h2>Archetype win rate</h2>\n";
        h << "<p class=\"meta\">Win rate of every build carrying that role (bar past the dashed 50% "
             "line = wins more than half). Colour also encodes strength.</p>\n";
        {
            std::vector<WR> items;
            std::array<int, RoleCount> order{};
            for (int i = 0; i < RoleCount; ++i) order[i] = i;
            std::sort(order.begin(), order.end(), [&](int a, int b) {
                return double(roleW[a]) / std::max<long>(1, roleN[a]) >
                       double(roleW[b]) / std::max<long>(1, roleN[b]);
            });
            for (int i : order) {
                CI c = wilson(roleW[i], roleN[i]);
                items.push_back({kRoleName[i], c.mean, c.lo, c.hi, roleN[i]});
            }
            winrateChart(items);
        }

        h << "<h2>Composition win rate</h2>\n";
        h << "<p class=\"meta\">Win rate of teams fielding each make-up, best first. Comps below the "
             "sample threshold are marked <i>too few</i>.</p>\n";
        {
            std::vector<WR> items;
            for (const CompRow& cr : crows)
                items.push_back({comps.label(cr.id), cr.wr, cr.lo, cr.hi, cr.n});
            winrateChart(items);
        }

        // Matchup heatmap (only when the grid is small enough to read).
        h << "<h2>Matchup matrix</h2>\n";
        if (C <= 18) {
            h << "<p class=\"meta\">Cell = how often the <b>row</b> team beats the <b>column</b> team "
                 "(green &gt; 50%, red &lt; 50%). Blank = too few games.</p>\n";
            h << "<table class=\"mx\"><tr><th>row \\ col</th>";
            for (int j = 0; j < C; ++j) h << "<th>" << esc(comps.label(j)) << "</th>";
            h << "</tr>\n";
            for (int i = 0; i < C; ++i) {
                h << "<tr><td class=\"rh\">" << esc(comps.label(i)) << "</td>";
                for (int j = 0; j < C; ++j) {
                    if (muN[i][j] < kMinN) { h << "<td style=\"background:#fbfbfb;color:#bbb\">·</td>"; continue; }
                    double w = pct(double(muW[i][j]) / muN[i][j]);
                    h << "<td style=\"background:" << wrColor(w) << "\">" << f1(w) << "</td>";
                }
                h << "</tr>\n";
            }
            h << "</table>\n";
        } else {
            h << "<p class=\"meta\">" << C << " compositions — matrix too large to draw legibly; see "
              << esc(base) << ".matchups.csv (and TOP MATCHUPS in the text report).</p>\n";
        }

        h << "<p class=\"meta\">Full text detail: " << esc(outfile)
          << " &middot; data: *.roles.csv, *.comps.csv, *.matchups.csv</p>\n";
        h << "</body></html>\n";
        std::ofstream hf(base + ".html");
        if (hf) hf << h.str();
    }

    std::cout << r.str();
    { std::ofstream f(outfile); if (f) f << r.str(); }
    std::cout << "\n(reports written:\n  text  " << outfile << "\n  html  " << base << ".html"
              << "\n  csv   " << base << ".roles.csv, " << base << ".comps.csv, " << base
              << ".matchups.csv)\n";
    return 0;
}
