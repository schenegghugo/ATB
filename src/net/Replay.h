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

// A decoy-reveal commitment (CR.6 slice 2). Published at decoy-cast time, it
// binds the caster to which member will be the real one BEFORE the opponent
// reacts — so the reveal can't be chosen retroactively based on where damage
// landed. choice "a" = stay the original, "b" = become the twin. The nonce keeps
// the one-bit choice unguessable until revealed. Timeliness (that the commit was
// shown at cast, not invented later) is attested by double-submit: the opponent
// would not co-sign a scoresheet whose commitments they never saw in play.
struct DecoyCommit {
    std::string commit; // sha256Hex(choice + ":" + nonce), published at cast
    std::string choice; // "a" | "b" — revealed after the reveal (or at game end)
    std::string nonce;  // revealed with the choice; whitespace-free
};

// hash(choice, nonce) exactly as verify() recomputes it.
[[nodiscard]] std::string makeCommitment(const std::string& choice, const std::string& nonce);

struct GameRecord {
    int version = 1;
    std::string catalogHash;          // pins the spell catalog (== net::contentHashOf)
    std::string rulesetHash;          // pins the ruleset it was played under (ranked vs custom)
    unsigned seed = 0;                // regenerates the arena
    CharacterBuild player;            // seat Player
    CharacterBuild enemy;             // seat Enemy
    std::vector<net::Intent> intents; // human intents, in applied order
    std::vector<DecoyCommit> commits; // one per decoy cast, in cast order (any seat)
};

// The pinned catalog hash (same formula as net::contentHashOf).
[[nodiscard]] std::string catalogHash(const SpellCatalog& catalog);

// The pinned ruleset hash: content-only (a fixed version label), so the same rules
// hash identically wherever the file came from — this is what lets an official
// ranked ruleset be fetched from a URL and verified by hash.
[[nodiscard]] std::string rulesetHash(const Ruleset& ruleset);

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
// hash matched, both builds were legal, the intents drove the game to a finish,
// and every decoy cast's reveal matched its commitment (a member that acts must
// be the committed one; a pair left to expire reveals the ORIGINAL by rule, so a
// "b" commitment that expires fails — that is the dodge-the-damage cheat). Pairs
// still cloaked when the game ends are exempt from the choice check (the secret
// never resolved), but every listed commitment must still hash-verify. Set
// `requireCommitments` false for casual replays that never exchanged them.
struct VerifyResult {
    bool ok = false;
    std::optional<Faction> winner;
    std::string finalSnapshot; // serialized end state (for fingerprint / rewatch)
    std::string error;
};
[[nodiscard]] VerifyResult verify(const GameRecord& rec, const Ruleset& ruleset,
                                  const SpellCatalog& catalog, const std::vector<Entity>& creatures,
                                  bool requireCommitments = true);

} // namespace tb::replay
