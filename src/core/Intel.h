#pragma once
//
// Intel.h — the observed-opponent model (what a faction has *seen*, not what
// the ground truth is).
//
// A Brain that reads its foes' true loadouts plays like it can see the
// opponent's hand. Intel replaces that with knowledge honestly derivable from
// the match so far: a fold over the Battle's event stream (Event.h). Every
// Cast event carries actor + spell slot, so "which of this foe's spells have
// been revealed" is a pure function of battle history — no engine changes, no
// Brain-side state, deterministic for replays and the gauntlet.
//
// What counts as known:
//  - A revealed slot (the foe cast it at least once). Once seen, the spell's
//    catalog data — including its cooldown — is public, and because the event
//    log is a complete record of casts, the slot's current cooldown state is
//    exactly inferable; reading it from the entity is equivalent, not cheating.
//  - Summons and Objects are public templates (creatures.json): the moment one
//    spawns, everything about it is known. Intel tracks Champions only.
//  - Everything positional (HP, position, statuses) is on the open board.
//
// What stays unknown: unrevealed champion slots. The evaluator prices those
// with a decaying prior — cautious probing early, confident exploitation once
// a foe has had turns to show its threats and hasn't.
//
#include "Battle.h"

#include <vector>

namespace tb {

// What one faction has observed about one enemy Champion.
struct FoeIntel {
    std::vector<char> revealedSlots; // parallel to the foe's spells; 1 = seen cast
    int turnsObserved = 0;           // the foe's completed turn-starts so far —
                                     // drives the unknown-threat decay
    [[nodiscard]] bool revealed(std::size_t slot) const {
        return slot < revealedSlots.size() && revealedSlots[slot] != 0;
    }
};

// Everything `viewer` knows about its opponents. Indexed by EntityId; entries
// for allies / non-champions are default-empty and unused.
struct Intel {
    Faction viewer = Faction::Enemy;
    std::vector<FoeIntel> byId;
    // True when `attacker` is subject to hidden-information modelling from the
    // viewer's side (an enemy Champion); false = fully known (allies, summons,
    // objects — public templates).
    [[nodiscard]] bool tracks(const Battle& b, EntityId attacker) const {
        return b.unit(attacker).team != viewer && b.unit(attacker).isChampion();
    }
};

// Folds the battle's event stream into the viewer's knowledge. Requires event
// recording to have been ON for the real match (the default; the AI only turns
// it off on throwaway clones). Call once per planning pass on the *real*
// battle — foes don't act during our turn, so intel is constant while planning.
[[nodiscard]] Intel buildIntel(const Battle& battle, Faction viewer);

} // namespace tb
