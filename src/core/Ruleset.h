#pragma once
//
// Ruleset.h — A match's rules, as data.
//
// One struct that fully describes *how* a match is set up: team format, the
// build economy, banned spells, the closing ring, and the arena. Loaded from
// data/rules.json (data/RulesetJson) and consumed by the shared match builder so
// the game and the balance sim construct matches identically. The third pinned,
// hashable artifact beside the catalog and the bestiary (ARCHITECTURE §5/§7).
//
#include "Build.h"  // BuildRules (the economy)
#include "Storm.h"  // StormConfig (the closing ring)

#include <string>
#include <vector>

namespace tb {

// Arena parameters. If `map` is set, that static map is loaded (data/maps/<map>.json)
// instead of generating a random arena from width/height/coverage.
struct ArenaRules {
    int width = 21;
    int height = 15;
    double coverage = 0.18; // fraction of tiles that become wall/obstacle (random arenas)
    std::string map;        // static map key; empty = procedurally generate
};

struct Ruleset {
    int teamSize = 1;                       // champions per team: 1/2/3 → NvN
    BuildRules economy{};                   // budget + base stats + per-point costs
    std::vector<std::string> bannedSpells;  // spell keys excluded from builds
    StormConfig closingRing{};              // the arena-reduction ring
    ArenaRules arena{};
};

// The compiled default ruleset — the fallback when data/rules.json is absent, and
// the seed tb_ruleset_gen scaffolds from.
[[nodiscard]] inline Ruleset makeDefaultRuleset() { return Ruleset{}; }

} // namespace tb
