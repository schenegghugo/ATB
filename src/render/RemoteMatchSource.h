#pragma once
//
// RemoteMatchSource.h — Play a networked match through the MatchSource seam
// (Phase 4.4 / §7 component #6).
//
// The remote counterpart to LocalMatchSource: instead of driving an in-process
// Battle, it renders a deterministic *mirror* (net::MirrorSession) and forwards
// the local player's Intents to the authoritative server. The render code in
// main.cpp is identical — only the source of truth swaps. Raylib-free.
//
#include "MatchSource.h"
#include "net/MirrorSession.h"

#include <memory>
#include <optional>
#include <string>

namespace tb::render {

class RemoteMatchSource final : public MatchSource {
public:
    explicit RemoteMatchSource(std::unique_ptr<net::MirrorSession> session)
        : session_(std::move(session)) {}

    [[nodiscard]] const Battle& battle() const override { return session_->battle(); }
    [[nodiscard]] bool awaitingLocalInput() const override { return session_->awaitingMe(); }

    // Forward the action to the server; the mirror advances when the server echoes
    // it back (drained in update()). No optimistic prediction yet — on localhost
    // the round-trip is a frame or two; add prediction later for real latency.
    std::optional<std::string> submit(const net::Intent& in) override {
        session_->send(in);
        return std::nullopt;
    }

    // Drain any authoritative messages ready this frame (non-blocking) into the
    // mirror — the opponent's moves, our confirmed moves, and match end.
    std::optional<std::string> update(float) override {
        session_->pump(0);
        return std::nullopt;
    }

    [[nodiscard]] bool finished() const { return session_->finished(); }

private:
    std::unique_ptr<net::MirrorSession> session_;
};

} // namespace tb::render
