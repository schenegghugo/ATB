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

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace tb::render {

class Animator {
public:
    // A floating combat number that rises + fades over the struck/healed unit.
    // Spawned off the event stream (Damage / Heal / Shield-Status) with the target's
    // tile captured at emit time (the unit may move or die before the clip ends).
    enum class PopupKind { Damage, Heal, Shield };
    struct Popup {
        Vec2i tile;        // where it spawns (the target's tile when the event fired)
        std::string text;  // e.g. "-7", "+4", "6"
        PopupKind kind;
        double t0;         // spawn time (seconds), matched against `now`
    };

    // Seconds a floating number stays on screen before it's pruned.
    static constexpr double kPopupLife = 1.0;

    // Consume events appended since the last call, stamping `now` (seconds) onto
    // the actor of each new Cast. A shrinking event stream (a fresh Battle) is
    // detected and treated as a reset.
    void sync(const Battle& battle, double now) {
        const auto& evs = battle.events();
        if (evs.size() < seen_) reset(); // battle was replaced (rematch / new arena)
        for (std::size_t i = seen_; i < evs.size(); ++i) {
            const BattleEvent& ev = evs[i];
            if (ev.type == EventType::Cast) castAt_[ev.actor] = now;
            // A hit stamps the VICTIM so the renderer can shake + flash it. Both
            // spell damage and collision/ring damage carry EventType::Damage.
            else if (ev.type == EventType::Damage) {
                hitAt_[ev.target] = now;
                spawnPopup(battle, ev.target, "-" + std::to_string(ev.amount),
                           PopupKind::Damage, now);
            } else if (ev.type == EventType::Heal) {
                spawnPopup(battle, ev.target, "+" + std::to_string(ev.amount), PopupKind::Heal,
                           now);
            } else if (ev.type == EventType::Status && ev.status == StatusEffect::Kind::Shield) {
                spawnPopup(battle, ev.target, std::to_string(ev.amount), PopupKind::Shield, now);
            }
        }
        seen_ = evs.size();
        // Drop popups whose clip has fully elapsed (keeps the vector bounded).
        popups_.erase(std::remove_if(popups_.begin(), popups_.end(),
                                     [&](const Popup& p) { return now - p.t0 >= kPopupLife; }),
                      popups_.end());
    }

    // Seconds since `id` last began casting, or -1 if it never has. The renderer
    // pairs this with the pack's `cast` clip; once the elapsed time passes the
    // clip's duration the pack naturally reverts to the ambient/static frame, so
    // stale entries are harmless (and re-cast simply re-stamps the time).
    [[nodiscard]] double castElapsed(EntityId id, double now) const {
        auto it = castAt_.find(id);
        return it == castAt_.end() ? -1.0 : now - it->second;
    }

    // Seconds since `id` last took damage, or -1 if never. The renderer maps a
    // short window after this into a shake offset + red flash (see kHitClip).
    [[nodiscard]] double hitElapsed(EntityId id, double now) const {
        auto it = hitAt_.find(id);
        return it == hitAt_.end() ? -1.0 : now - it->second;
    }

    // The live floating numbers (spawned within the last kPopupLife seconds). The
    // renderer maps each to a rising, fading label; empty when nothing's happening.
    [[nodiscard]] const std::vector<Popup>& popups() const { return popups_; }

    void reset() {
        seen_ = 0;
        castAt_.clear();
        hitAt_.clear();
        popups_.clear();
    }

private:
    // Capture the target's current tile (it may move/die before the clip ends).
    void spawnPopup(const Battle& battle, EntityId target, std::string text, PopupKind kind,
                    double now) {
        popups_.push_back({battle.unit(target).pos, std::move(text), kind, now});
    }

    std::size_t seen_ = 0;
    std::unordered_map<EntityId, double> castAt_;
    std::unordered_map<EntityId, double> hitAt_;
    std::vector<Popup> popups_;
};

} // namespace tb::render
