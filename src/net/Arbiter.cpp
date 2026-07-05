//
// Arbiter.cpp — see Arbiter.h.
//
#include "Arbiter.h"

#include "AccountStore.h"
#include "Replay.h"
#include "data/Sha256.h"

namespace tb::net {

Arbiter::Arbiter(AccountStore& accounts, Ruleset ruleset, SpellCatalog catalog,
                 std::vector<Entity> creatures)
    : accounts_(accounts), ruleset_(std::move(ruleset)), catalog_(std::move(catalog)),
      creatures_(std::move(creatures)) {}

std::string Arbiter::gameKey(const std::string& a, const std::string& b,
                             const std::string& catalogHash, unsigned seed) {
    // Order-independent in the two usernames so both players derive the same key.
    const std::string& lo = a < b ? a : b;
    const std::string& hi = a < b ? b : a;
    return sha256Hex(lo + "\x1f" + hi + "\x1f" + catalogHash + "\x1f" + std::to_string(seed));
}

std::size_t Arbiter::pendingCount() const {
    std::lock_guard<std::mutex> lock(mu_);
    return pending_.size();
}

Arbiter::Result Arbiter::submit(const Submission& sub) {
    Result r; // defaults to Rejected

    const replay::RecordParse pr = replay::parseRecord(sub.notation);
    if (!pr.ok) { r.error = "unparseable record: " + pr.error; return r; }
    const std::string key = gameKey(sub.user, sub.opponent, pr.record.catalogHash, pr.record.seed);

    std::lock_guard<std::mutex> lock(mu_);
    if (resolved_.count(key)) { r.error = "game already recorded"; return r; }

    auto it = pending_.find(key);
    if (it == pending_.end()) { // first side to submit
        pending_[key] = sub;
        r.status = Status::Pending;
        return r;
    }
    if (it->second.user == sub.user) { // same side re-submitting — keep waiting
        r.status = Status::Pending;
        return r;
    }

    // Both sides are in. Whatever happens now, the game is decided — don't re-open.
    const Submission other = it->second; // copy before erasing
    pending_.erase(it);
    resolved_.insert(key);

    if (other.notation != sub.notation) { r.error = "the two scoresheets disagree"; return r; }
    if (other.seat == sub.seat) { r.error = "both players claim the same seat"; return r; }

    const replay::VerifyResult v = replay::verify(pr.record, ruleset_, catalog_, creatures_);
    if (!v.ok) { r.error = "verification failed: " + v.error; return r; }

    // Map the winning seat to a username.
    const std::string playerUser = sub.seat == Faction::Player ? sub.user : other.user;
    const std::string enemyUser = sub.seat == Faction::Player ? other.user : sub.user;
    r.status = Status::Ranked;
    if (v.winner) {
        const bool playerWon = *v.winner == Faction::Player;
        r.winner = playerWon ? playerUser : enemyUser;
        accounts_.recordResult(r.winner, playerWon ? enemyUser : playerUser);
    }
    // A draw leaves ratings unchanged and r.winner empty.
    return r;
}

} // namespace tb::net
