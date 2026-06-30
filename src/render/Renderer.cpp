#include "Renderer.h"

#include "raylib.h"

#include <algorithm>

namespace tb::render {

namespace {

// Tactical-mode palette.
constexpr Color kBackground{18, 20, 28, 255};
constexpr Color kGridLine{40, 46, 60, 255};
constexpr Color kFloor{30, 34, 46, 255};
constexpr Color kWall{70, 78, 96, 255};
constexpr Color kObstacle{120, 96, 60, 255};
constexpr Color kReach{60, 110, 200, 90};
constexpr Color kHover{230, 230, 240, 110};
constexpr Color kZoneOk{230, 140, 50, 120};   // selected spell would land
constexpr Color kZoneBad{120, 60, 60, 110};    // selected spell can't be cast
constexpr Color kStatusDot{230, 90, 200, 255}; // status-effect marker
constexpr Color kGroundWall{110, 120, 140, 255}; // Shelter walls
constexpr Color kGlyphZone{150, 70, 200, 80};    // Glyph trap area
constexpr Color kPortal{70, 200, 220, 255};      // Portal entry/exit
constexpr Color kStorm{200, 40, 50, 110};        // collapsed (closing-ring) tiles
constexpr Color kPlayer{70, 170, 110, 255};
constexpr Color kEnemy{200, 80, 80, 255};
constexpr Color kLos{240, 220, 120, 200};
constexpr Color kText{220, 224, 235, 255};

Rectangle tileRect(const Layout& l, Vec2i p) {
    return Rectangle{static_cast<float>(l.originX + p.x * l.tileSize),
                     static_cast<float>(l.originY + p.y * l.tileSize),
                     static_cast<float>(l.tileSize), static_cast<float>(l.tileSize)};
}

Vector2 tileCenter(const Layout& l, Vec2i p) {
    return Vector2{l.originX + p.x * l.tileSize + l.tileSize / 2.0f,
                   l.originY + p.y * l.tileSize + l.tileSize / 2.0f};
}

void drawBar(int x, int y, int w, int h, float frac, Color fill, const char* label) {
    DrawRectangle(x, y, w, h, Color{0, 0, 0, 120});
    DrawRectangle(x, y, static_cast<int>(w * std::clamp(frac, 0.0f, 1.0f)), h, fill);
    DrawRectangleLines(x, y, w, h, Color{0, 0, 0, 180});
    DrawText(label, x + 6, y + h / 2 - 8, 16, kText);
}

void drawEntityPanel(const Layout& l, const Grid& g, const Entity& e, Color color, int slot,
                     int slotCount, bool isActive) {
    const int gaps = std::max(slotCount, 1);
    const int y = l.originY + g.height() * l.tileSize + 10;
    const int panelW = (l.screenWidth(g) - l.originX * 2 - 16 * (gaps - 1)) / gaps;
    const int x = l.originX + slot * (panelW + 16);

    if (isActive) DrawRectangleLines(x - 4, y - 4, panelW + 8, l.hudHeight - 12, color);

    DrawText(TextFormat("%s%s", e.name.c_str(), isActive ? "  (active)" : ""), x, y, 18, color);
    drawBar(x, y + 24, panelW, 18, e.maxHp ? static_cast<float>(e.hp) / e.maxHp : 0.0f, kEnemy,
            TextFormat("HP %d/%d", e.hp, e.maxHp));
    drawBar(x, y + 46, panelW / 2 - 4, 16, e.maxAp ? static_cast<float>(e.ap) / e.maxAp : 0.0f,
            Color{90, 150, 230, 255}, TextFormat("AP %d", e.ap));
    drawBar(x + panelW / 2 + 4, y + 46, panelW / 2 - 4, 16,
            e.maxMp ? static_cast<float>(e.mp) / e.maxMp : 0.0f, Color{90, 200, 130, 255},
            TextFormat("MP %d", e.mp));
}

} // namespace

Vec2i screenToGrid(const Layout& l, int px, int py) {
    return Vec2i{(px - l.originX) / l.tileSize, (py - l.originY) / l.tileSize};
}

void drawFrame(const Layout& l, const Battle& battle, const ViewState& view) {
    const Grid& g = battle.grid();
    ClearBackground(kBackground);

    // --- Tiles ---------------------------------------------------------------
    for (int y = 0; y < g.height(); ++y) {
        for (int x = 0; x < g.width(); ++x) {
            Vec2i p{x, y};
            Rectangle r = tileRect(l, p);
            Color c = kFloor;
            switch (g.at(p)) {
                case TileType::Wall: c = kWall; break;
                case TileType::Obstacle: c = kObstacle; break;
                case TileType::Walkable: c = kFloor; break;
            }
            DrawRectangleRec(r, c);
            DrawRectangleLinesEx(r, 1.0f, kGridLine);
            if (battle.inStorm(p)) DrawRectangleRec(r, kStorm); // collapsed by the ring
        }
    }

    // --- Ground effects (under units, over the floor) ------------------------
    for (const GroundEffect& gx : battle.groundEffects()) {
        if (gx.kind == GroundKind::Wall) {
            for (Vec2i p : gx.tiles) {
                Rectangle r = tileRect(l, p);
                DrawRectangleRec(r, kGroundWall);
                DrawRectangleLinesEx(r, 2.0f, Color{20, 22, 30, 255});
            }
        } else if (gx.kind == GroundKind::Glyph) {
            for (Vec2i p : gx.tiles) DrawRectangleRec(tileRect(l, p), kGlyphZone);
        } else if (gx.kind == GroundKind::Portal) {
            for (Vec2i p : gx.tiles) { // entry
                Vector2 c = tileCenter(l, p);
                DrawCircleV(c, l.tileSize * 0.30f, Fade(kPortal, 0.7f));
                DrawCircleLines(static_cast<int>(c.x), static_cast<int>(c.y), l.tileSize * 0.30f,
                                kPortal);
                DrawLineEx(c, tileCenter(l, gx.exit), 2.0f, Fade(kPortal, 0.5f));
            }
            DrawCircleLines(static_cast<int>(tileCenter(l, gx.exit).x),
                            static_cast<int>(tileCenter(l, gx.exit).y), l.tileSize * 0.30f, kPortal);
        }
    }

    // --- Reachable highlight (active unit's movement range) ------------------
    for (Vec2i p : view.reachable) {
        DrawRectangleRec(tileRect(l, p), kReach);
    }

    // --- Selected-spell zone preview ----------------------------------------
    for (Vec2i p : view.spellZone) {
        DrawRectangleRec(tileRect(l, p), view.spellCastable ? kZoneOk : kZoneBad);
    }

    // --- Hover ---------------------------------------------------------------
    if (view.hoveredValid) {
        DrawRectangleRec(tileRect(l, view.hoveredTile), kHover);
    }

    // --- Line of sight preview (from the active unit to the cursor) ----------
    if (view.showLosToHover && view.hoveredValid) {
        Vec2i from = battle.unit(battle.activeUnit()).pos;
        bool clear = hasLineOfSight(g, from, view.hoveredTile);
        DrawLineEx(tileCenter(l, from), tileCenter(l, view.hoveredTile), 2.0f,
                   clear ? kLos : Color{180, 60, 60, 200});
    }

    // --- Entities ------------------------------------------------------------
    const auto& units = battle.units();
    for (const Entity& e : units) {
        if (!e.alive()) continue;
        Color color = e.team == Faction::Player ? kPlayer : kEnemy;
        if (e.invisible()) color = Fade(color, 0.35f); // concealed: drawn ghosted
        Vector2 c = tileCenter(l, e.pos);
        DrawCircleV(c, l.tileSize * 0.36f, color);
        DrawCircleLines(static_cast<int>(c.x), static_cast<int>(c.y),
                        l.tileSize * 0.36f, Color{0, 0, 0, 160});
        // Active status effects as small markers above the unit.
        for (std::size_t s = 0; s < e.statuses.size(); ++s) {
            DrawRectangle(static_cast<int>(c.x) - 12 + static_cast<int>(s) * 8,
                          static_cast<int>(c.y) - l.tileSize / 2 - 6, 6, 6, kStatusDot);
        }
    }

    // --- HUD: one status panel per *living* unit (dead summons/bombs vanish) --
    std::vector<EntityId> living;
    for (EntityId i = 0; i < units.size(); ++i)
        if (units[i].alive()) living.push_back(i);
    const int slotCount = static_cast<int>(living.size());
    const EntityId activeId = battle.activeUnit();
    for (int slot = 0; slot < slotCount; ++slot) {
        const EntityId id = living[slot];
        const Entity& e = units[id];
        Color color = e.team == Faction::Player ? kPlayer : kEnemy;
        drawEntityPanel(l, g, e, color, slot, slotCount,
                        battle.phase() != Phase::Finished && id == activeId);
    }

    if (!view.statusLine.empty()) {
        DrawText(view.statusLine.c_str(), l.originX, 4, 16, kText);
    }
    {
        const bool closing = battle.inStorm(Vec2i{0, 0}); // a corner is in => ring active
        const char* rt = closing ? TextFormat("Round %d  -  RING CLOSING", battle.round())
                                  : TextFormat("Round %d", battle.round());
        const int w = MeasureText(rt, 16);
        DrawText(rt, (l.screenWidth(g) - w) / 2, 4, 16, closing ? Color{235, 90, 95, 255} : kText);
    }
    if (!view.spellLabel.empty()) {
        const int w = MeasureText(view.spellLabel.c_str(), 16);
        DrawText(view.spellLabel.c_str(), l.screenWidth(g) - l.originX - w, 4, 16,
                 view.spellCastable ? kZoneOk : kText);
    }
}

} // namespace tb::render
