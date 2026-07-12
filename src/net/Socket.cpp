//
// Socket.cpp — see Socket.h. Blocking Berkeley-sockets TCP; POSIX + winsock.
//
#include "Socket.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <cstring>

namespace tb::net {
namespace {

constexpr std::uint32_t kMaxMessage = 16u * 1024u * 1024u; // sanity cap on a frame

#ifdef _WIN32
// Winsock needs process-wide init before any socket call. Runs once, stays up
// for the process lifetime (WSACleanup at exit buys nothing for a game/client).
bool ensureWsa() {
    static const bool ok = [] {
        WSADATA wsa;
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }();
    return ok;
}

void closefd(int fd) { ::closesocket(static_cast<SOCKET>(fd)); }

// Windows socket handles fit in 32 bits (kernel handle guarantee), so the
// header's `int fd_` stays portable.
int toFd(SOCKET s) { return static_cast<int>(s); }
#else
bool ensureWsa() { return true; }
void closefd(int fd) { ::close(fd); }
#endif

// Read exactly n bytes into buf; false on EOF/timeout/error.
bool readAll(int fd, char* buf, std::size_t n) {
    std::size_t got = 0;
    while (got < n) {
#ifdef _WIN32
        const int r = ::recv(static_cast<SOCKET>(fd), buf + got, static_cast<int>(n - got), 0);
#else
        const ssize_t r = ::recv(fd, buf + got, n - got, 0);
#endif
        if (r <= 0) return false; // 0 = peer closed, <0 = error/timeout
        got += static_cast<std::size_t>(r);
    }
    return true;
}

bool writeAll(int fd, const char* buf, std::size_t n) {
    std::size_t put = 0;
    while (put < n) {
#ifdef _WIN32
        // Windows raises no SIGPIPE; send() just fails on a closed peer.
        const int w = ::send(static_cast<SOCKET>(fd), buf + put, static_cast<int>(n - put), 0);
#else
        // MSG_NOSIGNAL: writing to a peer that has closed returns EPIPE instead of
        // raising SIGPIPE (which would silently kill the process).
        const ssize_t w = ::send(fd, buf + put, n - put, MSG_NOSIGNAL);
#endif
        if (w <= 0) return false;
        put += static_cast<std::size_t>(w);
    }
    return true;
}

void setNoDelay(int fd) {
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
}

} // namespace

// --- Connection -------------------------------------------------------------
Connection::~Connection() { close(); }

Connection::Connection(Connection&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }

Connection& Connection::operator=(Connection&& o) noexcept {
    if (this != &o) {
        close();
        fd_ = o.fd_;
        o.fd_ = -1;
    }
    return *this;
}

void Connection::close() {
    if (fd_ >= 0) {
        closefd(fd_);
        fd_ = -1;
    }
}

std::optional<Connection> Connection::connect(const std::string& host, uint16_t port) {
    if (!ensureWsa()) return std::nullopt;
#ifdef _WIN32
    const SOCKET s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return std::nullopt;
    const int fd = toFd(s);
#else
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return std::nullopt;
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        closefd(fd);
        return std::nullopt;
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closefd(fd);
        return std::nullopt;
    }
    setNoDelay(fd); // small turn-based msgs
    return Connection(fd);
}

void Connection::setReadTimeout(int seconds) {
    if (fd_ < 0) return;
#ifdef _WIN32
    const DWORD ms = static_cast<DWORD>(seconds) * 1000u;
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
#else
    timeval tv{};
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

bool Connection::send(const std::string& msg) {
    if (fd_ < 0 || msg.size() > kMaxMessage) return false;
    std::uint32_t len = htonl(static_cast<std::uint32_t>(msg.size()));
    char header[4];
    std::memcpy(header, &len, 4);
    if (!writeAll(fd_, header, 4)) return false;
    return writeAll(fd_, msg.data(), msg.size());
}

bool Connection::waitReadable(int timeoutMs) const {
    if (fd_ < 0) return false;
#ifdef _WIN32
    WSAPOLLFD pfd{};
    pfd.fd = static_cast<SOCKET>(fd_);
    pfd.events = POLLIN;
    const int r = ::WSAPoll(&pfd, 1, timeoutMs);
#else
    pollfd pfd{};
    pfd.fd = fd_;
    pfd.events = POLLIN;
    const int r = ::poll(&pfd, 1, timeoutMs);
#endif
    return r > 0 && (pfd.revents & POLLIN) != 0;
}

std::optional<std::string> Connection::recv() {
    if (fd_ < 0) return std::nullopt;
    char header[4];
    if (!readAll(fd_, header, 4)) return std::nullopt;
    std::uint32_t netlen = 0;
    std::memcpy(&netlen, header, 4);
    const std::uint32_t len = ntohl(netlen);
    if (len > kMaxMessage) return std::nullopt; // refuse absurd frames
    std::string body(len, '\0');
    if (len > 0 && !readAll(fd_, body.data(), len)) return std::nullopt;
    return body;
}

// --- Listener ---------------------------------------------------------------
Listener::~Listener() { close(); }

Listener::Listener(Listener&& o) noexcept : fd_(o.fd_), port_(o.port_) {
    o.fd_ = -1;
    o.port_ = 0;
}

Listener& Listener::operator=(Listener&& o) noexcept {
    if (this != &o) {
        close();
        fd_ = o.fd_;
        port_ = o.port_;
        o.fd_ = -1;
        o.port_ = 0;
    }
    return *this;
}

void Listener::close() {
    if (fd_ >= 0) {
        closefd(fd_);
        fd_ = -1;
    }
}

std::optional<Listener> Listener::bind(uint16_t port, const std::string& host) {
    if (!ensureWsa()) return std::nullopt;
#ifdef _WIN32
    const SOCKET s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return std::nullopt;
    const int fd = toFd(s);
#else
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return std::nullopt;
#endif
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) { // "0.0.0.0" => any
        closefd(fd);
        return std::nullopt;
    }
    addr.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closefd(fd);
        return std::nullopt;
    }
    if (::listen(fd, 8) != 0) {
        closefd(fd);
        return std::nullopt;
    }
    // Read back the actual port (relevant when the caller passed 0).
    sockaddr_in bound{};
    socklen_t blen = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &blen) != 0) {
        closefd(fd);
        return std::nullopt;
    }
    Listener l;
    l.fd_ = fd;
    l.port_ = ntohs(bound.sin_port);
    return l;
}

std::optional<Connection> Listener::accept() {
    if (fd_ < 0) return std::nullopt;
#ifdef _WIN32
    const SOCKET c = ::accept(static_cast<SOCKET>(fd_), nullptr, nullptr);
    if (c == INVALID_SOCKET) return std::nullopt;
    const int cfd = toFd(c);
#else
    const int cfd = ::accept(fd_, nullptr, nullptr);
    if (cfd < 0) return std::nullopt;
#endif
    setNoDelay(cfd);
    return Connection(cfd);
}

} // namespace tb::net
