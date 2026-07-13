#include "SettingsScreen.h"

#include "Ui.h"
#include "raylib.h"

namespace tb::render {

namespace {

// One column of pick buttons: "(none)" + a button per name; `current` gets the
// picked highlight. Returns true and sets `picked` when a row is clicked.
bool pickerColumn(float x, float& y, float w, const char* noneLabel,
                  const std::vector<std::string>& names, const std::string& current, Vector2 m,
                  std::string& picked) {
    const float bh = 30.0f, gap = 8.0f;
    bool clicked = false;
    auto row = [&](const std::string& name, const char* label) {
        const bool cur = current == name;
        if (ui::button({x, y, w, bh}, label, m, cur ? ui::kPicked : ui::kPanel)) {
            picked = name;
            clicked = true;
        }
        y += bh + gap;
    };
    row("", noneLabel);
    for (const std::string& n : names) row(n, n.c_str());
    return clicked;
}

} // namespace

SettingsScreen::Result SettingsScreen::runFrame(int screenW, int screenH, const View& view) {
    const Vector2 m = GetMousePosition();
    ClearBackground(ui::kBg);

    const float W = static_cast<float>(screenW);
    const float H = static_cast<float>(screenH);
    const float x = 48.0f;

    DrawText("SETTINGS", static_cast<int>(x), 44, 26, ui::kText);
    DrawText("Pick a UI theme and sprite pack — applied live, saved to settings.json.",
             static_cast<int>(x), 76, 13, ui::kMuted);

    float y = 116.0f;
    for (const auto& [label, value] : view.rows) {
        DrawText(label.c_str(), static_cast<int>(x), static_cast<int>(y), 16, ui::kMuted);
        DrawText(value.c_str(), static_cast<int>(x) + 240, static_cast<int>(y), 16, ui::kText);
        y += 28.0f;
    }
    y += 16.0f;

    Result result = Result::None;

    // UI scale — the resizable-board control (saved to settings.json, applied live).
    DrawText("UI SCALE  (board size — saved)", static_cast<int>(x), static_cast<int>(y), 15,
             ui::kAccent);
    {
        Rectangle minus{x + 260, y - 6, 34, 28}, plus{x + 386, y - 6, 34, 28};
        if (ui::button(minus, "-", m, ui::kPanel)) result = Result::ScaleDown;
        DrawText(TextFormat("%.0f%%", view.uiScale * 100.0f), static_cast<int>(x) + 312,
                 static_cast<int>(y), 18, ui::kText);
        if (ui::button(plus, "+", m, ui::kPanel)) result = Result::ScaleUp;
    }
    y += 44.0f;

    // Two picker columns, side by side: themes | packs.
    const float colW = 260.0f;
    const float themesX = x, packsX = x + colW + 48.0f;
    float themesY = y + 30.0f, packsY = y + 30.0f;
    DrawText("UI THEME  (themes/*.json)", static_cast<int>(themesX), static_cast<int>(y), 15,
             ui::kAccent);
    DrawText("SPRITE PACK  (packs/)", static_cast<int>(packsX), static_cast<int>(y), 15,
             ui::kAccent);
    if (pickerColumn(themesX, themesY, colW, "(built-in)", view.themes, view.curTheme, m, picked_))
        result = Result::SetTheme;
    if (pickerColumn(packsX, packsY, colW, "(primitives)", view.packs, view.curPack, m, picked_))
        result = Result::SetPack;

    // Ricing loop: edit the current theme file, come back, hit Reload.
    if (!view.curTheme.empty()) {
        if (ui::button({themesX, themesY + 8.0f, colW, 30.0f}, "Reload theme file", m,
                       ui::kPanelHot))
            result = Result::ReloadTheme;
    }

    if (!view.status.empty())
        DrawText(view.status.c_str(), static_cast<int>(x), static_cast<int>(H) - 84, 14,
                 ui::kAccent);
    DrawText("Env overrides: ATB_PACK, ATB_CONNECT, ATB_USER/ATB_PASS, ATB_LOBBY, ATB_DATA_DIR",
             static_cast<int>(x), static_cast<int>(H) - 60, 13, ui::kMuted);

    if (ui::button({x, H - 44, 140, 34}, "< Back", m, ui::kPanel)) result = Result::Back;
    return result;
}

} // namespace tb::render
