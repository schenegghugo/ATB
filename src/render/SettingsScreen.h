#pragma once
//
// SettingsScreen.h — Read-only settings/info panel (v1).
//
// Shows the loaded content, sprite pack, and network defaults, plus controls.
// Editable settings + persistence to a config file are a follow-up; for now it is
// an informational screen with a Back button. main.cpp supplies the rows.
//
#include <string>
#include <utility>
#include <vector>

namespace tb::render {

class SettingsScreen {
public:
    enum class Result { None, Back };

    // `rows` is a list of (label, value) pairs to display.
    Result runFrame(int screenW, int screenH, const std::vector<std::pair<std::string, std::string>>& rows);
};

} // namespace tb::render
