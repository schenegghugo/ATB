#include "Intel.h"

namespace tb {

Intel buildIntel(const Battle& battle, Faction viewer) {
    Intel intel;
    intel.viewer = viewer;
    intel.byId.resize(battle.unitCount());
    for (EntityId i = 0; i < battle.unitCount(); ++i)
        if (intel.tracks(battle, i))
            intel.byId[i].revealedSlots.assign(battle.unit(i).spells.size(), 0);

    for (const BattleEvent& ev : battle.events()) {
        if (ev.type == EventType::Cast) {
            if (ev.actor < intel.byId.size() && intel.tracks(battle, ev.actor) &&
                ev.spellSlot >= 0 &&
                ev.spellSlot < static_cast<int>(intel.byId[ev.actor].revealedSlots.size()))
                intel.byId[ev.actor].revealedSlots[ev.spellSlot] = 1;
        } else if (ev.type == EventType::TurnStart) {
            if (ev.actor < intel.byId.size() && intel.tracks(battle, ev.actor))
                ++intel.byId[ev.actor].turnsObserved;
        }
    }
    return intel;
}

} // namespace tb
