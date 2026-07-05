#pragma once
//
// Arbiter.h — Records ranked results from submitted scoresheets (CR.4).
//
// The "verify, don't host" trust anchor: two players each submit their copy of the
// game notation (net/Replay). The arbiter pairs the two submissions, requires them
// to AGREE (double attestation — a loser can't submit a different sheet), re-runs
// verify() to get the authoritative winner, and records Elo via AccountStore. No
// live match is hosted; a completed game is just re-simulated. Thread-safe.
//
// Auth note: `user` is assumed already authenticated by the network layer (login);
// the arbiter records results for that identity. A single submission NEVER changes
// a rating — you can fabricate a whole notation yourself, so both sides must agree.
//
#include "core/Entity.h" // Faction
#include "core/Ruleset.h"
#include "core/Spells.h" // SpellCatalog

#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tb::net {

class AccountStore;

class Arbiter {
public:
    Arbiter(AccountStore& accounts, Ruleset ruleset, SpellCatalog catalog,
            std::vector<Entity> creatures);

    struct Submission {
        std::string user;     // authenticated submitter
        std::string opponent; // claimed opponent
        Faction seat;         // the seat the submitter played (Player / Enemy)
        std::string notation; // their copy of the game record (scoresheet)
    };

    enum class Status { Pending, Ranked, Rejected };
    struct Result {
        Status status = Status::Rejected;
        std::string winner; // username; empty on a draw / pending / rejected
        std::string error;  // set on Rejected
    };

    // Submit one player's scoresheet. First side -> Pending; the second matching
    // side -> Ranked (Elo recorded) or Rejected (records disagree / illegal /
    // fails to verify). A re-submit by the same side stays Pending.
    Result submit(const Submission& sub);

    [[nodiscard]] std::size_t pendingCount() const;

private:
    [[nodiscard]] static std::string gameKey(const std::string& a, const std::string& b,
                                             const std::string& catalogHash, unsigned seed);

    AccountStore& accounts_;
    Ruleset ruleset_;
    SpellCatalog catalog_;
    std::vector<Entity> creatures_;

    mutable std::mutex mu_;
    std::unordered_map<std::string, Submission> pending_; // gameKey -> first submission
    std::unordered_set<std::string> resolved_;            // gameKeys already decided
};

} // namespace tb::net
