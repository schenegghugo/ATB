#pragma once
//
// DraftScreen.h — the NvN draft/scout pre-game (team pre-game redesign, slice 6, GUI).
//
// After a team seek/challenge pairs two parties, all 2N players land here and draft one
// champion at a time in snake order. The per-pick countdown is ALWAYS on screen (the fix
// for "the clock isn't shown in the editor"): on YOUR turn the shared BuildEditorScreen
// is driven in Draft mode with the clock in its header; otherwise you SCOUT — a read-only
// board of every seat (locked builds revealed, so you can counter-build). When the last
// pick lands the server forms the team match; this screen waits for that Paired event and
// returns Matched, and main.cpp routes into the live match via routePairing().
//
// Immediate-mode like the other screens: runFrame() does input AND draws in one pass.
//
#include "BuildEditorScreen.h"
#include "net/Lobby.h"

#include <string>

namespace tb::render {

class DraftScreen {
public:
    enum class Result { None, Matched, Cancelled };

    // Enter a fresh draft (resets the polled state + local countdown).
    void begin(const net::DraftInfo& d);

    // One frame. `editor` is the shared build editor (its front build is what you lock).
    // Returns Matched (pairing() valid → the match), or Cancelled (→ lobby).
    Result runFrame(int screenW, int screenH, net::LobbySession& session, BuildEditorScreen& editor);

    [[nodiscard]] const net::PairedInfo& pairing() const { return paired_; }
    void setStatus(std::string s) { status_ = std::move(s); }

private:
    [[nodiscard]] bool myTurn() const;
    [[nodiscard]] Faction myFaction() const;
    void drawBoard(int screenW, int screenH) const;

    net::DraftInfo d_;
    net::DraftState state_;
    net::PairedInfo paired_;
    float remaining_ = 0.0f; // local per-pick countdown (server enforces its own)
    float pollTimer_ = 0.0f;
    int lastPick_ = -1;      // detect a pick advance → reset the countdown
    bool waitingPaired_ = false;
    std::string status_;
};

} // namespace tb::render
