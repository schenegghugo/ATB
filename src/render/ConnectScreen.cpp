#include "ConnectScreen.h"

#include "raylib.h"

#include <array>
#include <string>

namespace tb::render {
namespace {

constexpr Color kBg{18, 20, 28, 255};
constexpr Color kPanel{30, 34, 46, 255};
constexpr Color kPanelHot{44, 50, 66, 255};
constexpr Color kText{220, 224, 235, 255};
constexpr Color kMuted{150, 156, 170, 255};
constexpr Color kAccent{230, 140, 50, 255};
constexpr Color kBad{210, 90, 90, 255};
constexpr Color kLine{0, 0, 0, 160};

bool hovered(Rectangle r, Vector2 m) { return CheckCollisionPointRec(m, r); }
bool pressed(Rectangle r, Vector2 m) { return hovered(r, m) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT); }

void textCentered(const char* s, Rectangle r, int size, Color c) {
    const int w = MeasureText(s, size);
    DrawText(s, static_cast<int>(r.x + (r.width - w) / 2),
             static_cast<int>(r.y + (r.height - size) / 2), size, c);
}

bool button(Rectangle r, const char* label, Vector2 m, Color base) {
    DrawRectangleRec(r, hovered(r, m) ? kPanelHot : base);
    DrawRectangleLinesEx(r, 1.0f, kLine);
    textCentered(label, r, 18, kText);
    return pressed(r, m);
}

// A labeled text field. Draws the box + value (masked when `secret`), takes focus
// on click, and edits the bound string when focused. Returns the (possibly
// updated) focus index.
int field(Rectangle box, const char* label, std::string& value, int idx, int focus, Vector2 m,
          bool secret) {
    DrawText(label, static_cast<int>(box.x), static_cast<int>(box.y) - 18, 14, kMuted);
    const bool focused = focus == idx;
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && hovered(box, m)) focus = idx;

    if (focused) {
        int key = GetCharPressed();
        while (key > 0) {
            if (key >= 32 && key < 127 && value.size() < 48) value.push_back(static_cast<char>(key));
            key = GetCharPressed();
        }
        if (IsKeyPressed(KEY_BACKSPACE) && !value.empty()) value.pop_back();
    }
    DrawRectangleRec(box, focused ? kPanelHot : kPanel);
    DrawRectangleLinesEx(box, 1.0f, focused ? kAccent : kLine);

    std::string shown = secret ? std::string(value.size(), '*') : value;
    if (focused) shown += "_";
    DrawText(shown.c_str(), static_cast<int>(box.x) + 8, static_cast<int>(box.y) + 9, 16, kText);
    return focus;
}

} // namespace

ConnectScreen::Result ConnectScreen::runFrame(int screenW, int screenH) {
    const Vector2 m = GetMousePosition();
    ClearBackground(kBg);

    const float W = static_cast<float>(screenW);
    const float panelW = 460.0f;
    const float x = (W - panelW) / 2.0f;
    float y = 90.0f;

    DrawText("LOG IN — ONLINE HOME", static_cast<int>(x), 44, 26, kText);
    DrawText("Log in to the lobby to seek + challenge. Blank username = guest (casual only).",
             static_cast<int>(x), 74, 13, kMuted);

    // Tab cycles focus between the four fields.
    if (IsKeyPressed(KEY_TAB)) focus_ = (focus_ + 1) % 4;

    y += 24;
    focus_ = field({x, y, panelW, 34}, "Server (host:port)", p_.host, 0, focus_, m, false);
    y += 64;
    focus_ = field({x, y, (panelW - 12) / 2, 34}, "Username (ranked)", p_.user, 1, focus_, m, false);
    focus_ = field({x + (panelW + 12) / 2, y, (panelW - 12) / 2, 34}, "Password", p_.pass, 2, focus_,
                   m, true);
    y += 64;
    focus_ = field({x, y, panelW, 34}, "Lobby code (optional)", p_.lobby, 3, focus_, m, false);
    y += 56;

    Result result = Result::None;
    Rectangle backBtn{x, y, 140, 38};
    Rectangle connectBtn{x + panelW - 160, y, 160, 38};
    if (button(backBtn, "< Back", m, kPanel)) result = Result::Back;
    if (button(connectBtn, "Connect >", m, kAccent) ||
        (IsKeyPressed(KEY_ENTER) && !p_.host.empty()))
        result = Result::Connect;

    if (!status_.empty())
        DrawText(status_.c_str(), static_cast<int>(x), static_cast<int>(y) + 50, 15, kBad);

    // Clicking empty space drops focus (so typing doesn't hit a stale field).
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && result == Result::None) {
        const bool onField = m.y >= 114 && m.y <= y - 22 && m.x >= x && m.x <= x + panelW;
        if (!onField) focus_ = -1;
    }
    return result;
}

} // namespace tb::render
