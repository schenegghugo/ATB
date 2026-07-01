# Architecture

This document is the design contract for Tactical Battler. It explains how the
pieces fit, why the boundaries are where they are, and — most importantly for an
open-source sandbox — **what you can safely change and where the trust lines
sit**. If you're here to add a spell, draw your own sprites, write an AI, bolt on
your own renderer, or think about networked play, start here.

The guiding idea: **the game is a self-contained, headless combat engine.**
Graphics, sound, input, networking, and persistence are all things you *attach*
to it through a narrow API — never things it depends on. That one decision is
what makes everything else (modding, a dedicated server, replays, automated
balancing) cheap instead of a rewrite.

---

## 1. Layers and the dependency rule

```
render/      Raylib frontend — windows, sprites, input.  (swappable / optional)
   │  depends on
   ▼
data/        persistence seam — BuildRepository, catalog loading, schema.sql
   │  depends on
   ▼
core/        the engine — Grid, Battle, Spells, Build, AI.   NO graphics, NO DB.
```

**The rule, in one line: dependencies only ever point downward.** `core/` knows
nothing about Raylib, sockets, files, or JSON. `data/` knows about persistence
but not about windows. `render/` is the only layer allowed to touch a GPU.

This is enforced by the build (`CMakeLists.txt`): `core/` + `data/` compile into
a static library, **`tb_core`**, that links with *zero* graphics and *zero*
database dependencies. That same library runs:

- inside the graphical client (`tactical_battler`),
- inside the headless test/demo binaries (`tb_headless`, `tb_build_demo`, …),
- inside the balance simulator (`tb_balance`, ~1,260 full AI-vs-AI matches/sec),
- and — by design — inside a **dedicated match server** (see §7), unchanged.

Configuring with `-DTB_BUILD_GUI=OFF` builds exactly this headless slice (core +
demos) and skips Raylib entirely — no fetch, no GL/X11. **CI runs that build on
every PR** and exercises the demo/assertion suite, so a change that breaks the
headless boundary or a test can't be merged.

If you ever find yourself wanting to `#include` Raylib or a socket library from
`core/`, stop: the thing you want belongs in a higher layer, handed *into* the
core as data or pulled *out* of it through an accessor.

---

## 2. The engine core (`src/core/`)

| File | Responsibility |
|------|----------------|
| `Grid` | Tiles (`Walkable`/`Wall`/`Obstacle`), procedural gen with a BFS reachability gate, BFS pathfinding, Bresenham line-of-sight. |
| `Battle` | The combat state machine: roster of `Entity`, initiative order, turn lifecycle, the data-driven spell/effect pipeline, ground effects, forced movement, the closing-ring "storm". |
| `Spells` | The **catalog** (dictionary of skills): `SpellDef` = gameplay `Spell` + stable id + build-point cost. |
| `Build` | Classless point-buy `CharacterBuild`: a budget spent across catalog skills and stat upgrades, validated, then *hydrated* into a live `Entity`. |
| `AI` | A headless turn-level planner — beam-searches action sequences within the AP/MP budget, scoring end-of-turn states. |

> **Planned (MILESTONES "Core split"):** `Battle.h` is being split by concern —
> `core/Combat.h` (the spell/effect data model), `core/Entity.h` (`Entity` + its
> kind/control/snapshot), and `core/Battle.h` (the engine only). `Grid` stays the
> arena terrain; match *config* (the closing ring, economy, …) moves into the
> `Ruleset` (below), leaving `Battle` as pure engine state + logic.

### The roster invariant

`Battle` owns `std::vector<Entity> units_`, addressed by a stable
`EntityId` (a raw index). The vector is **append-only** — entities are never
erased — so an id is permanent and safe to hold across turns, in the AI, in the
renderer, and (eventually) over the wire. Summons and objects enter the roster
this way mid-battle; should the vector ever need slot reuse, the typedef can hide
a free-list without churning the public API.

### Why the core is deterministic

Given the same initial `Battle` and the same sequence of actions, the outcome is
**identical, every time** — there is no hidden global state and no wall-clock or
RNG reads buried in resolution. (Procedural arena generation takes an explicit
seed.) Determinism is not an accident; it is the property that makes three later
features nearly free:

- **Server validation** — the server re-runs the same resolution the client
  previewed and gets the same answer (§7).
- **Replays** — store `seed + intents`, re-simulate to play back. No frame
  recording (§8).
- **Balance simulation** — reproducible per-seed reports (`tb_balance`).

Treat determinism as a hard invariant. Anything that introduces
non-reproducibility into `core/` resolution is a bug.

---

## 3. The content model: spells are *data*, not code paths

A spell is **pure data**, not a function or a subclass:

```cpp
struct Spell {
    std::string name;
    int apCost, minRange, maxRange;   // Manhattan range
    bool needsLineOfSight;
    TargetShape shape;                // Single | Line | Cross | Circle
    int radius, cooldown;
    std::vector<Effect> effects;      // the payload
};

struct Effect {
    enum class Type { Damage, Heal, Push, Pull, ApplyStatus, Spawn };
    Type type;
    int amount;
    StatusEffect status;   // when ApplyStatus  (DoT / Shield / ApBuff / MpBuff / Invisible)
    GroundSpec ground;     // when Spawn         (Wall / Glyph / Portal)
};
```

A new spell is a new *combination of existing effects* — no state-machine
changes, no new `if` branches in `Battle`. This is what "data-driven content"
means here, and it carries a property that matters enormously for safety:

> **Content can only express what the engine's `Effect` vocabulary allows.**

A spell definition — whether written in C++ or loaded from a file — cannot
invent a new mechanic, run arbitrary code, or escape the sandbox. The *worst* a
hand-authored definition can do is pick absurd numbers (999 damage, 0 AP). That
matters only if such a definition reaches a competitive match — and §5 explains
why it never does. The effect vocabulary is the security boundary.

### Classless, point-buy builds

There are no classes. A `CharacterBuild` is a budget spent across catalog spells
(each has a `buildCost`) and stat upgrades. A glass cannon and a bruiser are
just different spends of the same points. Builds are validated against the
budget, then hydrated into an `Entity`. The gameplay core never sees a "class".

---

## 4. The catalog seam (current state → target state)

The **catalog** is the dictionary of all spells the game knows about.

**Status: shipped.** The engine loads the catalog from a **versioned JSON file**
(`data/catalog.json`) via `data/CatalogJson`. **That file is the source of truth**
— hand-editable, the thing balance/content is tuned in. `makeDefaultCatalog()`
remains in C++ as the *absent-file fallback* and a *scaffold* (`tb_catalog_gen`)
to bootstrap a fresh file; it may diverge from the committed file, and that's
expected. Builds reference spells by stable id.

```
   data/catalog.json            ← the canonical content (versioned, hashable)
        │  loaded + schema-validated by data/
        ▼
   SpellCatalog (in memory)     ← unchanged API:  find(id), findByKey(key), all()
        │  referenced by id
        ▼
   CharacterBuild → hydrate → Entity → Battle      ← core/ is untouched
```

The loader carries:

1. **A version + content hash.** The file declares a `version`; the loader
   computes a `sha256` of the exact bytes — the trust anchor for the handshake
   (§5/§7).
2. **Strict schema validation.** A malformed or unknown-field file is rejected
   *loudly* at load (all errors, with context), never silently coerced.
3. **CI is a validity gate, not a drift gate.** Because the data file is
   canonical, CI runs `tb_catalog_gen --check data/catalog.json` (it must load +
   validate) rather than diffing it against the compiled seed. The same applies
   to `creatures.json` and `rules.json` (CI runs `--check` on all three).

Crucially, **`core/` did not change** for any of this. `SpellCatalog` keeps its
current interface; only *where the entries come from* moves from a compiled
function to a validated file. Compiled content remains available as the fallback
and as the generator of the official file.

### Why both compiled and data-driven content coexist

They serve different contribution paths, and we keep both:

- **Compiled + PR review** — the trusted path for *official* content. A spell
  that ships in ranked should be reviewed anyway; a data-file PR is a tiny,
  readable diff that makes that review *easier*, not harder.
- **Data files** — the low-friction path for local experiments, custom lobbies,
  and non-programmers. Edit JSON, no recompile.

---

## 5. Trust model and modding tiers

The question every moddable competitive game must answer: *if players can author
content, how do you stop cheating?* The answer separates two things that feel
coupled but aren't:

- **Format** — how content is *described* (compiled vs JSON). Irrelevant to
  cheating.
- **Authority** — *whose copy* of the content is canonical for a given match,
  and *who computes the outcome*. This is the only axis that matters.

This applies to **content** (spells, builds — things that change *outcomes*) and
to the **ruleset** (the match format — team size, banned spells, closing-ring,
arena, economy). **Presentation** (sprites, sound, palette — things that change
only what *your* screen shows) is a different category entirely and is trust-free;
see §6.

There are **three pinned, hashable artifacts**, all loaded by the same JSON +
validation machinery: `catalog.json` (spells), `creatures.json` (the bestiary),
and `rules.json` (the ruleset). A match is defined by *which versions* of these
the server adjudicates with.

Content + ruleset modding is therefore a **privilege tier**, not a global
free-for-all:

| Tier | Catalog / creatures / ruleset used | Authority | Mods? |
|------|------------------------------------|-----------|-------|
| **Local / sandbox** | anything on your disk | your machine | anything goes — it's yours |
| **Custom lobby** | a set pinned by hash; the host uploads it, the server validates + caches it, joiners pull those exact files | dedicated server | consensual: everyone in the lobby ran the same hashed content + ruleset |
| **Ranked** | the server's reviewed **official** catalog + creatures + ruleset (fixed format) | dedicated server | none — official only |

Your local files drive only **what your client draws and previews**. In a server
match they have *no bearing on the outcome*, because the server adjudicates with
its own catalog + creatures + ruleset (§7). A hacked local file in ranked does
nothing. You curate the **official ranked** set; you never have to curate the
modding universe — custom lobbies pin their own by hash. **Competitive forces a
format; custom allows an agreed `rules.json`.**

---

## 6. Presentation: sprite & asset packs

Drawing your own sprites is the *easiest* kind of modding to make safe, because
of a clean asymmetry with content:

> **Content changes outcomes → needs server authority. Presentation changes only
> what your client renders → needs no authority at all.**

A sprite pack can *never* be a competitive exploit: the server doesn't know it
exists, it never touches resolution, and it can't reveal information your client
doesn't already legitimately hold (the authoritative server sends the same state
regardless of how you choose to paint it). So presentation packs are **100%
client-side, recompile-free, and validation-free** — drop a folder in, restart,
done. No tier system, no hashing.

### Where it lives

Entirely in `render/`. `tb_core` stays graphics-free (§1). The engine's only job
is to expose **stable semantic keys** the renderer can map to art — it already
does this for almost everything.

### How the renderer works today (the baseline)

`Renderer.cpp` is **pure geometric primitives with a hardcoded palette**: tiles
are colored rectangles keyed off `TileType`, units are circles colored by
`Faction`, ground effects are shapes keyed off `GroundKind`, statuses are dots.
There are *no* textures and *no* asset loading yet. Every draw is driven by
read-only `Battle` state plus the per-frame `ViewState` — the frontend already
knows nothing the engine didn't hand it.

This primitive renderer becomes the **built-in default pack / fallback** (see
below). It is never removed: it guarantees the game is *always* renderable, even
with no pack or a half-finished one.

### The semantic keys a pack targets

The renderer keys art off identifiers the core already exposes as stable enums
and slugs — no engine change needed for most of it:

| Visual element | Stable key (exists today) |
|----------------|---------------------------|
| Floor / wall / obstacle tile | `TileType` |
| Player / enemy unit tint | `Faction` |
| Shelter wall / Glyph / Portal | `GroundKind` |
| Status markers (DoT, Shield, buffs, Invisible) | `StatusEffect::Kind` |
| Spell icons / cast VFX | `Spell::name` / the catalog `key` slug (`"fireball"`) |

The **one small, optional, cosmetic addition** for per-creature art: an
`appearanceKey` string on a build/entity (e.g. `"pyromancer"`), defaulting
gracefully so a pack that doesn't supply it just falls back to the `Faction`
tint. It is purely cosmetic — it never affects resolution, so it carries no
trust weight even though it can be authored in data.

### Pack format (atlas-based)

Raylib draws sub-rectangles of a *bound* texture, so the canonical format is a
**texture atlas** — one image holding many sprites — not hundreds of singular
files (which would force a GPU texture bind per sprite, the slow path). One atlas
= one bound `Texture2D`, so sprites batch into far fewer draw calls, animations
are just sub-rects advancing on the same texture, and there's one decode/upload
instead of hundreds of file opens (which also shrinks WASM web builds — fewer
files to preload).

A pack is a directory with a manifest, one or more atlas images, and an optional
palette:

```
mypack/
  pack.json        ← manifest: meta + atlas list + key → sprite map + palette
  atlas.png        ← the packed sprite sheet (a pack may have several pages)
```

`pack.json` (parsed by the shared `data/Json` layer, §‑Phase‑1) maps each
**semantic key** to a sprite — which atlas page, the sub-rectangle, an anchor,
and optionally animation clips:

```jsonc
{
  "schema": 1,
  "name": "Pixel Dungeon", "version": "1.0.0",
  "tileSize": 32,
  "atlases": { "main": "atlas.png" },
  "palette": { "wall": "#464e60" },          // optional: re-theme primitives
  "sprites": {
    "tiles.wall":       { "atlas": "main", "rect": [0, 0, 32, 32] },
    "units.pyromancer": { "atlas": "main", "rect": [0, 64, 32, 48], "anchor": "bottom",
                          "anim": { "rects": [[0,64,32,48],[32,64,32,48]], "fps": 4, "loop": true } },
    "spells.fireball":  { "atlas": "main", "rect": [0, 96, 32, 32],
                          "cast": { "rects": [[0,96,32,32],[32,96,32,32],[64,96,32,32]],
                                    "fps": 12, "loop": false } }
  }
}
```

- `rect` = `[x, y, w, h]` into the named atlas (the static / first frame).
- `anchor` = `center` (default) | `bottom` (stand a tall sprite on its tile).
- `anim` / event clips (`cast`, later `hit`/`death`) — see *Animations* below.
- A pack may ship **only a `palette`** and no sprites — a pure re-theme, no art.

Atlases can come from any packer (TexturePacker, free-tex-packer) or be hand-
drawn; only the rect coordinates matter. Loose per-file PNGs stay a possible
*authoring* convenience (the engine can pack a folder into a runtime atlas), but
the canonical on-disk and in-VRAM form is the atlas.

### Animations

A **clip** is an ordered list of atlas sub-rects plus `fps` and `loop`. Two
sources drive them:

- **Ambient** (`anim`) — loops while a unit simply exists (idle / flap).
- **Event** (`cast`, later `hit` / `death` / `move`) — played once when the
  engine reports that event, then the sprite returns to its ambient state. The
  engine surfaces these as state deltas the renderer already sees (a cast, a
  damage application); the renderer maps delta → clip.

Because clips are just rects on the already-bound atlas, animation costs no extra
texture loads or binds. Static-only packs are fully valid — a still sprite is
just a one-frame clip.

### Resolution + fallback (packs are incremental)

The renderer resolves each key through the active pack:

```
draw(key):  pack binds a sprite (atlas rect / clip) for key?  → blit from the atlas
            else pack has a palette color for key?            → fill the primitive
            else                                              → built-in primitive
```

Because every key falls back independently, a pack can restyle **just the
walls** and leave everything else default. There is no "all or nothing" — packs
are partial by design, which makes them approachable for first-time artists.

### Loading & selection

A client setting points at a pack directory; it loads at startup, with optional
**hot-reload** for fast iteration (re-scan the folder, re-upload textures — no
restart, no recompile). Multiple installed packs can be switched in a settings
menu. None of this involves the server.

### Spell icons & the clickable spell bar

Spell icons are the first place a pack's art becomes *interactive*, not just
decorative. The battle screen gets a **clickable spell bar**: a HUD row of the
active unit's loadout, where clicking a button selects that spell — so the player
isn't forced to memorise the `1`–`9` hotkeys. The bar is *additive*: the hotkey
digit stays drawn on each button, so the keyboard path still works.

Each slot's visual state is fully derivable from the `Entity` the renderer
already receives (`spells[i]`, `spellCooldowns[i]`, current `ap`):

- **selected** — highlighted border (the spell right-click will cast)
- **affordable** — enough AP *and* off cooldown
- **unaffordable** — dimmed
- **on cooldown** — greyed with the remaining-turn count overlaid
- hovering a button shows the spell's tooltip (name, AP, range, cooldown).

Casting is unchanged — right-click the target tile. The bar only replaces the
*selection* gesture, which is the smallest change that removes the
memorise-the-numbers burden.

**Icon resolution** follows the same fallback ladder as everything else, so the
bar is usable with zero custom art. The icon is just the spell's atlas sprite
(the `rect` of its `spells.<key>` entry):

```
spells.<key> sprite in the active pack  →  built-in default icon  →  procedural
badge (the hotkey digit + a short name on a faction-tinted chip)
```

The same atlas sprite is reused in the **editor's skill dictionary** — one
frame, both screens.

**Resolving the icon key without touching `core/`.** `Entity.spells[i]` is a
`Spell` (combat data only) — it deliberately carries no catalog slug or id. The
frontend already holds the `CharacterBuild` and the `SpellCatalog`, and loadout
order equals `build.spellIds` order by construction in `instantiate()`, so the
icon key is just `slot → build.spellIds[slot] → catalog.find(id)->key`. No core
change is needed. (Only if spells ever need to render detached from their build —
summons, networked rosters — would a stable `key`/`id` get promoted onto `Spell`.)

**Geometry is single-sourced.** The bar's per-slot rectangles are computed by a
`Layout` helper used by *both* the draw pass and the click hit-test, so they
can't drift. `main.cpp` hit-tests those rects **before** the grid move/cast
logic, so a click on the HUD bar selects a spell instead of being misread as a
board move. `ViewState` gains `selectedSpell` plus a parallel `spellIconKeys`
(the slugs the frontend resolved); the renderer reads cost/cooldown/AP straight
off the active `Entity`.

### v1 scope vs later

- **v1**: atlas-based packs (static `rect` per key) + palette, manifest-driven,
  fallback to primitives; clickable spell bar with procedural-badge fallback for
  icons. One atlas page, no animation required.
- **Later**: ambient `anim` + event clips (`cast`/`hit`/`death`), multi-page
  atlases, runtime packing of loose PNGs, particles, and sound packs (the same
  key→asset manifest idea, mapping events like "fireball cast" → a sound). The
  manifest already reserves `anim`/`cast`, so adding these is additive.

---

## 7. Server-authoritative PvP

The headless deterministic core *is* the server-side match resolver. Networked
play adds a transport and a lobby on top — it does not change `core/`.

**The one rule that makes it secure: the client sends _intents_, never
_outcomes_.**

```
client A ─┐                              dedicated server (runs tb_core)
          ├─ handshake: agree catalog + creatures + ruleset (sha256 match or reject)
client B ─┘
          │  each submits a build (spell ids + stat spends)
          ▼  server validates vs catalog + ruleset (budget, bans) ──► reject if illegal
   server builds the Battle from the ruleset (buildMatch) in RAM
          │
   turn loop:
     active client sends  INTENT:  "cast spell #3 at (5,7)"  /  "move to (8,4)"
     server:  canCast()? in range? enough AP?  →  cast()  →  broadcast state delta
     clients RENDER the delta; they never compute damage themselves
          │
   match ends → write a small result record → free the Battle
```

The client may *preview* a cast locally (it has the same engine and catalog) for
responsiveness, but the server's resolution is the only one that counts. Because
the core is deterministic, the preview and the authoritative result agree when
the client is honest — and diverge harmlessly (client-side only) when it isn't.

Transport can be **WebSocket or plain TCP**: the game is turn-based and
latency-tolerant, so no rollback/prediction netcode is required.

### What changes — and what doesn't

The headline is how *little* of the existing code moves. The `Battle` verbs
already **are** the intent vocabulary, and they already validate and refuse
illegal calls — networking wraps them, it doesn't rewrite them.

| Stays exactly as-is | Why it already fits |
|---------------------|---------------------|
| `core/` combat resolution | `moveToward` / `cast` / `endTurn` are the intents; `canCast` is the legality check; movement "never mutates on failure". |
| `validateBuild()` | Already pure and UI-safe — the server calls the *same* function to admit a build. |
| `instantiate()` | Builds a roster `Entity` identically on client and server. |
| `serializeBuild` / `deserializeBuild` | Already a text round-trip — reuse it as the build wire payload. |
| Determinism | Makes the client's local *preview* match the server's authoritative result when honest. |
| Sprite/asset packs (§6) | Client-only; the server never knows they exist. |

| New work | Layer |
|----------|-------|
| Intent + state-snapshot wire format | `data/` (or a new `net/`) |
| Authoritative match runner | server (wraps `tb_core`) |
| Transport + session | `net/` |
| Lobby / matchmaking | server service |
| Identity / accounts / ratings | server + SQLite (the `schema.sql` seam) |
| Client `MatchSource` seam | `render/` + `main.cpp` |

### New components

1. **Serialization (`net/` or `data/`).** Three small formats: an **intent**
   (`move{dest}` / `cast{spellIdx,target}` / `endTurn`), a **state snapshot** the
   server broadcasts after applying one (a whole `Battle` is a few KB, so a
   near-full snapshot per turn is cheap — deltas are an optimisation, not a
   requirement), and the **build payload** (reuse `serializeBuild`). Each is
   round-trippable and unit-testable headless, exactly like the existing build
   round-trip test.
2. **Authoritative match runner (server).** Owns one `Battle`. For each inbound
   intent it checks *(a)* the sender controls the active unit, *(b)* the intent is
   legal (`canCast` / range / AP / LOS — `Battle` already enforces this and
   refuses cleanly), then applies it and broadcasts the new snapshot. Adds a
   **turn clock** (forfeit on timeout) and disconnect handling. This is
   `tb_core` + a thin loop — no new combat logic.
3. **Transport + session (`net/`).** A WebSocket/TCP listener, message framing,
   heartbeats, and a connection→player mapping. Deliberately dumb: it moves bytes,
   it does not understand the game.
4. **Lobby / matchmaking.** *Ranked*: queue, pair players for the ruleset's
   `teamSize`, pin the official catalog + creatures + **ruleset** (versions +
   hashes), `validateBuild()` both vs the ruleset (budget + bans), spawn a runner.
   *Custom*: a host creates a lobby, uploads a catalog/creatures/`rules.json`
   (server schema-validates + hashes + caches for the lobby's lifetime), shares a
   join code, joiners pull those exact hashed files; the **ruleset** is the agreed
   format (no ad-hoc per-lobby toggles — it's all in `rules.json`).
5. **Identity / persistence.** Lightweight accounts and a results/ratings store in
   SQLite — the `BuildRepository` + `schema.sql` seam already points here; this
   extends it with `matches` and `ratings`.
6. **Client `MatchSource` seam (`render/` + `main.cpp`).** Today `main.cpp` drives
   the `Battle` in-process. Abstract that behind a small interface with two
   implementations: **local** (drives the `Battle` directly — current behaviour)
   and **remote** (sends intents, applies server snapshots to a local mirror
   `Battle` for rendering + optimistic preview). The render code is shared
   verbatim — only the *source of truth* swaps. Plus lobby/queue UI screens.

### Trust checkpoints (where each check lives)

1. **Handshake** — reject a client whose catalog / creatures / **ruleset**
   versions + `sha256`s don't match the match's pinned set (§4, §5).
2. **Build admission** — `validateBuild(build, catalog, ruleset)` at match start;
   reject on any error, budget overrun, or **banned spell**. Same function the
   editor runs live.
3. **Per intent** — active-unit ownership *and* `Battle` legality, server-side,
   *before* mutating. The client's local preview is advisory only.
4. **Never trust outcomes** — the wire carries intents, never
   client-computed damage/results. This is the invariant the whole model rests on.

### Suggested build order

Networking depends on the content seam landing first — the **catalog loader
(roadmap #1)** and **content hash (#2)** are what let client and server agree on
canonical content. Then:

1. **Serialization + headless round-trip tests** — intents and snapshots, proven
   deterministic offline (no sockets yet).
2. **`MatchSource` refactor (local only)** — pure refactor; the game plays
   identically, verifying the seam before any network exists.
3. **In-process loopback "server"** — run the intent/snapshot loop through the
   runner with no real socket, to prove end-to-end determinism.
4. **Real transport + 1v1 custom match** — direct connect / join code.
5. **Lobby, matchmaking, accounts, ranked MMR.**
6. **Reconnect + spectate** (free from snapshots) and **replays** (free from
   `seed + intents`, roadmap #7).

---

## 8. What the server stores (it is small)

A common misconception is that a match server must "host every possible match."
It does not. A match is a *computation*, not a stored document. The server holds
exactly three things:

1. **Catalogs** — a handful of small JSON files (the official versions). The
   11-spell catalog is a few KB; even hundreds of spells is ~100 KB. Custom
   lobbies pin their own by hash for the lobby's lifetime.
2. **Live matches** — `Battle` objects in **RAM, not disk**: a 20×15 grid plus a
   few entities, a few KB each, **freed the instant the match ends**. Matches are
   constructed on demand when players queue, never enumerated or pre-generated.
3. **Results / replays (optional)** — a result row (winner, length, builds) is a
   few hundred bytes in a local SQLite file. A **replay** is just `seed +
   intents` (a few KB), re-simulated for playback thanks to determinism — no
   frame recording.

Note that **sprite/asset packs never touch the server** (§6) — they're a
client-only concern and add nothing to this list.

### Self-hosting capacity

A single small box (e.g. an HP EliteDesk Mini G3, ~quad-core, 8–16 GB) is far
more than enough for a hobby launch. The proof is already in the repo:
`tb_balance` resolves **~1,260 full AI-vs-AI matches per second on one thread**.
A *live* match is far cheaper — it's gated by human think-time (seconds/turn),
not CPU; resolving one human turn is microseconds. The practical limits are RAM
for connection/lobby state (KB each → thousands of concurrent matches) and
bandwidth (intents + deltas ≈ hundreds of bytes every few seconds). You will run
out of *players* long before the *simulation* breaks a sweat.

**Recommended topology for one dedicated box:** authoritative dedicated server
for all matches. Best integrity, simplest trust model, ample headroom. Avoid
peer/listen-server authority for ranked (the host could cheat); reserve it, if
ever, for casual customs.

---

## 9. Extension points — what you came to hack

| You want to… | Where | Status |
|--------------|-------|--------|
| **Restyle the game with your own sprites/palette** | drop a pack folder + `pack.json` in; no code, no recompile, no server involvement (§6). | seam planned; primitive fallback works now |
| **Add / change a spell** | the catalog: edit `data/catalog.json` (or `makeDefaultCatalog()`, the compiled fallback). Combine existing `Effect`s. | works now (data + compiled) |
| **Add a new _mechanic_** (something no `Effect` can express) | extend the `Effect`/`StatusEffect`/`GroundKind` vocabulary in `core/`, then resolve it in `Battle`. This is an *engine* change, reviewed accordingly. | core change |
| **Write your own AI** | `AI.cpp` today. A pluggable `Brain` strategy interface (so alternatives drop in without forking) is planned. | compiled today; interface planned |
| **Write a whole new frontend** (different engine, web, TUI…) | implement against the `Battle` read API (`grid()`, `units()`, `affectedTiles()`, …) and drive it with intents. `render/` + `main.cpp` are the reference frontend. | API stable |
| **Change the match format** (team size, banned spells, closing-ring, arena, economy) | edit `data/rules.json` — read by both the game and the balance sim via a shared `buildMatch()`. | works now |
| **Swap persistence** | implement `BuildRepository` (in-memory and flat-file impls ship; `schema.sql` targets SQLite/Postgres). | seam in place |
| **Tune balance** | `tb_balance [matches] [seed] [outfile]` — per-spell Monte-Carlo report (text + HTML charts + CSV); reads `data/catalog.json` + `data/rules.json`. `tb_team_balance` is the NvN team-composition sibling. | works now |

The read-only `Battle` API (`grid()`, `units()`, `unitAt()`, `affectedTiles()`,
`unitsAt()`, `clearLineOfSight()`, storm/ring accessors) is deliberately rich so
a frontend can render everything without the engine knowing the frontend exists.

---

## 10. Roadmap

Ordered roughly by how much each unlocks the sandbox vision.

1. **Catalog loader** — `data/catalog.json` + schema validation; `makeDefaultCatalog()`
   becomes its generator. Engine `core/` unchanged. *(Foundational for content modding.)*
2. **Catalog versioning + content hash** — the trust anchor for §5/§7.
3. **Sprite/asset pack seam** — route the renderer's draws through a pack
   manifest with primitive fallback (§6); add the optional cosmetic
   `appearanceKey`. *(Foundational for presentation modding — independent of the
   content track, and safe to ship first since it touches no authority.)*
4. **Clickable spell bar + spell icons** — a HUD loadout row that selects spells
   by click (not just `1`–`9`), with the pack-icon → default → procedural-badge
   fallback (§6). Pairs with the asset seam; touches only `render/` + `main.cpp`.
5. **Pluggable AI** — a `Brain` strategy interface so community AIs drop in
   without forking `AI.cpp`.
6. **Networked PvP** — transport (WebSocket/TCP), lobby/matchmaking, the
   intents-not-outcomes loop of §7, on a self-hosted authoritative server.
   *(Full component + trust + build-order breakdown in §7.)*
7. **Replays** — persist `seed + intents`; a viewer that re-simulates.
8. **Content/balance backlog** — Portal is AI-unused (its step-on-entry mechanic
   needs deeper planning than the beam search reaches); Fireball is the weakest
   attack (radius-buff candidate). Good first issues.

*(Items 1–2 — catalog loader + content hash — are done; see MILESTONES Phase 1.
Creatures.json followed the same pattern.)*

**Match rulesets (shipped).** `data/rules.json` — the **third pinned artifact**
beside catalog + creatures — datafies the match format (team size, banned spells,
closing-ring, arena, economy) and is read by **both** the game and the balance sim
via a shared `buildMatch()`, so they construct matches identically. Full breakdown
in `MILESTONES.md` ("Core split" + "Match rulesets").

**Parallel track — Web/WASM build.** Independent of the sequence above and
available now that the GUI exists: because `core/` is portable C++20 and Raylib
supports Emscripten/HTML5, the game can compile to **WebAssembly** for a
browser-playable build shareable on itch.io. Frontend + build-system work only —
`core/` is untouched (the main loop moves to an `emscripten_set_main_loop`
callback). See `MILESTONES.md` and `CONTRIBUTING.md` for the breakdown.

Anything in this list is additive: none of it requires changing the engine's
combat resolution, because the seams described above already anticipate it.
