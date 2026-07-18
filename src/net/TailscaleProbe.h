#pragma once
//
// TailscaleProbe.h — read the machine's Tailscale state to drive the "Play Online"
// flight check (Online onboarding, see MILESTONES.md).
//
// Everything comes from one `tailscale status --json` invocation, parsed with the
// project's own JSON reader (data/Json). The parse step is pure and unit-tested
// against captured fixtures (tests/tailscale_demo.cpp); the probe/flight-check
// steps shell out (Subprocess) and open a TCP socket (Socket), so they live in
// tb_transport, not tb_core. The client cannot install Tailscale or sign a user in
// — those are privileged, per-user steps — so this only ever *detects and guides*.
//
#include <cstdint>
#include <string>
#include <vector>

namespace tb::net {

// Mirrors Tailscale's ipn BackendState. NoState/Starting = daemon coming up;
// NeedsLogin/NeedsMachineAuth/Stopped = up but not usable; Running = good.
enum class TsBackend { NoState, NeedsLogin, NeedsMachineAuth, Stopped, Starting, Running, Unknown };

// One device on the tailnet (a `Peer` entry, or the machine itself).
struct TsPeer {
    std::string host;             // HostName, e.g. "Homeboi"
    std::string dns;              // DNSName leaf (lowercased), e.g. "homeboi"
    std::vector<std::string> ips; // TailscaleIPs (IPv4 first, as Tailscale orders them)
    bool online = false;          // reachable right now
};

// The parsed result of `tailscale status --json`.
struct TsStatus {
    bool installed = false; // the tailscale binary was found on this machine
    bool daemonUp = false;  // status --json produced a BackendState (daemon answered)
    TsBackend backend = TsBackend::Unknown;
    std::string tailnet; // CurrentTailnet.Name — which network we're signed into
    std::string selfHost;
    std::vector<std::string> selfIps;
    std::vector<TsPeer> peers;

    [[nodiscard]] bool loggedIn() const { return backend == TsBackend::Running; }
    // Find a peer by tailnet IP (exact) or by hostname / DNS leaf (case-insensitive).
    // nullptr if none matches. This is the robust "is my host on this network"
    // signal — it beats matching the tailnet name and doubles as IP auto-detection.
    [[nodiscard]] const TsPeer* findPeer(const std::string& hostOrIp) const;
};

// Pure: turn `tailscale status --json` text into a TsStatus. `installed` is left
// false here (the caller knows whether the binary was found); everything else is
// read from the document. Malformed/empty input yields a default (Unknown) status.
[[nodiscard]] TsStatus parseTailscaleStatus(const std::string& json);

// Locate the tailscale CLI; empty if not installed. Checks well-known per-OS paths
// then PATH.
[[nodiscard]] std::string findTailscaleBinary();

// Full probe: find the binary, run `status --json`, parse. `installed`/`daemonUp`
// reflect how far it got, so the flight check can pinpoint the first broken step.
[[nodiscard]] TsStatus probeTailscale();

// Can we open a TCP connection to host:port? (`host` must be a dotted IPv4 — pass a
// tailnet IP.) Blocking up to the OS connect timeout; run it off the UI thread.
[[nodiscard]] bool tcpReachable(const std::string& host, std::uint16_t port);

// The onboarding ladder: the FIRST unmet requirement between the user and a lobby.
// Each step has exactly one cause and one fix, so the UI shows only this one.
struct FlightCheck {
    enum class Step {
        Install,         // Tailscale isn't installed
        StartService,    // installed, but the daemon/service isn't up
        SignIn,          // up, but not signed in (or toggled off)
        JoinNetwork,     // signed in, but the host isn't on this tailnet
        HostOffline,     // host is on the tailnet but offline
        HostUnreachable, // host is online but the game port didn't answer
        Ready            // clear — good to connect
    };
    Step step = Step::Install;
    TsStatus status;
    std::string hostIp; // the target host's tailnet IPv4, once known (auto-detected)
};

// Pure decision given a probed status, the resolved target peer (or nullptr), and
// whether the TCP poke succeeded. Exposed for unit testing.
[[nodiscard]] FlightCheck::Step flightStepFor(const TsStatus& s, const TsPeer* target, bool tcpOk);

// End-to-end flight check for a target server: probe → resolve the host peer →
// (if reachable so far) TCP-poke the port → the first unmet step. `hostArg` is the
// host portion the user is aiming at (a tailnet IP or a device name).
[[nodiscard]] FlightCheck runFlightCheck(const std::string& hostArg, std::uint16_t port);

} // namespace tb::net
