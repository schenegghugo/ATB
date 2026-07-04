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
    [[nodiscard]] static std::unique_ptr<MirrorSession>
    connect(const std::string& host, uint16_t port, const std::string& contentHash,
            const CharacterBuild& build, const Ruleset& ruleset, const SpellCatalog& catalog,
            const std::vector<Entity>& creatures, std::string* error, int readTimeoutSec = 15);

    [[nodiscard]] Faction seat() const { return seat_; }
    [[nodiscard]] const Battle& battle() const { return runner_.battle(); }
    [[nodiscard]] bool finished() const { return ended_ || runner_.finished(); }
    // The local seat holds the active unit (its input is live).
    [[nodiscard]] bool awaitingMe() const { return !finished() && runner_.awaitingSeat() == seat_; }

    // Send one action for our unit to the server (authoritative). The mirror only
    // advances when the server echoes it back through pump().
    bool send(const Intent& in) { return conn_.send(proto_intent(in)); }

    // Apply any server messages ready within timeoutMs (0 = non-blocking poll) to
    // the mirror. Returns false once the match has ended or the link broke.
    bool pump(int timeoutMs);

private:
    MirrorSession(Connection conn, MatchRunner runner, Faction seat)
        : conn_(std::move(conn)), runner_(std::move(runner)), seat_(seat) {}
    static std::string proto_intent(const Intent& in); // avoids leaking Protocol.h here

    Connection conn_;
    MatchRunner runner_;
    Faction seat_ = Faction::Player;
    bool ended_ = false;
};

} // namespace tb::net
