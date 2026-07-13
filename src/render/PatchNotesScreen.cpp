#include "PatchNotesScreen.h"

#include "Ui.h"
#include "raylib.h"

#include <algorithm>
#include <string>
#include <vector>

namespace tb::render {

namespace {
// Split the notes blob into lines once (cheap; the text is small).
std::vector<std::string> splitLines(const char* text) {
    std::vector<std::string> lines;
    std::string cur;
    for (const char* p = text; *p; ++p) {
        if (*p == '\n') { lines.push_back(cur); cur.clear(); }
        else cur.push_back(*p);
    }
    lines.push_back(cur);
    return lines;
}
} // namespace

PatchNotesScreen::Result PatchNotesScreen::runFrame(int screenW, int screenH) {
    const Vector2 m = GetMousePosition();
    ClearBackground(ui::kBg);

    const float W = static_cast<float>(screenW);
    const float H = static_cast<float>(screenH);

    const char* title = "PATCH NOTES";
    DrawText(title, 24, 20, 34, ui::kText);
    DrawText(kVersionCodename, 24, 60, 16, ui::kAccent);

    // Scrollable body between the header and the bottom bar.
    const int lineH = 20, fontSize = 16;
    const float top = 92.0f, bottom = H - 60.0f;
    const int viewH = static_cast<int>(bottom - top);
    const std::vector<std::string> lines = splitLines(kPatchNotes);
    const int contentH = static_cast<int>(lines.size()) * lineH;
    const int maxScroll = std::max(0, contentH - viewH);

    // Wheel scrolls; clamp so you can't overscroll past either end.
    scroll_ = std::clamp(scroll_ - static_cast<int>(GetMouseWheelMove()) * lineH * 3, 0, maxScroll);

    BeginScissorMode(24, static_cast<int>(top), screenW - 48, viewH);
    int y = static_cast<int>(top) - scroll_;
    for (const std::string& ln : lines) {
        if (y + lineH >= static_cast<int>(top) && y <= static_cast<int>(bottom)) {
            // Indented sub-lines and headings read a touch dimmer / brighter.
            const bool heading = !ln.empty() && ln[0] != ' ' && ln.find("  ") == std::string::npos;
            DrawText(ln.c_str(), 28, y, fontSize, heading ? ui::kText : ui::kMuted);
        }
        y += lineH;
    }
    EndScissorMode();

    // A scrollbar hint when the content overflows.
    if (maxScroll > 0) {
        const float trackH = bottom - top;
        const float thumbH = std::max(24.0f, trackH * viewH / static_cast<float>(contentH));
        const float thumbY = top + (trackH - thumbH) * scroll_ / static_cast<float>(maxScroll);
        DrawRectangle(screenW - 20, static_cast<int>(thumbY), 5, static_cast<int>(thumbH),
                      ui::kPanelHot);
    }

    Result result = Result::None;
    if (ui::button({24, H - 46, 140, 34}, "< Back", m, ui::kPanel)) result = Result::Back;
    DrawText("scroll to read  -  Esc to go back", 180, static_cast<int>(H) - 38, 13, ui::kMuted);
    return result;
}

} // namespace tb::render
