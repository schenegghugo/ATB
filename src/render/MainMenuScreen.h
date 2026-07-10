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

    // The web build hides entries that make no sense in a browser: showOnline
    // (no TCP transport) and showQuit (a tab isn't quit from a button).
    Result runFrame(int screenW, int screenH, bool showOnline = true, bool showQuit = true);
};

} // namespace tb::render
