#pragma once
//
// Net.h — Wire formats for server-authoritative PvP (Phase 4.1).
//
// Three small, round-trippable JSON payloads the transport (Phase 4.4) will
// carry, proven deterministic headless here with *no sockets*:
//   - Intent   — a player's action: move{dest} / cast{spellIdx,target} / endTurn.
//   - Snapshot — the near-full match state the server broadcasts after applying
//                one intent; clients render it and never compute outcomes.
//   - the build payload reuses core serializeBuild/deserializeBuild verbatim
//     (see core/Build.h) — it is already a text round-trip, so nothing new here.
//
// The Battle verbs already ARE the intent vocabulary (ARCHITECTURE.md §7), so
// applyIntent() is a thin, legality-checked dispatch over the public engine API,
// and snapshotOf() reads public accessors only. core/ is untouched.
//
#include "../core/Battle.h" // Battle, Phase, GroundEffect, EntityId, Faction, EntityKind
#include "../core/Combat.h" // StatusEffect, GroundKind
#include "../core/Grid.h"   // Vec2i

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tb::net {

// One in-match chat line: who said it (by seat) + the text.
struct ChatLine {
    Faction seat = Faction::Player;
    std::string text;
};

// --- Intent -----------------------------------------------------------------
// One player action. `move` walks toward `target` (spends MP, like moveToward);
// `cast` resolves spell slot `spellIdx` at `target`; `endTurn` passes.
struct Intent {
    enum class Kind : std::uint8_t { Move, Cast, EndTurn };
    Kind kind = Kind::EndTurn;
    int spellIdx = -1; // Cast only
    Vec2i target{};    // Move: destination tile; Cast: target tile (portal: the ENTRY)
    Vec2i target2{};   // Cast: optional second tile — the player-placed portal EXIT
    bool hasTarget2 = false;
    int rotation = 0;  // Cast: 90° steps for a Line spell's heading (Shelter walls)

    static Intent move(Vec2i dest) { return {Kind::Move, -1, dest}; }
    static Intent cast(int slot, Vec2i tgt, int rot = 0) {
        return {Kind::Cast, slot, tgt, {}, false, rot};
    }
    // A cast with a player-chosen second tile (portal: entry + explicit exit).
    static Intent castTo(int slot, Vec2i entry, Vec2i exit) {
        return {Kind::Cast, slot, entry, exit, true};
    }
    static Intent endTurn() { return {Kind::EndTurn, -1, {}}; }

    [[nodiscard]] bool operator==(const Intent& o) const {
        return kind == o.kind && spellIdx == o.spellIdx && target.x == o.target.x &&
               target.y == o.target.y && hasTarget2 == o.hasTarget2 && rotation == o.rotation &&
               (!hasTarget2 || (target2.x == o.target2.x && target2.y == o.target2.y));
    }
    [[nodiscard]] bool operator!=(const Intent& o) const { return !(*this == o); }
};

// Apply `in` as `actor`'s action with the SAME legality the engine enforces
// (canCast / range / AP / LOS for Cast; moveToward for Move, which never mutates
// on an illegal move). Returns whether the intent had an effect. This is exactly
// what the authoritative runner (Phase 4.3) calls per inbound intent.
bool applyIntent(Battle& b, EntityId actor, const Intent& in);

// --- Snapshot ---------------------------------------------------------------
// The renderable, authoritative match state, addressing units by stable
// EntityId. Deliberately excludes each unit's full spell list (the loadout is
// known from match setup / the submitted build) — a lean state delta, not a
// Battle you can rebuild. Adding spells for mid-match spectators is a later step.
struct Snapshot {
    struct Unit {
        EntityId id = 0;
        std::string name;
        Faction team = Faction::Player;
        EntityKind kind = EntityKind::Champion;
        Vec2i pos{};
        int hp = 0, maxHp = 0;
        int ap = 0, maxAp = 0;
        int mp = 0, maxMp = 0;
        int initiative = 0;
        int fuse = 0;
        std::vector<StatusEffect> statuses;
        std::vector<int> spellCooldowns;
    };
    struct Ground {
        GroundKind kind = GroundKind::Wall;
        Faction owner = Faction::Player;
        std::vector<Vec2i> tiles;
        int remainingTurns = 0;
        int magnitude = 0;
        Vec2i center{};
        Vec2i exit{};
        Element element = Element::None; // elemental surface (None = neutral)
        bool blocksLos = false;          // Steam / cloud
    };

    Phase phase = Phase::PlayerTurn;
    bool finished = false;
    std::optional<Faction> winner;
    EntityId active = 0;
    int round = 0;
    Vec2i stormCenter{};
    int safeRadius = 0;
    int stormDamage = 0;
    std::vector<Unit> units;
    std::vector<Ground> ground;
};

// Extract a Snapshot from a live Battle (read-only; public accessors only).
[[nodiscard]] Snapshot snapshotOf(const Battle& b);

// --- JSON round-trip --------------------------------------------------------
template <class T>
struct Parse {
    bool ok = false;
    T value;
    std::vector<std::string> errors; // contextual, all collected (empty when ok)
};

[[nodiscard]] std::string serializeIntent(const Intent& in);   // compact JSON
[[nodiscard]] Parse<Intent> parseIntent(const std::string& text);

[[nodiscard]] std::string serializeSnapshot(const Snapshot& s); // compact JSON
[[nodiscard]] Parse<Snapshot> parseSnapshot(const std::string& text);

} // namespace tb::net
