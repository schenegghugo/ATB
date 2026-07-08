#pragma once
//
// Correspondence.h — Peer-to-peer correspondence play over the mailbox relay,
// with in-game decoy commitments (CR.6 slice 3).
//
// This is the client half of the "verify, don't host" ranked model. Two players
// share a match setup (ruleset + catalog + creatures + seed + both builds) and a
// relay game id; each runs an identical, deterministic mirror MatchRunner (like
// MirrorSession, but neither side is authoritative — the shared determinism is).
// A player submits their own intents locally and POSTs them to the relay; the
// opponent's intents are pulled from the relay and applied. Both mirrors stay in
// lockstep because only the awaiting seat can act, so the human-intent stream is
// globally ordered.
//
// The new piece over 4.4's live mirror is the DECOY COMMITMENT FLOW. When the
// local player casts a decoy, they must commit up-front to which member will be
// the real one ('a' stay original / 'b' become the twin): the session mints a
// nonce, computes commit = sha256(choice ":" nonce), and ships the HASH ONLY
// alongside the move — so the choice is pinned before the opponent reacts, but
// stays hidden. The choice+nonce are exchanged only after the game finishes
// (finalize()), so both peers rebuild a byte-identical GameRecord. That record is
// the scoresheet: replay::verify() reproduces the winner and the arbiter ranks it.
// (Timeliness — that the hash was shown at cast, not invented later — is attested
// by double-submit: the opponent won't co-sign a sheet whose commits it never
// saw. See Replay.h / Arbiter.h.)
//
// v1 limits: teamSize 1 (one build per side); the caller decides the decoy choice
// at cast (inherent to commit-reveal) and must then act honestly from the
// committed member — dishonesty is caught by verify(), not prevented here.
//
#include "MatchRunner.h"
#include "MoveChannel.h" // MoveChannel transport seam
#include "Replay.h"      // GameRecord
#include "core/Build.h"
#include "core/Entity.h" // Faction
#include "core/Ruleset.h"
#include "core/Spells.h" // SpellCatalog

#include <cstddef>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace tb::net {

// The shared match setup — identical on both peers (a hash mismatch would diverge
// the mirrors; the arbiter's content pins guard the ranked case).
struct CorrespondenceSetup {
    Ruleset ruleset;
    SpellCatalog catalog;
    std::vector<Entity> creatures;
    unsigned seed = 0;
    CharacterBuild player; // seat Player
    CharacterBuild enemy;  // seat Enemy
};

class CorrespondenceSession {
public:
    // `channel` is this client's move transport (a RelayChannel over a direct relay,
    // or a LobbyChannel over a lobby session); `game` the shared game id; `mySeat`
    // which side this client plays; `user` its authenticated name (the move sender +
    // arbiter identity). Both peers pass an identical setup + game id and differ only
    // in mySeat/user.
    CorrespondenceSession(std::unique_ptr<MoveChannel> channel, std::string game,
                          CorrespondenceSetup setup, Faction mySeat, std::string user);

    // Swap the move transport (e.g. after a dropped session reconnects). The mirror
    // state + poll cursor are unchanged, so play resumes against the same log.
    void rebind(std::unique_ptr<MoveChannel> channel) { channel_ = std::move(channel); }

    [[nodiscard]] const Battle& battle() const { return runner_.battle(); }
    [[nodiscard]] Faction seat() const { return mySeat_; }
    [[nodiscard]] bool finished() const { return runner_.finished(); }
    // The local seat holds the active unit (its input is live).
    [[nodiscard]] bool awaitingMe() const {
        return !finished() && runner_.awaitingSeat() == mySeat_;
    }

    // Submit one of my own intents. If it is a decoy cast, `decoyChoice` ('a' /
    // 'b') is REQUIRED: the session mints the commitment and ships its hash with
    // the move. Returns false (no mutation, nothing sent) if it is not my turn,
    // the intent is illegal, or a decoy cast is missing its choice; *error is set.
    bool submitLocal(const Intent& in, std::optional<char> decoyChoice = std::nullopt,
                     std::string* error = nullptr);

    // Pull and apply any of the opponent's posted intents. Returns true if at
    // least one was applied. Poll this on a timer for correspondence play.
    bool sync();

    // After finished(): exchange the choice+nonce reveals for every commitment so
    // both peers end with an identical scoresheet. Idempotent — call until it
    // returns true (every commitment resolved). Returns true immediately when the
    // game had no decoy casts.
    bool finalize();

    // The accumulated scoresheet. Byte-identical across both peers only once
    // finalize() has returned true (before that, opponent commits lack the reveal).
    [[nodiscard]] const replay::GameRecord& record() const { return rec_; }
    [[nodiscard]] std::string notation() const { return replay::serializeRecord(rec_); }

private:
    [[nodiscard]] Faction opponentSeat() const {
        return mySeat_ == Faction::Player ? Faction::Enemy : Faction::Player;
    }
    // True if casting `slot` from `actor` would spawn a decoy (creates a cloak pair).
    [[nodiscard]] bool isDecoyCast(EntityId actor, int slot) const;
    // Record any cloak pair created by the intent just applied. `wireCommit` is the
    // hash carried on the opponent's message (nullptr for my own casts, where I
    // already pushed the full commit). Advances seenCloaks_.
    void trackNewCloaks(const std::string* wireCommit);
    [[nodiscard]] std::string mintNonce();

    std::unique_ptr<MoveChannel> channel_;
    std::string game_;
    Faction mySeat_;
    std::string user_;
    MatchRunner runner_;
    replay::GameRecord rec_;

    std::size_t pollCursor_ = 0;    // relay log index polled up to
    std::size_t seenCloaks_ = 0;    // cloak pairs observed (== rec_.commits.size())
    std::vector<std::size_t> myCommits_; // indices into rec_.commits I authored
    bool revealsPosted_ = false;
    std::mt19937 rng_;
};

} // namespace tb::net
