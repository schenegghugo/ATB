#pragma once
//
// Spectate.h — Watch a live match as a read-only mirror (Phase 5.2).
//
// A spectator is just another deterministic mirror: the lobby logs each live
// match's broadcast stream (the Player-seat `welcome`, every `applied` intent, and
// the final `end`), and a watcher polls that log (LobbySession::watchPoll) and
// feeds it here in order. The welcome builds the same initial Battle both players
// built; each applied intent keeps it in lockstep — exactly MirrorSession's replay
// loop, minus the socket. In-match chat stays private to the players (it is never
// logged). Raylib-free.
//
#include "MatchRunner.h"
#include "core/Ruleset.h"
#include "core/Spells.h" // SpellCatalog

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tb::net {

class SpectatorMirror {
public:
    // The pinned content the mirror rebuilds the match from — it must match the
    // server's (the lobby's content-hash login guards this, as for MirrorSession).
    SpectatorMirror(Ruleset ruleset, SpellCatalog catalog, std::vector<Entity> creatures);

    // Feed one logged broadcast message, in log order. Returns false if the message
    // was unusable (unparseable, or match traffic before the welcome) — skip it.
    bool feed(const std::string& msg);

    // False until the welcome has been fed — there is no battle to read yet.
    [[nodiscard]] bool ready() const { return runner_ != nullptr; }
    [[nodiscard]] const Battle& battle() const { return runner_->battle(); } // requires ready()
    [[nodiscard]] bool finished() const { return ended_ || (runner_ && runner_->finished()); }
    // Set when the server ended the match by forfeit (idle clock / disconnect) —
    // there is no death in the mirror to infer the winner from.
    [[nodiscard]] std::optional<Faction> forfeitWinner() const { return forfeitWinner_; }

private:
    Ruleset ruleset_;
    SpellCatalog catalog_;
    std::vector<Entity> creatures_;
    std::unique_ptr<MatchRunner> runner_;
    bool ended_ = false;
    std::optional<Faction> forfeitWinner_;
};

} // namespace tb::net
