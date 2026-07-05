#pragma once
//
// MailboxRelay.h — Store-and-forward relay for correspondence play (CR.3).
//
// A dumb, per-game, append-only log of opaque strings — NO game logic. Two players
// post their move-strings and poll for the other's, so they never open a direct
// P2P socket: both connect OUT to the relay, which sidesteps NAT entirely. Trivial
// to self-host (the EliteDesk); the trust lives in the arbiter (CR.4), not here.
//
#include "Socket.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace tb::net {

struct MailEntry {
    std::string sender;
    std::string msg;
};

// The pure store: thread-safe per-game append-only logs.
class Mailbox {
public:
    // Append `msg` from `sender` to `game`; returns the new log length.
    std::size_t post(const std::string& game, const std::string& sender, const std::string& msg);
    // Entries at indices [from, end).
    [[nodiscard]] std::vector<MailEntry> poll(const std::string& game, std::size_t from) const;
    [[nodiscard]] std::size_t size(const std::string& game) const;

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::vector<MailEntry>> logs_;
};

// Serve `box` over TCP: accept connections (one thread each) and answer post/poll
// requests. Accepts until `maxConns` have connected (< 0 = forever, the daemon
// mode), then joins every connection thread (each exits when its client
// disconnects) and returns. Blocks.
void serveRelay(Listener& listener, Mailbox& box, int maxConns = -1, int readTimeoutSec = 300);

// Client side: connect, then post / poll.
class RelayClient {
public:
    [[nodiscard]] static std::optional<RelayClient> connect(const std::string& host, uint16_t port,
                                                            int readTimeoutSec = 15);
    RelayClient(RelayClient&&) = default;
    RelayClient& operator=(RelayClient&&) = default;

    // Post `msg`; returns the new log length (nullopt on error).
    [[nodiscard]] std::optional<std::size_t> post(const std::string& game, const std::string& sender,
                                                  const std::string& msg);
    struct PollResult {
        std::vector<MailEntry> entries; // new entries since `from`
        std::size_t next = 0;           // cursor to poll from next time
    };
    [[nodiscard]] std::optional<PollResult> poll(const std::string& game, std::size_t from);

private:
    explicit RelayClient(Connection c) : conn_(std::move(c)) {}
    Connection conn_;
};

} // namespace tb::net
