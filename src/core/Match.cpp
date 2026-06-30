#include "Match.h"

#include "Grid.h" // ArenaConfig, generateArena

#include <vector>

namespace tb {

namespace {

// Pick `count` walkable, unoccupied spawn tiles for a team, starting at column
// `colX` and spreading vertically around the centre (then to neighbouring columns
// toward the middle). For teamSize 1 this yields exactly (colX, height/2) — the
// historical spawn — so single-champion matches are unchanged.
std::vector<Vec2i> teamSpawns(const Grid& g, int colX, int count, std::vector<Vec2i>& occupied) {
    std::vector<Vec2i> out;
    const int cy = g.height() / 2;
    auto isFree = [&](Vec2i p) {
        if (!g.inBounds(p) || !g.isWalkable(p)) return false;
        for (Vec2i o : occupied)
            if (o == p) return false;
        return true;
    };
    const int toward = colX < g.width() / 2 ? 1 : -1; // step toward the centre
    for (int dx = 0; dx <= 4 && static_cast<int>(out.size()) < count; ++dx) {
        for (int k = 0; k < 2 * g.height() && static_cast<int>(out.size()) < count; ++k) {
            const int dy = (k % 2 == 0) ? k / 2 : -(k / 2 + 1); // 0,-1,1,-2,2,…
            const Vec2i p{colX + toward * dx, cy + dy};
            if (isFree(p)) {
                out.push_back(p);
                occupied.push_back(p);
            }
        }
    }
    return out;
}

} // namespace

Battle buildMatch(const Ruleset& rules, const std::vector<CharacterBuild>& playerTeam,
                  const std::vector<CharacterBuild>& enemyTeam, const SpellCatalog& catalog,
                  unsigned seed, std::vector<Entity> creatures) {
    ArenaConfig cfg;
    cfg.width = rules.arena.width;
    cfg.height = rules.arena.height;
    cfg.coverage = static_cast<float>(rules.arena.coverage);
    cfg.seed = seed;
    cfg.playerSpawn = {1, cfg.height / 2};
    cfg.enemySpawn = {cfg.width - 2, cfg.height / 2};
    Grid grid = generateArena(cfg);

    std::vector<Entity> roster;
    std::vector<Vec2i> occupied;
    auto placeTeam = [&](const std::vector<CharacterBuild>& team, Faction faction, int colX) {
        const std::vector<Vec2i> spawns = teamSpawns(grid, colX, static_cast<int>(team.size()), occupied);
        for (std::size_t i = 0; i < team.size(); ++i) {
            const Vec2i pos = i < spawns.size() ? spawns[i] : Vec2i{colX, cfg.height / 2};
            roster.push_back(instantiate(team[i], catalog, faction, pos, rules.economy));
        }
    };
    placeTeam(playerTeam, Faction::Player, 1);
    placeTeam(enemyTeam, Faction::Enemy, cfg.width - 2);

    Battle battle(std::move(grid), std::move(roster), rules.closingRing);
    battle.setCreatures(std::move(creatures));
    return battle;
}

} // namespace tb
