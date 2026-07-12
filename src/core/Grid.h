#pragma once
//
// Grid.h — Headless arena representation.
//
// Pure data + free functions. No Raylib, no rendering, no game state.
// A flat std::vector<TileType> in row-major order gives us a cache-friendly
// layout that BFS / LOS sweeps can stream through linearly.
//
#include <cstdint>
#include <vector>

namespace tb {

// ---------------------------------------------------------------------------
// Basic value types
// ---------------------------------------------------------------------------

struct Vec2i {
    int x = 0;
    int y = 0;

    friend constexpr bool operator==(Vec2i a, Vec2i b) { return a.x == b.x && a.y == b.y; }
    friend constexpr bool operator!=(Vec2i a, Vec2i b) { return !(a == b); }
};

// Manhattan distance — used for spell range checks (grid-style measurement).
[[nodiscard]] constexpr int manhattan(Vec2i a, Vec2i b) {
    int dx = a.x - b.x;
    int dy = a.y - b.y;
    return (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
}

enum class TileType : std::uint8_t {
    Walkable, // free to traverse, transparent to LOS
    Wall,     // blocks movement AND line of sight
    Obstacle  // blocks movement, but LOS passes over it (e.g. a pit / low cover)
};

[[nodiscard]] constexpr bool blocksMovement(TileType t) { return t != TileType::Walkable; }
[[nodiscard]] constexpr bool blocksLineOfSight(TileType t) { return t == TileType::Wall; }

// ---------------------------------------------------------------------------
// Grid
// ---------------------------------------------------------------------------

class Grid {
public:
    Grid() = default;
    Grid(int width, int height)
        : width_(width), height_(height), tiles_(static_cast<std::size_t>(width) * height, TileType::Walkable) {}

    [[nodiscard]] int width() const { return width_; }
    [[nodiscard]] int height() const { return height_; }

    [[nodiscard]] bool inBounds(Vec2i p) const {
        return p.x >= 0 && p.y >= 0 && p.x < width_ && p.y < height_;
    }

    [[nodiscard]] TileType at(Vec2i p) const { return tiles_[index(p)]; }
    void set(Vec2i p, TileType t) { tiles_[index(p)] = t; }

    [[nodiscard]] bool isWalkable(Vec2i p) const {
        return inBounds(p) && !blocksMovement(at(p));
    }

    [[nodiscard]] std::size_t index(Vec2i p) const {
        return static_cast<std::size_t>(p.y) * width_ + p.x;
    }

private:
    int width_ = 0;
    int height_ = 0;
    std::vector<TileType> tiles_;
};

// ---------------------------------------------------------------------------
// Generation
// ---------------------------------------------------------------------------

struct ArenaConfig {
    int width = 21;
    int height = 15;
    float coverage = 0.18f;     // fraction of tiles that become wall/obstacle
    float obstacleRatio = 0.4f; // of the covered tiles, how many are see-through obstacles
    Vec2i playerSpawn{1, 7};
    Vec2i enemySpawn{19, 7};
    unsigned int seed = 0;      // 0 => derive from clock
};

// Generates an arena, scattering walls/obstacles per the config, and guarantees
// a walkable path exists between the two spawns (regenerates until valid).
[[nodiscard]] Grid generateArena(const ArenaConfig& cfg);

// ---------------------------------------------------------------------------
// Queries (pure, reusable by gameplay and AI)
// ---------------------------------------------------------------------------

// True if any walkable path connects start and goal (BFS reachability).
// `blocked` tiles are treated as impassable in addition to the grid's own walls
// (used to account for entity occupancy).
[[nodiscard]] bool isReachable(const Grid& grid, Vec2i start, Vec2i goal,
                               const std::vector<Vec2i>& blocked = {});

// Shortest walkable path from start to goal (inclusive of both endpoints).
// Returns empty if unreachable. `blocked` are extra impassable tiles, except the
// goal itself is always considered targetable.
[[nodiscard]] std::vector<Vec2i> findPath(const Grid& grid, Vec2i start, Vec2i goal,
                                          const std::vector<Vec2i>& blocked = {});

// All tiles reachable from `start` within `maxSteps` MP (BFS flood, excludes the
// start tile). Used for movement-range highlighting and AI scoring.
[[nodiscard]] std::vector<Vec2i> reachableWithin(const Grid& grid, Vec2i start, int maxSteps,
                                                 const std::vector<Vec2i>& blocked = {});

// BFS walking distance (in tiles) from `source` to every tile; -1 where
// unreachable. Used by the AI to score closing on a target around obstacles.
[[nodiscard]] std::vector<int> distanceField(const Grid& grid, Vec2i source,
                                             const std::vector<Vec2i>& blocked = {});

// Bresenham line-of-sight: true if no Wall tile (and none of the extra opaque
// tiles, e.g. temporary Shelter walls) sits between a and b.
[[nodiscard]] bool hasLineOfSight(const Grid& grid, Vec2i a, Vec2i b,
                                  const std::vector<Vec2i>& extraOpaque = {});

} // namespace tb
