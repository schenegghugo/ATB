//
// GameServer.cpp — see GameServer.h.
//
#include "GameServer.h"

#include "AccountStore.h"
#include "MatchRunner.h"
#include "Protocol.h"
#include "core/Battle.h"
#include "core/Build.h"
#include "core/Match.h"
#include "data/CatalogJson.h"
#include "data/Net.h"
#include "data/Sha256.h"

#include <cstdlib>
#include <limits>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>

namespace tb::net {

std::string contentHashOf(const SpellCatalog& catalog) {
    return sha256Hex(serializeCatalog(catalog, "match"));
}

namespace {

struct Admit {
    bool ok = false;
    CharacterBuild build;
    std::string user;  // the authenticated username (empty in custom/unranked mode)
    std::string lobby; // private room code (empty = open matchmaking)
    int rating = kDefaultRating;
    std::string error;
};

// Read one client's hello and apply the admission checkpoints.
Admit handshake(Connection& c, const MatchConfig& cfg) {
    Admit a;
    const std::optional<std::string> raw = c.recv();
    if (!raw) { a.error = "no handshake received"; return a; }
    const std::optional<proto::Msg> m = proto::parse(*raw);
    if (!m || m->type != "hello") { a.error = "expected a hello message"; return a; }

    // Checkpoint 1 — content hash (catalog/creatures/ruleset must match, §4/§5).
    if (m->field("content") != cfg.contentHash) { a.error = "content hash mismatch"; return a; }

    // Checkpoint 2 — login (RANKED only): username + password against the store,
    // auto-registering a new name. Custom/unranked servers (no store) skip this.
    if (cfg.accounts) {
        const AuthResult au = cfg.accounts->authenticate(m->field("user"), m->field("pass"));
        if (!au.ok) { a.error = "login: " + au.error; return a; }
        a.user = m->field("user");
        a.rating = au.account.rating;
    }

    // Checkpoint 3 — build admission: parseable + valid vs the ruleset (budget,
    // bans) — the SAME validateBuild the editor runs live.
    const std::optional<CharacterBuild> build = deserializeBuild(m->field("build"));
    if (!build) { a.error = "malformed build payload"; return a; }
    const BuildValidation v =
        validateBuild(*build, cfg.catalog, cfg.ruleset.economy, cfg.ruleset.bannedSpells);
    if (!v.ok) { a.error = "build rejected by ruleset"; return a; }

    a.ok = true;
    a.build = *build;
    a.lobby = m->field("lobby");
    return a;
}

} // namespace

// Run one already-admitted match to completion over the two connections.
ServeResult runAdmittedMatch(Connection c0, Connection c1, const CharacterBuild& b0,
                             const CharacterBuild& b1, const MatchConfig& cfg, int clockSec) {
    ServeResult res;

    // A concrete NON-ZERO seed, chosen once and sent to both clients so the server
    // and both mirrors generate the *same* arena. (generateArena treats seed 0 as
    // "time-seed", which would hand each side a different map and desync them.)
    // Kept in [1, 1e9] so it round-trips cleanly as a JSON integer.
    const unsigned seed = std::random_device{}() % 1000000000u + 1u;

    // Seat by connection order, and hand each client the setup it needs to build
    // an identical deterministic mirror (both builds + the arena seed).
    const std::string pBuild = serializeBuild(b0);
    const std::string eBuild = serializeBuild(b1);
    c0.send(proto::welcome(Faction::Player, static_cast<int>(seed), pBuild, eBuild, clockSec));
    c1.send(proto::welcome(Faction::Enemy, static_cast<int>(seed), pBuild, eBuild, clockSec));

    Battle battle = buildMatch(cfg.ruleset, {b0}, {b1}, cfg.catalog, seed, cfg.creatures);
    MatchRunner runner(std::move(battle), Seat::Human, Seat::Human);

    auto connFor = [&](Faction f) -> Connection& { return f == Faction::Player ? c0 : c1; };

    // Read the awaited seat's next intent, apply it authoritatively, and broadcast
    // it to BOTH mirrors. Clients reproduce everything else (AI/summon/inert turns)
    // deterministically off this stream — so only human intents cross the wire.
    for (int guard = 0; guard < 20000 && !runner.finished(); ++guard) {
        const std::optional<Faction> awaiting = runner.awaitingSeat();
        if (!awaiting) { res.error = "no seat awaited but match not finished"; return res; }

        const std::optional<std::string> raw = connFor(*awaiting).recv();
        if (!raw) {
            // The active player ran out their move clock (the conn read timeout) or
            // dropped → they FORFEIT; the opponent wins. Both are told the winner
            // (no death to infer it from). Elo is recorded by the caller on r.winner.
            const Faction winner = opposing(*awaiting);
            res.winner = winner;
            res.ok = true;
            res.error = "forfeit (idle clock / disconnect)";
            c0.send(proto::endMsg(winner, /*forfeit=*/true));
            c1.send(proto::endMsg(winner, /*forfeit=*/true));
            res.finalSnapshot = serializeSnapshot(runner.snapshot());
            return res;
        }
        const std::optional<proto::Msg> m = proto::parse(*raw);
        if (!m || m->type != "intent") continue; // ignore noise
        const Parse<Intent> in = parseIntent(m->field("intent"));
        if (!in.ok) continue; // malformed — wait for a valid one

        // The runner enforces ownership + legality; a rejected intent is a no-op
        // (never trust the client's outcome). Broadcast the applied intent so both
        // mirrors advance in lockstep with the authoritative Battle.
        runner.submit(*awaiting, in.value);
        const std::string msg = proto::applied(*awaiting, in.value);
        if (!c0.send(msg) || !c1.send(msg)) { res.error = "broadcast failed (disconnect)"; return res; }
    }

    c0.send(proto::endMsg());
    c1.send(proto::endMsg());
    res.ok = runner.finished();
    res.winner = runner.battle().winner();
    res.finalSnapshot = serializeSnapshot(runner.snapshot());
    if (!res.ok) res.error = "match did not finish within bound";
    return res;
}

ServeResult serveOneMatch(Listener& listener, const MatchConfig& cfg, int readTimeoutSec) {
    std::optional<Connection> c0 = listener.accept();
    std::optional<Connection> c1 = listener.accept();
    if (!c0 || !c1) return {false, std::nullopt, {}, "accept failed"};
    c0->setReadTimeout(readTimeoutSec);
    c1->setReadTimeout(readTimeoutSec);

    const Admit a0 = handshake(*c0, cfg);
    const Admit a1 = handshake(*c1, cfg);
    if (!a0.ok) c0->send(proto::error(a0.error));
    if (!a1.ok) c1->send(proto::error(a1.error));
    if (!a0.ok || !a1.ok)
        return {false, std::nullopt, {}, !a0.ok ? ("player: " + a0.error) : ("enemy: " + a1.error)};

    return runAdmittedMatch(std::move(*c0), std::move(*c1), a0.build, a1.build, cfg, readTimeoutSec);
}

int serveMatches(Listener& listener, const MatchConfig& cfg, int maxMatches, int readTimeoutSec) {
    struct Waiter {
        Connection conn;
        CharacterBuild build;
        std::string user;
        int rating = kDefaultRating;
    };

    std::vector<std::thread> matches;                  // joined at the end only in bounded mode
    std::vector<Waiter> pool;                          // open matchmaking (empty lobby)
    std::unordered_map<std::string, Waiter> rooms;     // private lobby code -> waiting host
    int started = 0;

    // Start a match on its own thread; a `rated` open-matchmaking result updates Elo.
    auto startMatch = [&](Waiter x, Waiter y, bool rated) {
        std::thread t([&cfg, c0 = std::move(x.conn), b0 = std::move(x.build), u0 = std::move(x.user),
                       c1 = std::move(y.conn), b1 = std::move(y.build), u1 = std::move(y.user), rated,
                       readTimeoutSec]() mutable {
            const ServeResult r =
                runAdmittedMatch(std::move(c0), std::move(c1), b0, b1, cfg, readTimeoutSec);
            if (rated && cfg.accounts && r.ok && r.winner) {
                const bool playerWon = *r.winner == Faction::Player;
                cfg.accounts->recordResult(playerWon ? u0 : u1, playerWon ? u1 : u0);
            }
        });
        if (maxMatches < 0) t.detach(); // daemon: fire-and-forget (cfg outlives the process)
        else matches.push_back(std::move(t));
        ++started;
    };

    while (maxMatches < 0 || started < maxMatches) {
        std::optional<Connection> conn = listener.accept();
        if (!conn) break; // listener closed
        conn->setReadTimeout(readTimeoutSec);
        Admit a = handshake(*conn, cfg);
        if (!a.ok) { conn->send(proto::error(a.error)); continue; } // rejected — drop
        Waiter w{std::move(*conn), std::move(a.build), std::move(a.user), a.rating};

        if (!a.lobby.empty()) {
            // Private lobby: pair with whoever else presents the same code (unrated),
            // else hold this player as the room's host.
            auto it = rooms.find(a.lobby);
            if (it == rooms.end()) { rooms.emplace(a.lobby, std::move(w)); continue; }
            Waiter host = std::move(it->second);
            rooms.erase(it);
            startMatch(std::move(host), std::move(w), /*rated=*/false);
            continue;
        }

        // Open matchmaking: pair with the closest-rated waiter (ranked); unranked
        // everyone is kDefaultRating, so this degrades to FIFO.
        if (pool.empty()) { pool.push_back(std::move(w)); continue; }
        std::size_t best = 0;
        int bestDiff = std::numeric_limits<int>::max();
        for (std::size_t i = 0; i < pool.size(); ++i) {
            const int d = std::abs(pool[i].rating - w.rating);
            if (d < bestDiff) { bestDiff = d; best = i; }
        }
        Waiter opp = std::move(pool[best]);
        pool.erase(pool.begin() + static_cast<std::ptrdiff_t>(best));
        startMatch(std::move(opp), std::move(w), /*rated=*/cfg.accounts != nullptr);
    }

    for (std::thread& t : matches)
        if (t.joinable()) t.join();
    return started;
}

} // namespace tb::net
