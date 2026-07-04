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
#include "core/Entity.h"
#include "core/Ruleset.h"
#include "core/Spells.h" // SpellCatalog

#include <optional>
#include <string>
#include <vector>

namespace tb::net {

struct MatchConfig {
    Ruleset ruleset;
    SpellCatalog catalog;
    std::vector<Entity> creatures;
    std::string contentHash; // the pinned catalog hash clients must match (see contentHashOf)
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

} // namespace tb::net
