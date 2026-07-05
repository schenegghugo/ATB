#pragma once
//
// ConnectScreen.h — Raylib UI for joining a networked match (Phase 4.5).
//
// Collects the server address + optional ranked login + optional private-lobby
// code, then hands them back to main.cpp, which drives the MirrorSession/
// RemoteMatchSource. Immediate-mode like BuildEditorScreen: runFrame() does input
// AND draws in one pass (between BeginDrawing()/EndDrawing()).
//
#include <string>

namespace tb::render {

class ConnectScreen {
public:
    enum class Result { None, Connect, Back };

    struct Params {
        std::string host = "127.0.0.1:5555"; // host[:port]
        std::string user;                    // ranked login (blank = custom/unranked)
        std::string pass;
        std::string lobby; // private room code shared with a friend (blank = open matchmaking)
    };

    explicit ConnectScreen(Params defaults) : p_(std::move(defaults)) {}

    // One frame of input + drawing. Returns Connect when the user commits.
    Result runFrame(int screenW, int screenH);

    [[nodiscard]] const Params& params() const { return p_; }
    void setStatus(std::string s) { status_ = std::move(s); } // e.g. a connect error to show

private:
    Params p_;
    int focus_ = 0; // which text field has keyboard focus (0..3), -1 = none
    std::string status_;
};

} // namespace tb::render
