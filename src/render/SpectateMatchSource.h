#pragma once
//
// SpectateMatchSource.h — Watch a live lobby match through the MatchSource seam
// (Phase 5.2).
//
// Read-only: a SpectatorMirror replays the match's logged broadcast stream, and
// update() polls the lobby session (throttled) for new entries. The mirror must
// already be ready() — main.cpp primes it from log cursor 0 before constructing
// this (the welcome is logged the moment the match starts, so it's always there
// for a listed game). Raylib-free, like the other sources.
//
#include "MatchSource.h"
#include "net/Lobby.h"    // LobbySession, ChannelPoll
#include "net/Spectate.h" // SpectatorMirror

#include <memory>
#include <string>
#include <utility>

namespace tb::render {

class SpectateMatchSource final : public MatchSource {
public:
    // `session` is borrowed and must outlive this source (it does — the lobby
    // connection owns the app's whole online lifetime, as CorrespondenceMatchSource
    // relies on too). `cursor` is the next log index after the priming drain.
    SpectateMatchSource(std::unique_ptr<net::SpectatorMirror> mirror, net::LobbySession* session,
                        std::string game, std::size_t cursor)
        : mirror_(std::move(mirror)), session_(session), game_(std::move(game)), cursor_(cursor) {}

    [[nodiscard]] const Battle& battle() const override { return mirror_->battle(); }
    [[nodiscard]] bool awaitingLocalInput() const override { return false; } // read-only
    std::optional<std::string> submit(const net::Intent&) override { return std::nullopt; }

    std::optional<std::string> update(float dt) override {
        if (mirror_->finished()) return std::nullopt;
        timer_ += dt;
        if (timer_ < kPollSec) return std::nullopt;
        timer_ = 0.0f;
        if (const std::optional<net::ChannelPoll> cp = session_->watchPoll(game_, cursor_)) {
            for (const net::MailEntry& e : cp->entries) mirror_->feed(e.msg);
            cursor_ = cp->next;
        }
        return std::nullopt;
    }

    [[nodiscard]] bool matchOver() const override { return mirror_->finished(); }
    [[nodiscard]] std::optional<Faction> winner() const override {
        if (const std::optional<Faction> f = mirror_->forfeitWinner()) return f;
        return battle().winner();
    }

private:
    static constexpr float kPollSec = 0.5f; // watch-log poll throttle (a blocking RPC)
    std::unique_ptr<net::SpectatorMirror> mirror_;
    net::LobbySession* session_;
    std::string game_;
    std::size_t cursor_ = 0;
    float timer_ = 0.0f;
};

} // namespace tb::render
