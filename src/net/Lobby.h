#pragma once
//
// Lobby.h — The Online Home: seek board + directed challenges (Phase 4.5 slice 5).
//
// A logged-in client browses and challenges without committing to a match up front:
// post an OPEN SEEK (anyone may accept) or send a DIRECTED CHALLENGE to a named
// user; the target sees it and accepts/declines. Each seek/challenge carries a
// MatchFormat — rated or casual, and a clock: Unlimited (→ correspondence),
// Per-move, or chess-style main+increment (both → a live authoritative match).
//
// Concurrency model: ONE CONNECTION PER ROLE, strict request→reply.
//   - session conn: lobby requests only; async pairings arrive via `poll` (the GUI
//     pumps every frame anyway), so no socket ever has two writers.
//   - match conn:   opened on a `paired{token}` — carries just the one-time token;
//     the server already holds both builds, replies `welcome`, and drives the
//     existing authoritative match loop (MirrorSession::joinToken mirrors it).
//
// v1 (slice 1): teamSize 1; live (Per-move / Chess) formats route to the live
// server; Unlimited (correspondence) is rejected until slice 2 wires it. The seek
// board / pending pairings are in-memory. A real chess clock is slice 3 (Per-move
// uses the per-move read timeout today).
//
#include "AccountStore.h" // AccountView, kDefaultRating
#include "Socket.h"
#include "core/Build.h"
#include "core/Entity.h" // Faction
#include "core/Ruleset.h"
#include "core/Spells.h" // SpellCatalog

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tb::net {

// How a game is played: the clock (→ which transport) + rated/casual + team size.
struct MatchFormat {
    enum class Time : std::uint8_t { Unlimited, PerMove, Chess };
    Time time = Time::PerMove;
    int perMoveSec = 30;          // Time::PerMove — seconds allowed per turn
    int mainSec = 300, incSec = 5; // Time::Chess — bank + increment (slice 3 enforces)
    bool rated = false;           // rated → ranked ruleset + Elo; casual → casual ruleset
    int teamSize = 1;             // v1 = 1
    [[nodiscard]] bool live() const { return time != Time::Unlimited; }
};

struct LobbyConfig {
    SpellCatalog catalog;
    std::vector<Entity> creatures;
    Ruleset casualRules;              // casual seeks/challenges play under these
    Ruleset rankedRules;              // rated ones pin these (e.g. rules.ranked.json)
    std::string contentHash;          // catalog pin clients must match (contentHashOf)
    AccountStore* accounts = nullptr; // required for rated play / non-guest login
};

// Serve the lobby. Accepts up to `maxConns` connections total — sessions and match
// joins each count one — then joins every connection thread and returns
// (< 0 = daemon, accept forever). A session ends when its client disconnects; its
// open seek + outgoing challenges are withdrawn with it.
void serveLobby(Listener& listener, const LobbyConfig& cfg, int maxConns = -1,
                int readTimeoutSec = 300);

// --- Client side -------------------------------------------------------------

struct SeekInfo {
    int id = 0;
    std::string user;
    int rating = kDefaultRating;
    MatchFormat format;
};

struct ChallengeInfo { // an INCOMING challenge (someone challenged me)
    int id = 0;
    std::string from;
    int fromRating = kDefaultRating;
    MatchFormat format;
};

struct PairedInfo {
    std::string token; // hand to MirrorSession::joinToken()
    Faction seat = Faction::Player;
    bool rated = false; // pick ranked vs casual ruleset locally to match the server
};

// A lobby session: connect + (optionally) log in, then browse/seek/challenge/poll.
// All calls are blocking request→reply on this session's own socket.
class LobbySession {
public:
    // user/pass empty = guest (casual only). nullptr return + *error on failure.
    [[nodiscard]] static std::unique_ptr<LobbySession>
    connect(const std::string& host, uint16_t port, const std::string& contentHash,
            const std::string& user, const std::string& pass, std::string* error,
            int readTimeoutSec = 15);

    [[nodiscard]] const AccountView& account() const { return acct_; } // rating/W/L at login
    [[nodiscard]] bool guest() const { return guest_; }

    // Open seeks (anyone may accept). Exactly one open seek per session (re-seek
    // replaces it).
    bool seek(const MatchFormat& fmt, const CharacterBuild& build, std::string* error = nullptr);
    bool cancelSeek();
    [[nodiscard]] std::optional<std::vector<SeekInfo>> listSeeks();
    // Accept an open seek — pairs immediately (the seek's owner learns via poll()).
    [[nodiscard]] std::optional<PairedInfo> acceptSeek(int seekId, const CharacterBuild& build,
                                                       std::string* error = nullptr);

    // Directed challenges (to a specific username).
    bool challenge(const std::string& toUser, const MatchFormat& fmt, const CharacterBuild& build,
                   std::string* error = nullptr);
    [[nodiscard]] std::optional<std::vector<ChallengeInfo>> listChallenges(); // incoming
    [[nodiscard]] std::optional<PairedInfo> acceptChallenge(int id, const CharacterBuild& build,
                                                            std::string* error = nullptr);
    bool declineChallenge(int id);

    // One pending pairing event, if any (my seek/challenge was accepted). Call every
    // frame / poll tick.
    [[nodiscard]] std::optional<PairedInfo> poll();

private:
    explicit LobbySession(Connection c) : conn_(std::move(c)) {}
    // send one request, read one reply; nullopt on link failure.
    [[nodiscard]] std::optional<std::string> rpc(const std::string& request);

    Connection conn_;
    AccountView acct_;
    bool guest_ = true;
};

} // namespace tb::net
