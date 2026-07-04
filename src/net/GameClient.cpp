//
// GameClient.cpp — see GameClient.h.
//
#include "GameClient.h"

#include "MirrorSession.h"

#include <cstdlib>
#include <limits>

namespace tb::net {

std::vector<Intent> chasePolicy(Faction seat, const Snapshot& s) {
    const Snapshot::Unit* me = nullptr;
    for (const Snapshot::Unit& u : s.units)
        if (u.id == s.active) { me = &u; break; }
    if (!me) return {Intent::endTurn()};

    const Snapshot::Unit* foe = nullptr;
    int best = std::numeric_limits<int>::max();
    for (const Snapshot::Unit& u : s.units) {
        if (u.team == seat || u.hp <= 0) continue;
        const int d = std::abs(u.pos.x - me->pos.x) + std::abs(u.pos.y - me->pos.y);
        if (d < best) { best = d; foe = &u; }
    }
    if (!foe) return {Intent::endTurn()};
    return {Intent::cast(0, foe->pos), Intent::move(foe->pos), Intent::endTurn()};
}

ClientResult playClient(const std::string& host, uint16_t port, const std::string& contentHash,
                        const CharacterBuild& build, const Ruleset& ruleset,
                        const SpellCatalog& catalog, const std::vector<Entity>& creatures,
                        const Policy& policy, int readTimeoutSec) {
    ClientResult r;
    std::string err;
    std::unique_ptr<MirrorSession> ms = MirrorSession::connect(
        host, port, contentHash, build, ruleset, catalog, creatures, &err, readTimeoutSec);
    if (!ms) { r.error = err; return r; }
    r.seat = ms->seat();

    bool sentThisTurn = false;
    for (int guard = 0; guard < 40000; ++guard) {
        if (ms->finished()) {
            r.finalSnapshot = serializeSnapshot(snapshotOf(ms->battle()));
            r.ok = true;
            return r;
        }
        if (ms->awaitingMe() && !sentThisTurn) {
            for (const Intent& in : policy(ms->seat(), snapshotOf(ms->battle())))
                if (!ms->send(in)) { r.error = "sending intent failed"; return r; }
            sentThisTurn = true;
        } else {
            // Wait for (and apply) the server's authoritative intent stream.
            if (!ms->pump(readTimeoutSec * 1000) && !ms->finished()) {
                r.error = "disconnected mid-match";
                return r;
            }
            if (!ms->awaitingMe()) sentThisTurn = false; // our turn passed — act again next time
        }
    }
    r.error = "match did not finish within bound";
    return r;
}

} // namespace tb::net
