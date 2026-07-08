#pragma once
//
// Lobby.h — The Online Home backend (Phase 4.5: sessions, seek board, spectate).
//
// The lichess-style flow: a client connects and AUTHENTICATES without a build
// (the session), then browses — post a seek (rated/casual + teamSize + turn
// clock + build), list open seeks, accept one, list live games, watch one. The
// build is a parameter of the seek, not a gate before connecting.
//
// Concurrency model: ONE CONNECTION PER ROLE, strict request→reply.
//   - session conn: lobby requests only; pairing events arrive via `poll`
//     (the GUI pumps every frame anyway), so no socket ever has two writers.
//   - match conn:   opened on `paired{token}`; speaks the existing match
//     protocol (welcome → intents/applied → end), plus a per-turn clock the
//     server enforces (timeout/disconnect = forfeit; rated forfeits score Elo).
//   - watch conn:   read-only; gets the match setup + the applied-intent
//     backlog, then the live stream — a spectator is just another mirror.
//
// v1 limits: teamSize must be 1 (the wire carries one build per side until team
// payloads land); pending pairings don't expire; the seek board is in-memory.
//
#include "AccountStore.h" // AccountView
#include "MirrorSession.h"
#include "Socket.h"
#include "core/Build.h"
#include "core/Entity.h"
#include "core/Ruleset.h"
#include "core/Spells.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tb::net {

struct LobbyConfig {
    SpellCatalog catalog;
    std::vector<Entity> creatures;
    Ruleset casualRules;              // casual seeks play under these
    Ruleset rankedRules;              // ranked seeks pin these (e.g. rules.ranked.json)
    std::string contentHash;          // catalog pin clients must match (contentHashOf)
    AccountStore* accounts = nullptr; // required for ranked seeking
};

// Serve the lobby. Accepts `maxConns` connections in total — sessions, match
// joins and watchers each count one — then drains every thread and returns
// (< 0 = daemon, accept forever). Sessions end when their client disconnects;
// a session's open seek is withdrawn with it.
void serveLobby(Listener& listener, const LobbyConfig& cfg, int maxConns = -1);

// --- Client side -------------------------------------------------------------

struct SeekInfo {
    int id = 0;
    std::string user; // "guest" when unauthenticated
    int rating = kDefaultRating;
    bool rated = false;
    int teamSize = 1;
    int clockSec = 60;
};

struct GameInfo {
    int id = 0;
    std::string a, b;
    int ratingA = kDefaultRating, ratingB = kDefaultRating;
    bool rated = false;
};

struct PairedInfo {
    std::string token; // hand to joinMatch()
    Faction seat = Faction::Player;
};

// A lobby session: connect + (optionally) log in, then browse/seek/poll. All
// calls are blocking request→reply on this session's own socket.
class LobbySession {
public:
    // user/pass empty = guest (casual only). nullptr return + *error on failure.
    [[nodiscard]] static std::unique_ptr<LobbySession>
    connect(const std::string& host, uint16_t port, const std::string& contentHash,
            const std::string& user, const std::string& pass, std::string* error,
            int readTimeoutSec = 15);

    [[nodiscard]] const AccountView& account() const { return acct_; } // rating/W/L at login
    [[nodiscard]] bool guest() const { return guest_; }

    // Post a seek. Exactly one open seek per session (re-seek replaces it).
    bool seek(bool rated, int teamSize, int clockSec, const CharacterBuild& build,
              std::string* error = nullptr);
    bool cancelSeek();
    [[nodiscard]] std::optional<std::vector<SeekInfo>> listSeeks();
    // Accept an open seek — pairs immediately (the seek's owner learns via poll).
    [[nodiscard]] std::optional<PairedInfo> accept(int seekId, const CharacterBuild& build,
                                                   std::string* error = nullptr);
    // One pending pairing event, if any (call every frame / poll tick).
    [[nodiscard]] std::optional<PairedInfo> poll();
    [[nodiscard]] std::optional<std::vector<GameInfo>> listGames();

private:
    explicit LobbySession(Connection c) : conn_(std::move(c)) {}
    Connection conn_;
    AccountView acct_;
    bool guest_ = true;
};

// Join a paired match on a FRESH connection; returns the mirror to play through
// (same object the GUI's RemoteMatchSource wraps). The local ruleset must match
// the server's for the mode (the welcome carries the ruleset hash and joining
// fails loudly on a mismatch — no silent desync).
[[nodiscard]] std::unique_ptr<MirrorSession>
joinMatch(const std::string& host, uint16_t port, const std::string& token, const Ruleset& ruleset,
          const SpellCatalog& catalog, const std::vector<Entity>& creatures, std::string* error,
          int readTimeoutSec = 60);

// Watch a live game to completion on a fresh read-only connection: rebuilds a
// mirror from the setup + backlog, follows the applied stream, and returns the
// final serialized snapshot ("" + *error on failure).
[[nodiscard]] std::string watchGame(const std::string& host, uint16_t port, int gameId,
                                    const Ruleset& ruleset, const SpellCatalog& catalog,
                                    const std::vector<Entity>& creatures, std::string* error,
                                    int readTimeoutSec = 60);

} // namespace tb::net
