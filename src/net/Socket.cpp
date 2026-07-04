//
// Socket.cpp — see Socket.h. Blocking Berkeley-sockets TCP, POSIX only.
//
#include "Socket.h"

#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace tb::net {
namespace {

constexpr std::uint32_t kMaxMessage = 16u * 1024u * 1024u; // sanity cap on a frame

// Read exactly n bytes into buf; false on EOF/timeout/error.
bool readAll(int fd, char* buf, std::size_t n) {
    std::size_t got = 0;
    while (got < n) {
        const ssize_t r = ::read(fd, buf + got, n - got);
        if (r <= 0) return false; // 0 = peer closed, <0 = error/timeout
        got += static_cast<std::size_t>(r);
    }
    return true;
}

bool writeAll(int fd, const char* buf, std::size_t n) {
    std::size_t put = 0;
    while (put < n) {
        // MSG_NOSIGNAL: writing to a peer that has closed returns EPIPE instead of
        // raising SIGPIPE (which would silently kill the process).
        const ssize_t w = ::send(fd, buf + put, n - put, MSG_NOSIGNAL);
        if (w <= 0) return false;
        put += static_cast<std::size_t>(w);
    }
    return true;
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
        ::close(fd_);
        fd_ = -1;
    }
}

std::optional<Connection> Connection::connect(const std::string& host, uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return std::nullopt;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        return std::nullopt;
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return std::nullopt;
    }
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)); // small turn-based msgs
    return Connection(fd);
}

void Connection::setReadTimeout(int seconds) {
    if (fd_ < 0) return;
    timeval tv{};
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
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
    pollfd pfd{};
    pfd.fd = fd_;
    pfd.events = POLLIN;
    const int r = ::poll(&pfd, 1, timeoutMs);
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
        ::close(fd_);
        fd_ = -1;
    }
}

std::optional<Listener> Listener::bind(uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return std::nullopt;
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // localhost-only for now
    addr.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return std::nullopt;
    }
    if (::listen(fd, 8) != 0) {
        ::close(fd);
        return std::nullopt;
    }
    // Read back the actual port (relevant when the caller passed 0).
    sockaddr_in bound{};
    socklen_t blen = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &blen) != 0) {
        ::close(fd);
        return std::nullopt;
    }
    Listener l;
    l.fd_ = fd;
    l.port_ = ntohs(bound.sin_port);
    return l;
}

std::optional<Connection> Listener::accept() {
    if (fd_ < 0) return std::nullopt;
    const int cfd = ::accept(fd_, nullptr, nullptr);
    if (cfd < 0) return std::nullopt;
    int one = 1;
    ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return Connection(cfd);
}

} // namespace tb::net
