//
// tailscale_demo.cpp — Test for the online flight-check probe (net/TailscaleProbe).
//
// Exercises the PURE parts — `tailscale status --json` parsing, peer matching, and
// the flight-check step ladder — against captured fixtures under tests/fixtures/.
// The side-effecting bits (running the CLI, TCP poke) aren't tested here; they're
// verified in-game. The tailscale_running.json fixture is real output captured on
// a live tailnet; nostate/needslogin are hand-authored for those states.
//
#include "net/TailscaleProbe.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace tb::net;

static int g_fails = 0;
#define CHECK(cond, msg)                                                                            \
    do {                                                                                            \
        if (cond) std::printf("  [PASS] %s\n", msg);                                                \
        else { std::printf("  [FAIL] %s\n", msg); ++g_fails; }                                      \
    } while (0)

static std::string slurp(const std::string& path) {
    std::ifstream f(path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int main(int argc, char** argv) {
    // The test runs from the build dir; allow an explicit fixtures path override.
    const std::string dir = argc > 1 ? argv[1] : "tests/fixtures/";

    std::printf("Parse a live 'Running' status (real tailnet capture)\n");
    {
        const TsStatus s = [&] {
            TsStatus t = parseTailscaleStatus(slurp(dir + "tailscale_running.json"));
            t.installed = true; // probeTailscale would set this; parse leaves it false
            return t;
        }();
        CHECK(s.daemonUp, "daemon reported a BackendState");
        CHECK(s.backend == TsBackend::Running, "BackendState == Running");
        CHECK(s.loggedIn(), "loggedIn() true when Running");
        CHECK(s.tailnet == "schenegghugo.github", "CurrentTailnet.Name parsed");
        CHECK(!s.peers.empty(), "peers parsed");

        const TsPeer* byIp = s.findPeer("100.81.97.122");
        CHECK(byIp != nullptr, "findPeer by tailnet IP");
        CHECK(byIp && byIp->host == "Homeboi", "matched the right host by IP");
        CHECK(byIp && byIp->online, "host reported online");

        const TsPeer* byName = s.findPeer("homeboi"); // case-insensitive hostname
        CHECK(byName == byIp, "findPeer by name matches the same peer (case-insensitive)");
        CHECK(s.findPeer("Homeboi") == byIp, "findPeer by exact HostName");
        CHECK(s.findPeer("nope") == nullptr, "findPeer returns null for a stranger");
        CHECK(s.findPeer("") == nullptr, "findPeer('') is null, not a crash");

        // Full ladder: everything green up to the port poke.
        CHECK(flightStepFor(s, byIp, /*tcpOk=*/true) == FlightCheck::Step::Ready,
              "Ready when host online + port answers");
        CHECK(flightStepFor(s, byIp, /*tcpOk=*/false) == FlightCheck::Step::HostUnreachable,
              "HostUnreachable when the port doesn't answer");
        CHECK(flightStepFor(s, nullptr, false) == FlightCheck::Step::JoinNetwork,
              "JoinNetwork when the host isn't a peer");

        TsPeer offline = *byIp;
        offline.online = false;
        CHECK(flightStepFor(s, &offline, false) == FlightCheck::Step::HostOffline,
              "HostOffline when the peer is offline");
    }

    std::printf("Parse a 'NoState' status (daemon/service still coming up)\n");
    {
        TsStatus s = parseTailscaleStatus(slurp(dir + "tailscale_nostate.json"));
        s.installed = true;
        CHECK(s.backend == TsBackend::NoState, "BackendState == NoState");
        CHECK(!s.loggedIn(), "not logged in");
        // The exact bug that bit the Windows client: installed, service not up.
        CHECK(flightStepFor(s, nullptr, false) == FlightCheck::Step::StartService,
              "NoState → StartService step (regression: the Windows 'NoState' wall)");
    }

    std::printf("Parse a 'NeedsLogin' status (up, not signed in)\n");
    {
        TsStatus s = parseTailscaleStatus(slurp(dir + "tailscale_needslogin.json"));
        s.installed = true;
        CHECK(s.backend == TsBackend::NeedsLogin, "BackendState == NeedsLogin");
        CHECK(flightStepFor(s, nullptr, false) == FlightCheck::Step::SignIn,
              "NeedsLogin → SignIn step");
    }

    std::printf("Not installed → Install step, regardless of anything else\n");
    {
        TsStatus s; // installed=false
        CHECK(flightStepFor(s, nullptr, false) == FlightCheck::Step::Install,
              "no binary → Install step");
    }

    std::printf("Malformed / empty input degrades to a safe default (no crash)\n");
    {
        const TsStatus s = parseTailscaleStatus("not json at all {{{");
        CHECK(!s.daemonUp && s.backend == TsBackend::Unknown, "garbage → Unknown, daemonUp false");
        CHECK(parseTailscaleStatus("").peers.empty(), "empty input → empty status");
    }

    if (g_fails == 0) std::printf("\nAll tailscale probe tests passed.\n");
    else std::printf("\n%d tailscale probe test(s) FAILED.\n", g_fails);
    return g_fails == 0 ? 0 : 1;
}
