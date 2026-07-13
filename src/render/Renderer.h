#pragma once
//
// Renderer.h — Raylib presentation layer.
//
// The ONLY place Raylib is allowed. Consumes the headless Battle state plus some
// transient view data and paints a minimalist "tactical mode" interface with
// geometric primitives. Swap this file out and the game core runs headless.
//
#include "../core/Battle.h"
#include "../data/Net.h" // net::ChatLine
#include "Theme.h"

#include <string>
#include <vector>

namespace tb::render {

// A raylib-free integer rectangle, so the public API stays engine-only.
struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    [[nodiscard]] bool contains(int px, int py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

struct Layout {
    int tileSize = 36;
    int originX = 16;
    int originY = 28;      // leaves a header band for the status line
    int spellBarHeight = 46; // clickable spell buttons for the active player unit
    int hudHeight = 96;

    [[nodiscard]] int screenWidth(const Grid& g) const { return originX * 2 + g.width() * tileSize; }
    [[nodiscard]] int screenHeight(const Grid& g) const {
        return originY + g.height() * tileSize + spellBarHeight + hudHeight;
    }
};

// Per-slot spell-button rectangle — the single source of truth shared by the
// renderer (draw) and the frontend (hit-test), so they can never disagree.
[[nodiscard]] Rect spellSlotRect(const Layout& l, const Grid& g, int slot, int slotCount);

// View-only transient state the frontend feeds in each frame.
struct ViewState {
    Vec2i hoveredTile{-1, -1};
    bool hoveredValid = false;
    std::vector<Vec2i> reachable;     // tiles the active unit can move to
    std::vector<Vec2i> castable;      // legal target tiles for the selected spell (green)
    bool showLosToHover = false;      // draw a sightline from active unit to hover
    std::string statusLine;          // bottom-of-screen feedback

    // Selected-spell preview.
    std::vector<Vec2i> spellZone;     // tiles the selected spell would hit
    bool spellCastable = false;       // tints the zone valid/invalid
    std::string spellLabel;          // e.g. "[2] Fireball  4 AP  cost 4"

    // Two-click portal placement: the entry tile chosen on the first click, shown
    // while the player picks an exit (castable then lists the walkable exit tiles).
    Vec2i portalEntry{-1, -1};
    bool portalEntrySet = false;

    // Clickable spell bar (drawn for the active player unit).
    bool showSpellBar = false;
    int selectedSpell = -1;                 // highlighted button (matches the digit hotkey)
    std::vector<std::string> spellIconKeys; // catalog key per active-unit spell (icons land in 2.2)

    // Combat log panel (drawn in the empty column right of the board, if it fits).
    int windowW = 0, windowH = 0; // actual window size (the board uses Layout::screen*)
    int logScroll = 0;            // lines scrolled up from the newest (0 = autoscroll to bottom)

    // Draggable layout (persisted ratios): the clock strip height and chat's share
    // of the column below it. showLayoutHandles draws the grab grips.
    int clockHeight = 68;
    float chatFraction = 0.5f;
    bool showLayoutHandles = false;

    // Move clock strip atop the log column (timed networked matches). Two big MM:SS:
    // YOU on the left, OPPONENT on the right; the side whose turn it is ticks + is
    // highlighted (red under 5 s). Hidden when showClock is false.
    bool showClock = false;
    float myClock = 0.0f, oppClock = 0.0f; // seconds remaining shown per side
    bool myTurnActive = false;             // true → my side is the one ticking

    // In-match chat (live networked matches): the log column splits — chat above the
    // combat log — with a text input at the chat panel's foot.
    bool showChat = false;
    const std::vector<net::ChatLine>* chatLog = nullptr; // the running transcript
    Faction localSeat = Faction::Player;                 // colour our own lines
    std::string chatDraft;                               // text being typed
    bool chatFocused = false;                            // input has keyboard focus
};

// The chat text-input rectangle (hit-test in the frontend), or an empty Rect when
// chat isn't shown. Single source of truth shared with the renderer.
[[nodiscard]] Rect chatInputRect(const Layout& l, const Grid& g, const ViewState& view);

// Draggable layout grips, shared with the frontend's hit-test. Each is an empty
// Rect (w==0) when it doesn't apply this frame:
//   boardResizeHandle — corner grip that resizes the board (drives Prefs::uiScale)
//   clockDivider      — strip below the clock (drives Prefs::clockHeight)
//   chatDivider       — strip between chat and log (drives Prefs::chatFraction)
[[nodiscard]] Rect boardResizeHandle(const Layout& l, const Grid& g);
[[nodiscard]] Rect clockDivider(const Layout& l, const Grid& g, const ViewState& view);
[[nodiscard]] Rect chatDivider(const Layout& l, const Grid& g, const ViewState& view);

// Converts a pixel position to a grid coordinate (caller checks inBounds).
[[nodiscard]] Vec2i screenToGrid(const Layout& l, int px, int py);

// Optional presentation pack (art/palette). Null = the built-in primitives, so
// the renderer works identically with no pack. Defined in SpritePack.h.
class SpritePack;
// Optional per-entity clip playback state (§2.4); null = no event animations.
class Animator;

void drawFrame(const Layout& l, const Battle& battle, const ViewState& view,
               const SpritePack* pack = nullptr, const Animator* anim = nullptr);

// Re-skin the battle board + HUD from a loaded theme (themes/*.json). The
// chrome half lives in ui::applyTheme (Ui.h); main.cpp calls both together.
void applyBattleTheme(const Theme& t);

} // namespace tb::render
