#include "DraftScreen.h"

#include "Ui.h"
#include "raylib.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace tb::render {
namespace {
using namespace tb::render::ui;

std::string factionName(Faction f) { return f == Faction::Player ? "your team" : "the enemy"; }
} // namespace

void DraftScreen::begin(const net::DraftInfo& d) {
    d_ = d;
    state_ = {};
    paired_ = {};
    remaining_ = 0.0f;
    pollTimer_ = 1.0f; // force an immediate poll on the first frame
    lastPick_ = -1;
    waitingPaired_ = false;
    status_.clear();
}

Faction DraftScreen::myFaction() const {
    if (d_.mySeat >= 0 && d_.mySeat < static_cast<int>(d_.seats.size()))
        return d_.seats[static_cast<std::size_t>(d_.mySeat)].faction;
    return Faction::Player;
}

bool DraftScreen::myTurn() const {
    if (state_.complete || d_.mySeat < 0) return false;
    if (state_.currentPick < 0 || state_.currentPick >= static_cast<int>(d_.pickOrder.size()))
        return false;
    return d_.pickOrder[static_cast<std::size_t>(state_.currentPick)] == d_.mySeat;
}

// The read-only scout board: both teams, each seat showing its locked (revealed) build,
// a PICKING marker for the active seat, or a hidden placeholder.
void DraftScreen::drawBoard(int screenW, int screenH) const {
    const float W = static_cast<float>(screenW);
    const int curSeat = (!state_.complete && state_.currentPick >= 0 &&
                         state_.currentPick < static_cast<int>(d_.pickOrder.size()))
                            ? d_.pickOrder[static_cast<std::size_t>(state_.currentPick)]
                            : -1;
    const Faction mine = myFaction();
    auto column = [&](float x, const char* title, Faction fac) {
        DrawText(title, static_cast<int>(x), 220, 18, fac == mine ? kAccent : kText);
        float y = 252.0f;
        for (int s = 0; s < static_cast<int>(d_.seats.size()); ++s) {
            const net::DraftSeatInfo& seat = d_.seats[static_cast<std::size_t>(s)];
            if (seat.faction != fac) continue;
            const bool locked = s < static_cast<int>(state_.seats.size()) &&
                                state_.seats[static_cast<std::size_t>(s)].locked;
            Rectangle row{x, y, 300, 44};
            DrawRectangleRec(row, kPanel);
            DrawRectangleLinesEx(row, 1.0f, s == curSeat ? kAccent : kLine);
            const std::string who = std::to_string(seat.index + 1) + "  " + seat.user +
                                    (s == d_.mySeat ? "  (you)" : "");
            DrawText(who.c_str(), static_cast<int>(x) + 10, static_cast<int>(y) + 5, 15, kText);
            std::string sub;
            Color sc = kMuted;
            if (locked) {
                sub = state_.seats[static_cast<std::size_t>(s)].build.name;
                sc = kGood;
            } else if (s == curSeat) {
                sub = "PICKING…";
                sc = kAccent;
            } else {
                sub = "— hidden —";
            }
            DrawText(sub.c_str(), static_cast<int>(x) + 10, static_cast<int>(y) + 24, 13, sc);
            y += 50.0f;
        }
    };
    column(W / 2 - 320, "YOUR TEAM", mine);
    column(W / 2 + 20, "ENEMY TEAM", mine == Faction::Player ? Faction::Enemy : Faction::Player);
}

DraftScreen::Result DraftScreen::runFrame(int screenW, int screenH, net::LobbySession& session,
                                          BuildEditorScreen& editor) {
    // Poll the authoritative draft state on a timer; tick the countdown locally between.
    remaining_ = std::max(0.0f, remaining_ - GetFrameTime());
    pollTimer_ += GetFrameTime();
    if (pollTimer_ >= 0.3f) {
        pollTimer_ = 0.0f;
        if (std::optional<net::DraftState> ds = session.draftPoll(d_.id)) {
            state_ = *ds;
            if (state_.currentPick != lastPick_) { // a pick landed → refresh the clock
                lastPick_ = state_.currentPick;
                remaining_ = static_cast<float>(state_.secondsLeft);
            } else {
                remaining_ = static_cast<float>(state_.secondsLeft);
            }
        }
    }

    // Draft complete → the server has formed the team match; wait for the Paired event.
    if (state_.complete || waitingPaired_) {
        waitingPaired_ = true;
        const net::LobbyEvent ev = session.poll();
        if (ev.kind == net::LobbyEvent::Kind::Paired) {
            paired_ = ev.paired;
            return Result::Matched;
        }
        if (ev.kind == net::LobbyEvent::Kind::Cancelled) return Result::Cancelled;
    }

    // My turn → drive the shared editor in Draft mode (clock in its header) and lock.
    if (myTurn() && !waitingPaired_) {
        // Scout line: the enemy champions revealed so far.
        std::string scout;
        for (int s = 0; s < static_cast<int>(state_.seats.size()); ++s) {
            const net::DraftSeatState& ss = state_.seats[static_cast<std::size_t>(s)];
            if (ss.faction != myFaction() && ss.locked)
                scout += (scout.empty() ? "Scouted: " : ", ") + ss.build.name;
        }
        const std::string label =
            "PICK " + std::to_string(state_.currentPick + 1) + "/" + std::to_string(d_.pickOrder.size());
        editor.setDraftContext(remaining_, label, scout);
        const BuildEditorScreen::Result r =
            editor.runFrame(screenW, screenH, BuildEditorScreen::Mode::Draft);
        if (r == BuildEditorScreen::Result::Lock) {
            std::string err;
            const net::DraftLockResult lr = session.draftLock(d_.id, editor.playerTeam().front(), &err);
            using S = net::DraftLockResult::Status;
            if (lr.status == S::Rejected) status_ = "Build illegal for this format: " + err;
            else if (lr.status == S::Cancelled) return Result::Cancelled;
            else pollTimer_ = 1.0f; // Locked/Complete → refresh the board immediately
        } else if (r == BuildEditorScreen::Result::Menu) {
            return Result::Cancelled; // leave to the lobby (the server times the pick out)
        }
        return Result::None;
    }

    // Otherwise: the scout view — header + countdown + read-only board.
    ClearBackground(kBg);
    const Vector2 m = GetMousePosition();
    const float W = static_cast<float>(screenW);
    const float cx = W / 2.0f;
    DrawText("DRAFT", static_cast<int>(cx - MeasureText("DRAFT", 40) / 2), 40, 40, kText);

    std::string sub;
    if (waitingPaired_ || state_.complete) {
        sub = "Draft complete — starting the match…";
    } else if (state_.currentPick >= 0 && state_.currentPick < static_cast<int>(d_.pickOrder.size())) {
        const int seat = d_.pickOrder[static_cast<std::size_t>(state_.currentPick)];
        const std::string who = d_.seats[static_cast<std::size_t>(seat)].user;
        sub = "Scouting — waiting for " + who + " (" + factionName(d_.seats[seat].faction) + ") to pick…";
    } else {
        sub = "Waiting for the draft to start…";
    }
    DrawText(sub.c_str(), static_cast<int>(cx - MeasureText(sub.c_str(), 18) / 2), 96, 18, kMuted);

    if (!state_.complete && !waitingPaired_) {
        const int secs = static_cast<int>(std::ceil(remaining_));
        const char* clock = TextFormat("%d", secs);
        DrawText(clock, static_cast<int>(cx - MeasureText(clock, 64) / 2), 130, 64,
                 secs <= 5 ? kBad : kAccent);
    }

    drawBoard(screenW, screenH);

    if (!waitingPaired_ &&
        button({cx - 90, static_cast<float>(screenH) - 60, 180, 40}, "Leave draft", m, kPanel)) {
        return Result::Cancelled;
    }
    if (!status_.empty())
        DrawText(status_.c_str(), static_cast<int>(cx - MeasureText(status_.c_str(), 15) / 2),
                 screenH - 96, 15, kBad);
    return Result::None;
}

} // namespace tb::render
