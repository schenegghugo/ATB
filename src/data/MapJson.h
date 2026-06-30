#pragma once
//
// MapJson.h — Load a static arena map as JSON into a Grid.
//
// A map is a rectangular tile grid authored as rows of characters:
//   '.' walkable   '#' wall (blocks move + LOS)   'o' obstacle (blocks move only)
// Referenced by the ruleset (`arena.map`) as an alternative to procedural
// generation. Validated on load — including that the canonical champion spawns are
// walkable and connected — so a broken map fails loudly instead of mid-match.
//
#include "../core/Grid.h"

#include <string>
#include <vector>

namespace tb {

inline constexpr int kMapSchemaVersion = 1;

struct MapLoad {
    bool ok = false;
    Grid grid;                       // valid when ok
    std::string name;
    std::string version;
    std::string sha256;              // digest of the source bytes (PvP anchor)
    std::vector<std::string> errors;
};

[[nodiscard]] MapLoad loadMapFromString(const std::string& json);
[[nodiscard]] MapLoad loadMapFromFile(const std::string& path);

} // namespace tb
