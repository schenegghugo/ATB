#pragma once
//
// PatchNotesScreen.h — the "what's new" screen, reachable from the main menu and
// the in-game pause menu. Immediate-mode + stateless bar its scroll offset;
// main.cpp routes the Back result. The notes text + the release codename live
// here as constants so the menu footer and this screen agree.
//
namespace tb::render {

// The human-facing codename for the current release (shown beside the version).
inline constexpr const char* kVersionCodename = "Build Books";

// Player-facing changelog, newest release first. Plain text, one item per line
// (kept under ~72 cols so it fits without wrapping).
inline constexpr const char* kPatchNotes =
    R"(0.1.1  —  "Build Books"

Share your builds, read every spell at a glance, and smoother casting.

BUILD BOOKS (save and share your builds)
  - Your saved builds now persist between sessions in a local build book,
    so the roster you author is still there next launch.
  - Load any saved build straight into a slot from the editor (Load button).
  - Copy code / Paste code: export a build to your clipboard and hand the
    code to a friend - they paste it right into their editor. Trade whole
    "build books" with zero fuss.

CLEARER SPELLS (the editor tells you what a spell does)
  - Every spell popup now shows its DAMAGE up front, plus a one-line, plain-
    language description of how it actually plays.
  - Surface spells finally read properly - Storm, for one, now explains its
    soak-then-lightning combo instead of looking like it does nothing.

SMOOTHER CASTING
  - Casting a spell now automatically deselects it and drops you back to move
    mode - no more accidental re-casts or a stray right-click to cancel.

FIXES
  - Bombs ride portals now: a bomb summoned onto - or shoved onto - a portal
    entry is teleported to the exit, just like a unit walking in. Lob or
    knock your bombs through a portal onto the enemy backline.


0.1.0  —  "Team Play"

Reworked controls, damage you can read at a glance, a terrain fix, and the
first online groundwork for 2v2 / 3v3.

CONTROLS (reworked - one button does it all now)
  - With NO spell selected, left-click MOVES your unit.
  - Pick a spell (click it, or press 1-9) to light up its range in green,
    then left-click a green tile to CAST.
  - Right-click, Esc, or click the spell again to go back to moving.
  - No more separate move-vs-cast buttons to remember.

COMBAT FEEDBACK
  - Damage, healing and shields now float up over the board as numbers, so
    you can read a fight without watching the combat log.

FIXES
  - Terrain no longer decays early: bombs and summons were secretly shortening
    your own portals, glyphs and walls by crowding the turn order. Ground
    effects now last their full duration no matter how many units are out.

ONLINE - TEAM PLAY (preview)
  - The lobby now has a 1v1 / 2v2 / 3v3 selector, and 2v2 / 3v3 seeks match
    each other. Full two-players-per-team control is still in the works - for
    now a team match pairs two players with one steering each side.

NOTE
  - Combat timing changed this release, so replays and correspondence games
    from 0.0.2 won't verify against 0.1.0 - hence the version jump.


0.0.2  —  "Elemental Surfaces"

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
