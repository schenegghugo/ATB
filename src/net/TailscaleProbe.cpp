//
// TailscaleProbe.cpp — see TailscaleProbe.h.
//
#include "TailscaleProbe.h"

#include "Socket.h"
#include "Subprocess.h"
#include "data/Json.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>

namespace tb::net {
namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// "homeboi.tail21ee73.ts.net." → "homeboi" (lowercased).
std::string dnsLeaf(const std::string& dns) {
    const std::size_t dot = dns.find('.');
    return lower(dot == std::string::npos ? dns : dns.substr(0, dot));
}

TsBackend backendFromString(const std::string& s) {
    if (s == "Running") return TsBackend::Running;
    if (s == "NeedsLogin") return TsBackend::NeedsLogin;
    if (s == "NeedsMachineAuth") return TsBackend::NeedsMachineAuth;
    if (s == "Stopped") return TsBackend::Stopped;
    if (s == "Starting") return TsBackend::Starting;
    if (s == "NoState") return TsBackend::NoState;
    return TsBackend::Unknown;
}

// Read a node's "TailscaleIPs" array of strings, keeping Tailscale's order (v4 first).
std::vector<std::string> readIps(const json::Value& node) {
    std::vector<std::string> ips;
    if (const json::Value* a = node.find("TailscaleIPs"); a && a->isArray())
        for (const json::Value& v : a->asArray())
            if (v.isString()) ips.push_back(v.asString());
    return ips;
}

// A dotted IPv4 like "100.81.97.122" (used to decide IP-match vs name-match).
bool looksLikeIpv4(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (c != '.' && !std::isdigit(static_cast<unsigned char>(c))) return false;
    return s.find('.') != std::string::npos;
}

const char* kExeName =
#ifdef _WIN32
    "tailscale.exe";
#else
    "tailscale";
#endif

// Scan $PATH for `name`.
std::string searchPath(const std::string& name) {
    const char* path = std::getenv("PATH");
    if (!path) return {};
#ifdef _WIN32
    const char sep = ';';
#else
    const char sep = ':';
#endif
    const std::string p(path);
    std::size_t start = 0;
    while (start <= p.size()) {
        const std::size_t end = p.find(sep, start);
        const std::string dir = p.substr(start, end == std::string::npos ? end : end - start);
        if (!dir.empty()) {
            std::error_code ec;
            const std::filesystem::path cand = std::filesystem::path(dir) / name;
            if (std::filesystem::exists(cand, ec)) return cand.string();
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return {};
}

} // namespace

const TsPeer* TsStatus::findPeer(const std::string& hostOrIp) const {
    if (hostOrIp.empty()) return nullptr;
    const bool byIp = looksLikeIpv4(hostOrIp);
    const std::string q = lower(hostOrIp);
    for (const TsPeer& p : peers) {
        if (byIp) {
            for (const std::string& ip : p.ips)
                if (ip == hostOrIp) return &p;
        } else if (lower(p.host) == q || p.dns == q) {
            return &p;
        }
    }
    return nullptr;
}

TsStatus parseTailscaleStatus(const std::string& text) {
    TsStatus s;
    const json::ParseResult pr = json::parse(text);
    if (!pr.ok || !pr.value.isObject()) return s;
    const json::Value& root = pr.value;

    if (const json::Value* b = root.find("BackendState"); b && b->isString() && !b->asString().empty()) {
        s.daemonUp = true;
        s.backend = backendFromString(b->asString());
    }
    if (const json::Value* ct = root.find("CurrentTailnet"); ct && ct->isObject())
        if (const json::Value* n = ct->find("Name"); n && n->isString()) s.tailnet = n->asString();

    if (const json::Value* self = root.find("Self"); self && self->isObject()) {
        if (const json::Value* h = self->find("HostName"); h && h->isString()) s.selfHost = h->asString();
        s.selfIps = readIps(*self);
    }

    if (const json::Value* peer = root.find("Peer"); peer && peer->isObject()) {
        for (const json::Value::Member& m : peer->asObject()) {
            const json::Value& v = m.second;
            if (!v.isObject()) continue;
            TsPeer p;
            if (const json::Value* h = v.find("HostName"); h && h->isString()) p.host = h->asString();
            if (const json::Value* d = v.find("DNSName"); d && d->isString()) p.dns = dnsLeaf(d->asString());
            if (const json::Value* o = v.find("Online"); o && o->isBool()) p.online = o->asBool();
            p.ips = readIps(v);
            s.peers.push_back(std::move(p));
        }
    }
    return s;
}

std::string findTailscaleBinary() {
    static const char* const candidates[] = {
#ifdef _WIN32
        "C:\\Program Files\\Tailscale\\tailscale.exe",
        "C:\\Program Files (x86)\\Tailscale\\tailscale.exe",
#elif defined(__APPLE__)
        "/Applications/Tailscale.app/Contents/MacOS/Tailscale",
        "/usr/local/bin/tailscale",
        "/opt/homebrew/bin/tailscale",
#else
        "/usr/bin/tailscale",
        "/usr/local/bin/tailscale",
        "/run/current-system/sw/bin/tailscale", // NixOS
#endif
    };
    for (const char* c : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(c, ec)) return c;
    }
    return searchPath(kExeName);
}

TsStatus probeTailscale() {
    const std::string bin = findTailscaleBinary();
    if (bin.empty()) return {}; // installed=false, everything else default
    std::string out;
    if (!runCapture(bin, {"status", "--json"}, out)) {
        TsStatus s;
        s.installed = true; // the binary is there; the daemon just isn't answering
        return s;
    }
    TsStatus s = parseTailscaleStatus(out);
    s.installed = true;
    return s;
}

bool tcpReachable(const std::string& host, std::uint16_t port) {
    return Connection::connect(host, port).has_value();
}

FlightCheck::Step flightStepFor(const TsStatus& s, const TsPeer* target, bool tcpOk) {
    using Step = FlightCheck::Step;
    if (!s.installed) return Step::Install;
    if (!s.daemonUp || s.backend == TsBackend::NoState || s.backend == TsBackend::Starting)
        return Step::StartService;
    if (s.backend != TsBackend::Running) return Step::SignIn; // NeedsLogin/MachineAuth/Stopped/Unknown
    if (!target) return Step::JoinNetwork;
    if (!target->online) return Step::HostOffline;
    if (!tcpOk) return Step::HostUnreachable;
    return Step::Ready;
}

FlightCheck runFlightCheck(const std::string& hostArg, std::uint16_t port) {
    FlightCheck fc;
    fc.status = probeTailscale();

    const TsPeer* target =
        fc.status.backend == TsBackend::Running ? fc.status.findPeer(hostArg) : nullptr;

    // The IP to poke / hand back: the peer's tailnet IPv4 if we resolved one, else
    // whatever the user typed (already an IP in the common case).
    std::string ip = hostArg;
    if (target && !target->ips.empty()) ip = target->ips.front();
    fc.hostIp = target ? ip : std::string{};

    // Only bother poking the port once everything up to "host online" is satisfied
    // — a TCP connect to a dead host otherwise stalls the probe for no reason.
    bool tcpOk = false;
    if (target && target->online && !ip.empty()) tcpOk = tcpReachable(ip, port);

    fc.step = flightStepFor(fc.status, target, tcpOk);
    return fc;
}

} // namespace tb::net
