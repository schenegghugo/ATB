#pragma once
//
// lobby_test_util.h — shared helpers for the lobby demos (Phase 4.5 slice 6.2).
//
// The ready-check flow means a pairing forms in two steps: someone accepts (→ a
// ready check), then BOTH submit a build + READY. `readyUp` runs that handshake and
// returns each side's PairedInfo.
//
#include "core/Build.h"
#include "core/Spells.h"
#include "net/Lobby.h"

namespace tbtest {

inline tb::CharacterBuild makeBuild(const char* name) {
    tb::CharacterBuild b;
    b.name = name;
    b.stats.hpPurchases = 2;
    b.spellIds = {tb::spellid::Attack};
    return b;
}

// Both players ready up (initiator plays Player, acceptor plays Enemy). The acceptor
// readies first (→ Waiting), the initiator second (→ Matched with its paired info),
// and the acceptor learns via poll(). Returns false if anything but that happens.
inline bool readyUp(tb::net::LobbySession& initiator, const tb::net::ReadyCheckInfo& initRc,
                    const tb::CharacterBuild& initBuild, tb::net::LobbySession& acceptor,
                    const tb::net::ReadyCheckInfo& accRc, const tb::CharacterBuild& accBuild,
                    tb::net::PairedInfo& initPaired, tb::net::PairedInfo& accPaired) {
    using namespace tb::net;
    if (acceptor.ready(accRc.id, accBuild).status != ReadyResult::Status::Waiting) return false;
    const ReadyResult r2 = initiator.ready(initRc.id, initBuild);
    if (r2.status != ReadyResult::Status::Matched) return false;
    initPaired = r2.paired;
    const LobbyEvent ev = acceptor.poll();
    if (ev.kind != LobbyEvent::Kind::Paired) return false;
    accPaired = ev.paired;
    return true;
}

} // namespace tbtest
