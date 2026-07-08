#pragma once
//
// ReplayMatchSource.h — Play back a recorded match through the MatchSource seam
// (Phase 5.1).
//
// A GameRecord (net/Replay) is the whole match: content hashes + seed + both builds
// + the ordered human intents. Because the core is deterministic, re-simulating it
// reproduces the game exactly — so a replay is just a read-only MatchSource that
// steps those intents through a MatchRunner on a timer. No input, no network; the
// existing board renderer draws it unchanged. Raylib-free.
//
#include "MatchSource.h"
#include "core/Match.h"       // buildMatch
#include "net/MatchRunner.h"  // MatchRunner, Seat
#include "net/Replay.h"       // GameRecord

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace tb::render {

class ReplayMatchSource final : public MatchSource {
public:
    ReplayMatchSource(const replay::GameRecord& rec, const Ruleset& ruleset,
                      const SpellCatalog& catalog, const std::vector<Entity>& creatures)
        : runner_(buildMatch(ruleset, {rec.player}, {rec.enemy}, catalog, rec.seed, creatures),
                  net::Seat::Human, net::Seat::Human),
          intents_(rec.intents) {}

    [[nodiscard]] const Battle& battle() const override { return runner_.battle(); }
    [[nodiscard]] bool awaitingLocalInput() const override { return false; } // read-only
    std::optional<std::string> submit(const net::Intent&) override { return std::nullopt; }

    // Advance one recorded intent per `interval` seconds (unless paused / stepping).
    std::optional<std::string> update(float dt) override {
        if (matchOver() || paused_) return std::nullopt;
        timer_ += dt;
        if (timer_ < interval_) return std::nullopt;
        timer_ = 0.0f;
        stepOnce();
        return std::nullopt;
    }

    [[nodiscard]] bool matchOver() const override {
        return runner_.finished() || cursor_ >= intents_.size();
    }

    // --- playback controls (driven by the GUI) ------------------------------
    void togglePause() { paused_ = !paused_; }
    [[nodiscard]] bool paused() const { return paused_; }
    void step() { stepOnce(); } // manual single-step (also nudges past a pause)
    void slower() { interval_ = std::min(2.0f, interval_ * 1.5f); }
    void faster() { interval_ = std::max(0.05f, interval_ / 1.5f); }
    [[nodiscard]] std::size_t cursor() const { return cursor_; }
    [[nodiscard]] std::size_t total() const { return intents_.size(); }

private:
    void stepOnce() {
        if (matchOver()) return;
        // The runner supplies the seat + drives any AI/summon/inert turns between
        // human intents — exactly as the live match did (replay::verify's core).
        if (const std::optional<Faction> seat = runner_.awaitingSeat())
            runner_.submit(*seat, intents_[cursor_]);
        ++cursor_;
    }

    net::MatchRunner runner_;
    std::vector<net::Intent> intents_;
    std::size_t cursor_ = 0;
    float timer_ = 0.0f;
    float interval_ = 0.6f; // seconds between steps
    bool paused_ = false;
};

} // namespace tb::render
