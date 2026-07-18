#include "FlightCheckScreen.h"

#include "net/Subprocess.h"

#include "raylib.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <string>

namespace tb::render {
namespace {

using Step = net::FlightCheck::Step;

constexpr Color kBg{18, 20, 28, 255};
constexpr Color kPanel{30, 34, 46, 255};
constexpr Color kPanelHot{44, 50, 66, 255};
constexpr Color kText{220, 224, 235, 255};
constexpr Color kMuted{150, 156, 170, 255};
constexpr Color kAccent{230, 140, 50, 255};
constexpr Color kGood{110, 190, 120, 255};
constexpr Color kBad{210, 90, 90, 255};
constexpr Color kDim{96, 102, 116, 255};
constexpr Color kLine{0, 0, 0, 160};

bool hovered(Rectangle r, Vector2 m) { return CheckCollisionPointRec(m, r); }
bool pressed(Rectangle r, Vector2 m) {
    return hovered(r, m) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

void textCentered(const char* s, Rectangle r, int size, Color c) {
    const int w = MeasureText(s, size);
    DrawText(s, static_cast<int>(r.x + (r.width - w) / 2),
             static_cast<int>(r.y + (r.height - size) / 2), size, c);
}

bool button(Rectangle r, const char* label, Vector2 m, Color base, Color fg = kText) {
    DrawRectangleRec(r, hovered(r, m) ? kPanelHot : base);
    DrawRectangleLinesEx(r, 1.0f, kLine);
    textCentered(label, r, 18, fg);
    return pressed(r, m);
}

// Split "host[:port]" → host + port (defaults to 5555, the client connect port).
void splitHostPort(const std::string& s, std::string& host, std::uint16_t& port) {
    const std::size_t colon = s.rfind(':');
    if (colon == std::string::npos) {
        host = s;
        port = 5555;
        return;
    }
    host = s.substr(0, colon);
    const int p = std::atoi(s.c_str() + colon + 1);
    port = (p > 0 && p < 65536) ? static_cast<std::uint16_t>(p) : 5555;
}

// 0-based ladder index of the first-unmet step (Ready = 6 = all six passed).
int stepIndex(Step s) {
    switch (s) {
    case Step::Install: return 0;
    case Step::StartService: return 1;
    case Step::SignIn: return 2;
    case Step::JoinNetwork: return 3;
    case Step::HostOffline: return 4;
    case Step::HostUnreachable: return 5;
    case Step::Ready: return 6;
    }
    return 0;
}

const std::array<const char*, 6> kRowLabels = {
    "Tailscale is installed", "Tailscale is running",  "Signed in to the network",
    "Host is on your network", "Host is online",       "Game server reachable",
};

} // namespace

void FlightCheckScreen::open(const std::string& server) {
    splitHostPort(server, host_, port_);
    detected_.clear();
    last_.reset();
    kick();
}

void FlightCheckScreen::kick() {
    if (fut_.valid()) return; // a probe is already in flight
    fut_ = std::async(std::launch::async,
                      [h = host_, p = port_] { return net::runFlightCheck(h, p); });
}

FlightCheckScreen::Result FlightCheckScreen::runFrame(int screenW, int screenH) {
    // Adopt a finished probe.
    if (fut_.valid() && fut_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        last_ = fut_.get();
        detected_.clear();
        if (last_->step == Step::Ready && !last_->hostIp.empty())
            detected_ = last_->hostIp + ":" + std::to_string(port_);
    }
    const bool running = fut_.valid();

    const Vector2 m = GetMousePosition();
    ClearBackground(kBg);
    const float W = static_cast<float>(screenW);
    const float panelW = 560.0f;
    const float x = (W - panelW) / 2.0f;

    DrawText("PLAY ONLINE - CONNECTION CHECK", static_cast<int>(x), 40, 26, kText);
    DrawText(TextFormat("Checking the path to  %s:%u", host_.c_str(), port_),
             static_cast<int>(x), 72, 14, kMuted);

    const int active = last_ ? stepIndex(last_->step) : -1;

    // The ladder. Passed rows tick green; the first unmet row is highlighted with
    // its fix; later rows stay dim.
    float y = 110.0f;
    const float rowH = 34.0f;
    for (int i = 0; i < 6; ++i) {
        const bool pass = active > i || (last_ && last_->step == Step::Ready);
        const bool isActive = active == i && !(last_ && last_->step == Step::Ready);

        const char* glyph = pass ? "[ok]" : isActive ? ">>" : "..";
        Color glyphC = pass ? kGood : isActive ? kAccent : kDim;
        Color labelC = pass ? kText : isActive ? kText : kDim;
        if (running && !last_) {
            glyph = "..";
            glyphC = kDim;
            labelC = kMuted;
        }
        DrawText(glyph, static_cast<int>(x), static_cast<int>(y), 18, glyphC);
        DrawText(kRowLabels[static_cast<std::size_t>(i)], static_cast<int>(x) + 44,
                 static_cast<int>(y), 18, labelC);
        y += rowH;
    }

    y += 12.0f;
    Result result = Result::None;

    // Spinner / detail + remediation for the current step.
    Rectangle actionBtn{x, y + 44, 220, 36};
    if (running && !last_) {
        const int dots = 1 + (static_cast<int>(GetTime() * 3.0) % 3);
        DrawText(TextFormat("Scanning your system%.*s", dots, "..."), static_cast<int>(x),
                 static_cast<int>(y), 16, kMuted);
    } else if (last_) {
        const net::FlightCheck& fc = *last_;
        const std::string tailnet = fc.status.tailnet.empty() ? "your tailnet" : fc.status.tailnet;

        auto detail = [&](const char* s) {
            DrawText(s, static_cast<int>(x), static_cast<int>(y), 16, kMuted);
        };
        // Copyable command helper (Linux remediation, where there's no app to open).
        auto copyCmd = [&](const char* cmd) {
            DrawText(cmd, static_cast<int>(x), static_cast<int>(y) + 22, 16, kAccent);
            if (button({x, y + 44, 200, 36}, "Copy command", m, kPanel)) SetClipboardText(cmd);
        };

        switch (fc.step) {
        case Step::Install:
            detail("Tailscale isn't installed on this computer.");
            if (button(actionBtn, "Get Tailscale", m, kAccent))
                net::openUrl("https://tailscale.com/download");
            break;
        case Step::StartService:
            detail("Tailscale is installed but hasn't started yet.");
#if defined(_WIN32) || defined(__APPLE__)
            if (button(actionBtn, "Open Tailscale", m, kAccent)) net::openTailscaleApp();
#else
            copyCmd("sudo systemctl start tailscaled");
#endif
            break;
        case Step::SignIn:
            detail(fc.status.backend == net::TsBackend::Stopped
                       ? "Tailscale is turned off. Turn it on and sign in."
                       : "You're not signed in to Tailscale yet.");
#if defined(_WIN32) || defined(__APPLE__)
            if (button(actionBtn, "Open Tailscale", m, kAccent)) net::openTailscaleApp();
#else
            copyCmd("sudo tailscale up");
#endif
            break;
        case Step::JoinNetwork:
            DrawText(TextFormat("You're signed in to %s, but this host isn't on that network.",
                                tailnet.c_str()),
                     static_cast<int>(x), static_cast<int>(y), 16, kMuted);
            DrawText("Ask the host to invite you to their tailnet, then check again.",
                     static_cast<int>(x), static_cast<int>(y) + 22, 15, kMuted);
            break;
        case Step::HostOffline:
            detail("The host is on your tailnet but currently offline.");
            DrawText("Ask them to launch the machine/game, then check again.", static_cast<int>(x),
                     static_cast<int>(y) + 22, 15, kMuted);
            break;
        case Step::HostUnreachable:
            DrawText(TextFormat("Reached the host, but nothing is listening on port %u.", port_),
                     static_cast<int>(x), static_cast<int>(y), 16, kMuted);
            DrawText("Ask the host to start the game server, then check again.", static_cast<int>(x),
                     static_cast<int>(y) + 22, 15, kMuted);
            break;
        case Step::Ready:
            DrawText(TextFormat("All set - ready to connect to %s.", detected_.c_str()),
                     static_cast<int>(x), static_cast<int>(y), 16, kGood);
            break;
        }
    }

    // Bottom bar: Back · Check again · (Ready → Enter Lobby) + a quiet bypass.
    const float by = static_cast<float>(screenH) - 70.0f;
    if (button({x, by, 130, 40}, "< Back", m, kPanel)) result = Result::Back;
    if (!running && button({x + 146, by, 150, 40}, "Check again", m, kPanel)) kick();

    const bool ready = last_ && last_->step == Step::Ready;
    if (ready) {
        if (button({x + panelW - 190, by, 190, 40}, "Enter Lobby >", m, kAccent))
            result = Result::Proceed;
    } else if (!running) {
        // Don't trap power users behind the wizard.
        if (button({x + panelW - 190, by, 190, 40}, "Continue anyway", m, kPanel, kMuted))
            result = Result::Proceed;
    }

    return result;
}

} // namespace tb::render
