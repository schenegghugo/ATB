#include "SettingsScreen.h"

#include "Ui.h"
#include "raylib.h"

namespace tb::render {

SettingsScreen::Result SettingsScreen::runFrame(
    int screenW, int screenH, const std::vector<std::pair<std::string, std::string>>& rows) {
    const Vector2 m = GetMousePosition();
    ClearBackground(ui::kBg);

    const float W = static_cast<float>(screenW);
    const float H = static_cast<float>(screenH);
    const float x = 48.0f;

    DrawText("SETTINGS", static_cast<int>(x), 44, 26, ui::kText);
    DrawText("Read-only for now — editable settings + saved defaults are coming.",
             static_cast<int>(x), 76, 13, ui::kMuted);

    float y = 120.0f;
    for (const auto& [label, value] : rows) {
        DrawText(label.c_str(), static_cast<int>(x), static_cast<int>(y), 16, ui::kMuted);
        DrawText(value.c_str(), static_cast<int>(x) + 240, static_cast<int>(y), 16, ui::kText);
        y += 30.0f;
    }

    DrawText("Env overrides: ATB_PACK, ATB_CONNECT, ATB_USER/ATB_PASS, ATB_LOBBY, ATB_DATA_DIR",
             static_cast<int>(x), static_cast<int>(H) - 60, 13, ui::kMuted);

    Result result = Result::None;
    if (ui::button({x, H - 44, 140, 34}, "< Back", m, ui::kPanel)) result = Result::Back;
    return result;
}

} // namespace tb::render
