//
// map_demo.cpp — Test for the static-map loader (data/MapJson).
// Char grid -> Grid, with strict validation incl. the spawn-reachability gate.
//
#include "data/MapJson.h"

#include <cstdio>
#include <fstream>
#include <string>

using namespace tb;

static int g_fails = 0;
#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) std::printf("  [PASS] %s\n", msg);                                               \
        else { std::printf("  [FAIL] %s\n", msg); ++g_fails; }                                     \
    } while (0)

static bool rejectsWith(const std::string& json, const std::string& needle) {
    MapLoad r = loadMapFromString(json);
    if (r.ok) return false;
    for (const std::string& e : r.errors)
        if (e.find(needle) != std::string::npos) return true;
    return false;
}

int main() {
    std::printf("Load a valid map\n");
    {
        const char* src = R"({
            "schema": 1, "version": "1.0.0", "name": "t",
            "tiles": [
              "......",
              ".#..o.",
              "......",
              "..##..",
              "......"
            ]
        })";
        MapLoad m = loadMapFromString(src);
        CHECK(m.ok, "valid map loads");
        CHECK(m.grid.width() == 6 && m.grid.height() == 5, "dimensions from rows");
        CHECK(m.grid.at(Vec2i{1, 1}) == TileType::Wall, "'#' -> Wall");
        CHECK(m.grid.at(Vec2i{4, 1}) == TileType::Obstacle, "'o' -> Obstacle");
        CHECK(m.grid.at(Vec2i{0, 0}) == TileType::Walkable, "'.' -> Walkable");
        CHECK(!m.sha256.empty(), "sha256 computed");
    }

    std::printf("Strict validation\n");
    {
        CHECK(rejectsWith(R"({"schema":1,"version":"x","tiles":["....","...."]})", "3x3"),
              "too small rejected");
        CHECK(rejectsWith(R"({"schema":1,"version":"x","tiles":["....","..","...."]})",
                          "equal length"),
              "ragged rows rejected");
        CHECK(rejectsWith(R"({"schema":1,"version":"x","tiles":["..X..",".....","....."]})",
                          "unknown tile char"),
              "bad char rejected");
        // A wall slab spanning the full height between the spawns: unreachable.
        CHECK(rejectsWith(R"({"schema":1,"version":"x","tiles":[".....#....",".....#....",".....#....",".....#....",".....#...."]})",
                          "not connected"),
              "blocked-off spawns rejected");
        CHECK(rejectsWith(R"({"schema":1,"version":"x","tiles":["....","....","...."],"wat":1})",
                          "unknown field"),
              "unknown field rejected");
    }

    std::printf("Shipped sample map (if present)\n");
    {
        if (std::ifstream("data/maps/duel.json").good()) {
            MapLoad m = loadMapFromFile("data/maps/duel.json");
            CHECK(m.ok, "data/maps/duel.json loads + validates");
            CHECK(m.grid.width() == 20 && m.grid.height() == 15, "duel is 20x15");
        } else {
            std::printf("  [skip] data/maps/duel.json not found from CWD\n");
        }
    }

    std::printf(g_fails == 0 ? "\nALL PASS (0 failures)\n" : "\n%d FAILURE(S)\n", g_fails);
    return g_fails == 0 ? 0 : 1;
}
