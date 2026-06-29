#pragma once
//
// Renderer.h — Raylib presentation layer.
//
// The ONLY place Raylib is allowed. Consumes the headless Battle state plus some
// transient view data and paints a minimalist "tactical mode" interface with
// geometric primitives. Swap this file out and the game core runs headless.
//
#include "../core/Battle.h"

#include <string>
#include <vector>

namespace tb::render {

struct Layout {
    int tileSize = 36;
    int originX = 16;
    int originY = 28; // leaves a header band for the status line
    int hudHeight = 96;

    [[nodiscard]] int screenWidth(const Grid& g) const { return originX * 2 + g.width() * tileSize; }
    [[nodiscard]] int screenHeight(const Grid& g) const {
        return originY + g.height() * tileSize + hudHeight;
    }
};

// View-only transient state the frontend feeds in each frame.
struct ViewState {
    Vec2i hoveredTile{-1, -1};
    bool hoveredValid = false;
    std::vector<Vec2i> reachable;     // tiles the active unit can move to
    bool showLosToHover = false;      // draw a sightline from active unit to hover
    std::string statusLine;          // bottom-of-screen feedback

    // Selected-spell preview.
    std::vector<Vec2i> spellZone;     // tiles the selected spell would hit
    bool spellCastable = false;       // tints the zone valid/invalid
    std::string spellLabel;          // e.g. "[2] Fireball  4 AP  cost 4"
};

// Converts a pixel position to a grid coordinate (caller checks inBounds).
[[nodiscard]] Vec2i screenToGrid(const Layout& l, int px, int py);

void drawFrame(const Layout& l, const Battle& battle, const ViewState& view);

} // namespace tb::render
