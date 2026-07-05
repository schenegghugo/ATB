#pragma once
//
// GameClient.h — Headless auto-client for a networked match (Phase 4.4).
//
// Connects, builds a deterministic mirror (MirrorSession), and plays it out with
// a Policy that decides each turn purely from the Snapshot. Used to test the
// transport end to end without a human or the GUI. (The GUI uses the same mirror
// via render/RemoteMatchSource.)
//
#include "core/Build.h"
#include "core/Entity.h"
#include "core/Ruleset.h"
#include "core/Spells.h" // SpellCatalog
#include "data/Net.h"    // Intent, Snapshot

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace tb::net {

// Decide a whole turn from the current Snapshot; must end with endTurn.
using Policy = std::function<std::vector<Intent>(Faction seat, const Snapshot&)>;

struct ClientResult {
    bool ok = false; // handshake accepted and the match played to a finish
    std::optional<Faction> seat;
    std::string finalSnapshot; // the mirror's final state (should match the server's)
    std::string error;
};

// Connect + play a full match, submitting policy()'s Intents on our turns. The
// pinned content (ruleset/catalog/creatures) must match the server's. `user`/
// `pass` are the ranked login (empty for custom/unranked servers).
ClientResult playClient(const std::string& host, uint16_t port, const std::string& contentHash,
                        const CharacterBuild& build, const Ruleset& ruleset,
                        const SpellCatalog& catalog, const std::vector<Entity>& creatures,
                        const Policy& policy, int readTimeoutSec = 15, const std::string& user = "",
                        const std::string& pass = "", const std::string& lobby = "");

// A deterministic default policy computable from a Snapshot alone: attack spell
// slot 0 at the nearest living foe, step toward it, then end the turn.
[[nodiscard]] std::vector<Intent> chasePolicy(Faction seat, const Snapshot& s);

} // namespace tb::net
