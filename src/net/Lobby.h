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
#include "MoveChannel.h"  // MoveChannel, ChannelPoll
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
    int readyCheckSec = 30;           // per-pairing ready-check window (both must READY)
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

// The result of accepting (or being accepted into) a game. A LIVE pairing carries a
// match token to join; a CORRESPONDENCE pairing carries the full setup to build a
// CorrespondenceSession (played over a LobbyChannel on this same session).
struct PairedInfo {
    bool live = true;   // true → live match (token); false → correspondence (game + setup)
    Faction seat = Faction::Player;
    bool rated = false; // pick ranked vs casual ruleset locally to match the server
    // live:
    std::string token; // hand to MirrorSession::joinToken()
    // correspondence:
    std::string game;   // relay/lobby game id
    unsigned seed = 0;  // regenerates the arena
    CharacterBuild player, enemy; // both builds (seat Player / Enemy)
};

// The outcome of submitting a finished correspondence scoresheet to the lobby.
struct SubmitResult {
    enum class Status { Ranked, Pending, Casual, Rejected };
    Status status = Status::Rejected;
    std::string winner; // username; empty on draw / pending / casual / rejected
    std::string error;  // set on Rejected
};

// A pairing has formed and BOTH players now get a ready check: pick/edit a build and
// READY within `seconds`, or the game is cancelled. No build was committed up front.
struct ReadyCheckInfo {
    int id = 0;
    std::string opponent;
    Faction seat = Faction::Player; // the seat you'll play
    bool rated = false;
    MatchFormat format;
    int seconds = 30; // the ready window
};

// The reply to ready(): Waiting (you readied, opponent hasn't), Matched (both ready →
// `paired`), Rejected (your build is illegal — re-pick), or Cancelled (timeout/decline).
struct ReadyResult {
    enum class Status { Waiting, Matched, Rejected, Cancelled };
    Status status = Status::Cancelled;
    PairedInfo paired;  // valid on Matched
    std::string error;  // Rejected reason
};

// An async lobby event drained by poll(): someone accepted your seek/challenge
// (ReadyCheck), both of you readied (Paired), or a pending ready check was cancelled.
struct LobbyEvent {
    enum class Kind { None, ReadyCheck, Paired, Cancelled };
    Kind kind = Kind::None;
    ReadyCheckInfo readyCheck;
    PairedInfo paired;
    std::string message; // cancel reason
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
    // replaces it). No build — you choose it at the ready check after pairing.
    bool seek(const MatchFormat& fmt, std::string* error = nullptr);
    bool cancelSeek();
    [[nodiscard]] std::optional<std::vector<SeekInfo>> listSeeks();
    // Accept an open seek → a ready check (the seek's owner learns via poll()).
    [[nodiscard]] std::optional<ReadyCheckInfo> acceptSeek(int seekId, std::string* error = nullptr);

    // Directed challenges (to a specific username).
    bool challenge(const std::string& toUser, const MatchFormat& fmt, std::string* error = nullptr);
    [[nodiscard]] std::optional<std::vector<ChallengeInfo>> listChallenges(); // incoming
    [[nodiscard]] std::optional<ReadyCheckInfo> acceptChallenge(int id, std::string* error = nullptr);
    bool declineChallenge(int id);

    // --- ready check (after a pairing) --------------------------------------
    // Submit your build + READY. Waiting → poll() for the Paired/Cancelled outcome;
    // Matched → the pairing is in `paired`; Rejected → your build is illegal.
    [[nodiscard]] ReadyResult ready(int readyCheckId, const CharacterBuild& build,
                                    std::string* error = nullptr);
    // Decline / leave a ready check (cancels it for both).
    bool cancelReady(int readyCheckId);

    // One async event, if any (a ready check appeared, both readied, or one was
    // cancelled). Call every frame / poll tick.
    [[nodiscard]] LobbyEvent poll();

    // --- correspondence move transport (used by LobbyChannel) ---------------
    // Post one move-string to a correspondence game's server-side log; returns the
    // new log length. The sender is the authenticated user (server-side).
    [[nodiscard]] std::optional<std::size_t> corrPost(const std::string& game,
                                                      const std::string& msg);
    // Poll the game's log from `from`.
    [[nodiscard]] std::optional<ChannelPoll> corrPoll(const std::string& game, std::size_t from);
    // Submit this seat's finished scoresheet; a rated game ranks once both agree.
    [[nodiscard]] SubmitResult submitScore(const std::string& game, Faction seat,
                                           const std::string& notation);

private:
    explicit LobbySession(Connection c) : conn_(std::move(c)) {}
    // send one request, read one reply; nullopt on link failure.
    [[nodiscard]] std::optional<std::string> rpc(const std::string& request);

    Connection conn_;
    AccountView acct_;
    bool guest_ = true;
};

// A MoveChannel that carries correspondence moves over a lobby session (the server
// holds the log). Non-owning — the LobbySession must outlive it.
class LobbyChannel : public MoveChannel {
public:
    explicit LobbyChannel(LobbySession* session) : session_(session) {}
    std::optional<std::size_t> post(const std::string& game, const std::string& /*sender*/,
                                    const std::string& msg) override {
        return session_->corrPost(game, msg);
    }
    std::optional<ChannelPoll> poll(const std::string& game, std::size_t from) override {
        return session_->corrPoll(game, from);
    }

private:
    LobbySession* session_;
};

} // namespace tb::net
