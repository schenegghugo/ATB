#include "Renderer.h"

#include "SpritePack.h"

#include "raylib.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace tb::render {

namespace {

// Semantic keys a pack targets for tiles (sprites are dotted; palette is short).
struct TileKeys { const char* sprite; const char* palette; };
TileKeys tileKeys(TileType t) {
    switch (t) {
        case TileType::Wall: return {"tiles.wall", "wall"};
        case TileType::Obstacle: return {"tiles.obstacle", "obstacle"};
        case TileType::Walkable: break;
    }
    return {"tiles.floor", "floor"};
}

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
constexpr Color kTextDim{150, 156, 172, 255};    // greyed button label (cd / unaffordable)
constexpr Color kBtnReady{54, 62, 84, 255};      // castable now
constexpr Color kBtnCooldown{38, 42, 52, 255};   // recharging
constexpr Color kBtnPoor{72, 48, 52, 255};       // not enough AP
constexpr Color kBtnSelected{235, 200, 90, 255}; // selected-button border
constexpr Color kBtnCdText{232, 150, 80, 255};   // cooldown counter

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
    const int y = l.originY + g.height() * l.tileSize + l.spellBarHeight + 10;
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

// The clickable spell bar for the active player unit. Button geometry comes from
// the shared spellSlotRect (below), so this and the frontend's hit-test agree.
// Each button's icon resolves through the pack (spells.<key>) with a text-badge
// fallback, so the bar is fully usable with zero art.
void drawSpellBar(const Layout& l, const Battle& battle, const ViewState& view,
                  const SpritePack* pack) {
    if (!view.showSpellBar || battle.phase() == Phase::Finished) return;
    const Grid& g = battle.grid();
    const Entity& u = battle.unit(battle.activeUnit());
    const int count = static_cast<int>(u.spells.size());
    for (int s = 0; s < count; ++s) {
        const Rect rc = spellSlotRect(l, g, s, count);
        const Rectangle r{static_cast<float>(rc.x), static_cast<float>(rc.y),
                          static_cast<float>(rc.w), static_cast<float>(rc.h)};
        const Spell& sp = u.spells[s];
        const int cd = s < static_cast<int>(u.spellCooldowns.size()) ? u.spellCooldowns[s] : 0;
        const bool ready = cd == 0;
        const bool afford = u.ap >= sp.apCost;
        const bool usable = ready && afford;
        const bool selected = s == view.selectedSpell;

        DrawRectangleRec(r, !ready ? kBtnCooldown : !afford ? kBtnPoor : kBtnReady);
        DrawRectangleLinesEx(r, selected ? 3.0f : 1.0f, selected ? kBtnSelected : kGridLine);

        // Icon from the pack (spells.<catalog key>); dim it when unusable. If the
        // pack has no icon, the digit + name badge below stands in.
        bool hasIcon = false;
        if (pack && s < static_cast<int>(view.spellIconKeys.size()) && !view.spellIconKeys[s].empty()) {
            const float side = static_cast<float>(rc.h - 6);
            const Rectangle icon{rc.x + 3.0f, rc.y + 3.0f, side, side};
            const Color tint = usable ? WHITE : Color{255, 255, 255, 110};
            hasIcon = pack->drawSprite("spells." + view.spellIconKeys[s], icon, tint);
        }
        const int textX = hasIcon ? rc.x + rc.h : rc.x + 24;

        // Hotkey digit stays on the button (additive to click-to-select).
        DrawText(TextFormat("%d", s + 1), rc.x + 4, rc.y + 3, 16, usable ? kText : kTextDim);
        DrawText(sp.name.c_str(), textX, rc.y + 6, 15, usable ? kText : kTextDim);
        DrawText(TextFormat("%d AP", sp.apCost), textX, rc.y + rc.h - 17, 14,
                 afford ? kText : kBtnCdText);
        if (cd > 0) {
            const char* cds = TextFormat("CD %d", cd);
            DrawText(cds, rc.x + rc.w - MeasureText(cds, 14) - 6, rc.y + rc.h - 17, 14, kBtnCdText);
        }
    }
}

const char* statusWord(StatusEffect::Kind k) {
    switch (k) {
        case StatusEffect::Kind::DamageOverTime: return "a damage-over-time";
        case StatusEffect::Kind::Shield: return "a Shield";
        case StatusEffect::Kind::ApBuff: return "an AP buff";
        case StatusEffect::Kind::MpBuff: return "an MP buff";
        case StatusEffect::Kind::Invisible: return "Invisibility";
        case StatusEffect::Kind::Rewind: return "a Rewind marker";
    }
    return "a status";
}

// Turn one event into a log line + colour. Returns false to *hide* it (Move is
// noise in a combat log — the events still exist for animation/replay).
bool formatEvent(const Battle& b, const BattleEvent& ev, std::string& out, Color& col) {
    const std::string who = b.unit(ev.actor).name;
    const std::string tgt = b.unit(ev.target).name;
    switch (ev.type) {
        case EventType::Move:
            return false;
        case EventType::TurnStart:
            out = "-- " + who + " --"; col = kTextDim; return true;
        case EventType::Cast: {
            const auto& sp = b.unit(ev.actor).spells;
            const std::string name =
                (ev.spellSlot >= 0 && ev.spellSlot < static_cast<int>(sp.size())) ? sp[ev.spellSlot].name
                                                                                  : "a spell";
            out = who + " casts " + name; col = kText; return true;
        }
        case EventType::Damage: {
            const char* w = ev.source == DamageSource::Storm ? " ring"
                          : ev.source == DamageSource::Collision ? " collision" : "";
            out = "  " + tgt + " takes " + std::to_string(ev.amount) + w + " damage";
            col = Color{232, 120, 110, 255}; return true;
        }
        case EventType::Heal:
            out = "  " + tgt + " recovers " + std::to_string(ev.amount) + " HP";
            col = Color{120, 210, 140, 255}; return true;
        case EventType::Status:
            out = "  " + tgt + " gains " + statusWord(ev.status);
            col = Color{190, 155, 235, 255}; return true;
        case EventType::Death:
            out = tgt + " is defeated"; col = Color{240, 90, 95, 255}; return true;
    }
    return false;
}

// Scrolling combat log in the empty column to the right of the board. Skipped if
// the window isn't wide enough to give it a readable strip.
void drawCombatLog(const Layout& l, const Battle& battle, const ViewState& view) {
    const Grid& g = battle.grid();
    const int x0 = l.screenWidth(g) + 8;
    const int panelW = view.windowW - x0 - 8;
    if (panelW < 200) return; // too narrow — the board fills the window
    const int y0 = l.originY;
    const int panelH = view.windowH - y0 - 8;

    DrawRectangle(x0, y0, panelW, panelH, Color{12, 14, 20, 230});
    DrawRectangleLines(x0, y0, panelW, panelH, kGridLine);
    DrawText("COMBAT LOG", x0 + 8, y0 + 6, 14, kText);

    const int lineH = 16, top = y0 + 28;
    const int rows = std::max(0, (panelH - 34) / lineH);

    std::vector<std::pair<std::string, Color>> lines;
    for (const BattleEvent& ev : battle.events()) {
        std::string text;
        Color col;
        if (formatEvent(battle, ev, text, col)) lines.emplace_back(std::move(text), col);
    }
    const int total = static_cast<int>(lines.size());
    const int maxStart = std::max(0, total - rows);
    const int start = std::clamp(maxStart - view.logScroll, 0, maxStart);
    for (int i = 0; i < rows && start + i < total; ++i)
        DrawText(lines[start + i].first.c_str(), x0 + 8, top + i * lineH, 13, lines[start + i].second);
    if (start < maxStart) // more history above the fold
        DrawText("^ scroll", x0 + panelW - MeasureText("^ scroll", 12) - 8, y0 + 8, 12, kTextDim);
}

} // namespace

Rect spellSlotRect(const Layout& l, const Grid& g, int slot, int slotCount) {
    const int gap = 8;
    const int count = std::max(slotCount, 1);
    const int availW = l.screenWidth(g) - l.originX * 2;
    const int bw = (availW - gap * (count - 1)) / count;
    return Rect{l.originX + slot * (bw + gap), l.originY + g.height() * l.tileSize + 6, bw,
                l.spellBarHeight - 12};
}

Vec2i screenToGrid(const Layout& l, int px, int py) {
    return Vec2i{(px - l.originX) / l.tileSize, (py - l.originY) / l.tileSize};
}

void drawFrame(const Layout& l, const Battle& battle, const ViewState& view,
               const SpritePack* pack) {
    const Grid& g = battle.grid();
    ClearBackground(kBackground);

    // --- Tiles ---------------------------------------------------------------
    // Resolution ladder per tile: pack sprite → pack palette colour → primitive.
    for (int y = 0; y < g.height(); ++y) {
        for (int x = 0; x < g.width(); ++x) {
            Vec2i p{x, y};
            Rectangle r = tileRect(l, p);
            const TileType tt = g.at(p);
            const Color def = tt == TileType::Wall ? kWall : tt == TileType::Obstacle ? kObstacle : kFloor;
            const TileKeys keys = tileKeys(tt);
            if (!(pack && pack->drawSprite(keys.sprite, r)))
                DrawRectangleRec(r, pack ? pack->paletteOr(keys.palette, def) : def);
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
        // Pack unit sprite (units.player / units.enemy) → primitive circle. Tint
        // carries the ghosting so concealed units read the same either way.
        const char* uk = e.team == Faction::Player ? "units.player" : "units.enemy";
        const Rectangle dest{c.x - l.tileSize * 0.5f, c.y - l.tileSize * 0.5f,
                             static_cast<float>(l.tileSize), static_cast<float>(l.tileSize)};
        const Color tint = e.invisible() ? Color{255, 255, 255, 90} : WHITE;
        if (!(pack && pack->drawSprite(uk, dest, tint))) {
            DrawCircleV(c, l.tileSize * 0.36f, color);
            DrawCircleLines(static_cast<int>(c.x), static_cast<int>(c.y),
                            l.tileSize * 0.36f, Color{0, 0, 0, 160});
        }
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

    drawSpellBar(l, battle, view, pack);
    drawCombatLog(l, battle, view);

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
