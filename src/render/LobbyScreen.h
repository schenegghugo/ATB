#pragma once
//
// LobbyScreen.h — The Online Home UI (Phase 4.5 slice 5, GUI).
//
// Drives a connected net::LobbySession: browse/create open seeks, send + accept
// directed challenges, and pick a format (rated toggle + a clock preset that
// selects Unlimited → correspondence, or Per-move / Chess → a live match). When a
// pairing lands (you accept one, or someone accepts yours — learned via poll), it
// returns Paired and main.cpp routes into the right MatchSource.
//
// Immediate-mode like ConnectScreen: runFrame() does input AND draws in one pass.
// Blocking lobby RPCs (list/poll) are throttled to a timer so the UI stays smooth
// on localhost/LAN (async I/O is a follow-up).
//
#include "net/Lobby.h"

#include <string>
#include <vector>

namespace tb::render {

class LobbyScreen {
public:
    enum class Result { None, Paired, Back, EditBuild };

    // `session` is the live lobby connection; `myBuild` is the team's champion used
    // when seeking/challenging/accepting; `ratedAvailable` gates the rated toggle
    // (false for guests or when the ranked ruleset isn't loaded locally).
    Result runFrame(int screenW, int screenH, net::LobbySession& session,
                    const CharacterBuild& myBuild, bool ratedAvailable);

    [[nodiscard]] const net::PairedInfo& pairing() const { return pairing_; }
    void setStatus(std::string s) { status_ = std::move(s); }

private:
    void refresh(net::LobbySession& session);

    net::PairedInfo pairing_;
    std::string status_;
    std::string challengeUser_; // the username to direct-challenge
    bool challengeFocus_ = false;

    bool rated_ = false;
    int preset_ = 0; // index into the clock presets

    std::vector<net::SeekInfo> seeks_;
    std::vector<net::ChallengeInfo> challenges_;
    float refreshTimer_ = 1.0f; // forces an immediate refresh on first frame
};

} // namespace tb::render
