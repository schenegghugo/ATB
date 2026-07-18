#pragma once
//
// FlightCheckScreen.h — the "Play Online" connection flight check (GUI).
//
// Shown the first time a player clicks Play Online (and reachable afterwards from
// the Connect screen). It probes the machine's Tailscale state off-thread and,
// by process of elimination, walks the user down a ladder — install → start →
// sign in → join the network → host online → game reachable — showing only the
// first unmet step and one concrete fix for it. When every step passes it hands
// the auto-detected host address back to main.cpp to prefill the Connect screen.
//
// Immediate-mode like ConnectScreen: runFrame() does input AND draws in one pass.
// The probe (a subprocess + a TCP poke) runs in a std::future so the UI never
// blocks; the user re-runs it with "Check again" after acting on a step.
//
#include "net/TailscaleProbe.h"

#include <cstdint>
#include <future>
#include <optional>
#include <string>

namespace tb::render {

class FlightCheckScreen {
public:
    enum class Result { None, Proceed, Back };

    // Point the check at a server address ("host[:port]") and launch the first
    // probe. Call whenever entering the screen (from the menu or Connect).
    void open(const std::string& server);

    Result runFrame(int screenW, int screenH);

    // "host:port" auto-detected when the check reached Ready (empty otherwise) —
    // main.cpp prefills the Connect screen with it.
    [[nodiscard]] const std::string& detectedServer() const { return detected_; }

private:
    void kick(); // launch an async probe (no-op if one is already running)

    std::string host_ = "127.0.0.1";
    std::uint16_t port_ = 5555;
    std::string detected_;

    std::future<net::FlightCheck> fut_;
    std::optional<net::FlightCheck> last_; // most recent completed result
};

} // namespace tb::render
