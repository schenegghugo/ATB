#pragma once
//
// Ui.h — Tiny shared immediate-mode widgets + palette for the menu/screens.
//
// Header-only helpers so ConnectScreen / MainMenuScreen / SettingsScreen don't
// each re-copy the same button + colour code. (BuildEditorScreen predates this
// and keeps its own copies.)
//
#include "raylib.h"

#include <string>

namespace tb::render::ui {

inline constexpr Color kBg{18, 20, 28, 255};
inline constexpr Color kPanel{30, 34, 46, 255};
inline constexpr Color kPanelHot{44, 50, 66, 255};
inline constexpr Color kText{220, 224, 235, 255};
inline constexpr Color kMuted{150, 156, 170, 255};
inline constexpr Color kAccent{230, 140, 50, 255};
inline constexpr Color kGood{90, 200, 130, 255};
inline constexpr Color kBad{210, 90, 90, 255};
inline constexpr Color kLine{0, 0, 0, 160};

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
