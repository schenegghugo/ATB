#pragma once
//
// SettingsScreen.h — Settings: live theme + sprite-pack pickers (v2).
//
// The screen stays stateless: main.cpp feeds it the discovered themes/packs
// and the current selection each frame; a click reports back as a Result and
// main.cpp applies + persists it (settings.json via data/Prefs). The info rows
// from v1 remain at the top.
//
#include <string>
#include <utility>
#include <vector>

namespace tb::render {

class SettingsScreen {
public:
    enum class Result {
        None,
        Back,
        SetTheme,    // picked() names themes/<name>.json ("" = built-in defaults)
        SetPack,     // picked() names packs/<name>     ("" = primitives, no pack)
        ReloadTheme, // re-read the current theme file (ricing: edit, then reload)
        ScaleUp,     // nudge the UI scale up (resizable board)
        ScaleDown,   // nudge the UI scale down
    };

    struct View {
        std::vector<std::pair<std::string, std::string>> rows; // (label, value) info
        std::vector<std::string> themes; // names discovered under themes/
        std::vector<std::string> packs;  // names discovered under packs/
        std::string curTheme;            // "" = built-in defaults
        std::string curPack;             // "" = primitives
        float uiScale = 1.0f;            // current board size multiplier
        std::string status;              // last apply result / load errors
    };

    Result runFrame(int screenW, int screenH, const View& view);

    // The theme/pack name a SetTheme/SetPack result refers to.
    [[nodiscard]] const std::string& picked() const { return picked_; }

private:
    std::string picked_;
};

} // namespace tb::render
