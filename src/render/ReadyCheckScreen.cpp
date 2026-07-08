#include "ReadyCheckScreen.h"

#include "Ui.h"
#include "raylib.h"

#include <cmath>
#include <string>

namespace tb::render {
namespace {
using namespace tb::render::ui;

std::string formatTag(const net::MatchFormat& f) {
    std::string s = f.time == net::MatchFormat::Time::Unlimited ? "Unlimited (correspondence)"
                    : f.time == net::MatchFormat::Time::PerMove
                        ? (std::to_string(f.perMoveSec) + "s / move")
                        : (std::to_string(f.mainSec / 60) + " min + " + std::to_string(f.incSec) + "s");
    return s + (f.rated ? "  ·  rated" : "  ·  casual");
}
} // namespace

void ReadyCheckScreen::begin(const net::ReadyCheckInfo& rc) {
    rc_ = rc;
    paired_ = {};
    remaining_ = static_cast<float>(rc.seconds);
    readied_ = false;
    pollTimer_ = 0.0f;
    status_.clear();
}

ReadyCheckScreen::Result ReadyCheckScreen::runFrame(int screenW, int screenH,
                                                    net::LobbySession& session,
                                                    const CharacterBuild& myBuild) {
    const Vector2 m = GetMousePosition();
    ClearBackground(kBg);

    remaining_ -= GetFrameTime();
    if (remaining_ <= 0.0f) { // ran out the window → cancel for both
        session.cancelReady(rc_.id);
        return Result::Cancelled;
    }

    // While readied, poll for the outcome (opponent readied → Paired; or Cancelled).
    if (readied_) {
        pollTimer_ += GetFrameTime();
        if (pollTimer_ >= 0.3f) {
            pollTimer_ = 0.0f;
            const net::LobbyEvent ev = session.poll();
            if (ev.kind == net::LobbyEvent::Kind::Paired) {
                paired_ = ev.paired;
                return Result::Matched;
            }
            if (ev.kind == net::LobbyEvent::Kind::Cancelled) return Result::Cancelled;
        }
    }

    const float W = static_cast<float>(screenW);
    const float cx = W / 2.0f;
    DrawText("MATCH FOUND", static_cast<int>(cx - MeasureText("MATCH FOUND", 40) / 2), 90, 40, kText);
    const std::string vs = "vs " + rc_.opponent + "   (you: " +
                           (rc_.seat == Faction::Player ? "Player" : "Enemy") + ")";
    DrawText(vs.c_str(), static_cast<int>(cx - MeasureText(vs.c_str(), 20) / 2), 148, 20, kMuted);
    const std::string fmt = formatTag(rc_.format);
    DrawText(fmt.c_str(), static_cast<int>(cx - MeasureText(fmt.c_str(), 18) / 2), 176, 18, kMuted);

    // Big countdown.
    const int secs = static_cast<int>(std::ceil(remaining_));
    const char* clock = TextFormat("%d", secs);
    DrawText(clock, static_cast<int>(cx - MeasureText(clock, 72) / 2), 220, 72,
             secs <= 5 ? kBad : kAccent);

    // Current build.
    const std::string b = "Build: " + (myBuild.name.empty() ? std::string("(unnamed)") : myBuild.name);
    DrawText(b.c_str(), static_cast<int>(cx - MeasureText(b.c_str(), 18) / 2), 312, 18, kText);

    Result result = Result::None;
    const float by = 356.0f;
    if (readied_) {
        const char* w = "Waiting for your opponent…";
        DrawText(w, static_cast<int>(cx - MeasureText(w, 20) / 2), static_cast<int>(by), 20, kGood);
        if (button({cx - 90, by + 40, 180, 40}, "Cancel", m, kPanel)) {
            session.cancelReady(rc_.id);
            result = Result::Cancelled;
        }
    } else {
        if (button({cx - 280, by, 170, 44}, "Edit build", m, kPanel)) result = Result::EditBuild;
        if (button({cx - 95, by, 190, 44}, "READY", m, kAccent)) {
            std::string err;
            const net::ReadyResult r = session.ready(rc_.id, myBuild, &err);
            switch (r.status) {
                case net::ReadyResult::Status::Matched: paired_ = r.paired; result = Result::Matched; break;
                case net::ReadyResult::Status::Waiting: readied_ = true; break;
                case net::ReadyResult::Status::Cancelled: result = Result::Cancelled; break;
                case net::ReadyResult::Status::Rejected:
                    status_ = "Build illegal for this format: " + err + " — Edit build.";
                    break;
            }
        }
        if (button({cx + 110, by, 170, 44}, "Decline", m, kBad)) {
            session.cancelReady(rc_.id);
            result = Result::Cancelled;
        }
    }

    if (!status_.empty())
        DrawText(status_.c_str(), static_cast<int>(cx - MeasureText(status_.c_str(), 15) / 2),
                 screenH - 40, 15, kBad);
    return result;
}

} // namespace tb::render
