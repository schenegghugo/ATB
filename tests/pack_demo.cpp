//
// pack_demo.cpp — Test for the presentation-pack manifest loader
// (render/PackManifest). Raylib-free: validates pack.json → PackManifest without
// a GL context, so it runs in headless CI. Texture loading + draw live in
// render/SpritePack (GUI-only) and are verified in-game.
//
#include "render/PackManifest.h"

#include <cstdio>
#include <string>

using namespace tb::render;

static int g_fails = 0;
#define CHECK(cond, msg)                                                                            \
    do {                                                                                            \
        if (cond) std::printf("  [PASS] %s\n", msg);                                                \
        else { std::printf("  [FAIL] %s\n", msg); ++g_fails; }                                      \
    } while (0)

static bool rejectsWith(const std::string& json, const std::string& needle) {
    PackManifestLoad r = loadPackManifestFromString(json);
    if (r.ok) return false;
    for (const std::string& e : r.errors)
        if (e.find(needle) != std::string::npos) return true;
    return false;
}

int main() {
    std::printf("Load a full valid pack (atlas + sprites + palette)\n");
    {
        const char* src = R"({
            "schema": 1, "name": "Test Pack", "version": "1.0.0", "tileSize": 32,
            "atlases": { "main": "atlas.png" },
            "palette": { "wall": "#464e60", "floor": "#1e2230ff" },
            "sprites": {
              "tiles.wall":      { "atlas": "main", "rect": [0, 0, 32, 32] },
              "units.player":    { "atlas": "main", "rect": [0, 64, 32, 48], "anchor": "bottom" },
              "spells.fireball": { "atlas": "main", "rect": [0, 96, 32, 32],
                                   "cast": { "rects": [[0,96,32,32]], "fps": 12, "loop": false } }
            }
        })";
        PackManifestLoad r = loadPackManifestFromString(src);
        CHECK(r.ok, "valid pack loads");
        CHECK(r.manifest.name == "Test Pack", "name parsed");
        CHECK(r.manifest.tileSize == 32, "tileSize parsed");
        CHECK(r.manifest.findAtlasFile("main") && *r.manifest.findAtlasFile("main") == "atlas.png",
              "atlas filename parsed");
        const SpriteDef* fb = r.manifest.findSprite("spells.fireball");
        CHECK(fb && fb->w == 32 && fb->h == 32, "sprite rect parsed");
        const SpriteDef* pl = r.manifest.findSprite("units.player");
        CHECK(pl && pl->anchor == SpriteDef::Anchor::Bottom, "anchor 'bottom' parsed");
        CHECK(fb && fb->anchor == SpriteDef::Anchor::Center, "anchor defaults to center");
        const RGBA* wall = r.manifest.findPalette("wall");
        CHECK(wall && wall->r == 0x46 && wall->g == 0x4e && wall->b == 0x60 && wall->a == 255,
              "#RRGGBB palette parsed");
        const RGBA* floor = r.manifest.findPalette("floor");
        CHECK(floor && floor->a == 0xff, "#RRGGBBAA alpha parsed");
        CHECK(r.manifest.findSprite("nope") == nullptr, "missing key resolves to nullptr");
    }

    std::printf("Palette-only pack (pure re-theme, no art)\n");
    {
        const char* src = R"({ "schema": 1, "name": "Recolour", "palette": { "wall": "#ffffff" } })";
        PackManifestLoad r = loadPackManifestFromString(src);
        CHECK(r.ok, "palette-only pack is valid (no atlases/sprites required)");
        CHECK(r.manifest.findPalette("wall") != nullptr, "palette entry present");
    }

    std::printf("Strict validation rejects malformed packs\n");
    {
        CHECK(rejectsWith(R"({"schema": 2})", "schema"), "wrong schema rejected");
        CHECK(rejectsWith(R"({"schema":1,"bogus":1})", "unknown field"), "unknown top-level field rejected");
        CHECK(rejectsWith(R"({"schema":1,"sprites":{"x":{"rect":[0,0,8,8]}}})", "atlas"),
              "sprite missing atlas rejected");
        CHECK(rejectsWith(R"({"schema":1,"atlases":{"m":"a.png"},"sprites":{"x":{"atlas":"m","rect":[0,0,8]}}})",
                          "rect"),
              "sprite bad rect rejected");
        CHECK(rejectsWith(R"({"schema":1,"sprites":{"x":{"atlas":"missing","rect":[0,0,8,8]}}})",
                          "unknown atlas"),
              "sprite referencing unknown atlas rejected");
        CHECK(rejectsWith(R"({"schema":1,"palette":{"wall":"464e60"}})", "#RRGGBB"),
              "palette missing '#' rejected");
        CHECK(rejectsWith(R"({"schema":1,"palette":{"wall":"#zzzzzz"}})", "non-hex"),
              "palette non-hex rejected");
        CHECK(rejectsWith("not json", "parse error"), "garbage rejected");
    }

    if (g_fails == 0) std::printf("\nALL PASS (0 failures)\n");
    else std::printf("\n%d FAILURE(S)\n", g_fails);
    return g_fails == 0 ? 0 : 1;
}
