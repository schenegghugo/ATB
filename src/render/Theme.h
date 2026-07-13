#pragma once
//
// Theme.h — the riceable UI palette, loaded from themes/<name>.json.
//
// Every colour the chrome and battle HUD draw with lives here as a semantic
// key; a theme file overrides any subset ("#RRGGBB" / "#RRGGBBAA") and keeps
// the defaults for the rest — same philosophy as a palette-only sprite pack.
// Raylib-free (like PackManifest) so themes parse and validate headlessly;
// applying a Theme to the live palettes is the GUI's job (ui::applyTheme +
// render::applyBattleTheme).
//
#include "PackManifest.h" // RGBA + parseHexColor

#include <string>
#include <vector>

namespace tb::render {

struct Theme {
    // Layout metrics — the resizable-UI extension. A theme ships a base board
    // density (tileSize) and an optional multiplier; the player's settings.json
    // `uiScale` multiplies on top of these. Everything else derives from tileSize.
    struct Metrics {
        int tileSize = 36;    // battle board tile edge, px
        double uiScale = 1.0; // theme-level size multiplier (composes with prefs)
    };
    Metrics metrics{};

    // Chrome (menus / screens / shared widgets — ui::k*).
    RGBA bg{18, 20, 28, 255};
    RGBA panel{30, 34, 46, 255};
    RGBA panelHot{44, 50, 66, 255};
    RGBA text{220, 224, 235, 255};
    RGBA muted{150, 156, 170, 255};
    RGBA accent{230, 140, 50, 255};
    RGBA good{90, 200, 130, 255};
    RGBA bad{210, 90, 90, 255};
    RGBA line{0, 0, 0, 160};
    RGBA picked{40, 90, 60, 255};    // build editor: selected card
    RGBA pickedHot{52, 116, 78, 255};

    // Battle board + HUD (Renderer.cpp). Tile colours here are the *primitive*
    // fallbacks — a sprite pack's palette/art still wins where it provides one.
    RGBA gridLine{40, 46, 60, 255};
    RGBA floor{30, 34, 46, 255};
    RGBA wall{70, 78, 96, 255};
    RGBA obstacle{120, 96, 60, 255};
    RGBA reach{60, 110, 200, 90};
    RGBA castable{70, 200, 120, 95}; // legal target tiles for the selected spell
    RGBA hover{230, 230, 240, 110};
    RGBA zoneOk{230, 140, 50, 120};
    RGBA zoneBad{120, 60, 60, 110};
    RGBA statusDot{230, 90, 200, 255};
    RGBA groundWall{110, 120, 140, 255};
    RGBA glyphZone{150, 70, 200, 80}; // neutral surface (element-less glyph)
    // Elemental-surface tints (0.0.2). Primitive fallbacks — a pack's
    // `surfaces.<element>` sprite still wins where it provides one.
    RGBA surfFire{220, 90, 40, 120};
    RGBA surfWater{60, 120, 210, 110};
    RGBA surfIce{160, 210, 230, 120};
    RGBA surfPoison{110, 180, 60, 120};
    RGBA surfElectric{235, 210, 70, 150};
    RGBA surfHeal{80, 200, 130, 110};
    RGBA surfOil{55, 48, 40, 160};
    RGBA surfSteam{205, 205, 215, 130};
    RGBA portal{70, 200, 220, 255};
    RGBA storm{200, 40, 50, 110};
    RGBA player{70, 170, 110, 255};
    RGBA enemy{200, 80, 80, 255};
    RGBA los{240, 220, 120, 200};
    RGBA textDim{150, 156, 172, 255};
    RGBA btnReady{54, 62, 84, 255};
    RGBA btnCooldown{38, 42, 52, 255};
    RGBA btnPoor{72, 48, 52, 255};
    RGBA btnSelected{235, 200, 90, 255};
    RGBA btnCdText{232, 150, 80, 255};
};

inline constexpr int kThemeSchemaVersion = 1;

struct ThemeLoad {
    bool ok = false;
    Theme theme;                     // defaults for every omitted colour
    std::string name;                // display name ("" if absent)
    std::string version;
    std::vector<std::string> errors; // all problems, each with context
};

[[nodiscard]] ThemeLoad loadThemeFromString(const std::string& json);
[[nodiscard]] ThemeLoad loadThemeFromFile(const std::string& path);

} // namespace tb::render
