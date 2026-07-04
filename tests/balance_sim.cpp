//
// balance_sim.cpp — Monte-Carlo balance harness + detailed report.
//
// Generates random classless builds, runs thousands of AI-vs-AI matches on
// random arenas, and writes a full report covering outcomes, how matches end,
// length distribution, per-spell win rates with confidence intervals,
// cost-efficiency, stat-investment, and spell-pair synergies.
//
// Outputs (siblings of <outfile>, with its .txt stripped for the base name):
//   <outfile>            plain-text report (also echoed to stdout), with a
//                        plain-English SUMMARY up top and per-spell verdicts
//   <base>.html          self-contained charts (inline SVG, no deps) — open in a browser
//   <base>.spells.csv    per-spell table   <base>.pairs.csv    synergies
//   <base>.length.csv    length histogram  <base>.outcomes.csv win/loss + how games end
//
//   usage: tb_balance [matches] [seed] [outfile]
//          defaults: 4000  12345  output/balance_report.txt
//
// NOTE: the planner casts every spell except Portal (its step-on-entry teleport
// needs deeper lookahead), so Portal's numbers reflect point opportunity-cost.
//
#include "core/AI.h"
#include "core/Battle.h"
#include "core/Build.h"
#include "core/Grid.h"
#include "core/Creatures.h"
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
    return s.hpPurchases * r.hpCost + s.bonusAp * r.apCost + s.bonusMp * r.mpCost +
           s.bonusInitiative * r.initCost;
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
    Build out;
    out.def = b;
    out.statPts = statCost(b.stats, rules);
    for (int id : b.spellIds)
        if (const SpellDef* d = catalog.find(id)) out.spellPts += d->buildCost;
    return out;
}

// ---- Match simulation ------------------------------------------------------
void takeTurn(Battle& b, const Brain& brain) { runEnemyTurn(b, /*autoEndTurn=*/true, brain); }

struct Outcome {
    int result = 0; // +1 A (first actor), -1 B, 0 draw
    int length = 0;
    DamageSource source = DamageSource::Spell;
};

Outcome runMatch(const Ruleset& ruleset, const SpellCatalog& catalog,
                 const std::vector<CharacterBuild>& A, const std::vector<CharacterBuild>& B,
                 unsigned seed, const Grid* staticArena, const std::vector<Entity>& creatures,
                 const Brain& brain) {
    Battle battle = buildMatch(ruleset, A, B, catalog, seed, creatures, staticArena);

    Outcome o;
    for (; o.length < kHalfTurnCap && battle.phase() != Phase::Finished; ++o.length)
        takeTurn(battle, brain);
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
    long initAppear = 0, initWin = 0;
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
    bump(g.initAppear, g.initWin, b.def.stats.bonusInitiative > 0);
    if (b.statPts == 0) { g.noStatAppear++; if (won) g.noStatWin++; }
    int bucket = b.statPts == 0 ? 0 : b.statPts <= 3 ? 1 : b.statPts <= 6 ? 2 : 3;
    g.bucketN[bucket]++; if (won) g.bucketW[bucket]++;
}

} // namespace

int main(int argc, char** argv) {
    const int matches = argc > 1 ? std::atoi(argv[1]) : 4000;
    const unsigned seed = argc > 2 ? static_cast<unsigned>(std::atoi(argv[2])) : 12345u;
    const std::string outfile = argc > 3 ? argv[3] : "output/balance_report.txt";
    // Make sure the report's directory exists (e.g. the default output/).
    if (std::filesystem::path parent = std::filesystem::path(outfile).parent_path();
        !parent.empty())
        std::filesystem::create_directories(parent);

    // Balance the *actual* content: load data/catalog.json (override the dir via
    // ATB_DATA_DIR) so data edits drive the report. Absent file → the compiled
    // catalog; present-but-invalid → hard error (don't silently report the wrong set).
    std::string dataDir = "data";
    if (const char* dir = std::getenv("ATB_DATA_DIR"); dir && *dir) dataDir = dir;
    const std::string catalogPath = dataDir + "/catalog.json";
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
    const std::string rulesPath = dataDir + "/rules.json";
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

    // Bestiary (for Summon effects). Same policy; absent → compiled default.
    std::vector<Entity> creatures;
    {
        const std::string creaturesPath = dataDir + "/creatures.json";
        if (std::ifstream(creaturesPath).good()) {
            CreatureLoad load = loadCreaturesFromFile(creaturesPath);
            if (!load.ok) {
                std::fprintf(stderr, "balance: creatures '%s' is invalid:\n", creaturesPath.c_str());
                for (const std::string& e : load.errors) std::fprintf(stderr, "  - %s\n", e.c_str());
                return 1;
            }
            creatures = std::move(load.creatures);
        } else {
            creatures = makeDefaultCreatures();
        }
    }

    // ATB_MAP overrides the ruleset's arena for this run: a key under data/maps/,
    // a path ending in .json, or empty to force a random arena.
    if (const char* mp = std::getenv("ATB_MAP")) ruleset.arena.map = mp;

    // ATB_TEAM overrides team size (1 = 1v1, 2 = 2v2, ...) so you can sweep formats
    // without editing rules.json. Clamped to >= 1.
    if (const char* tv = std::getenv("ATB_TEAM"); tv && *tv)
        ruleset.teamSize = std::max(1, std::atoi(tv));

    // ATB_BRAIN selects the AI both sides play (default: the beam search). An
    // unknown name is fatal — don't silently report the wrong AI's balance.
    const Brain* brain = &defaultBrain();
    if (const char* bn = std::getenv("ATB_BRAIN"); bn && *bn) {
        brain = brainByName(bn);
        if (!brain) {
            std::fprintf(stderr, "balance: unknown ATB_BRAIN='%s'; available:", bn);
            for (std::string_view n : brainNames()) std::fprintf(stderr, " %.*s", (int)n.size(), n.data());
            std::fprintf(stderr, "\n");
            return 1;
        }
    }

    // Static map (if named): all matches share it.
    std::optional<Grid> staticArena;
    if (!ruleset.arena.map.empty()) {
        const std::string& m = ruleset.arena.map;
        const bool isPath = m.size() >= 5 && m.compare(m.size() - 5, 5, ".json") == 0;
        const std::string mapPath = isPath ? m : dataDir + "/maps/" + m + ".json";
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
        Outcome o = runMatch(ruleset, catalog, defsA, defsB, rng(), arenaPtr, creatures, *brain);

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

    // ---- Per-spell aggregation (shared by the verdict, the table, and the CSV) ---
    struct Row { int id; double wr, lo, hi, pick; long n; };
    std::vector<Row> rows;
    for (const SpellDef& d : catalog.all()) {
        CI c = wilson(g.wins[d.id], g.appears[d.id]);
        rows.push_back({d.id, c.mean, c.lo, c.hi, double(g.appears[d.id]) / (2.0 * matches),
                        g.appears[d.id]});
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.wr > b.wr; });

    // Plain-language classification: 0 ok, 1 strong, 2 weak, 3 niche, 4 AI-unused.
    auto classify = [&](const Row& rw) -> int {
        if (rw.id == spellid::Portal) return 4;
        if (rw.pick < 0.08) return 3;          // <8% pick — niche / too costly
        if (pct(rw.lo) > 50.0) return 1;        // CI entirely above 50% — likely OP
        if (pct(rw.hi) < 50.0) return 2;        // CI entirely below 50% — likely weak
        return 0;
    };
    // CSV token, plain-English verdict (text/HTML tables), and HTML bar colour per class.
    static const char* kFlagTag[5] = {"ok", "strong", "weak", "niche", "ai-unused"};
    static const char* kVerdict[5] = {"balanced", "TOO STRONG", "too weak", "niche", "AI-unused"};
    static const char* kColor[5] = {"#27ae60", "#c0392b", "#2980b9", "#7f8c8d", "#95a5a6"};

    int nStrong = 0, nWeak = 0;
    for (const Row& rw : rows) {
        if (classify(rw) == 1) ++nStrong;
        else if (classify(rw) == 2) ++nWeak;
    }
    const int roundLen = std::max(1, 2 * teamSize); // half-turns per full round
    const double aFrac = matches ? double(aWins) / matches : 0.0;
    const double medHalf = percentile(lengths, .50);

    // ---- Compose report ----------------------------------------------------
    std::ostringstream r;
    auto line = [&](const std::string& s = "") { r << s << "\n"; };
    auto rule = [&]() { r << std::string(74, '-') << "\n"; };

    r << "================ TACTICAL BATTLER — BALANCE REPORT ================\n";
    line();

    // ---- Plain-English summary (read this first) ---------------------------
    {
        auto names = [&](int flag) {
            std::string s;
            for (const Row& rw : rows)
                if (classify(rw) == flag) {
                    if (const SpellDef* d = catalog.find(rw.id))
                        s += (s.empty() ? "" : ", ") + d->key;
                }
            return s;
        };
        line("SUMMARY  (plain English — the rest of the file is the detail behind this)");
        const int nSpells = static_cast<int>(rows.size());
        if (nStrong == 0 && nWeak == 0)
            r << "  * Balance: looks healthy — no spell is clearly too strong or too weak"
              << " (of " << nSpells << " tested).\n";
        else {
            r << "  * Balance: " << nStrong << " spell(s) look too strong, " << nWeak
              << " too weak (of " << nSpells << ").\n";
            if (nStrong) r << "      too strong: " << names(1) << "\n";
            if (nWeak)   r << "      too weak:   " << names(2) << "\n";
        }
        const double edge = pct(aFrac) - 50.0;
        r << "  * Going first wins " << std::lround(pct(aFrac)) << "% of games — "
          << (std::abs(edge) < 3.0 ? "about fair" : (edge > 0 ? "a first-move advantage"
                                                             : "a second-move advantage"))
          << " (50% = perfectly fair).\n";
        long dec0 = aWins + bWins;
        r << "  * Most games are decided by a spell kill (" << std::lround(dec0 ? pct(double(bySpell) / dec0) : 0)
          << "%); a typical game lasts about " << std::lround(medHalf / roundLen) << " round(s).\n";
        if (draws) r << "  * " << draws << " game(s) hit the turn cap without a winner (counted as draws).\n";
        line("  * Charts: open the .html file in a browser. Raw tables: the .csv files (spreadsheet).");
    }
    line();
    rule();
    line("RUN");
    r << "  matches            " << matches << "\n";
    r << "  seed               " << seed << "\n";
    r << "  wall time          " << elapsed << " s  ("
      << (elapsed > 0 ? matches / elapsed : 0) << " matches/s)\n";
    r << "  ruleset            " << rulesetSource << "\n";
    r << "  AI brain           " << brain->name() << "\n";
    r << "  format             " << teamSize << "v" << teamSize << "\n";
    if (staticArena)
        r << "  arena              static map '" << ruleset.arena.map << "'\n";
    else
        r << "  arena              random " << ruleset.arena.width << "x" << ruleset.arena.height
          << " (coverage " << ruleset.arena.coverage << ")\n";
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
    r << "MATCH LENGTH (half-turns; one actor acts per half-turn, " << roundLen
      << " = 1 full round)\n";
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
    line("  a spell is 'balanced' near 50%; 'TOO STRONG'/'too weak' = its whole CI is above/below 50%");
    line("  spell        cost   pick%   winrate  [   95% CI   ]   lift   val/pt   verdict");
    for (const Row& row : rows) {
        const SpellDef* d = catalog.find(row.id);
        double lift = pct(row.wr) - 50.0;
        double valPerPt = d->buildCost ? lift / d->buildCost : 0.0;
        char buf[176];
        std::snprintf(buf, sizeof buf,
                      "  %-10s   %2dpt  %5.1f%%  %6.1f%%  [%5.1f–%5.1f]  %+5.1f  %+6.2f   %s",
                      d->key.c_str(), d->buildCost, pct(row.pick), pct(row.wr), pct(row.lo),
                      pct(row.hi), lift, valPerPt, kVerdict[classify(row)]);
        r << buf << "\n";
    }
    line();
    line("STAT INVESTMENT");
    auto statRow = [&](const char* name, long w, long n) {
        CI c = wilson(w, n);
        char buf[160];
        // n < 20 is too small a sample to read anything into — say so instead of
        // printing a wild 0%/100% that looks like a finding.
        if (n < 20)
            std::snprintf(buf, sizeof buf, "  %s     -       (too few: n=%ld)", name, n);
        else
            std::snprintf(buf, sizeof buf, "  %s  %5.1f%%  [%4.1f–%4.1f]  n=%ld", name, pct(c.mean),
                          pct(c.lo), pct(c.hi), n);
        r << buf << "\n";
    };
    statRow("bought +HP   ", g.hpWin, g.hpAppear);
    statRow("bought +AP   ", g.apWin, g.apAppear);
    statRow("bought +MP   ", g.mpWin, g.mpAppear);
    statRow("bought +Init ", g.initWin, g.initAppear);
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
    line("  - winrate = share of games won by builds that included the spell (50% = neutral).");
    line("  - verdict: 'balanced' = CI straddles 50%; 'TOO STRONG'/'too weak' = whole CI above/");
    line("    below 50%; 'niche' = picked <8% of the time; 'AI-unused' = planner never casts it.");
    line("  - [95% CI] is the plausible winrate range; a wider range means fewer samples.");
    line("  - lift = winrate - 50%. val/pt = lift per build point (cost efficiency).");
    line("  - 'vs solo-avg' > 0 means the pair wins more than its two spells do alone");
    line("    (emergent synergy, e.g. poison + invisible).");
    line("  - Non-overlapping CIs between two spells => a real difference, not noise.");
    line("  - Portal is AI-unused; treat its row as a point opportunity-cost baseline.");
    line("  - Build generator fills spells first, stats with leftover points, so the stat");
    line("    section compares lean+stats builds against spell-crammed ones (a real signal:");
    line("    extra spells you're too AP-starved to cast lose to durability).");

    // ---- Emit: text to stdout + text/CSV/HTML files ------------------------
    std::cout << r.str();
    { std::ofstream f(outfile); if (f) f << r.str(); }

    // Derive sibling file names from the report name (strip a trailing ".txt").
    std::string base = outfile;
    if (base.size() > 4 && base.compare(base.size() - 4, 4, ".txt") == 0) base.resize(base.size() - 4);
    auto f1 = [](double v) { char b[32]; std::snprintf(b, sizeof b, "%.1f", v); return std::string(b); };
    auto f2 = [](double v) { char b[32]; std::snprintf(b, sizeof b, "%.2f", v); return std::string(b); };

    // ---- CSV tables (clean numeric columns, no formatting — open in a spreadsheet) ---
    {
        std::ofstream c(base + ".spells.csv");
        c << "spell,cost,pick_pct,winrate_pct,ci_lo_pct,ci_hi_pct,lift,val_per_pt,n,verdict\n";
        for (const Row& row : rows) {
            const SpellDef* d = catalog.find(row.id);
            double lift = pct(row.wr) - 50.0, vpp = d->buildCost ? lift / d->buildCost : 0.0;
            c << d->key << ',' << d->buildCost << ',' << f1(pct(row.pick)) << ',' << f1(pct(row.wr))
              << ',' << f1(pct(row.lo)) << ',' << f1(pct(row.hi)) << ',' << f1(lift) << ','
              << f2(vpp) << ',' << row.n << ',' << kFlagTag[classify(row)] << "\n";
        }
    }
    {
        std::ofstream c(base + ".pairs.csv");
        c << "spell_a,spell_b,winrate_pct,n,vs_solo_avg_pct\n";
        for (const Pair& p : pairs) {
            double soloAvg = (solo(p.a) + solo(p.b)) / 2.0;
            c << catalog.find(p.a)->key << ',' << catalog.find(p.b)->key << ',' << f1(pct(p.wr))
              << ',' << p.n << ',' << f1(pct(p.wr - soloAvg)) << "\n";
        }
    }
    {
        std::ofstream c(base + ".length.csv");
        c << "half_turns,count\n";
        for (size_t i = 0; i < lengths.size();) { // lengths is sorted: one row per distinct value
            int v = lengths[i]; size_t j = i;
            while (j < lengths.size() && lengths[j] == v) ++j;
            c << v << ',' << (j - i) << "\n";
            i = j;
        }
    }
    {
        std::ofstream c(base + ".outcomes.csv");
        c << "metric,count,pct\n";
        long dec = aWins + bWins;
        auto row2 = [&](const char* k, long v, long tot) {
            c << k << ',' << v << ',' << f1(tot ? pct(double(v) / tot) : 0.0) << "\n";
        };
        row2("a_wins_first", aWins, matches);
        row2("b_wins_second", bWins, matches);
        row2("draws", draws, matches);
        row2("end_spell_kill", bySpell, dec);
        row2("end_ring_storm", byStorm, dec);
        row2("end_collision", byCollision, dec);
    }

    // ---- Self-contained HTML report (inline SVG; no external/CDN dependencies) ---
    {
        auto esc = [](const std::string& s) {
            std::string o;
            for (char ch : s) o += ch == '&' ? "&amp;" : ch == '<' ? "&lt;" : ch == '>' ? "&gt;"
                                                                                        : std::string(1, ch);
            return o;
        };
        std::ostringstream h;
        h << "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
             "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
             "<title>Balance Report</title><style>"
             "body{font:14px/1.5 system-ui,sans-serif;margin:24px;color:#222;max-width:920px}"
             "h1{font-size:20px}h2{font-size:16px;margin-top:28px;border-bottom:1px solid #ddd;padding-bottom:4px}"
             ".meta{color:#555;font-size:13px}.meta b{color:#222}"
             ".sum{background:#f6f8fa;border:1px solid #e1e4e8;border-radius:6px;padding:8px 16px}"
             "table{border-collapse:collapse;font-size:13px}td,th{padding:3px 10px;text-align:left}"
             "th{border-bottom:1px solid #ccc}tr:nth-child(even){background:#fafafa}svg{max-width:100%}"
             ".legend span{display:inline-block;margin-right:14px;font-size:12px}"
             ".sw{display:inline-block;width:11px;height:11px;border-radius:2px;vertical-align:middle;margin-right:4px}"
             "</style></head><body>\n";
        h << "<h1>Tactical Battler — Balance Report</h1>\n";
        h << "<p class=\"meta\"><b>" << matches << "</b> matches &middot; seed <b>" << seed
          << "</b> &middot; " << teamSize << "v" << teamSize << " &middot; ruleset <b>"
          << esc(rulesetSource) << "</b> &middot; catalog <b>" << esc(catalogSource) << "</b></p>\n";

        h << "<div class=\"sum\"><ul>";
        if (nStrong == 0 && nWeak == 0)
            h << "<li><b>Balance looks healthy</b> — no spell is clearly too strong or too weak (of "
              << rows.size() << ").</li>";
        else
            h << "<li><b>" << nStrong << "</b> spell(s) too strong, <b>" << nWeak << "</b> too weak (of "
              << rows.size() << ").</li>";
        double edge = pct(aFrac) - 50.0;
        h << "<li>Going first wins <b>" << std::lround(pct(aFrac)) << "%</b> — "
          << (std::abs(edge) < 3.0 ? "about fair"
                                   : (edge > 0 ? "a first-move advantage" : "a second-move advantage"))
          << " (50% = fair).</li>";
        h << "<li>A typical game lasts ~<b>" << std::lround(medHalf / roundLen)
          << "</b> round(s); most are decided by a spell kill.</li>";
        h << "</ul></div>\n";

        h << "<p class=\"legend\">";
        const char* ln[5] = {"balanced", "too strong", "too weak", "niche", "AI-unused"};
        for (int k = 0; k < 5; ++k)
            h << "<span><span class=\"sw\" style=\"background:" << kColor[k] << "\"></span>" << ln[k]
              << "</span>";
        h << "</p>\n";

        // Per-spell win-rate chart: bars 0..100%, dashed 50% line, black 95%-CI whiskers.
        h << "<h2>Per-spell win rate</h2>\n";
        h << "<p class=\"meta\">Each bar = the share of games won by builds that include that spell "
             "(50% = neutral). The black line is the <b>confidence range</b>: where the true win rate "
             "most likely sits &mdash; a wider line means fewer games, so less certainty. "
             "Bar colour = verdict (see key above).</p>\n";
        {
            const int n = static_cast<int>(rows.size());
            const int x0 = 130, barW = 560, rh = 22, top = 26, W = x0 + barW + 70, H = top + n * rh + 10;
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
                const Row& row = rows[i];
                const SpellDef* d = catalog.find(row.id);
                int fl = classify(row);
                double yc = top + i * rh + rh / 2.0, bx = X(pct(row.wr));
                double xl = X(pct(row.lo)), xh = X(pct(row.hi));
                h << "<text x=\"8\" y=\"" << yc + 4 << "\" font-size=\"12\">" << esc(d->key) << "</text>"
                  << "<rect x=\"" << x0 << "\" y=\"" << top + i * rh + 4 << "\" width=\""
                  << std::max(0.0, bx - x0) << "\" height=\"" << rh - 8 << "\" fill=\"" << kColor[fl]
                  << "\" rx=\"2\"/>"
                  << "<line x1=\"" << xl << "\" y1=\"" << yc << "\" x2=\"" << xh << "\" y2=\"" << yc
                  << "\" stroke=\"#333\"/>"
                  << "<line x1=\"" << xl << "\" y1=\"" << yc - 4 << "\" x2=\"" << xl << "\" y2=\"" << yc + 4
                  << "\" stroke=\"#333\"/>"
                  << "<line x1=\"" << xh << "\" y1=\"" << yc - 4 << "\" x2=\"" << xh << "\" y2=\"" << yc + 4
                  << "\" stroke=\"#333\"/>"
                  << "<text x=\"" << x0 + barW + 6 << "\" y=\"" << yc + 4 << "\" font-size=\"11\">"
                  << f1(pct(row.wr)) << "%</text>\n";
            }
            h << "</svg>\n";
        }

        // Match-length histogram (buckets of 8 half-turns).
        h << "<h2>Match length (half-turns, " << roundLen << " = 1 round)</h2>\n";
        {
            const int bw = 8, nb = 7;
            std::array<long, 7> hist{};
            for (int v : lengths) hist[std::min(v / bw, nb - 1)]++;
            long mx = 1;
            for (long v : hist) mx = std::max(mx, v);
            const int x0 = 80, barW = 520, rh = 22, top = 8, W = x0 + barW + 60, H = top + nb * rh + 6;
            h << "<svg width=\"" << W << "\" height=\"" << H << "\" viewBox=\"0 0 " << W << " " << H << "\">\n";
            for (int i = 0; i < nb; ++i) {
                char lbl[24];
                if (i == nb - 1) std::snprintf(lbl, sizeof lbl, "%d+", i * bw);
                else std::snprintf(lbl, sizeof lbl, "%d-%d", i * bw, i * bw + bw - 1);
                double yc = top + i * rh + rh / 2.0, w = double(hist[i]) / mx * barW;
                h << "<text x=\"8\" y=\"" << yc + 4 << "\" font-size=\"11\">" << lbl << "</text>"
                  << "<rect x=\"" << x0 << "\" y=\"" << top + i * rh + 3 << "\" width=\"" << w
                  << "\" height=\"" << rh - 6 << "\" fill=\"#5b8def\" rx=\"2\"/>"
                  << "<text x=\"" << x0 + w + 6 << "\" y=\"" << yc + 4 << "\" font-size=\"11\">" << hist[i]
                  << "</text>\n";
            }
            h << "</svg>\n";
        }

        // Outcomes + how decisive games end.
        h << "<h2>Outcomes</h2>\n";
        {
            long dec = aWins + bWins;
            struct B { const char* name; long v; long tot; const char* col; };
            std::array<B, 6> bs = {{{"A wins (1st)", aWins, matches, "#27ae60"},
                                    {"B wins (2nd)", bWins, matches, "#2980b9"},
                                    {"draws (cap)", draws, matches, "#95a5a6"},
                                    {"end: spell kill", bySpell, dec, "#e67e22"},
                                    {"end: ring/storm", byStorm, dec, "#8e44ad"},
                                    {"end: collision", byCollision, dec, "#c0392b"}}};
            const int x0 = 120, barW = 480, rh = 22, top = 8, W = x0 + barW + 70,
                      H = top + static_cast<int>(bs.size()) * rh + 6;
            h << "<svg width=\"" << W << "\" height=\"" << H << "\" viewBox=\"0 0 " << W << " " << H << "\">\n";
            for (size_t i = 0; i < bs.size(); ++i) {
                double frac = bs[i].tot ? double(bs[i].v) / bs[i].tot : 0.0;
                double yc = top + i * rh + rh / 2.0;
                h << "<text x=\"8\" y=\"" << yc + 4 << "\" font-size=\"11\">" << bs[i].name << "</text>"
                  << "<rect x=\"" << x0 << "\" y=\"" << top + i * rh + 3 << "\" width=\"" << frac * barW
                  << "\" height=\"" << rh - 6 << "\" fill=\"" << bs[i].col << "\" rx=\"2\"/>"
                  << "<text x=\"" << x0 + frac * barW + 6 << "\" y=\"" << yc + 4 << "\" font-size=\"11\">"
                  << f1(pct(frac)) << "%</text>\n";
            }
            h << "</svg>\n";
        }

        // Win-rate bar chart (0..100% with a dashed 50% line + CI whiskers), reused
        // for both stat views below. Rows with n < 20 print "too few" instead of a bar.
        struct WR { std::string name; long w; long n; };
        auto winrateChart = [&](const std::vector<WR>& items) {
            const int n = static_cast<int>(items.size());
            const int x0 = 130, barW = 480, rh = 22, top = 26, W = x0 + barW + 130, H = top + n * rh + 10;
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
                h << "<text x=\"8\" y=\"" << yc + 4 << "\" font-size=\"12\">" << items[i].name << "</text>";
                if (items[i].n < 20) {
                    h << "<text x=\"" << x0 + 4 << "\" y=\"" << yc + 4
                      << "\" font-size=\"11\" fill=\"#888\">(too few: n=" << items[i].n << ")</text>\n";
                    continue;
                }
                CI c = wilson(items[i].w, items[i].n);
                double bx = X(pct(c.mean)), xl = X(pct(c.lo)), xh = X(pct(c.hi));
                h << "<rect x=\"" << x0 << "\" y=\"" << top + i * rh + 4 << "\" width=\""
                  << std::max(0.0, bx - x0) << "\" height=\"" << rh - 8 << "\" fill=\""
                  << (pct(c.lo) > 50.0 ? "#27ae60" : pct(c.hi) < 50.0 ? "#c0392b" : "#7f8c8d") << "\" rx=\"2\"/>"
                  << "<line x1=\"" << xl << "\" y1=\"" << yc << "\" x2=\"" << xh << "\" y2=\"" << yc
                  << "\" stroke=\"#333\"/>"
                  << "<text x=\"" << x0 + barW + 6 << "\" y=\"" << yc + 4 << "\" font-size=\"11\">"
                  << f1(pct(c.mean)) << "% (n=" << items[i].n << ")</text>\n";
            }
            h << "</svg>\n";
        };

        h << "<h2>Stat spending — win rate by what the build bought</h2>\n";
        h << "<p class=\"meta\">Each bar = win rate of builds that spent points on that stat (bar past "
             "the dashed 50% line = wins more than half). Points go to spells first and stats with the "
             "leftover, so this compares lean builds with stat padding against spell-crammed ones.</p>\n";
        winrateChart({{"bought +HP", g.hpWin, g.hpAppear},
                      {"bought +AP", g.apWin, g.apAppear},
                      {"bought +MP", g.mpWin, g.mpAppear},
                      {"bought +Init", g.initWin, g.initAppear},
                      {"no stat spend", g.noStatWin, g.noStatAppear}});

        h << "<h2>Stat spending — win rate by total points spent on stats</h2>\n";
        h << "<p class=\"meta\">Groups builds by how many of their points went to stats (rather than "
             "spells). A rising trend means durability/tempo beats cramming extra spells.</p>\n";
        winrateChart({{"0 pts (all spells)", g.bucketW[0], g.bucketN[0]},
                      {"1-3 pts", g.bucketW[1], g.bucketN[1]},
                      {"4-6 pts", g.bucketW[2], g.bucketN[2]},
                      {"7+ pts", g.bucketW[3], g.bucketN[3]}});

        h << "<h2>Top spell synergies</h2>\n";
        h << "<p class=\"meta\"><b>Synergy</b> = how much more the pair wins than the two spells do on "
             "their own (average). Positive = better together than apart. <b>Games together</b> = number "
             "of simulated games whose build had both spells.</p>\n";
        h << "<table><tr><th>spell pair</th><th>win rate together</th><th>games together</th>"
             "<th>synergy (vs. each alone)</th></tr>\n";
        for (size_t i = 0; i < pairs.size() && i < 12; ++i) {
            const Pair& p = pairs[i];
            double dv = pct(p.wr - (solo(p.a) + solo(p.b)) / 2.0);
            h << "<tr><td>" << esc(catalog.find(p.a)->key) << " + " << esc(catalog.find(p.b)->key)
              << "</td><td>" << f1(pct(p.wr)) << "%</td><td>" << p.n << "</td><td>" << (dv >= 0 ? "+" : "")
              << f1(dv) << " pts</td></tr>\n";
        }
        h << "</table>\n";
        h << "<p class=\"meta\">Full text detail: " << esc(outfile)
          << " &middot; raw data: *.spells.csv, *.pairs.csv, *.length.csv, *.outcomes.csv</p>\n";
        h << "</body></html>\n";
        std::ofstream hf(base + ".html");
        if (hf) hf << h.str();
    }

    std::cout << "\n(reports written:\n  text  " << outfile << "\n  html  " << base << ".html"
              << "\n  csv   " << base << ".spells.csv, " << base << ".pairs.csv, " << base
              << ".length.csv, " << base << ".outcomes.csv)\n";
    return 0;
}
