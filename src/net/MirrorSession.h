#pragma once
//
// MirrorSession.h — The client half of a networked match (Phase 4.4).
//
// A deterministic *mirror* MatchRunner, built from the server's welcome/setup and
// kept in lockstep by replaying the authoritative `applied`-intent broadcasts.
// Because the core is deterministic, the mirror reproduces the whole match
// (including AI/summon/inert turns) from just the human intents on the wire.
//
// Shared by the headless GameClient (auto-play) and the GUI RemoteMatchSource, so
// both render/act off one source of truth. Raylib-free.
//
#include "MatchRunner.h"
#include "Socket.h"
#include "core/Build.h"
#include "core/Entity.h"
#include "core/Ruleset.h"
#include "core/Spells.h" // SpellCatalog
#include "data/Net.h"    // Intent

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tb::net {

class MirrorSession {
public:
    // Connect, handshake (content hash + build), and build the mirror from the
    // server's setup. Returns nullptr on any failure (host down, rejected, bad
    // setup) with *error populated. `ruleset`/`catalog`/`creatures` are the
    // client's pinned content (they must match the server's, or the mirror
    // diverges — the handshake hash guards exactly this).
    // `user`/`pass` are the ranked login (empty for custom/unranked servers). `lobby`
    // is a private room code shared with a friend (empty = open matchmaking).
    [[nodiscard]] static std::unique_ptr<MirrorSession>
    connect(const std::string& host, uint16_t port, const std::string& contentHash,
            const CharacterBuild& build, const Ruleset& ruleset, const SpellCatalog& catalog,
            const std::vector<Entity>& creatures, std::string* error, int readTimeoutSec = 15,
            const std::string& user = "", const std::string& pass = "", const std::string& lobby = "");

    // Join a match the lobby has already paired us into (Phase 4.5): open a fresh
    // connection carrying only the one-time `token`, then build the mirror from the
    // server's `welcome` (the server already holds both builds from the seek /
    // challenge). Same setup path as connect(), minus the hello/build handshake.
    [[nodiscard]] static std::unique_ptr<MirrorSession>
    joinToken(const std::string& host, uint16_t port, const std::string& token,
              const Ruleset& ruleset, const SpellCatalog& catalog,
              const std::vector<Entity>& creatures, std::string* error, int readTimeoutSec = 60);

    [[nodiscard]] Faction seat() const { return seat_; }
    [[nodiscard]] const Battle& battle() const { return runner_.battle(); }
    [[nodiscard]] bool finished() const { return ended_ || runner_.finished(); }
    // The authoritative winner if the server ended the match by FORFEIT (idle clock /
    // disconnect) — the mirror has no death to infer it from. nullopt otherwise (a
    // normal end is read from battle().winner()).
    [[nodiscard]] std::optional<Faction> forfeitWinner() const { return forfeitWinner_; }
    // The per-move idle window (seconds) the server enforces; 0 = no clock. Sent in
    // the welcome so the client can show a countdown.
    [[nodiscard]] int clockSec() const { return clockSec_; }
    // True chess clock (6.3): the match runs accumulating banks (main + increment).
    [[nodiscard]] bool chessClock() const { return mainSec_ > 0; }
    // A seat's remaining bank right now: the last authoritative value from the
    // server, ticked down locally while that seat is the one deciding.
    [[nodiscard]] float bankSeconds(Faction f) const;
    // MY champion holds the active unit (its input is live). In team play a faction has
    // several champions piloted by different humans, so this is finer than "my faction
    // is awaited": only the pilot of the ACTIVE champion may act. (1v1: myUnit_ is the
    // sole champion, so this reduces to "my faction is awaited".)
    [[nodiscard]] bool awaitingMe() const {
        return !finished() && runner_.awaitingSeat() == seat_ &&
               runner_.battle().activeUnit() == myUnit_;
    }
    // Which champion within my faction I pilot (0..teamSize-1); 0 for 1v1.
    [[nodiscard]] int controllerSeat() const { return controllerSeat_; }

    // Send one action for our unit to the server (authoritative). The mirror only
    // advances when the server echoes it back through pump().
    bool send(const Intent& in) { return conn_.send(proto_intent(in)); }

    // In-match chat: send a line, and read the running transcript (both seats),
    // filled from the server's broadcasts drained in pump().
    bool sendChat(const std::string& text) { return conn_.send(proto_chat(text)); }
    [[nodiscard]] const std::vector<ChatLine>& chat() const { return chat_; }

    // Apply any server messages ready within timeoutMs (0 = non-blocking poll) to
    // the mirror. Returns false once the match has ended or the link broke.
    bool pump(int timeoutMs);

private:
    MirrorSession(Connection conn, MatchRunner runner, Faction seat, int clockSec)
        : conn_(std::move(conn)), runner_(std::move(runner)), seat_(seat), clockSec_(clockSec) {}
    static std::string proto_intent(const Intent& in); // avoids leaking Protocol.h here
    static std::string proto_chat(const std::string& text);
    // After our first frame is sent, read the server's `welcome` and build the
    // mirror — shared by connect() (hello) and joinToken() (lobby pairing).
    static std::unique_ptr<MirrorSession>
    fromWelcome(Connection conn, const Ruleset& ruleset, const SpellCatalog& catalog,
                const std::vector<Entity>& creatures, std::string* error);

    Connection conn_;
    MatchRunner runner_;
    Faction seat_ = Faction::Player;
    int controllerSeat_ = 0; // which champion of my faction I pilot (team play)
    EntityId myUnit_ = 0;    // the EntityId of that champion (for awaitingMe)
    bool ended_ = false;
    std::optional<Faction> forfeitWinner_;
    int clockSec_ = 0;
    // Chess clock (0 = not chess): banks from the last server message + when it
    // landed, so bankSeconds() can tick the deciding seat between messages.
    int mainSec_ = 0, incSec_ = 0;
    float bankP_ = 0.0f, bankE_ = 0.0f;
    std::chrono::steady_clock::time_point bankStamp_;
    std::vector<ChatLine> chat_;
};

} // namespace tb::net
