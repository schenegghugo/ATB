#pragma once
//
// GameServer.h — Serve one authoritative 1v1 custom match over TCP (Phase 4.4).
//
// Wraps the MatchRunner (§4.3) with the transport + the trust checkpoints from
// ARCHITECTURE.md §7: the handshake (content hash) and build admission
// (validateBuild vs the ruleset). Per-intent ownership + legality are already
// enforced by the runner. Blocking, single match, then returns — a lobby /
// multi-match server (Phase 4.5) will drive many of these.
//
#include "Socket.h"
#include "core/Build.h" // CharacterBuild
#include "core/Entity.h"
#include "core/Ruleset.h"
#include "core/Spells.h" // SpellCatalog

#include <optional>
#include <string>
#include <vector>

namespace tb::net {

class AccountStore; // forward decl — ranked mode only

struct MatchConfig {
    Ruleset ruleset;
    SpellCatalog catalog;
    std::vector<Entity> creatures;
    std::string contentHash;         // the pinned catalog hash clients must match (see contentHashOf)
    AccountStore* accounts = nullptr; // non-null ⇒ RANKED: require login + record Elo. null ⇒ custom.
};

// The handshake anchor: sha256 of the catalog's canonical serialization, computed
// the SAME way on client and server so identical content hashes identically
// (independent of the file's author version label). Creatures/ruleset would
// extend this in the same manner.
[[nodiscard]] std::string contentHashOf(const SpellCatalog& catalog);

struct ServeResult {
    bool ok = false; // a full match was played to a finish
    std::optional<Faction> winner;
    std::string finalSnapshot; // serialized (empty if aborted)
    std::string error;         // set when ok == false
};

// Accept two clients on `listener`, handshake + admit each (content hash, then
// validateBuild), seat them (first = Player, second = Enemy), build the Battle,
// and run the authoritative loop over the socket until the match finishes.
// `readTimeoutSec` turns a wedged client into a clean abort instead of a hang.
ServeResult serveOneMatch(Listener& listener, const MatchConfig& cfg, int readTimeoutSec = 15);

// Run one already-admitted match to completion over two connections (seats by
// argument order: c0 = Player, c1 = Enemy). Sends each side the `welcome` setup,
// then the authoritative intent → apply → broadcast loop until the match finishes.
// The connections' read timeouts act as the per-move clock (a wedged client aborts
// cleanly). Exposed so the lobby (Phase 4.5) can drive matches it has already paired
// + admitted, reusing the exact same authoritative path. Does NOT record Elo — the
// caller does that on the result (see serveMatches).
ServeResult runAdmittedMatch(Connection c0, Connection c1, const CharacterBuild& b0,
                             const CharacterBuild& b1, const MatchConfig& cfg, int clockSec = 0);

// Persistent matchmaking server (Phase 4.5): accept players, admit each, and pair
// them FIFO — every two admitted players start a match that runs in its own
// thread, so many matches play concurrently. Runs until `maxMatches` have started
// (< 0 = forever, the daemon mode; joins nothing — match threads are detached);
// with a finite cap it joins every match thread before returning (for tests).
// Returns the number of matches started. Independent Battles share no mutable
// state, so concurrent matches are safe. (v1 admits sequentially — a slow client
// briefly stalls the queue; a real server would handshake off-thread.)
int serveMatches(Listener& listener, const MatchConfig& cfg, int maxMatches = -1,
                 int readTimeoutSec = 15);

} // namespace tb::net
