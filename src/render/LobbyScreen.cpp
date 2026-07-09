#include "LobbyScreen.h"

#include "Ui.h"
#include "raylib.h"

#include <algorithm>
#include <string>

namespace tb::render {
namespace {

using namespace tb::render::ui;

// Clock presets: the label + the timing fields it sets. Unlimited → correspondence;
// the rest → a live match. The rated flag rides separately (a toggle).
struct Preset {
    const char* label;
    net::MatchFormat::Time time;
    int perMove, mainSec, inc;
};
constexpr Preset kPresets[] = {
    {"Unlimited", net::MatchFormat::Time::Unlimited, 0, 0, 0},
    {"30s / move", net::MatchFormat::Time::PerMove, 30, 0, 0},
    {"60s / move", net::MatchFormat::Time::PerMove, 60, 0, 0},
    {"5 min + 5s", net::MatchFormat::Time::Chess, 0, 300, 5},
    {"10 min", net::MatchFormat::Time::Chess, 0, 600, 0},
};
constexpr int kNumPresets = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));

net::MatchFormat formatFrom(int preset, bool rated) {
    const Preset& p = kPresets[preset];
    net::MatchFormat f;
    f.time = p.time;
    f.perMoveSec = p.perMove;
    f.mainSec = p.mainSec;
    f.incSec = p.inc;
    f.rated = rated;
    f.teamSize = 1;
    return f;
}

// A one-word tag for a format, for list rows (e.g. "Unlimited rated").
std::string formatTag(const net::MatchFormat& f) {
    std::string s = f.time == net::MatchFormat::Time::Unlimited ? "Unlimited"
                    : f.time == net::MatchFormat::Time::PerMove
                        ? (std::to_string(f.perMoveSec) + "s/move")
                        : (std::to_string(f.mainSec / 60) + "+" + std::to_string(f.incSec));
    s += f.rated ? " rated" : " casual";
    return s;
}

// A labeled single-line text field (click to focus, edits when focused).
void field(Rectangle box, const char* label, std::string& value, bool& focus, Vector2 m,
           std::size_t maxLen = 24) {
    if (label && *label)
        DrawText(label, static_cast<int>(box.x), static_cast<int>(box.y) - 18, 14, kMuted);
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) focus = hovered(box, m);
    if (focus) {
        int key = GetCharPressed();
        while (key > 0) {
            if (key >= 32 && key < 127 && value.size() < maxLen) value.push_back(static_cast<char>(key));
            key = GetCharPressed();
        }
        if (IsKeyPressed(KEY_BACKSPACE) && !value.empty()) value.pop_back();
    }
    DrawRectangleRec(box, focus ? kPanelHot : kPanel);
    DrawRectangleLinesEx(box, 1.0f, focus ? kAccent : kLine);
    std::string shown = value + (focus ? "_" : "");
    DrawText(shown.c_str(), static_cast<int>(box.x) + 8, static_cast<int>(box.y) + 9, 16, kText);
}

} // namespace

void LobbyScreen::refresh(net::LobbySession& session) {
    if (auto s = session.listSeeks()) seeks_ = std::move(*s);
    if (auto c = session.listChallenges()) challenges_ = std::move(*c);
    if (auto g = session.listGames()) games_ = std::move(*g);
    if (auto mg = session.myCorrGames()) myGames_ = std::move(*mg);
    if (auto cp = session.chatPoll(chatCursor_)) { // lobby chat (4.6)
        for (net::MailEntry& e : cp->entries) chat_.push_back(std::move(e));
        chatCursor_ = cp->next;
        if (chat_.size() > 100) chat_.erase(chat_.begin(), chat_.end() - 100);
    }
}

LobbyScreen::Result LobbyScreen::runFrame(int screenW, int screenH, net::LobbySession& session,
                                          const CharacterBuild& myBuild, bool ratedAvailable) {
    const Vector2 m = GetMousePosition();
    ClearBackground(kBg);

    // Throttled polling: learn of a pairing + refresh the boards on a timer so a
    // blocking RPC never runs every frame.
    refreshTimer_ += GetFrameTime();
    if (refreshTimer_ >= 0.8f) {
        refreshTimer_ = 0.0f;
        const net::LobbyEvent ev = session.poll();
        if (ev.kind == net::LobbyEvent::Kind::ReadyCheck) { // accepted seek or queue match
            rc_ = ev.readyCheck;
            queued_ = false; // a queue match consumed the slot
            return Result::ReadyCheck;
        }
        refresh(session);
    }
    if (!ratedAvailable) rated_ = false;

    const float W = static_cast<float>(screenW);
    const float margin = 40.0f;

    // Header.
    const std::string who = session.guest()
                                ? std::string("Guest (casual only)")
                                : (session.account().user + "  (" +
                                   std::to_string(session.account().rating) + " Elo)");
    DrawText("ONLINE HOME", static_cast<int>(margin), 34, 26, kText);
    DrawText(who.c_str(), static_cast<int>(margin), 66, 14, kMuted);
    Result result = Result::None;
    if (button({W - margin - 130, 34, 130, 34}, "< Back", m, kPanel)) result = Result::Back;
    // Current build + edit: the build you seek/challenge/accept with comes from the
    // editor; "Edit build" pops to it and returns here.
    const std::string buildLabel =
        std::string("Build: ") + (myBuild.name.empty() ? "(unnamed)" : myBuild.name);
    DrawText(buildLabel.c_str(), static_cast<int>(W - margin - 320), 72, 14, kMuted);
    if (button({W - margin - 130, 70, 130, 30}, "Edit build", m, kPanel)) result = Result::EditBuild;

    // Format bar: rated toggle + clock presets.
    float y = 104.0f;
    DrawText("FORMAT", static_cast<int>(margin), static_cast<int>(y) - 20, 14, kMuted);
    Rectangle ratedBtn{margin, y, 120, 32};
    if (button(ratedBtn, rated_ ? "Rated: ON" : "Rated: OFF", m, rated_ ? kAccent : kPanel,
               ratedAvailable))
        rated_ = !rated_;
    float px = margin + 136;
    for (int i = 0; i < kNumPresets; ++i) {
        Rectangle b{px, y, 118, 32};
        if (button(b, kPresets[i].label, m, i == preset_ ? kAccent : kPanel)) preset_ = i;
        px += 124;
    }
    const net::MatchFormat fmt = formatFrom(preset_, rated_);

    // Two columns.
    y = 172.0f;
    const float colW = (W - 3 * margin) / 2.0f;
    const float lx = margin;
    const float rx = margin * 2 + colW;

    // --- Left: open seeks ---------------------------------------------------
    DrawText("OPEN SEEKS", static_cast<int>(lx), static_cast<int>(y), 16, kText);
    // Quick match: join the auto-pairing queue (widening Elo band) — the ready
    // check arrives via poll, exactly like an accepted seek.
    if (button({lx + colW - 310, y - 4, 150, 28}, queued_ ? "Leave queue" : "Quick match", m,
               queued_ ? kPanelHot : kAccent)) {
        if (queued_) {
            session.queueLeave();
            queued_ = false;
            status_ = "Left the queue.";
        } else {
            std::string err;
            if (session.queueJoin(fmt, &err)) {
                queued_ = true;
                status_ = "In queue — pairing widens over time…";
            } else {
                status_ = "Queue rejected: " + err;
            }
        }
    }
    if (button({lx + colW - 150, y - 4, 150, 28}, "Create seek", m, kAccent)) {
        std::string err;
        if (session.seek(fmt, &err)) status_ = "Seek posted — waiting for an opponent…";
        else status_ = "Seek rejected: " + err;
        refresh(session);
    }
    float ry = y + 36;
    for (const net::SeekInfo& s : seeks_) {
        Rectangle row{lx, ry, colW, 40};
        DrawRectangleRec(row, kPanel);
        DrawRectangleLinesEx(row, 1.0f, kLine);
        DrawText(TextFormat("%s  (%d)", s.user.c_str(), s.rating), static_cast<int>(lx) + 10,
                 static_cast<int>(ry) + 6, 15, kText);
        DrawText(formatTag(s.format).c_str(), static_cast<int>(lx) + 10, static_cast<int>(ry) + 23,
                 12, kMuted);
        if (session.account().user != s.user &&
            button({lx + colW - 92, ry + 6, 84, 28}, "Accept", m, kAccent)) {
            std::string err;
            if (std::optional<net::ReadyCheckInfo> r = session.acceptSeek(s.id, &err)) {
                rc_ = *r;
                return Result::ReadyCheck;
            }
            status_ = "Accept failed: " + err;
        }
        ry += 46;
    }
    if (seeks_.empty()) {
        DrawText("(none yet)", static_cast<int>(lx) + 4, static_cast<int>(y) + 40, 14, kMuted);
        ry += 46;
    }

    // --- Left, below the seeks: live games (spectate, Phase 5.2) ------------
    float wy = ry + 24;
    DrawText("LIVE GAMES", static_cast<int>(lx), static_cast<int>(wy), 16, kText);
    wy += 28;
    for (const net::LiveGameInfo& g : games_) {
        Rectangle row{lx, wy, colW, 40};
        DrawRectangleRec(row, kPanel);
        DrawRectangleLinesEx(row, 1.0f, kLine);
        DrawText(TextFormat("%s  vs  %s", g.userP.c_str(), g.userE.c_str()),
                 static_cast<int>(lx) + 10, static_cast<int>(wy) + 6, 15, kText);
        DrawText(g.rated ? "rated" : "casual", static_cast<int>(lx) + 10,
                 static_cast<int>(wy) + 23, 12, kMuted);
        if (button({lx + colW - 92, wy + 6, 84, 28}, "Watch", m, kPanelHot)) {
            watch_ = g;
            return Result::Watch;
        }
        wy += 46;
    }
    if (games_.empty())
        DrawText("(none in progress)", static_cast<int>(lx) + 4, static_cast<int>(wy), 14, kMuted);

    // --- Right: challenge form + incoming challenges ------------------------
    DrawText("CHALLENGE A PLAYER", static_cast<int>(rx), static_cast<int>(y), 16, kText);
    field({rx, y + 52, colW - 150, 32}, "Their username", challengeUser_, challengeFocus_, m);
    if (button({rx + colW - 140, y + 52, 140, 32}, "Send challenge", m, kAccent)) {
        std::string err;
        if (challengeUser_.empty()) status_ = "Enter a username to challenge.";
        else if (session.challenge(challengeUser_, fmt, &err))
            status_ = "Challenge sent to " + challengeUser_ + ".";
        else status_ = "Challenge rejected: " + err;
    }

    float cy = y + 116;
    DrawText("INCOMING CHALLENGES", static_cast<int>(rx), static_cast<int>(cy) - 8, 14, kMuted);
    cy += 14;
    for (const net::ChallengeInfo& c : challenges_) {
        Rectangle row{rx, cy, colW, 40};
        DrawRectangleRec(row, kPanel);
        DrawRectangleLinesEx(row, 1.0f, kLine);
        DrawText(TextFormat("%s  (%d)", c.from.c_str(), c.fromRating), static_cast<int>(rx) + 10,
                 static_cast<int>(cy) + 6, 15, kText);
        DrawText(formatTag(c.format).c_str(), static_cast<int>(rx) + 10, static_cast<int>(cy) + 23,
                 12, kMuted);
        if (button({rx + colW - 176, cy + 6, 80, 28}, "Accept", m, kAccent)) {
            std::string err;
            if (std::optional<net::ReadyCheckInfo> r = session.acceptChallenge(c.id, &err)) {
                rc_ = *r;
                return Result::ReadyCheck;
            }
            status_ = "Accept failed: " + err;
        }
        if (button({rx + colW - 90, cy + 6, 80, 28}, "Decline", m, kPanel)) {
            session.declineChallenge(c.id);
            refresh(session);
        }
        cy += 46;
    }
    if (challenges_.empty()) {
        DrawText("(none)", static_cast<int>(rx) + 4, static_cast<int>(cy), 14, kMuted);
        cy += 24;
    }

    // --- Right, below the challenges: my correspondence games (cold resume) --
    cy += 18;
    DrawText("MY CORRESPONDENCE GAMES", static_cast<int>(rx), static_cast<int>(cy), 14, kMuted);
    cy += 22;
    for (const net::PairedInfo& g : myGames_) {
        Rectangle row{rx, cy, colW, 40};
        DrawRectangleRec(row, kPanel);
        DrawRectangleLinesEx(row, 1.0f, kLine);
        DrawText(TextFormat("vs  %s", g.opponent.empty() ? "?" : g.opponent.c_str()),
                 static_cast<int>(rx) + 10, static_cast<int>(cy) + 6, 15, kText);
        DrawText(g.rated ? "rated - unlimited" : "casual - unlimited", static_cast<int>(rx) + 10,
                 static_cast<int>(cy) + 23, 12, kMuted);
        if (button({rx + colW - 92, cy + 6, 84, 28}, "Resume", m, kAccent)) {
            resume_ = g;
            return Result::Resume;
        }
        cy += 46;
    }
    if (myGames_.empty())
        DrawText("(none open)", static_cast<int>(rx) + 4, static_cast<int>(cy), 14, kMuted);

    // --- Bottom: lobby chat (4.6) --------------------------------------------
    // A rolling transcript + input strip above the status line. Enter sends; the
    // server's safety levers (length / rate / mute) reject with a reason.
    {
        const float chatH = 150.0f;
        const float cy0 = static_cast<float>(screenH) - 44 - chatH;
        Rectangle panel{margin, cy0, W - 2 * margin, chatH - 40};
        DrawRectangleRec(panel, kPanel);
        DrawRectangleLinesEx(panel, 1.0f, kLine);
        DrawText("LOBBY CHAT", static_cast<int>(margin), static_cast<int>(cy0) - 18, 14, kMuted);
        const int lines = static_cast<int>((panel.height - 12) / 18);
        const int first = std::max(0, static_cast<int>(chat_.size()) - lines);
        float ty = cy0 + 6;
        for (std::size_t i = static_cast<std::size_t>(first); i < chat_.size(); ++i) {
            const bool mine = chat_[i].sender == session.account().user;
            DrawText(TextFormat("%s: %s", chat_[i].sender.c_str(), chat_[i].msg.c_str()),
                     static_cast<int>(margin) + 8, static_cast<int>(ty), 14,
                     mine ? kAccent : kText);
            ty += 18;
        }
        if (chat_.empty())
            DrawText("(say hello)", static_cast<int>(margin) + 8, static_cast<int>(ty), 14, kMuted);

        field({margin, cy0 + panel.height + 6, W - 2 * margin - 90, 30}, "", chatDraft_,
              chatFocus_, m, /*maxLen=*/200);
        const bool send =
            button({W - margin - 84, cy0 + panel.height + 6, 84, 30}, "Send", m, kAccent) ||
            (chatFocus_ && IsKeyPressed(KEY_ENTER));
        if (send && !chatDraft_.empty()) {
            std::string err;
            if (session.chatSend(chatDraft_, &err)) {
                chatDraft_.clear();
                refreshTimer_ = 1.0f; // pick my line up on the next frame
            } else {
                status_ = "Chat rejected: " + err;
            }
        }
    }

    if (!status_.empty())
        DrawText(status_.c_str(), static_cast<int>(margin), screenH - 36, 15, kAccent);

    return result;
}

} // namespace tb::render
