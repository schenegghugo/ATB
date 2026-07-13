#include "MainMenuScreen.h"

#include "Ui.h"
#include "raylib.h"

namespace tb::render {

MainMenuScreen::Result MainMenuScreen::runFrame(int screenW, int screenH, const char* version) {
    const Vector2 m = GetMousePosition();
    ClearBackground(ui::kBg);

    const float W = static_cast<float>(screenW);
    const float H = static_cast<float>(screenH);

    // Title.
    const char* title = "TACTICAL BATTLER";
    const int titleSize = 48;
    DrawText(title, static_cast<int>((W - MeasureText(title, titleSize)) / 2),
             static_cast<int>(H * 0.18f), titleSize, ui::kText);
    const char* tagline = "a hackable, data-driven tactics sandbox";
    DrawText(tagline, static_cast<int>((W - MeasureText(tagline, 16)) / 2),
             static_cast<int>(H * 0.18f) + 58, 16, ui::kMuted);

    // Menu buttons, centred column.
    struct Item {
        const char* label;
        Result result;
        Color base;
    };
    const Item items[] = {
        {"Local Match", Result::LocalMatch, ui::kAccent},
        {"Play Online", Result::PlayOnline, ui::kPanel},
        {"Build Editor", Result::BuildEditor, ui::kPanel},
        {"Settings", Result::Settings, ui::kPanel},
        {"Quit", Result::Quit, ui::kPanel},
    };

    const float bw = 280.0f, bh = 46.0f, gap = 12.0f;
    const float bx = (W - bw) / 2.0f;
    float by = H * 0.40f;

    Result result = Result::None;
    for (const Item& it : items) {
        if (ui::button({bx, by, bw, bh}, it.label, m, it.base)) result = it.result;
        by += bh + gap;
    }

    const char* ver = (version && *version) ? version : "dev";
    DrawText(TextFormat("%s  —  no account needed for local/custom play", ver), 16,
             static_cast<int>(H) - 26, 13, ui::kMuted);
    // Also echo the version under the title, right-aligned to the tagline width.
    DrawText(ver, static_cast<int>((W + MeasureText(tagline, 16)) / 2) - MeasureText(ver, 14),
             static_cast<int>(H * 0.18f) + 78, 14, ui::kAccent);
    return result;
}

} // namespace tb::render
