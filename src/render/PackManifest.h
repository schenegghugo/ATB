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
#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace tb::render {

struct RGBA {
    unsigned char r = 0, g = 0, b = 0, a = 255;
};

// "#RRGGBB" or "#RRGGBBAA" → RGBA. Returns false (recording an error under
// `ctx`) otherwise. Shared by the pack manifest and the UI theme loader.
bool parseHexColor(const std::string& s, const std::string& ctx, RGBA& out,
                   std::vector<std::string>& e);

// An animation clip: ordered atlas sub-rects played at `fps` on the sprite's
// atlas (§2.4). Two flavours live on a SpriteDef: `anim` (ambient, loops) and
// `cast` (event clip, played once when the matching engine event arrives). Each
// frame is {x, y, w, h} on the same atlas as the owning sprite.
struct Clip {
    std::vector<std::array<int, 4>> frames; // ordered sub-rects (JSON key: "rects")
    double fps = 8.0;
    bool loop = true;

    // The sub-rect to show `t` seconds into the clip. Ambient (loop) wraps; a
    // one-shot (!loop) holds its last frame past the end. `t < 0` → first frame.
    [[nodiscard]] const std::array<int, 4>& frameAt(double t) const {
        const int n = static_cast<int>(frames.size());
        if (n <= 1 || t <= 0.0) return frames.front();
        int i = static_cast<int>(t * fps);
        i = loop ? (i % n) : std::min(i, n - 1);
        return frames[static_cast<std::size_t>(i)];
    }
    // Total run time in seconds (a looping clip has no natural end → 0).
    [[nodiscard]] double duration() const {
        return loop || fps <= 0.0 ? 0.0 : frames.size() / fps;
    }
};

struct SpriteDef {
    std::string atlas;                 // key into PackManifest::atlases
    int x = 0, y = 0, w = 0, h = 0;    // sub-rectangle into that atlas (the static frame)
    enum class Anchor { Center, Bottom } anchor = Anchor::Center;
    std::optional<Clip> anim;          // ambient loop (else the static rect)
    std::optional<Clip> cast;          // one-shot, triggered by a Cast event
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
