#pragma once
//
// PackManifest.h — the *data* half of a presentation pack (§6 in ARCHITECTURE).
//
// Deliberately raylib-free so it can be parsed + validated headlessly (and unit
// tested without a GL context). `SpritePack` (the raylib half) owns the textures
// and the actual draw/resolve; this file only turns `pack.json` into a struct.
//
// A pack maps stable semantic keys → sprites (atlas sub-rects) and/or palette
// colours. Every key is optional and resolved independently, so a pack may
// restyle just the walls, or ship only a palette and no art at all.
//
#include <string>
#include <unordered_map>
#include <vector>

namespace tb::render {

struct RGBA {
    unsigned char r = 0, g = 0, b = 0, a = 255;
};

struct SpriteDef {
    std::string atlas;                 // key into PackManifest::atlases
    int x = 0, y = 0, w = 0, h = 0;    // sub-rectangle into that atlas
    enum class Anchor { Center, Bottom } anchor = Anchor::Center;
    // `anim` / `cast` clips are reserved in the manifest (v1 ignores them; parsing
    // tolerates them so packs can author ahead of the animation milestone).
};

struct PackManifest {
    int schema = 1;
    std::string name, version;
    int tileSize = 32;
    std::vector<std::pair<std::string, std::string>> atlases; // name → image filename (ordered)
    std::unordered_map<std::string, SpriteDef> sprites;       // dotted key → sprite (e.g. spells.fireball)
    std::unordered_map<std::string, RGBA> palette;            // short key → colour (e.g. wall)

    [[nodiscard]] const SpriteDef* findSprite(const std::string& key) const {
        auto it = sprites.find(key);
        return it == sprites.end() ? nullptr : &it->second;
    }
    [[nodiscard]] const RGBA* findPalette(const std::string& key) const {
        auto it = palette.find(key);
        return it == palette.end() ? nullptr : &it->second;
    }
    [[nodiscard]] const std::string* findAtlasFile(const std::string& name) const {
        for (const auto& [n, file] : atlases)
            if (n == name) return &file;
        return nullptr;
    }
};

struct PackManifestLoad {
    bool ok = false;
    PackManifest manifest;
    std::vector<std::string> errors;
};

// Same valid/absent/malformed contract as the catalog/creature/ruleset loaders:
// all errors collected with context; `ok` false leaves `manifest` at defaults.
[[nodiscard]] PackManifestLoad loadPackManifestFromString(const std::string& text);
[[nodiscard]] PackManifestLoad loadPackManifestFromFile(const std::string& path);

} // namespace tb::render
