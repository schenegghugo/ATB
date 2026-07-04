#pragma once
//
// MatchRunner.h — The authoritative in-process match runner (Phase 4.3).
//
// Owns one Battle and is the ONLY thing that mutates it — the server-side match
// resolver from ARCHITECTURE.md §7. Clients submit Intents for their seat; the
// runner enforces the trust checks (the sender owns the active unit; the intent
// is legal — the Battle verbs already refuse illegal calls), applies the intent,
// then drives every server-controlled turn (AI champions, summons, inert
// objects) up to the next human decision point, exposing the resulting Snapshot
// to "broadcast".
//
// No sockets: a transport (Phase 4.4) will wrap this. Because the core is
// deterministic, the whole intent → apply → snapshot loop is verifiable now,
// in-process (the loopback "server").
//
#include "core/Battle.h"
#include "data/Net.h" // Intent, Snapshot, applyIntent, snapshotOf

#include <cstdint>
#include <optional>

namespace tb::net {

// How a Faction's seat is filled: a connected Human client submits Intents, or an
// AI seat the server plays itself. (Summons and inert Objects are always
// server-driven, whatever their team's seat.)
enum class Seat : std::uint8_t { Human, AI };

class MatchRunner {
public:
    // Constructs at the first decision point: server-controlled turns are played
    // immediately, so awaitingSeat() is valid right away.
    MatchRunner(Battle battle, Seat playerSeat, Seat enemySeat);

    [[nodiscard]] bool finished() const { return battle_.phase() == Phase::Finished; }

    // The human seat that must act now, or std::nullopt when the match is finished
    // (there is never a server-controlled unit waiting — advance() drains those).
    [[nodiscard]] std::optional<Faction> awaitingSeat() const;

    // Submit `in` from `seat`. Returns false with NO mutation unless: the match is
    // live, `seat` is Human, the active unit is that seat's own Champion, and the
    // intent is legal (illegal casts/moves are refused by the engine). On success
    // it applies the intent and drives server-controlled turns to the next human
    // decision point. (endTurn always applies; a blocked move counts as rejected.)
    bool submit(Faction seat, const Intent& in);

    [[nodiscard]] const Battle& battle() const { return battle_; }
    [[nodiscard]] Snapshot snapshot() const { return snapshotOf(battle_); }

private:
    // Play AI champions / summons / inert passes until the match finishes or a
    // human seat must act.
    void advance();
    // The active unit is resolved by the server (not a waiting human): a summon,
    // an inert object, or a Champion whose seat is AI-filled.
    [[nodiscard]] bool serverControlsActive() const;
    [[nodiscard]] Seat seatOf(Faction f) const { return f == Faction::Player ? player_ : enemy_; }

    Battle battle_;
    Seat player_;
    Seat enemy_;
};

} // namespace tb::net
