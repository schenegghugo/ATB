# Elemental surfaces (milestone E — shipped in 0.0.2)

Status: **implemented (0.0.2).** This began as the E.0 design lock and now
documents what shipped; where the build simplified the design, a note calls it
out (search "shipped:"). It covers the roster, the reaction matrix, the
determinism rules, and the content vocabulary. Everything here survives one hard
constraint:

> **A match re-simulates identically from its notation** (verify-don't-host, see
> `ARCHITECTURE.md` §7). Surface reactions are therefore fully deterministic —
> fixed resolution order, no RNG, bounded chains.

The good news: we are *not* building a new subsystem. Surfaces are the existing
`GroundEffect` substrate (`Battle.cpp` — `tickGround()`, `onEnterTile()`,
`spawnGround()`) with one new axis (an element) and a reaction lookup. The
multi-turn spells reuse the existing `onDeath` + `fuse` machinery that already
powers Bomb.

---

## 1. The element roster

Six elements, exactly the set from the 0.0.2 brief, plus **Oil** as a strongly
recommended seventh (it is the classic enabler — without a flammable, half the
fire combos have nothing to bite on).

| Element    | Persists? | Traits                                    |
|------------|-----------|-------------------------------------------|
| `Fire`     | yes       | burns flammables; evaporates water; melts ice |
| `Water`    | yes       | conductive; douses fire; freezes to ice   |
| `Ice`      | yes       | slippery; melts to water under fire        |
| `Poison`   | yes       | DoT; **flammable** (ignites → blast)        |
| `Electric` | yes       | live floor — **stuns** on enter (skip next turn); painting it on Water/Ice electrifies those tiles |
| `Heal`     | yes       | benign pool; mends units standing in it    |
| `Oil` *(opt)* | yes    | flammable; slows movement; freezes poorly   |

One derived surface is a byproduct of reactions, not paintable directly:

| Surface  | From            | Behaviour                                  |
|----------|-----------------|--------------------------------------------|
| `Steam`  | Fire + Water    | **cloud** — blocks line of sight, no damage |

> **Cloud vs floor.** `Steam` (and conceptually a poison *cloud*) block LOS. We
> model that as a `blocksLos` flag on the `GroundEffect` and feed those tiles
> into `hasLineOfSight()` alongside walls — no separate cloud system.

### Calls resolved in E.0
- **Oil: in** — the flammable enabler stays.
- **Electric: a persistent surface** (a live floor that stuns on enter), not a
  momentary trigger — so there is no separate `charged` state.
- **Glyph retires its repel** — it becomes the pure neutral (`element = None`)
  surface; elemental spells paint onto *any* surface, Glyph's included.
- **Heal: a surface** (a stand-in-it pool that mends), inert offensively — it
  only ever gets destroyed (fire boils it away).

---

## 2. Surfaces = `GroundEffect` + an element

Concretely, E.1 adds:

```cpp
enum class Element : std::uint8_t { None, Fire, Water, Ice, Poison, Electric, Heal, Oil, Steam };

struct GroundEffect {          // existing, + these:
    // ...kind, owner, tiles, remainingTurns...
    Element element = Element::None;  // NEW — the surface's element
    bool    blocksLos = false;        // NEW — Steam / cloud
};
```

`GroundKind::Glyph` is repurposed as the **neutral surface** carrier (a surface
*is* a glyph with an element). `Wall` and `Portal` are untouched. Every enum row
goes in `SpellEnums.h` with the count `static_assert` bumped, and the `Snapshot`
ground block in `Net.cpp` gains `element` / `blocksLos` so spectators
and the verify hash see them.

---

## 3. Passive behaviour (E.2)

What a surface *does* on its own, split between the two existing hooks.

**On enter** (`onEnterTile`, fires when a unit steps onto the tile):

| Element    | On enter                                                    |
|------------|-------------------------------------------------------------|
| `Fire`     | apply `Burning` (DoT) — unless the unit is `Wet`             |
| `Poison`   | apply `Poisoned` (DoT)                                       |
| `Ice`      | apply `Wet` *(shipped: the slip-slide physics is deferred past 0.0.2)* |
| `Water`    | apply `Wet` (clears `Burning`)                              |
| `Heal`     | heal a flat amount (once per entry)                         |
| `Electric` | apply `Stunned` (skip next turn)                             |
| `Oil`      | apply `Oiled` + halve remaining MP this step                 |
| `Steam`    | apply `Wet`                                                  |

**On tick** (`tickGround`, once per turn at the owner-order boundary — surfaces
age here already):

| Element    | On tick (to units standing in it)                            |
|------------|-------------------------------------------------------------|
| `Fire`     | `Burning` DoT continues via the status; surface ages         |
| `Electric` | small zap to units standing in it (`Stun` applies on *enter* only, never re-applied per tick) |
| others     | age only                                                     |

New unit statuses this needs (added to `StatusEffect::Kind`, +rows in
`SpellEnums.h`): **`Wet`**, **`Burning`** *(or reuse `DamageOverTime` with a
flavour)*, **`Frozen`** (rooted — may act, can't move; from Ice), **`Stunned`**
(full turn skipped; from Electric), **`Oiled`**. Electrifying a `Frozen` unit
**upgrades** it to `Stunned`. `Frozen` + `Stunned` are the two genuinely new
mechanics (turn control); the rest are DoT/flag reuse.

---

## 4. The reaction matrix (E.3) — the centrepiece

When an **incoming** element lands on a tile that already holds a **surface**,
look the pair up here. `→ X` is the resulting surface; the parenthetical is the
**immediate burst** applied that instant.

Rows = existing surface, columns = incoming element.

| existing ↓ / incoming → | **Fire** | **Water** | **Ice** | **Electric** | **Poison** | **Oil** |
|---|---|---|---|---|---|---|
| **(none / bare)** | → Fire | → Water | → Ice | → **Electric** *(stuns on enter)* | → Poison | → Oil |
| **Water** | → **Steam** *(extinguish; blocks LOS)* | → Water (refresh) | → **Ice** *(freeze units → Frozen)* | → **Electric** *(the wet tiles become a live field; shock + Stun everyone on them NOW)* | → Poison *(contaminate)* | → Oil floats *(Oil over Water)* |
| **Ice** | → **Water** *(melt)* | → Ice | → Ice (refresh) | → **Electric** *(shock; **Frozen units → Stunned**)* | → Ice (inert) | → Oil-on-ice |
| **Fire** | → Fire (refresh) | → **(none)** *(douse; small Steam)* | → **Water** *(quench)* | → Fire *(shock only)* | → **Fire+** *(**explosion**: fire burst r1)* | → **Fire+** *(**explosion** r1)* |
| **Poison** | → **(none)** *(**explosion**: fire burst r1)* | → Poison *(spreads)* | → **Ice** *(frozen ooze)* | → Poison *(shock only)* | → Poison (refresh) | → Oil+Poison *(both flammable)* |
| **Oil** | → **(none)** *(**explosion**: fire burst r1)* | → Oil | → Ice *(oily ice)* | → Oil *(shock only)* | → Poison over Oil | → Oil (refresh) |
| **Heal** | → **(none)** *(boil away)* | → Heal | → Ice | → Heal *(shock only)* | → **(none)** *(spoiled)* | → Oil |
| **Steam** | → **(none)** *(dissipate)* | → Water *(condense)* | → Water | → Steam *(shock all in the cloud)* | → Steam | → Steam |

Notes that keep it sane:

- **Explosion** = a one-shot `Fire` damage burst in radius 1 that *consumes* the
  flammable surface (result `none`). *(Shipped: it does not leave a residual Fire.)*
- **Electric is a live floor.** Painting it on Water or Ice converts those tiles
  to an `Electric` surface; the shock + `Stun` burst hits everyone standing on
  them *that instant*, and the surface then stuns anyone who later steps in
  (on-enter) and lightly zaps stationary occupants (on-tick). *Shipped:
  conversion is tile-local (each painted tile), not a flood-fill of the whole
  connected pool — the shipped spells (Storm) already paint the entire wet zone,
  so the effect is the same in practice.*
- `Heal` is deliberately inert offensively — it only ever gets destroyed.

---

## 5. Determinism rules (non-negotiable)

These make the matrix safe under verify-don't-host. E.3's unit tests assert them.

1. **Zone order is fixed.** `affectedTiles()` already returns a stable,
   RNG-free vector (row-major for Circle/Cross; along the ray for Line/Cone).
   Reactions resolve tile-by-tile **in that order**.
2. **Surface lookup is first-match in spawn order.** `ground_` is append-only;
   when a tile has (pathologically) two surfaces, the earliest wins.
3. **One reaction per tile per cast.** A paint reacting on a tile may *change*
   that tile's surface but cannot re-trigger a second reaction on it in the same
   cast. This is the chain cap — it makes infinite ignite/melt loops impossible.
   (A genuinely staged combo, e.g. Storm, reacts again *next turn* as a fresh
   cast — bounded by turns, not recursion.)
4. **Bursts hit units by ascending `EntityId`.** Same rule the engine already
   uses in `unitsAt()`.
5. **No floating point, no RNG, no wall-clock.** Damage/heal are integers.

---

## 6. New content vocabulary (E.4)

Two additions to the pure-data `Effect` model in `Combat.h` (the security
boundary — content can still only express typed effects):

```cpp
enum class Effect::Type { ... , PaintSurface };   // NEW

struct GroundSpec { ... };        // reused; gains: Element element;
```

- **`PaintSurface`** — carries `Element element` + `duration`; spawns/overwrites
  a surface across the cast's zone, running §4 per tile. This is the workhorse:
  most elemental spells are `{ Damage, PaintSurface(element) }`.

**Multi-turn spells reuse `onDeath` + `fuse` — no new scheduler.** An `Entity`
already has `Spell onDeath` resolved at its tile when it dies, and `fuse` counts
down to trigger it (that's Bomb). Storm's delayed bolt is exactly that:

- **`TargetShape::Cone`** — new shape for Blizzard. It needs a *facing*, which is
  the mouse-wheel rotation we shipped for Shelter in 0.0.1 — `affectedTiles`
  already takes a `rotation`. Cone reuses it verbatim.

> Alternative considered: a dedicated `DelayedArea` effect + a pending-cast queue
> on `Battle`. Rejected for 0.0.2 — the `onDeath`/`fuse` object already handles
> the countdown *and* the careful copy-before-resolve recursion guard. Reuse beats
> a new subsystem.

---

## 7. Worked examples

### Storm — 4 AP, 3-turn CD, Circle
1. **Cast (turn N):** `PaintSurface(Water)` over the target Circle — the *rain*
   soaks the zone (units gain `Wet`). Same cast **summons a fused cloud object**
   at the centre: `fuse = 1`, `onDeath = { Circle r2, effects: [Damage,
   PaintSurface(Electric)] }`.
2. **Turn N+1:** the cloud's fuse hits 0 → it "dies" → `onDeath` fires the
   **lightning**: a radius-2 circle of damage that paints `Electric`. On the
   still-`Water` tiles this hits `Water + Electric` in §4 → the wet zone becomes a
   live **Electric** field, shocking **and Stunning** everyone standing in it; the
   dry tiles it catches turn to `Electric` floor too. Wet units take bonus shock.

Everything above is existing machinery: paint + summon + fuse + onDeath. The only
new pieces are the `Water`/`Electric` surfaces and the one matrix cell.

### Blizzard — 3 AP, 2-turn CD, Cone
1. **Cast:** `{ Damage, PaintSurface(Ice) }` over a `Cone` in the caster's facing
   (wheel-rotated). Ice on bare ground → `Ice`; on existing `Water` → freezes it
   (units caught → `Frozen` — rooted: may act, can't move); on `Fire` → quenches to `Water`.

---

## 8. Notation / snapshot / verify impact

- **Intent notation is unchanged** — surfaces are *derived* from casts, so a
  recorded cast list still replays to the identical board (given §5). No new
  token; the 0.0.1 replay format round-trips as-is.
- **Snapshot** gains the three `GroundEffect` fields (§2) so spectators and the
  final-state hash agree. That is a snapshot-schema touch, not an intent one.
- **Determinism fingerprint** (`tb_determinism_demo`) **will change** the first
  time the default catalog uses an elemental spell — re-lock the known-answer as
  the last step of E.8, never before the matrix is frozen.
- **Catalog `schema`** bumps once (new enum values + `PaintSurface`); adding
  string rows is otherwise backward-compatible (`SpellEnums.h` header note).

---

## 9. Balance & AI hooks (forward pointers)

- **AI (E.6):** `AI.cpp` already folds `groundEffects()` into its feature hash;
  add element + surface/status flags so the evaluator can (first pass) refuse to
  path into harmful surfaces and value soaking a `Wet` foe before a shock, and
  (later) actively set up reactions.
- **Balance (E.8):** every new spell gets a `buildCost` + cooldown and goes
  through `tb_balance`; ranked exposure is gated in `rules.ranked.json`.

---

## 10. Decisions (all resolved — as shipped in 0.0.2)

- Oil **in**; Glyph **retires its repel** (pure neutral surface); Electric is a
  **persistent surface**; **`Frozen`** = root (may act, can't move) and
  **`Stunned`** = full skip, with Electric **upgrading** `Frozen` → `Stunned`.
- Heal **is a surface** (a heal pool), inert offensively.
- Shipped placeholder numbers (tune via `tb_balance`): burn/poison DoT 5/4 per
  turn, Electric 6 on enter / 3 on tick, heal pool 8, explosion 15 (r1), Frozen 2
  turns, Stunned 1 turn, surface lifetimes 2–3 turns. Steam inherits the paint's
  duration (no separate knob).

The reaction damage numbers are estimates that passed the E.8 balance pass; treat
them as a starting point for future tuning, not sacred constants.
