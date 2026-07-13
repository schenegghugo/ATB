//
// theme_demo.cpp — Test for the UI theme loader (render/Theme). Raylib-free:
// validates themes/*.json → Theme without a GL context, so it runs in headless
// CI. Applying a theme to the live palettes is GUI-only and verified in-game.
//
// Run from the repo root it also validates every SHIPPED theme (themes/), the
// same gate the catalog/creature/ruleset files get.
//
#include "render/Theme.h"

#include "data/Prefs.h" // listThemes — discover the shipped files

#include <cstdio>
#include <string>

using namespace tb;
using namespace tb::render;

static int g_fails = 0;
#define CHECK(cond, msg)                                                                            \
    do {                                                                                            \
        if (cond) std::printf("  [PASS] %s\n", msg);                                                \
        else { std::printf("  [FAIL] %s\n", msg); ++g_fails; }                                      \
    } while (0)

static bool rejectsWith(const std::string& json, const std::string& needle) {
    ThemeLoad r = loadThemeFromString(json);
    if (r.ok) return false;
    for (const std::string& e : r.errors)
        if (e.find(needle) != std::string::npos) return true;
    return false;
}

int main() {
    std::printf("A theme overrides a subset; the rest keep defaults\n");
    {
        const ThemeLoad r = loadThemeFromString(R"({
            "schema": 1, "name": "Test", "version": "0.1",
            "colors": { "accent": "#ff0000", "bg": "#01020304", "player": "#0a0B0c" }
        })");
        CHECK(r.ok, "valid theme loads");
        CHECK(r.name == "Test", "name parsed");
        CHECK(r.theme.accent.r == 255 && r.theme.accent.g == 0 && r.theme.accent.b == 0 &&
                  r.theme.accent.a == 255,
              "#RRGGBB parsed (alpha defaults to 255)");
        CHECK(r.theme.bg.r == 1 && r.theme.bg.g == 2 && r.theme.bg.b == 3 && r.theme.bg.a == 4,
              "#RRGGBBAA parsed");
        CHECK(r.theme.player.r == 10 && r.theme.player.g == 11 && r.theme.player.b == 12,
              "mixed-case hex accepted");
        const Theme defaults;
        CHECK(r.theme.panel.r == defaults.panel.r && r.theme.text.b == defaults.text.b,
              "omitted keys keep the built-in defaults");
    }

    std::printf("An empty theme is valid (all defaults)\n");
    {
        const ThemeLoad r = loadThemeFromString(R"({ "schema": 1 })");
        CHECK(r.ok, "colors object is optional");
        const Theme defaults;
        CHECK(r.theme.metrics.tileSize == defaults.metrics.tileSize &&
                  r.theme.metrics.uiScale == defaults.metrics.uiScale,
              "metrics default when the block is absent");
    }

    std::printf("The metrics block overrides layout sizing (resizable-UI extension)\n");
    {
        const ThemeLoad r = loadThemeFromString(R"({
            "schema": 1, "metrics": { "tileSize": 44, "uiScale": 1.25 }
        })");
        CHECK(r.ok, "theme with metrics loads");
        CHECK(r.theme.metrics.tileSize == 44, "tileSize parsed");
        CHECK(r.theme.metrics.uiScale == 1.25, "uiScale parsed");
        CHECK(rejectsWith(R"({ "metrics": { "tile": 44 } })", "unknown field"),
              "unknown metrics key is an error");
        CHECK(rejectsWith(R"({ "metrics": [] })", "expected an object"),
              "metrics must be an object");
    }

    std::printf("Malformed themes are rejected with context\n");
    {
        CHECK(rejectsWith("{", "json:"), "parse errors are reported");
        CHECK(rejectsWith(R"({ "schema": 2 })", "unsupported \"schema\""), "wrong schema");
        CHECK(rejectsWith(R"({ "colours": {} })", "unknown field \"colours\""),
              "unknown top-level field (typo) is an error");
        CHECK(rejectsWith(R"({ "colors": { "acent": "#ff0000" } })", "unknown colour key"),
              "unknown colour key (typo) is an error");
        CHECK(rejectsWith(R"({ "colors": { "accent": "#ff00" } })", "#RRGGBB"),
              "wrong hex length is an error");
        CHECK(rejectsWith(R"({ "colors": { "accent": "#ggff00" } })", "non-hex"),
              "non-hex digits are an error");
        CHECK(rejectsWith(R"({ "colors": { "accent": 16711680 } })", "must be a"),
              "non-string colour value is an error");
        CHECK(rejectsWith(R"({ "colors": [] })", "expected an object"),
              "colors must be an object");
    }

    std::printf("Every shipped theme validates (themes/)\n");
    {
        const std::vector<std::string> shipped = listThemes("themes");
        CHECK(!shipped.empty(), "themes/ found with at least one theme (run from the repo root)");
        for (const std::string& name : shipped) {
            const ThemeLoad r = loadThemeFromFile("themes/" + name + ".json");
            for (const std::string& e : r.errors)
                std::printf("    themes/%s.json: %s\n", name.c_str(), e.c_str());
            CHECK(r.ok, ("themes/" + name + ".json is valid").c_str());
        }
    }

    if (g_fails) {
        std::printf("\n%d CHECK(S) FAILED\n", g_fails);
        return 1;
    }
    std::printf("\nAll theme checks passed.\n");
    return 0;
}
