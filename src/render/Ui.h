#pragma once
//
// Ui.h — Tiny shared immediate-mode widgets + palette for the menu/screens.
//
// Header-only helpers so ConnectScreen / MainMenuScreen / SettingsScreen don't
// each re-copy the same button + colour code. (BuildEditorScreen predates this
// and keeps its own copies.)
//
#include "Theme.h"

#include "raylib.h"

#include <string>

namespace tb::render::ui {

// The live chrome palette. Defaults match Theme's defaults; a loaded theme
// reassigns them via applyTheme() (ricing — see themes/*.json).
inline Color kBg{18, 20, 28, 255};
inline Color kPanel{30, 34, 46, 255};
inline Color kPanelHot{44, 50, 66, 255};
inline Color kText{220, 224, 235, 255};
inline Color kMuted{150, 156, 170, 255};
inline Color kAccent{230, 140, 50, 255};
inline Color kGood{90, 200, 130, 255};
inline Color kBad{210, 90, 90, 255};
inline Color kLine{0, 0, 0, 160};
inline Color kPicked{40, 90, 60, 255};    // build editor: selected card
inline Color kPickedHot{52, 116, 78, 255};

inline Color toColor(RGBA c) { return Color{c.r, c.g, c.b, c.a}; }

inline void applyTheme(const Theme& t) {
    kBg = toColor(t.bg);
    kPanel = toColor(t.panel);
    kPanelHot = toColor(t.panelHot);
    kText = toColor(t.text);
    kMuted = toColor(t.muted);
    kAccent = toColor(t.accent);
    kGood = toColor(t.good);
    kBad = toColor(t.bad);
    kLine = toColor(t.line);
    kPicked = toColor(t.picked);
    kPickedHot = toColor(t.pickedHot);
}

inline bool hovered(Rectangle r, Vector2 m) { return CheckCollisionPointRec(m, r); }
inline bool pressed(Rectangle r, Vector2 m) {
    return hovered(r, m) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

inline void textCentered(const char* s, Rectangle r, int size, Color c) {
    const int w = MeasureText(s, size);
    DrawText(s, static_cast<int>(r.x + (r.width - w) / 2),
             static_cast<int>(r.y + (r.height - size) / 2), size, c);
}

inline bool button(Rectangle r, const char* label, Vector2 m, Color base, bool enabled = true) {
    DrawRectangleRec(r, !enabled ? kPanel : (hovered(r, m) ? kPanelHot : base));
    DrawRectangleLinesEx(r, 1.0f, kLine);
    textCentered(label, r, 18, enabled ? kText : kMuted);
    return enabled && pressed(r, m);
}

} // namespace tb::render::ui
