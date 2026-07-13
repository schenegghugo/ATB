#pragma once
//
// MainMenuScreen.h — The title / landing screen (mode-first).
//
// Immediate-mode like the other screens. Stateless: runFrame() returns the chosen
// action; main.cpp routes it (Local/Online carry an intent into the build editor).
//
namespace tb::render {

class MainMenuScreen {
public:
    enum class Result { None, LocalMatch, PlayOnline, BuildEditor, Settings, Quit };

    // `version` (e.g. the compiled ATB_VERSION) is shown in the footer.
    Result runFrame(int screenW, int screenH, const char* version = "");
};

} // namespace tb::render
