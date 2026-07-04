#pragma once
//
// MatchSource.h — The frontend's source of match truth (Phase 4.2).
//
// The seam between the UI (input + rendering, in main.cpp) and *who drives the
// Battle*. Today there is one implementation, LocalMatchSource, which owns and
// advances an in-process Battle — exactly the behaviour main.cpp had inline. A
// future RemoteMatchSource (Phase 4.6) will send Intents to the authoritative
// server and mirror its snapshots, reusing the very same render code: only the
// source of truth swaps.
//
// The vocabulary is the PvP Intent (data/Net.h): the UI turns clicks/keys into
// move/cast/endTurn Intents and submits them; it never mutates the Battle
// directly. Rendering reads battle() (the real Battle locally; a mirror
// remotely). This class is raylib-free — pure core + net — so it is unit-tested
// headless (tb_matchsource_demo).
//
#include "core/Battle.h"
#include "data/Net.h"

#include <optional>
#include <string>

namespace tb::render {

class MatchSource {
public:
    virtual ~MatchSource() = default;

    // The Battle to render (source of truth locally; a mirror remotely).
    [[nodiscard]] virtual const Battle& battle() const = 0;

    // True when the local player controls the active unit right now — i.e. the
    // UI should accept their input and submit() is valid.
    [[nodiscard]] virtual bool awaitingLocalInput() const = 0;

    // Apply the local player's action for the active unit. Returns a HUD status
    // describing the outcome (std::nullopt = no message). Only call when
    // awaitingLocalInput() is true.
    virtual std::optional<std::string> submit(const net::Intent& in) = 0;

    // Advance non-local turns (AI champions/summons, inert objects) on the
    // driver's own pacing. dt = seconds since the last frame. Returns a HUD
    // status when something notable happened this tick (else std::nullopt).
    virtual std::optional<std::string> update(float dt) = 0;
};

// Drives the Battle directly, in-process — the single-player / hotseat path and
// the behaviour the GUI has always had. AI/inert turns are paced by a small
// timer so the player can watch them resolve.
class LocalMatchSource final : public MatchSource {
public:
    explicit LocalMatchSource(Battle battle);

    [[nodiscard]] const Battle& battle() const override { return battle_; }
    [[nodiscard]] bool awaitingLocalInput() const override;
    std::optional<std::string> submit(const net::Intent& in) override;
    std::optional<std::string> update(float dt) override;

private:
    Battle battle_;
    float aiTimer_ = 0.0f;
    static constexpr float kAiTick = 0.35f; // seconds between watched AI actions
};

} // namespace tb::render
