//
// MatchRunner.cpp — see MatchRunner.h. Thin authoritative loop over tb_core: no
// new combat logic, just ownership/legality gating + server-side turn driving.
//
#include "MatchRunner.h"

#include "core/AI.h" // runEnemyTurn (drives AI champions + summons)

namespace tb::net {

MatchRunner::MatchRunner(Battle battle, Seat playerSeat, Seat enemySeat)
    : battle_(std::move(battle)), player_(playerSeat), enemy_(enemySeat) {
    advance(); // resolve any opening server-controlled turns up front
}

bool MatchRunner::serverControlsActive() const {
    if (battle_.phase() == Phase::Finished) return false;
    const Entity& a = battle_.unit(battle_.activeUnit());
    if (a.kind != EntityKind::Champion) return true; // summons + inert objects
    return seatOf(a.team) == Seat::AI;               // AI-filled champion seat
}

void MatchRunner::advance() {
    // Bounded so a pathological match can't spin forever; a real match ends long
    // before this. Each iteration resolves exactly one server-controlled turn.
    for (int guard = 0; guard < 100000 && serverControlsActive(); ++guard) {
        if (battle_.controlOf(battle_.activeUnit()) == Control::Inert)
            battle_.endTurn(); // an object: its fuse/ignition ticked at turn start
        else
            runEnemyTurn(battle_, /*autoEndTurn=*/true); // AI champion or summon
    }
}

std::optional<Faction> MatchRunner::awaitingSeat() const {
    if (battle_.phase() == Phase::Finished || serverControlsActive()) return std::nullopt;
    return battle_.unit(battle_.activeUnit()).team;
}

bool MatchRunner::submit(Faction seat, const Intent& in) {
    if (battle_.phase() == Phase::Finished) return false;
    const EntityId active = battle_.activeUnit();
    const Entity& a = battle_.unit(active);

    // Ownership: only the human seat that owns the active Champion may act.
    if (a.kind != EntityKind::Champion) return false;
    if (seatOf(a.team) != Seat::Human) return false;
    if (seat != a.team) return false;

    // Legality: the engine verbs refuse illegal casts/moves without mutating.
    const bool applied = applyIntent(battle_, active, in);
    advance(); // no-op while it is still this seat's turn; drives AI after endTurn
    return applied;
}

} // namespace tb::net
