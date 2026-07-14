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
    enum class Result { None, ReadyCheck, Back, EditBuild, Watch, Resume };

    // `session` is the live lobby connection; `myBuild` shows the current build (the
    // one you'll ready with); `ratedAvailable` gates the rated toggle (false for
    // guests or when the ranked ruleset isn't loaded locally). Accepting a seek /
    // challenge (or someone accepting yours, via poll) returns ReadyCheck.
    Result runFrame(int screenW, int screenH, net::LobbySession& session,
                    const CharacterBuild& myBuild, bool ratedAvailable);

    [[nodiscard]] const net::ReadyCheckInfo& readyCheck() const { return rc_; }
    // The live game picked from the "Live games" list (valid when Watch returned).
    [[nodiscard]] const net::LiveGameInfo& watchGame() const { return watch_; }
    // The correspondence game picked for cold resume (valid when Resume returned).
    [[nodiscard]] const net::PairedInfo& resumeGame() const { return resume_; }
    void setStatus(std::string s) { status_ = std::move(s); }

private:
    void refresh(net::LobbySession& session);

    net::ReadyCheckInfo rc_;
    net::LiveGameInfo watch_;
    net::PairedInfo resume_;
    std::string status_;
    std::string challengeUser_; // the username to direct-challenge
    bool challengeFocus_ = false;

    bool rated_ = false;
    int preset_ = 0; // index into the clock presets
    int teamSize_ = 1; // 1/2/3 → 1v1 / 2v2 / 3v3 (seeks match same team size)
    bool queued_ = false; // in the quick-match queue

    std::vector<net::SeekInfo> seeks_;
    std::vector<net::ChallengeInfo> challenges_;
    std::vector<net::LiveGameInfo> games_; // live matches (watchable)
    std::vector<net::PairedInfo> myGames_; // my correspondence games (resumable)

    // Lobby chat (4.6): rolling transcript + draft input.
    std::vector<net::MailEntry> chat_;
    std::size_t chatCursor_ = 0;
    std::string chatDraft_;
    bool chatFocus_ = false;
    float refreshTimer_ = 1.0f; // forces an immediate refresh on first frame
};

} // namespace tb::render
