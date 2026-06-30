#include "MapJson.h"

#include "JsonRead.h"
#include "Sha256.h"

#include <fstream>
#include <sstream>
#include <string>

namespace tb {

namespace {

bool tileFromChar(char c, TileType& out) {
    switch (c) {
        case '.': out = TileType::Walkable; return true;
        case '#': out = TileType::Wall; return true;
        case 'o': out = TileType::Obstacle; return true;
        default: return false;
    }
}

} // namespace

MapLoad loadMapFromString(const std::string& text) {
    MapLoad result;
    result.sha256 = sha256Hex(text);

    json::ParseResult pr = json::parse(text);
    if (!pr.ok) {
        result.errors.push_back("JSON parse error: " + pr.error);
        return result;
    }
    const json::Value& root = pr.value;
    if (!root.isObject()) {
        result.errors.push_back("root: expected an object");
        return result;
    }
    auto& e = result.errors;
    jsonread::checkAllowed(root, {"schema", "version", "name", "tiles"}, "root", e);

    int schema = 0;
    if (jsonread::wantInt(root, "schema", "root", schema, e) && schema != kMapSchemaVersion)
        e.push_back("root: unsupported schema " + std::to_string(schema) + " (this build supports " +
                    std::to_string(kMapSchemaVersion) + ")");

    if (const json::Value* v = root.find("version"); v && v->isString()) result.version = v->asString();
    else e.push_back("root: \"version\" must be a string");

    if (const json::Value* nv = root.find("name")) {
        if (nv->isString()) result.name = nv->asString();
        else e.push_back("root: \"name\" must be a string");
    }

    const json::Value* tiles = root.find("tiles");
    if (!tiles || !tiles->isArray() || tiles->asArray().empty()) {
        e.push_back("root: \"tiles\" must be a non-empty array of row strings");
        return result;
    }
    const json::Value::Array& rows = tiles->asArray();
    if (!rows[0].isString() || rows[0].asString().empty()) {
        e.push_back("tiles[0]: rows must be non-empty strings");
        return result;
    }
    const int width = static_cast<int>(rows[0].asString().size());
    const int height = static_cast<int>(rows.size());
    if (width < 3 || height < 3) e.push_back("map must be at least 3x3");

    Grid grid(width, height);
    for (int y = 0; y < height; ++y) {
        if (!rows[y].isString()) {
            e.push_back("tiles[" + std::to_string(y) + "]: must be a string");
            return result;
        }
        const std::string& row = rows[y].asString();
        if (static_cast<int>(row.size()) != width) {
            e.push_back("tiles[" + std::to_string(y) + "]: width " + std::to_string(row.size()) +
                        " != " + std::to_string(width) + " (rows must be equal length)");
            continue;
        }
        for (int x = 0; x < width; ++x) {
            TileType t;
            if (!tileFromChar(row[static_cast<std::size_t>(x)], t))
                e.push_back("tiles[" + std::to_string(y) + "]: unknown tile char '" +
                            std::string(1, row[static_cast<std::size_t>(x)]) + "' (use . # o)");
            else
                grid.set(Vec2i{x, y}, t);
        }
    }

    // The champion spawns buildMatch uses must be walkable and connected, or the
    // map is unplayable.
    if (e.empty()) {
        const Vec2i sp{1, height / 2}, ep{width - 2, height / 2};
        if (!grid.isWalkable(sp) || !grid.isWalkable(ep))
            e.push_back("spawn tiles (1," + std::to_string(height / 2) + ") and (" +
                        std::to_string(width - 2) + "," + std::to_string(height / 2) +
                        ") must be walkable");
        else if (!isReachable(grid, sp, ep))
            e.push_back("the two spawn tiles are not connected by a walkable path");
    }

    if (e.empty()) result.grid = std::move(grid);
    result.ok = e.empty();
    return result;
}

MapLoad loadMapFromFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        MapLoad result;
        result.errors.push_back("could not open map file: " + path);
        return result;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return loadMapFromString(ss.str());
}

} // namespace tb
