//
// balance_sim.cpp — Monte-Carlo balance harness + detailed report.
//
// Generates random classless builds, runs thousands of AI-vs-AI matches on
// random arenas, and writes a full report (stdout + file) covering outcomes,
// how matches end, length distribution, per-spell win rates with confidence
// intervals, cost-efficiency, stat-investment, and spell-pair synergies.
//
//   usage: tb_balance [matches] [seed] [outfile]
//          defaults: 4000  12345  balance_report.txt
//
// NOTE: the planner casts every spell except Portal (its step-on-entry teleport
// needs deeper lookahead), so Portal's numbers reflect point opportunity-cost.
//
#include "core/AI.h"
#include "core/Battle.h"
#include "core/Build.h"
#include "core/Grid.h"
#include "core/Match.h"
#include "core/Ruleset.h"
#include "core/Spells.h"
#include "data/CatalogJson.h"
#include "data/MapJson.h"
#include "data/RulesetJson.h"

#include <optional>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace tb;

namespace {

constexpr int kHalfTurnCap = 120;

bool isOffensive(int id) {
    return id == spellid::Attack || id == spellid::Fireball || id == spellid::Poison ||
           id == spellid::Knockback || id == spellid::Harpoon;
}

// ---- Build generation ------------------------------------------------------
struct Build {
    CharacterBuild def;
    int spellPts = 0;
    int statPts = 0;
};

int statCost(const StatAllocation& s, const BuildRules& r) {
    return s.hpPurchases * r.hpCost + s.bonusAp * r.apCost + s.bonusMp * r.mpCost;
}

Build randomBuild(std::mt19937& rng, const SpellCatalog& catalog, const BuildRules& rules,
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
    std::uniform_int_distribution<int> pick(0, 2);
    for (int guard = 0; guard < 24; ++guard) {
        StatAllocation trial = b.stats;
        switch (pick(rng)) {
            case 0: ++trial.hpPurchases; break;
            case 1: ++trial.bonusAp; break;
            default: ++trial.bonusMp; break;
        }
        CharacterBuild probe = b;
        probe.stats = trial;
        if (validateBuild(probe, catalog, rules, banned).ok) b.stats = trial;
    }
    Build out;
    out.def = b;
    out.statPts = statCost(b.stats, rules);
    for (int id : b.spellIds)
        if (const SpellDef* d = catalog.find(id)) out.spellPts += d->buildCost;
    return out;
}

// ---- Match simulation ------------------------------------------------------
void takeTurn(Battle& b) { runEnemyTurn(b, /*autoEndTurn=*/true); }

struct Outcome {
    int result = 0; // +1 A (first actor), -1 B, 0 draw
    int length = 0;
    DamageSource source = DamageSource::Spell;
};

Outcome runMatch(const Ruleset& ruleset, const SpellCatalog& catalog,
                 const std::vector<CharacterBuild>& A, const std::vector<CharacterBuild>& B,
                 unsigned seed, const Grid* staticArena) {
    Battle battle = buildMatch(ruleset, A, B, catalog, seed, {}, staticArena);

    Outcome o;
    for (; o.length < kHalfTurnCap && battle.phase() != Phase::Finished; ++o.length) takeTurn(battle);
    auto w = battle.winner();
    if (w) {
        o.result = (*w == Faction::Player) ? 1 : -1;
        o.source = battle.lastDeathSource();
    }
    return o;
}

// ---- Stats helpers ---------------------------------------------------------
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
double percentile(const std::vector<int>& sorted, double q) {
    if (sorted.empty()) return 0;
    double idx = q * (sorted.size() - 1);
    size_t lo = static_cast<size_t>(idx);
    double frac = idx - lo;
    if (lo + 1 >= sorted.size()) return sorted[lo];
    return sorted[lo] * (1 - frac) + sorted[lo + 1] * frac;
}

struct SideAgg {
    // Indexed by spell id. Sized to the catalog's max id + 1 at construction so it
    // grows with the catalog — a fixed-size array silently overflowed (and crashed)
    // when new spells pushed ids past the old hard-coded bound.
    std::vector<long> appears, wins;
    std::vector<std::vector<long>> pairN, pairW;
    // stat presence
    long hpAppear = 0, hpWin = 0, apAppear = 0, apWin = 0, mpAppear = 0, mpWin = 0;
    long noStatAppear = 0, noStatWin = 0;
    std::array<long, 4> bucketN{}, bucketW{}; // by statPts: 0, 1-3, 4-6, 7+

    explicit SideAgg(int idCount)
        : appears(idCount, 0), wins(idCount, 0),
          pairN(idCount, std::vector<long>(idCount, 0)),
          pairW(idCount, std::vector<long>(idCount, 0)) {}
};

void foldSide(SideAgg& g, const Build& b, bool won) {
    const auto& ids = b.def.spellIds;
    const int n = static_cast<int>(g.appears.size());
    for (int id : ids)
        if (id >= 0 && id < n) { g.appears[id]++; if (won) g.wins[id]++; }
    for (size_t i = 0; i < ids.size(); ++i)
        for (size_t j = i + 1; j < ids.size(); ++j) {
            int a = std::min(ids[i], ids[j]), c = std::max(ids[i], ids[j]);
            if (a >= 0 && c < n) { g.pairN[a][c]++; if (won) g.pairW[a][c]++; }
        }
    auto bump = [&](long& ap, long& wn, bool present) { if (present) { ap++; if (won) wn++; } };
    bump(g.hpAppear, g.hpWin, b.def.stats.hpPurchases > 0);
    bump(g.apAppear, g.apWin, b.def.stats.bonusAp > 0);
    bump(g.mpAppear, g.mpWin, b.def.stats.bonusMp > 0);
    if (b.statPts == 0) { g.noStatAppear++; if (won) g.noStatWin++; }
    int bucket = b.statPts == 0 ? 0 : b.statPts <= 3 ? 1 : b.statPts <= 6 ? 2 : 3;
    g.bucketN[bucket]++; if (won) g.bucketW[bucket]++;
}

} // namespace

int main(int argc, char** argv) {
    const int matches = argc > 1 ? std::atoi(argv[1]) : 4000;
    const unsigned seed = argc > 2 ? static_cast<unsigned>(std::atoi(argv[2])) : 12345u;
    const std::string outfile = argc > 3 ? argv[3] : "balance_report.txt";

    // Balance the *actual* content: load data/catalog.json (override the dir via
    // ATB_DATA_DIR) so data edits drive the report. Absent file → the compiled
    // catalog; present-but-invalid → hard error (don't silently report the wrong set).
    std::string catalogPath = "data/catalog.json";
    if (const char* dir = std::getenv("ATB_DATA_DIR"); dir && *dir)
        catalogPath = std::string(dir) + "/catalog.json";
    SpellCatalog catalog;
    std::string catalogSource, catalogVersion;
    if (std::ifstream(catalogPath).good()) {
        CatalogLoad load = loadCatalogFromFile(catalogPath);
        if (!load.ok) {
            std::fprintf(stderr, "balance: catalog '%s' is invalid:\n", catalogPath.c_str());
            for (const std::string& e : load.errors) std::fprintf(stderr, "  - %s\n", e.c_str());
            return 1;
        }
        catalog = std::move(load.catalog);
        catalogSource = catalogPath;
        catalogVersion = load.version;
    } else {
        catalog = makeDefaultCatalog();
        catalogSource = "built-in (makeDefaultCatalog)";
    }

    // Unified rules: load data/rules.json (override the dir via ATB_DATA_DIR) so
    // the sim and the game build matches the same way — tune the economy / ring /
    // arena / team size by editing rules.json. Absent → compiled default;
    // present-but-invalid → hard error.
    std::string rulesPath = "data/rules.json";
    if (const char* dir = std::getenv("ATB_DATA_DIR"); dir && *dir)
        rulesPath = std::string(dir) + "/rules.json";
    Ruleset ruleset;
    std::string rulesetSource;
    if (std::ifstream(rulesPath).good()) {
        RulesetLoad load = loadRulesetFromFile(rulesPath);
        if (!load.ok) {
            std::fprintf(stderr, "balance: ruleset '%s' is invalid:\n", rulesPath.c_str());
            for (const std::string& e : load.errors) std::fprintf(stderr, "  - %s\n", e.c_str());
            return 1;
        }
        ruleset = std::move(load.ruleset);
        rulesetSource = rulesPath;
    } else {
        ruleset = makeDefaultRuleset();
        rulesetSource = "built-in (makeDefaultRuleset)";
    }
    const BuildRules& rules = ruleset.economy;
    const StormConfig& storm = ruleset.closingRing;

    // Static map (if the ruleset names one): all matches share it.
    std::optional<Grid> staticArena;
    if (!ruleset.arena.map.empty()) {
        std::string mapDir = "data";
        if (const char* dir = std::getenv("ATB_DATA_DIR"); dir && *dir) mapDir = dir;
        const std::string mapPath = mapDir + "/maps/" + ruleset.arena.map + ".json";
        MapLoad mload = loadMapFromFile(mapPath);
        if (!mload.ok) {
            std::fprintf(stderr, "balance: arena map '%s' is invalid:\n", mapPath.c_str());
            for (const std::string& e : mload.errors) std::fprintf(stderr, "  - %s\n", e.c_str());
            return 1;
        }
        staticArena = std::move(mload.grid);
    }
    const Grid* arenaPtr = staticArena ? &*staticArena : nullptr;
    std::mt19937 rng(seed);

    long aWins = 0, bWins = 0, draws = 0;
    long bySpell = 0, byStorm = 0, byCollision = 0;
    long totalLen = 0;
    std::vector<int> lengths;
    lengths.reserve(matches);
    int maxSpellId = 0;
    for (const SpellDef& d : catalog.all()) maxSpellId = std::max(maxSpellId, d.id);
    SideAgg g(maxSpellId + 1);

    const int teamSize = ruleset.teamSize;
    const auto t0 = std::chrono::steady_clock::now();
    for (int m = 0; m < matches; ++m) {
        std::vector<Build> teamA, teamB;
        std::vector<CharacterBuild> defsA, defsB;
        for (int t = 0; t < teamSize; ++t) {
            teamA.push_back(randomBuild(rng, catalog, rules, ruleset.bannedSpells));
            teamB.push_back(randomBuild(rng, catalog, rules, ruleset.bannedSpells));
            defsA.push_back(teamA.back().def);
            defsB.push_back(teamB.back().def);
        }
        Outcome o = runMatch(ruleset, catalog, defsA, defsB, rng(), arenaPtr);

        lengths.push_back(o.length);
        totalLen += o.length;
        if (o.result > 0) ++aWins;
        else if (o.result < 0) ++bWins;
        else ++draws;
        if (o.result != 0) {
            switch (o.source) {
                case DamageSource::Spell: ++bySpell; break;
                case DamageSource::Storm: ++byStorm; break;
                case DamageSource::Collision: ++byCollision; break;
            }
        }
        for (const Build& a : teamA) foldSide(g, a, o.result > 0);
        for (const Build& b : teamB) foldSide(g, b, o.result < 0);
    }
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::sort(lengths.begin(), lengths.end());

    // ---- Compose report ----------------------------------------------------
    std::ostringstream r;
    auto line = [&](const std::string& s = "") { r << s << "\n"; };
    auto rule = [&]() { r << std::string(74, '-') << "\n"; };

    r << "================ TACTICAL BATTLER — BALANCE REPORT ================\n";
    line();
    line("RUN");
    r << "  matches            " << matches << "\n";
    r << "  seed               " << seed << "\n";
    r << "  wall time          " << elapsed << " s  ("
      << (elapsed > 0 ? matches / elapsed : 0) << " matches/s)\n";
    r << "  ruleset            " << rulesetSource << "\n";
    r << "  format             " << teamSize << "v" << teamSize << "\n";
    r << "  point budget       " << rules.pointBudget << "\n";
    r << "  HP economy         base " << rules.baseHp << ", +" << rules.hpStep << " HP per "
      << rules.hpCost << " pt\n";
    r << "  catalog            " << catalogSource;
    if (!catalogVersion.empty()) r << "  (v" << catalogVersion << ")";
    r << "\n";
    if (const SpellDef* fb = catalog.find(spellid::Fireball))
        r << "  fireball cost      " << fb->buildCost << " pt\n";
    r << "  closing ring       from round " << storm.startRound << ", " << storm.damage
      << " dmg/turn outside the safe square\n";
    line();
    rule();
    line("OUTCOMES   (A = first actor; symmetric matchups, so A% over 50 = first-move edge)");
    auto outRow = [&](const char* name, long w) {
        CI c = wilson(w, matches);
        char buf[160];
        std::snprintf(buf, sizeof buf, "  %s  %s  %5.1f%%  [%4.1f–%4.1f]  n=%ld", name,
                      bar(c.mean).c_str(), pct(c.mean), pct(c.lo), pct(c.hi), w);
        r << buf << "\n";
    };
    outRow("A wins (1st)", aWins);
    outRow("B wins (2nd)", bWins);
    outRow("draws (cap) ", draws);
    line();
    line("HOW DECISIVE MATCHES END");
    long dec = aWins + bWins;
    auto endRow = [&](const char* name, long c) {
        double f = dec ? double(c) / dec : 0;
        char buf[160];
        std::snprintf(buf, sizeof buf, "  %s  %s  %5.1f%%  n=%ld", name, bar(f).c_str(), pct(f), c);
        r << buf << "\n";
    };
    endRow("spell kill    ", bySpell);
    endRow("ring (storm)  ", byStorm);
    endRow("collision     ", byCollision);
    line();
    line("MATCH LENGTH (half-turns)");
    {
        char buf[200];
        std::snprintf(buf, sizeof buf,
                      "  min %d   p10 %.0f   median %.0f   mean %.1f   p90 %.0f   max %d",
                      lengths.front(), percentile(lengths, .10), percentile(lengths, .50),
                      (double)totalLen / matches, percentile(lengths, .90), lengths.back());
        r << buf << "\n";
    }
    {
        const int bw = 8, nb = 7; // buckets of 8 half-turns, last is overflow
        std::array<long, 7> hist{};
        for (int v : lengths) hist[std::min(v / bw, nb - 1)]++;
        for (int i = 0; i < nb; ++i) {
            int lo = i * bw, hi = lo + bw - 1;
            char label[24];
            if (i == nb - 1) std::snprintf(label, sizeof label, "%2d+   ", lo);
            else std::snprintf(label, sizeof label, "%2d-%-2d ", lo, hi);
            r << "  " << label << bar(double(hist[i]) / matches) << "  " << hist[i] << "\n";
        }
    }
    line();
    rule();
    line("PER-SPELL  (win rate of builds containing the spell, 95% Wilson CI)");
    line("  spell        cost   pick%   winrate  [   95% CI   ]   lift   val/pt");
    struct Row { int id; double wr, lo, hi, pick; long n; };
    std::vector<Row> rows;
    for (const SpellDef& d : catalog.all()) {
        CI c = wilson(g.wins[d.id], g.appears[d.id]);
        rows.push_back({d.id, c.mean, c.lo, c.hi, double(g.appears[d.id]) / (2.0 * matches),
                        g.appears[d.id]});
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.wr > b.wr; });
    for (const Row& row : rows) {
        const SpellDef* d = catalog.find(row.id);
        double lift = pct(row.wr) - 50.0;
        double valPerPt = d->buildCost ? lift / d->buildCost : 0.0;
        char buf[160];
        std::snprintf(buf, sizeof buf,
                      "  %-10s   %2dpt  %5.1f%%  %6.1f%%  [%5.1f–%5.1f]  %+5.1f  %+6.2f%s",
                      d->key.c_str(), d->buildCost, pct(row.pick), pct(row.wr), pct(row.lo),
                      pct(row.hi), lift, valPerPt, row.id == spellid::Portal ? "  (AI-unused)" : "");
        r << buf << "\n";
    }
    line();
    line("STAT INVESTMENT");
    auto statRow = [&](const char* name, long w, long n) {
        CI c = wilson(w, n);
        char buf[160];
        std::snprintf(buf, sizeof buf, "  %s  %5.1f%%  [%4.1f–%4.1f]  n=%ld", name, pct(c.mean),
                      pct(c.lo), pct(c.hi), n);
        r << buf << "\n";
    };
    statRow("bought +HP   ", g.hpWin, g.hpAppear);
    statRow("bought +AP   ", g.apWin, g.apAppear);
    statRow("bought +MP   ", g.mpWin, g.mpAppear);
    statRow("no stat spend", g.noStatWin, g.noStatAppear);
    {
        const char* names[4] = {"0 pts ", "1-3pts", "4-6pts", "7+ pts"};
        r << "  by stat-point spend:\n";
        for (int i = 0; i < 4; ++i)
            statRow(names[i], g.bucketW[i], g.bucketN[i]);
    }
    line();
    line("TOP SPELL SYNERGIES  (pairs co-picked >=" "30 times, by joint win rate)");
    struct Pair { int a, b; double wr; long n; };
    std::vector<Pair> pairs;
    const int idCount = static_cast<int>(g.appears.size());
    for (int a = 0; a < idCount; ++a)
        for (int b = a + 1; b < idCount; ++b)
            if (g.pairN[a][b] >= 30)
                pairs.push_back({a, b, double(g.pairW[a][b]) / g.pairN[a][b], g.pairN[a][b]});
    std::sort(pairs.begin(), pairs.end(), [](const Pair& x, const Pair& y) { return x.wr > y.wr; });
    line("  pair                       winrate   n     vs solo-avg");
    auto solo = [&](int id) { return double(g.wins[id]) / std::max<long>(1, g.appears[id]); };
    for (size_t i = 0; i < pairs.size() && i < 12; ++i) {
        const Pair& p = pairs[i];
        double soloAvg = (solo(p.a) + solo(p.b)) / 2.0;
        char buf[160];
        std::snprintf(buf, sizeof buf, "  %-10s + %-10s   %5.1f%%  %4ld   %+5.1f",
                      catalog.find(p.a)->key.c_str(), catalog.find(p.b)->key.c_str(), pct(p.wr),
                      p.n, pct(p.wr - soloAvg));
        r << buf << "\n";
    }
    line();
    rule();
    line("READING IT");
    line("  - lift = winrate - 50%. val/pt = lift per build point (cost efficiency).");
    line("  - 'vs solo-avg' > 0 means the pair wins more than its two spells do alone");
    line("    (emergent synergy, e.g. poison + invisible).");
    line("  - Non-overlapping CIs between two spells => a real difference, not noise.");
    line("  - Portal is AI-unused; treat its row as a point opportunity-cost baseline.");
    line("  - Build generator fills spells first, stats with leftover points, so the stat");
    line("    section compares lean+stats builds against spell-crammed ones (a real signal:");
    line("    extra spells you're too AP-starved to cast lose to durability).");

    // ---- Emit --------------------------------------------------------------
    std::cout << r.str();
    std::ofstream f(outfile);
    if (f) {
        f << r.str();
        std::cout << "\n(report written to " << outfile << ")\n";
    }
    return 0;
}
