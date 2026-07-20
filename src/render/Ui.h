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

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
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

// ---- Editable text field --------------------------------------------------
//
// Full "text zone" behaviour shared by every input box in the GUI: click to
// place the caret, click-drag or shift+arrows to select a run of characters,
// double-click to select all, Ctrl+A/C/X/V, Home/End, Ctrl+arrow word jumps,
// and Delete/Backspace (which delete the current selection when there is one).
//
// Immediate-mode: only one field is ever being edited at a time, so a single
// caret/selection state — keyed by the bound string's address — is enough. The
// caller keeps owning its own focus flag (int index, bool, whatever); this only
// edits + draws when told the field is focused.

struct TextEditState {
    const void* owner = nullptr; // &value of the field currently being edited
    int cursor = 0;              // caret index into value, in [0, len]
    int anchor = 0;              // selection anchor (== cursor means no selection)
    bool dragging = false;       // mouse is currently selecting
    float scroll = 0.0f;         // px the text is shifted left to keep the caret in view
    double blink = 0.0;          // GetTime() when the caret was last forced solid
    double lastClick = -1.0;     // for double-click detection
};
inline TextEditState gEdit;

struct EditOpts {
    std::size_t maxLen = 64;
    bool secret = false;              // render as '*' (also disables copy/cut)
    int fontSize = 16;
    int pad = 8;                      // inner horizontal padding
    const char* prefix = nullptr;     // non-editable muted text shown before the value
    const char* placeholder = nullptr; // muted hint shown when empty + unfocused
};

// Draws the field box + value and, when `focused`, runs a full text editor over
// `value`. Consumes typing / editing keys but never ENTER or TAB, so callers can
// still use those for submit / focus-cycling.
inline void editableField(Rectangle box, std::string& value, bool focused, Vector2 m,
                          EditOpts o = {}) {
    const int size = o.fontSize;
    const void* id = &value;
    const float prefixW = o.prefix ? static_cast<float>(MeasureText(o.prefix, size)) : 0.0f;

    auto isWord = [](unsigned char c) { return std::isalnum(c) != 0 || c == '_'; };
    auto measure = [&](int a, int b) { // px width of value[a, b)
        std::string sub = o.secret ? std::string(static_cast<std::size_t>(b - a), '*')
                                   : value.substr(a, b - a);
        return static_cast<float>(MeasureText(sub.c_str(), size));
    };

    if (focused) {
        int len = static_cast<int>(value.size());
        if (gEdit.owner != id) { // this field just took focus — caret to the end
            gEdit.owner = id;
            gEdit.cursor = gEdit.anchor = len;
            gEdit.dragging = false;
            gEdit.scroll = 0.0f;
            gEdit.blink = GetTime();
            gEdit.lastClick = -1.0; // don't inherit another field's click timing
        }
        gEdit.cursor = std::clamp(gEdit.cursor, 0, len);
        gEdit.anchor = std::clamp(gEdit.anchor, 0, len);

        const bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
        const bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL) ||
                          IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER);
        auto stroke = [](int k) { return IsKeyPressed(k) || IsKeyPressedRepeat(k); };
        auto touch = [] { gEdit.blink = GetTime(); }; // keep caret solid while acting
        auto prevWord = [&](int i) {
            while (i > 0 && !isWord(value[i - 1])) --i;
            while (i > 0 && isWord(value[i - 1])) --i;
            return i;
        };
        auto nextWord = [&](int i) {
            int n = static_cast<int>(value.size());
            while (i < n && !isWord(value[i])) ++i;
            while (i < n && isWord(value[i])) ++i;
            return i;
        };

        // Map a screen x to a caret index (nearest character boundary).
        const float textX = box.x + o.pad + prefixW - gEdit.scroll;
        auto indexAt = [&](float mx) {
            const float rel = mx - textX;
            int best = 0;
            float bestD = 1e9f;
            for (int i = 0; i <= len; ++i) {
                const float d = std::fabs(measure(0, i) - rel);
                if (d < bestD) { bestD = d; best = i; }
            }
            return best;
        };

        // Mouse: click to place caret, double-click to select all, drag to select.
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(m, box)) {
            const double now = GetTime();
            if (now - gEdit.lastClick < 0.35) {
                gEdit.anchor = 0;
                gEdit.cursor = len;
            } else {
                const int idx = indexAt(m.x);
                gEdit.cursor = idx;
                if (!shift) gEdit.anchor = idx;
                gEdit.dragging = true;
            }
            gEdit.lastClick = now;
            touch();
        }
        if (gEdit.dragging && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            gEdit.cursor = indexAt(m.x);
            touch();
        }
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) gEdit.dragging = false;

        auto delSel = [&]() -> bool {
            const int lo = std::min(gEdit.cursor, gEdit.anchor);
            const int hi = std::max(gEdit.cursor, gEdit.anchor);
            if (lo == hi) return false;
            value.erase(lo, hi - lo);
            gEdit.cursor = gEdit.anchor = lo;
            return true;
        };
        auto insert = [&](const std::string& t) {
            delSel();
            std::string clean;
            for (char c : t) {
                const auto u = static_cast<unsigned char>(c);
                if (u >= 32 && u < 127) clean.push_back(c);
            }
            const std::size_t room = o.maxLen > value.size() ? o.maxLen - value.size() : 0;
            if (clean.size() > room) clean.resize(room);
            value.insert(gEdit.cursor, clean);
            gEdit.cursor += static_cast<int>(clean.size());
            gEdit.anchor = gEdit.cursor;
            touch();
        };

        // Typing (skipped while a Ctrl-shortcut modifier is held so Ctrl+V etc.
        // don't also emit a character).
        int ch = GetCharPressed();
        while (ch > 0) {
            if (!ctrl && ch >= 32 && ch < 127) insert(std::string(1, static_cast<char>(ch)));
            ch = GetCharPressed();
        }

        // Clipboard + select-all.
        int lo = std::min(gEdit.cursor, gEdit.anchor);
        int hi = std::max(gEdit.cursor, gEdit.anchor);
        if (ctrl && IsKeyPressed(KEY_A)) {
            gEdit.anchor = 0;
            gEdit.cursor = static_cast<int>(value.size());
        }
        if (ctrl && !o.secret && lo != hi && (IsKeyPressed(KEY_C) || IsKeyPressed(KEY_X)))
            SetClipboardText(value.substr(lo, hi - lo).c_str());
        if (ctrl && IsKeyPressed(KEY_X) && !o.secret) { delSel(); touch(); }
        if (ctrl && IsKeyPressed(KEY_V)) {
            if (const char* cb = GetClipboardText()) insert(cb);
        }

        // Deletion.
        lo = std::min(gEdit.cursor, gEdit.anchor);
        hi = std::max(gEdit.cursor, gEdit.anchor);
        if (stroke(KEY_BACKSPACE)) {
            if (!delSel() && gEdit.cursor > 0) {
                const int from = ctrl ? prevWord(gEdit.cursor) : gEdit.cursor - 1;
                value.erase(from, gEdit.cursor - from);
                gEdit.cursor = gEdit.anchor = from;
            }
            touch();
        }
        if (stroke(KEY_DELETE)) {
            if (!delSel() && gEdit.cursor < static_cast<int>(value.size())) {
                const int to = ctrl ? nextWord(gEdit.cursor) : gEdit.cursor + 1;
                value.erase(gEdit.cursor, to - gEdit.cursor);
            }
            touch();
        }

        // Caret movement (shift extends the selection, plain collapses it).
        len = static_cast<int>(value.size());
        lo = std::min(gEdit.cursor, gEdit.anchor);
        hi = std::max(gEdit.cursor, gEdit.anchor);
        if (stroke(KEY_LEFT)) {
            if (ctrl) gEdit.cursor = prevWord(gEdit.cursor);
            else if (!shift && lo != hi) gEdit.cursor = lo;
            else gEdit.cursor = std::max(0, gEdit.cursor - 1);
            if (!shift) gEdit.anchor = gEdit.cursor;
            touch();
        }
        if (stroke(KEY_RIGHT)) {
            if (ctrl) gEdit.cursor = nextWord(gEdit.cursor);
            else if (!shift && lo != hi) gEdit.cursor = hi;
            else gEdit.cursor = std::min(len, gEdit.cursor + 1);
            if (!shift) gEdit.anchor = gEdit.cursor;
            touch();
        }
        if (stroke(KEY_HOME)) {
            gEdit.cursor = 0;
            if (!shift) gEdit.anchor = 0;
            touch();
        }
        if (stroke(KEY_END)) {
            gEdit.cursor = len;
            if (!shift) gEdit.anchor = len;
            touch();
        }

        // Keep the caret inside the box by scrolling the text horizontally.
        const float innerW = box.width - 2 * o.pad - prefixW;
        if (innerW > 0) {
            const float cX = measure(0, gEdit.cursor);
            if (cX - gEdit.scroll > innerW) gEdit.scroll = cX - innerW;
            if (cX - gEdit.scroll < 0) gEdit.scroll = cX;
            const float maxScroll = std::max(0.0f, measure(0, len) - innerW);
            gEdit.scroll = std::clamp(gEdit.scroll, 0.0f, maxScroll);
        }
    } else if (gEdit.owner == id) {
        gEdit.owner = nullptr; // lost focus — release the shared editor
    }

    // ---- Draw -------------------------------------------------------------
    DrawRectangleRec(box, focused ? kPanelHot : kPanel);
    DrawRectangleLinesEx(box, 1.0f, focused ? kAccent : kLine);

    const int textY = static_cast<int>(box.y) + (static_cast<int>(box.height) - size) / 2;
    const float scroll = focused ? gEdit.scroll : 0.0f;
    BeginScissorMode(static_cast<int>(box.x) + 1, static_cast<int>(box.y) + 1,
                     static_cast<int>(box.width) - 2, static_cast<int>(box.height) - 2);
    float baseX = box.x + o.pad - scroll;
    if (o.prefix) {
        DrawText(o.prefix, static_cast<int>(baseX), textY, size, kMuted);
        baseX += prefixW;
    }
    const int len = static_cast<int>(value.size());
    if (focused) {
        const int lo = std::min(gEdit.cursor, gEdit.anchor);
        const int hi = std::max(gEdit.cursor, gEdit.anchor);
        if (lo != hi) {
            const float sx = baseX + measure(0, lo);
            const float ex = baseX + measure(0, hi);
            DrawRectangle(static_cast<int>(sx), textY, static_cast<int>(ex - sx), size,
                          Fade(kAccent, 0.35f));
        }
    }
    if (value.empty() && !focused && o.placeholder) {
        DrawText(o.placeholder, static_cast<int>(baseX), textY, size, kMuted);
    } else {
        const std::string shown =
            o.secret ? std::string(static_cast<std::size_t>(len), '*') : value;
        DrawText(shown.c_str(), static_cast<int>(baseX), textY, size, kText);
    }
    if (focused && std::fmod(GetTime() - gEdit.blink, 1.0) < 0.5) {
        const int cx = static_cast<int>(baseX + measure(0, gEdit.cursor));
        DrawRectangle(cx, textY, 2, size, kAccent);
    }
    EndScissorMode();
}

} // namespace tb::render::ui
