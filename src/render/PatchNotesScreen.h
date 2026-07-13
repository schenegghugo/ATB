#pragma once
//
// PatchNotesScreen.h — the "what's new" screen, reachable from the main menu and
// the in-game pause menu. Immediate-mode + stateless bar its scroll offset;
// main.cpp routes the Back result. The notes text + the release codename live
// here as constants so the menu footer and this screen agree.
//
namespace tb::render {

// The human-facing codename for the current release (shown beside the version).
inline constexpr const char* kVersionCodename = "Elemental Surfaces";

// Player-facing changelog, newest release first. Plain text, one item per line
// (kept under ~72 cols so it fits without wrapping).
inline constexpr const char* kPatchNotes =
    R"(0.0.2  —  "Elemental Surfaces"

Divinity / BG3-style elemental surfaces you paint onto the floor and combine.

NEW SPELLS
  Storm      Rain soaks a zone; next turn lightning strikes it, electrifying
             the water to shock AND stun everyone standing in it.
  Blizzard   A cone of ice: damages, freezes (roots) foes, and ices the ground.
  Ignite / Puddle / Electrify   Cheap "painters" to set up your own reactions.

SURFACES & REACTIONS
  Fire, Water, Ice, Poison, Electric, Heal and Oil surfaces persist on the map.
  They react when they meet:
    - Water + Fire      = Steam (blocks line of sight)
    - Water + Electric  = a live field (shock + stun everyone on it)
    - Water + Ice       = Ice (freezes anyone caught)
    - Fire  + Poison/Oil= explosion
    - Fire melts Ice to Water; Water freezes to Ice; and more.
  New states: Wet, Burning, Frozen (rooted), Stunned (skip a turn), Oiled.

QUALITY OF LIFE
  - Shelter walls rotate 90 degrees with the mouse wheel while you aim.
  - Build editor: a Reset button refunds every spent point in one click.
  - Surfaces are themeable (surf* keys) and sprite-pack-able (surfaces.* keys).

UNDER THE HOOD
  - Reactions are fully deterministic - replays and verify-don't-host intact.
  - The AI avoids harmful surfaces and values burning / stunning foes.


0.0.1  —  first public build

  Ranked & custom play, correspondence PvP, replays, spectating, sprite packs,
  UI theming, and an adaptive minimax AI.)";

class PatchNotesScreen {
public:
    enum class Result { None, Back };

    // One frame of input + drawing. Returns Back when the user leaves.
    Result runFrame(int screenW, int screenH);

private:
    int scroll_ = 0; // vertical scroll offset in pixels (0 = top)
};

} // namespace tb::render
