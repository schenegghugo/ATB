#pragma once
//
// MoveChannel.h — the transport seam for correspondence moves (CR.6 / Phase 4.5).
//
// A CorrespondenceSession exchanges opaque move-strings through *some* append-only
// per-game log. This interface abstracts that log so the same session logic runs
// over either a direct mailbox relay (RelayChannel) or a lobby session connection
// (LobbyChannel, Phase 4.5) — post a move, poll the log from a cursor. Swapping the
// channel (rebind) lets a dropped transport reconnect without losing game state.
//
#include "MailboxRelay.h" // MailEntry, RelayClient

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace tb::net {

struct ChannelPoll {
    std::vector<MailEntry> entries; // new entries since the requested cursor
    std::size_t next = 0;           // cursor to poll from next time
};

// One correspondence game's append-only string log, addressed by a game id.
struct MoveChannel {
    virtual ~MoveChannel() = default;
    // Append `msg` from `sender`; returns the new log length (nullopt on error).
    virtual std::optional<std::size_t> post(const std::string& game, const std::string& sender,
                                            const std::string& msg) = 0;
    // Entries at [from, end) + the next cursor (nullopt on error).
    virtual std::optional<ChannelPoll> poll(const std::string& game, std::size_t from) = 0;
};

// Adapter over a direct relay connection (CR.3).
class RelayChannel : public MoveChannel {
public:
    explicit RelayChannel(RelayClient relay) : relay_(std::move(relay)) {}
    std::optional<std::size_t> post(const std::string& game, const std::string& sender,
                                    const std::string& msg) override {
        return relay_.post(game, sender, msg);
    }
    std::optional<ChannelPoll> poll(const std::string& game, std::size_t from) override {
        std::optional<RelayClient::PollResult> r = relay_.poll(game, from);
        if (!r) return std::nullopt;
        return ChannelPoll{std::move(r->entries), r->next};
    }

private:
    RelayClient relay_;
};

} // namespace tb::net
