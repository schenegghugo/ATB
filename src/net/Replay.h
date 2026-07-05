#pragma once
//
// Replay.h — The game notation: replay = scoresheet = shareable game (CR.2 / §5.1).
//
// A complete match is fully described by pinned content + the setup that
// regenerates the board + the ordered human intents. Because the core is
// deterministic (CR.1 locks it cross-platform), re-simulating that record
// reproduces the identical outcome. One artifact, three uses:
//   - a **replay** you can rewatch,
//   - a **scoresheet** a friend/arbiter re-simulates to verify the winner,
//   - a **shareable game** (one copy-pasteable string).
//
// The seat of each intent is implicit — the deterministic MatchRunner knows whose
// turn it is — so the notation is just the ordered intent list (like chess PGN
// doesn't label whose move it is).
//
#include "core/Build.h"
#include "core/Entity.h"
#include "core/Ruleset.h"
#include "core/Spells.h" // SpellCatalog
#include "data/Net.h"    // net::Intent

#include <optional>
#include <string>
#include <vector>

namespace tb::replay {

struct GameRecord {
    int version = 1;
    std::string catalogHash;          // pins the spell catalog (== net::contentHashOf)
    unsigned seed = 0;                // regenerates the arena
    CharacterBuild player;            // seat Player
    CharacterBuild enemy;             // seat Enemy
    std::vector<net::Intent> intents; // human intents, in applied order
};

// The pinned catalog hash (same formula as net::contentHashOf).
[[nodiscard]] std::string catalogHash(const SpellCatalog& catalog);

// Compact single-line notation (`ATB1 <hash> <seed> <playerB64> <enemyB64>
// <m/c/. tokens…>`), round-trippable byte-for-byte.
[[nodiscard]] std::string serializeRecord(const GameRecord& rec);

struct RecordParse {
    bool ok = false;
    GameRecord record;
    std::string error;
};
[[nodiscard]] RecordParse parseRecord(const std::string& text);

// Re-simulate the record against the given (trusted/official) content and report
// the authoritative outcome — the arbiter's core check. `ok` means: the catalog
// hash matched, both builds were legal, and the intents drove the game to a
// finish. Illegal intents can't forge a result (the engine refuses them), so the
// returned winner is the truth.
struct VerifyResult {
    bool ok = false;
    std::optional<Faction> winner;
    std::string finalSnapshot; // serialized end state (for fingerprint / rewatch)
    std::string error;
};
[[nodiscard]] VerifyResult verify(const GameRecord& rec, const Ruleset& ruleset,
                                  const SpellCatalog& catalog, const std::vector<Entity>& creatures);

} // namespace tb::replay
