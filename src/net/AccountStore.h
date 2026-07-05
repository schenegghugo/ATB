#pragma once
//
// AccountStore.h — Persistent player accounts for ranked play (Phase 4.5).
//
// Username + PBKDF2 password hash (Password.h) + Elo rating + W/L, backed by a
// JSON file (the project's hand-rolled json layer). Thread-safe — a mutex guards
// every operation, so the concurrent match threads (GameServer) can authenticate
// and record results safely. No email, no PII beyond the chosen username.
//
// This is the repository seam the milestone names: a SQLite backend can replace
// the JSON file behind this same class without touching callers.
//
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace tb::net {

inline constexpr int kDefaultRating = 1000;

struct AccountView {
    std::string user;
    int rating = kDefaultRating;
    int wins = 0;
    int losses = 0;
};

struct AuthResult {
    bool ok = false;
    bool created = false; // true if this call registered a new account
    std::string error;    // set when !ok
    AccountView account;
};

class AccountStore {
public:
    // Load from `path` (absent → a fresh store; present+invalid → fresh, with a
    // stderr warning so a corrupt file isn't silently trusted). Writes back to it.
    explicit AccountStore(std::string path);

    // Authenticate `user`/`password`. Unknown user → auto-register (claim the name
    // with this password). Known user → the password must match. Empty user or
    // password, or a bad password, is rejected.
    AuthResult authenticate(const std::string& user, const std::string& password);

    // Record a ranked result: Elo update (K = 32, zero-sum) + W/L, persisted.
    // No-op if either player is unknown or winner == loser.
    void recordResult(const std::string& winner, const std::string& loser);

    [[nodiscard]] int ratingOf(const std::string& user) const; // kDefaultRating if unknown
    [[nodiscard]] std::optional<AccountView> get(const std::string& user) const;
    [[nodiscard]] std::size_t size() const;

private:
    struct Record {
        std::string passwordHash;
        int rating = kDefaultRating;
        int wins = 0;
        int losses = 0;
    };
    void loadFrom(const std::string& path);
    void saveLocked() const; // caller holds mu_

    mutable std::mutex mu_;
    std::string path_;
    std::unordered_map<std::string, Record> accounts_;
};

} // namespace tb::net
