#pragma once
//
// PatchNotesScreen.h — the "what's new" screen, reachable from the main menu and
// the in-game pause menu. Immediate-mode + stateless bar its scroll offset;
// main.cpp routes the Back result. The notes text + the release codename live
// here as constants so the menu footer and this screen agree.
//
namespace tb::render {

// The human-facing codename for the current release (shown beside the version).
inline constexpr const char* kVersionCodename = "Fine Print";

// Player-facing changelog, newest release first. Plain text, one item per line
// (kept under ~72 cols so it fits without wrapping).
inline constexpr const char* kPatchNotes =
    R"(0.2.1  -  "Fine Print"

Every text box is now a real text field, cleaner on-screen text, and a
Shelter wall that holds its aim.

TEXT FIELDS (select, copy, paste - the works)
  - Click to place the cursor, drag or Shift+arrows to select a run, and
    double-click to select everything. Copy / cut / paste with the usual
    Ctrl+C / Ctrl+X / Ctrl+V.
  - Arrow keys, Home / End and Ctrl+arrow word-jumps move the cursor;
    Delete and Backspace remove the selection or a character (or a whole
    word with Ctrl).
  - Works everywhere you type: login, lobby, chat and build names.

FIXES
  - Shelter wall aiming holds its facing now. The first mouse-wheel notch
    LOCKS the wall's direction; each notch after turns it 90 degrees.
    Moving the mouse only slides the wall into place - it no longer spins
    as you reposition, and the preview matches where the wall really lands.
  - On-screen text no longer shows stray "?" marks: dashes, ellipses and
    dot separators that leaned on non-ASCII now render properly.


0.2.0  -  "The Draft"

Real 2v2 / 3v3: team up with a partner and draft your squad face to face.

PARTY UP
  - Invite a friend to your party from the Online Home (2v2 / 3v3 rows),
    watch your roster fill, then seek or challenge AS A TEAM. Two humans
    a side for 2v2, three for 3v3 - each pilots their own champion.

THE DRAFT (scout, then counter-build)
  - Team games now open with a turn-by-turn DRAFT, not a blind ready
    check: players lock their champion one at a time, in snake order.
  - Every locked build is REVEALED to everyone, so a later pick can
    scout the enemy and counter-build on the spot.
  - The per-pick countdown is ALWAYS on screen while you author - no
    more guessing how long you have. Run it out and a basic build is
    locked for you, so the draft never stalls.

FULL TEAM CONTROL
  - Completes the 0.1.0 preview: a team match routes each champion's
    turn to the player who controls it. On a teammate's turn you watch
    and plan; on yours, you act.

NOTE
  - Online 2v2 / 3v3 needs this version on BOTH sides - the team draft
    and match protocol are new. 1v1 play is unchanged.


0.1.1  -  "Build Books"

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


0.1.0  -  "Team Play"

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


0.0.2  -  "Elemental Surfaces"

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


0.0.1  -  first public build

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
