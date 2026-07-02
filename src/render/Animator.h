#pragma once
//
// Animator.h — transient presentation state for event-driven sprite clips (§2.4).
//
// The renderer's draw is otherwise a pure function of the Battle state, but a
// one-shot clip (a "cast" flash) needs to remember *when* it started so it can
// play forward and then hand back to the ambient loop. That memory lives here,
// in the frontend, not in `core/` — animation is cosmetic and never affects
// resolution.
//
// It works off the same deterministic `BattleEvent` stream the combat log reads
// (§2.3): each frame, `sync()` consumes events newer than it has seen and stamps
// the trigger time onto the acting entity. `castElapsed()` then tells the
// renderer how far into an entity's cast clip we are (or that none is playing).
// Raylib-free — no textures, just timing — so it needs no GL context.
//
#include "../core/Battle.h"
#include "../core/Event.h"

#include <cstddef>
#include <unordered_map>

namespace tb::render {

class Animator {
public:
    // Consume events appended since the last call, stamping `now` (seconds) onto
    // the actor of each new Cast. A shrinking event stream (a fresh Battle) is
    // detected and treated as a reset.
    void sync(const Battle& battle, double now) {
        const auto& evs = battle.events();
        if (evs.size() < seen_) reset(); // battle was replaced (rematch / new arena)
        for (std::size_t i = seen_; i < evs.size(); ++i)
            if (evs[i].type == EventType::Cast) castAt_[evs[i].actor] = now;
        seen_ = evs.size();
    }

    // Seconds since `id` last began casting, or -1 if it never has. The renderer
    // pairs this with the pack's `cast` clip; once the elapsed time passes the
    // clip's duration the pack naturally reverts to the ambient/static frame, so
    // stale entries are harmless (and re-cast simply re-stamps the time).
    [[nodiscard]] double castElapsed(EntityId id, double now) const {
        auto it = castAt_.find(id);
        return it == castAt_.end() ? -1.0 : now - it->second;
    }

    void reset() {
        seen_ = 0;
        castAt_.clear();
    }

private:
    std::size_t seen_ = 0;
    std::unordered_map<EntityId, double> castAt_;
};

} // namespace tb::render
