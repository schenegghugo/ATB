#pragma once
//
// Match.h — Build a Battle from a Ruleset (the unified setup path).
//
// The single place a match is constructed: arena from `rules.arena`, economy from
// `rules.economy`, closing ring from `rules.closingRing`, roster from the two
// teams' builds (teamSize is implied by the team sizes). Used by BOTH the game
// and the balance sim so they set up matches identically.
//
#include "Battle.h"
#include "Build.h"
#include "Ruleset.h"
#include "Spells.h"

#include <vector>

namespace tb {

// If `staticArena` is non-null it is used as the battlefield (a loaded static
// map); otherwise the arena is generated from `rules.arena` + `seed`.
[[nodiscard]] Battle buildMatch(const Ruleset& rules,
                                const std::vector<CharacterBuild>& playerTeam,
                                const std::vector<CharacterBuild>& enemyTeam,
                                const SpellCatalog& catalog, unsigned seed,
                                std::vector<Entity> creatures = {},
                                const Grid* staticArena = nullptr);

// The champion EntityIds of `faction`, in SEAT order (the order they were passed to
// buildMatch — index i is controller seat i). Excludes summons/objects. Deterministic
// across the server and every client mirror (same builds + seed → same roster), so it
// maps a controller seat ↔ its champion consistently for turn routing in team play.
[[nodiscard]] std::vector<EntityId> championSeats(const Battle& battle, Faction faction);

} // namespace tb
