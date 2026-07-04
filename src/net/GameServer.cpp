//
// GameServer.cpp — see GameServer.h.
//
#include "GameServer.h"

#include "MatchRunner.h"
#include "Protocol.h"
#include "core/Battle.h"
#include "core/Build.h"
#include "core/Match.h"
#include "data/CatalogJson.h"
#include "data/Net.h"
#include "data/Sha256.h"

#include <random>

namespace tb::net {

std::string contentHashOf(const SpellCatalog& catalog) {
    return sha256Hex(serializeCatalog(catalog, "match"));
}

namespace {

struct Admit {
    bool ok = false;
    CharacterBuild build;
    std::string error;
};

// Read one client's hello and apply the two admission checkpoints.
Admit handshake(Connection& c, const MatchConfig& cfg) {
    Admit a;
    const std::optional<std::string> raw = c.recv();
    if (!raw) { a.error = "no handshake received"; return a; }
    const std::optional<proto::Msg> m = proto::parse(*raw);
    if (!m || m->type != "hello") { a.error = "expected a hello message"; return a; }

    // Checkpoint 1 — content hash (catalog/creatures/ruleset must match, §4/§5).
    if (m->field("content") != cfg.contentHash) { a.error = "content hash mismatch"; return a; }

    // Checkpoint 2 — build admission: parseable + valid vs the ruleset (budget,
    // bans) — the SAME validateBuild the editor runs live.
    const std::optional<CharacterBuild> build = deserializeBuild(m->field("build"));
    if (!build) { a.error = "malformed build payload"; return a; }
    const BuildValidation v =
        validateBuild(*build, cfg.catalog, cfg.ruleset.economy, cfg.ruleset.bannedSpells);
    if (!v.ok) { a.error = "build rejected by ruleset"; return a; }

    a.ok = true;
    a.build = *build;
    return a;
}

} // namespace

ServeResult serveOneMatch(Listener& listener, const MatchConfig& cfg, int readTimeoutSec) {
    ServeResult res;

    std::optional<Connection> c0 = listener.accept();
    std::optional<Connection> c1 = listener.accept();
    if (!c0 || !c1) { res.error = "accept failed"; return res; }
    c0->setReadTimeout(readTimeoutSec);
    c1->setReadTimeout(readTimeoutSec);

    const Admit a0 = handshake(*c0, cfg);
    const Admit a1 = handshake(*c1, cfg);
    if (!a0.ok) c0->send(proto::error(a0.error));
    if (!a1.ok) c1->send(proto::error(a1.error));
    if (!a0.ok || !a1.ok) {
        res.error = !a0.ok ? ("player: " + a0.error) : ("enemy: " + a1.error);
        return res;
    }

    // A concrete NON-ZERO seed, chosen once and sent to both clients so the server
    // and both mirrors generate the *same* arena. (generateArena treats seed 0 as
    // "time-seed", which would hand each side a different map and desync them.)
    // Kept in [1, 1e9] so it round-trips cleanly as a JSON integer.
    const unsigned seed = std::random_device{}() % 1000000000u + 1u;

    // Seat by connection order, and hand each client the setup it needs to build
    // an identical deterministic mirror (both builds + the arena seed).
    const std::string pBuild = serializeBuild(a0.build);
    const std::string eBuild = serializeBuild(a1.build);
    c0->send(proto::welcome(Faction::Player, static_cast<int>(seed), pBuild, eBuild));
    c1->send(proto::welcome(Faction::Enemy, static_cast<int>(seed), pBuild, eBuild));

    Battle battle =
        buildMatch(cfg.ruleset, {a0.build}, {a1.build}, cfg.catalog, seed, cfg.creatures);
    MatchRunner runner(std::move(battle), Seat::Human, Seat::Human);

    auto connFor = [&](Faction f) -> Connection& { return f == Faction::Player ? *c0 : *c1; };

    // Read the awaited seat's next intent, apply it authoritatively, and broadcast
    // it to BOTH mirrors. Clients reproduce everything else (AI/summon/inert turns)
    // deterministically off this stream — so only human intents cross the wire.
    for (int guard = 0; guard < 20000 && !runner.finished(); ++guard) {
        const std::optional<Faction> awaiting = runner.awaitingSeat();
        if (!awaiting) { res.error = "no seat awaited but match not finished"; return res; }

        const std::optional<std::string> raw = connFor(*awaiting).recv();
        if (!raw) { res.error = "client disconnected mid-turn"; return res; }
        const std::optional<proto::Msg> m = proto::parse(*raw);
        if (!m || m->type != "intent") continue; // ignore noise
        const Parse<Intent> in = parseIntent(m->field("intent"));
        if (!in.ok) continue; // malformed — wait for a valid one

        // The runner enforces ownership + legality; a rejected intent is a no-op
        // (never trust the client's outcome). Broadcast the applied intent so both
        // mirrors advance in lockstep with the authoritative Battle.
        runner.submit(*awaiting, in.value);
        const std::string msg = proto::applied(*awaiting, in.value);
        if (!c0->send(msg) || !c1->send(msg)) { res.error = "broadcast failed (disconnect)"; return res; }
    }

    c0->send(proto::endMsg());
    c1->send(proto::endMsg());
    res.ok = runner.finished();
    res.winner = runner.battle().winner();
    res.finalSnapshot = serializeSnapshot(runner.snapshot());
    if (!res.ok) res.error = "match did not finish within bound";
    return res;
}

} // namespace tb::net
