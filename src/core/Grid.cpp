#include "Grid.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <queue>
#include <random>

namespace tb {

namespace {

// 4-directional neighbours (cardinal movement only — Dofus-style).
constexpr std::array<Vec2i, 4> kCardinals{{{1, 0}, {-1, 0}, {0, 1}, {0, -1}}};

// Build a boolean "is this tile blocked" mask combining grid walls + extra tiles.
// Returned as a flat vector aligned with grid indexing for O(1) lookups.
std::vector<char> buildBlockMask(const Grid& grid, const std::vector<Vec2i>& blocked) {
    std::vector<char> mask(static_cast<std::size_t>(grid.width()) * grid.height(), 0);
    for (Vec2i b : blocked) {
        if (grid.inBounds(b)) mask[grid.index(b)] = 1;
    }
    return mask;
}

} // namespace

// ---------------------------------------------------------------------------
// BFS core — fills a predecessor array. Returns the index of `goal` if reached,
// or -1. `goalBlockOverride` lets the goal tile be reachable even if flagged in
// the block mask (so we can path *to* an occupied tile's neighbourhood).
// ---------------------------------------------------------------------------
static bool bfs(const Grid& grid, Vec2i start, Vec2i goal,
                const std::vector<char>& blockMask, std::vector<int>* prevOut) {
    if (!grid.inBounds(start) || !grid.inBounds(goal)) return false;

    const int n = grid.width() * grid.height();
    std::vector<int> prev(n, -2); // -2 = unvisited, -1 = visited root
    std::queue<int> frontier;

    auto passable = [&](Vec2i p, bool isGoal) {
        if (!grid.isWalkable(p)) return false;
        if (!isGoal && blockMask[grid.index(p)]) return false;
        return true;
    };

    const int startIdx = static_cast<int>(grid.index(start));
    prev[startIdx] = -1;
    frontier.push(startIdx);

    const int goalIdx = static_cast<int>(grid.index(goal));

    while (!frontier.empty()) {
        int cur = frontier.front();
        frontier.pop();
        if (cur == goalIdx) {
            if (prevOut) *prevOut = std::move(prev);
            return true;
        }
        Vec2i cp{cur % grid.width(), cur / grid.width()};
        for (Vec2i d : kCardinals) {
            Vec2i np{cp.x + d.x, cp.y + d.y};
            if (!grid.inBounds(np)) continue;
            int ni = static_cast<int>(grid.index(np));
            if (prev[ni] != -2) continue;
            if (!passable(np, ni == goalIdx)) continue;
            prev[ni] = cur;
            frontier.push(ni);
        }
    }
    return false;
}

bool isReachable(const Grid& grid, Vec2i start, Vec2i goal, const std::vector<Vec2i>& blocked) {
    return bfs(grid, start, goal, buildBlockMask(grid, blocked), nullptr);
}

std::vector<Vec2i> findPath(const Grid& grid, Vec2i start, Vec2i goal,
                            const std::vector<Vec2i>& blocked) {
    std::vector<int> prev;
    if (!bfs(grid, start, goal, buildBlockMask(grid, blocked), &prev)) return {};

    std::vector<Vec2i> path;
    for (int cur = static_cast<int>(grid.index(goal)); cur != -1; cur = prev[cur]) {
        path.push_back(Vec2i{cur % grid.width(), cur / grid.width()});
    }
    std::reverse(path.begin(), path.end());
    return path;
}

std::vector<Vec2i> reachableWithin(const Grid& grid, Vec2i start, int maxSteps,
                                   const std::vector<Vec2i>& blocked) {
    std::vector<Vec2i> result;
    if (!grid.inBounds(start) || maxSteps <= 0) return result;

    const auto mask = buildBlockMask(grid, blocked);
    std::vector<int> dist(static_cast<std::size_t>(grid.width()) * grid.height(), -1);
    std::queue<int> frontier;

    const int startIdx = static_cast<int>(grid.index(start));
    dist[startIdx] = 0;
    frontier.push(startIdx);

    while (!frontier.empty()) {
        int cur = frontier.front();
        frontier.pop();
        if (dist[cur] >= maxSteps) continue;
        Vec2i cp{cur % grid.width(), cur / grid.width()};
        for (Vec2i d : kCardinals) {
            Vec2i np{cp.x + d.x, cp.y + d.y};
            if (!grid.inBounds(np) || !grid.isWalkable(np)) continue;
            int ni = static_cast<int>(grid.index(np));
            if (dist[ni] != -1 || mask[ni]) continue;
            dist[ni] = dist[cur] + 1;
            result.push_back(np);
            frontier.push(ni);
        }
    }
    return result;
}

std::vector<int> distanceField(const Grid& grid, Vec2i source, const std::vector<Vec2i>& blocked) {
    std::vector<int> dist(static_cast<std::size_t>(grid.width()) * grid.height(), -1);
    if (!grid.inBounds(source) || !grid.isWalkable(source)) return dist;

    const auto mask = buildBlockMask(grid, blocked);
    std::queue<int> frontier;
    const int startIdx = static_cast<int>(grid.index(source));
    dist[startIdx] = 0;
    frontier.push(startIdx);

    while (!frontier.empty()) {
        int cur = frontier.front();
        frontier.pop();
        Vec2i cp{cur % grid.width(), cur / grid.width()};
        for (Vec2i d : kCardinals) {
            Vec2i np{cp.x + d.x, cp.y + d.y};
            if (!grid.inBounds(np) || !grid.isWalkable(np)) continue;
            int ni = static_cast<int>(grid.index(np));
            if (dist[ni] != -1 || mask[ni]) continue;
            dist[ni] = dist[cur] + 1;
            frontier.push(ni);
        }
    }
    return dist;
}

// ---------------------------------------------------------------------------
// Bresenham line-of-sight
// ---------------------------------------------------------------------------
bool hasLineOfSight(const Grid& grid, Vec2i a, Vec2i b, const std::vector<Vec2i>& extraOpaque) {
    int x0 = a.x, y0 = a.y;
    const int x1 = b.x, y1 = b.y;
    const int dx = std::abs(x1 - x0);
    const int dy = -std::abs(y1 - y0);
    const int sx = x0 < x1 ? 1 : -1;
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    auto isOpaque = [&](Vec2i p) {
        if (blocksLineOfSight(grid.at(p))) return true;
        for (Vec2i o : extraOpaque)
            if (o == p) return true;
        return false;
    };

    while (true) {
        Vec2i p{x0, y0};
        // Endpoints never block their own sightline; interior opaque tiles do.
        if (p != a && p != b && grid.inBounds(p) && isOpaque(p))
            return false;
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Procedural generation
// ---------------------------------------------------------------------------
Grid generateArena(const ArenaConfig& cfg) {
    unsigned int seed = cfg.seed;
    if (seed == 0) {
        seed = static_cast<unsigned int>(
            std::chrono::steady_clock::now().time_since_epoch().count());
    }
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> roll(0.0f, 1.0f);

    const int totalTiles = cfg.width * cfg.height;
    const int targetCovered = static_cast<int>(totalTiles * cfg.coverage);

    // Keep a small clearance around each spawn so units never start boxed in.
    auto nearSpawn = [&](Vec2i p) {
        return manhattan(p, cfg.playerSpawn) <= 1 || manhattan(p, cfg.enemySpawn) <= 1;
    };

    for (int attempt = 0; attempt < 256; ++attempt) {
        Grid grid(cfg.width, cfg.height);

        int placed = 0;
        while (placed < targetCovered) {
            Vec2i p{static_cast<int>(roll(rng) * cfg.width),
                    static_cast<int>(roll(rng) * cfg.height)};
            if (!grid.inBounds(p)) continue;
            if (nearSpawn(p)) continue;
            if (grid.at(p) != TileType::Walkable) continue;

            grid.set(p, roll(rng) < cfg.obstacleRatio ? TileType::Obstacle : TileType::Wall);
            ++placed;
        }

        // Path-validation gate: regenerate instantly if spawns are disconnected.
        if (isReachable(grid, cfg.playerSpawn, cfg.enemySpawn)) {
            return grid;
        }
    }

    // Degenerate fallback (effectively never hit): empty walkable arena.
    return Grid(cfg.width, cfg.height);
}

} // namespace tb
