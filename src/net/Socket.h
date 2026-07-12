#pragma once
//
// Socket.h — Minimal blocking TCP with length-prefixed message framing (Phase
// 4.4). POSIX + winsock, and deliberately kept OUT of tb_core so the portable
// engine never depends on sockets — only the server/client transport links this.
//
// A message is [uint32 big-endian length][that many bytes]; send()/recv() move
// whole messages. Dependency-free (Berkeley sockets), matching the project's
// hand-rolled ethos (JSON, SHA-256). WebSocket framing can layer on later for the
// browser build; the message contract above is transport-agnostic.
//
#include <cstdint>
#include <optional>
#include <string>

namespace tb::net {

// A framed message channel over one connected TCP socket. Move-only; closes its
// fd on destruction. A read timeout (optional) turns a stuck peer into a clean
// disconnect instead of an indefinite hang.
class Connection {
public:
    Connection() = default;
    explicit Connection(int fd) : fd_(fd) {}
    ~Connection();
    Connection(Connection&& o) noexcept;
    Connection& operator=(Connection&& o) noexcept;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    [[nodiscard]] bool valid() const { return fd_ >= 0; }

    // Connect to host:port (host is a dotted IPv4, e.g. "127.0.0.1"). nullopt on
    // failure.
    [[nodiscard]] static std::optional<Connection> connect(const std::string& host, uint16_t port);

    // Apply a per-operation read timeout (seconds; 0 = block forever). Guards
    // tests/servers against a wedged peer.
    void setReadTimeout(int seconds);

    // Send one framed message; false on error/disconnect.
    bool send(const std::string& msg);
    // Receive one framed message; nullopt on clean EOF, timeout, or error.
    [[nodiscard]] std::optional<std::string> recv();
    // True if recv() would have data to read within timeoutMs (0 = poll now). Lets
    // the GUI drain server messages each frame without blocking the render loop.
    [[nodiscard]] bool waitReadable(int timeoutMs) const;

    void close();

private:
    int fd_ = -1;
};

// A listening TCP socket. bind(0) asks the OS for an ephemeral port (readable via
// port()), which keeps tests free of hard-coded ports.
class Listener {
public:
    ~Listener();
    Listener(Listener&& o) noexcept;
    Listener& operator=(Listener&& o) noexcept;
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    // Bind + listen. `host` is the interface to bind (dotted IPv4): "127.0.0.1"
    // (default) accepts only local connections — safe for tests; "0.0.0.0" accepts
    // from any interface (LAN/internet — only do this behind a firewall/VPN); or a
    // specific address (e.g. a Tailscale IP) to expose it to just that network.
    [[nodiscard]] static std::optional<Listener> bind(uint16_t port,
                                                      const std::string& host = "127.0.0.1");
    [[nodiscard]] uint16_t port() const { return port_; } // actual bound port
    [[nodiscard]] std::optional<Connection> accept();
    void close();

private:
    Listener() = default;
    int fd_ = -1;
    uint16_t port_ = 0;
};

} // namespace tb::net
