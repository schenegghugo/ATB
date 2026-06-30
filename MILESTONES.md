# Milestones

The execution plan for ATB, ordered by **what unblocks the most** and **what
makes the repo safe and attractive to contributors** — not by feature appeal.
Design rationale for each piece lives in [`ARCHITECTURE.md`](ARCHITECTURE.md);
this file is the *sequence* and the *step-by-step* for getting there.

**Phase order:** CI + contributing → catalog loader (with hash) → spell bar +
sprite packs → pluggable AI → networked PvP → replays.

Off the critical path, a **Web/WASM build** (see *Parallel track* below) can be
picked up anytime now that the GUI exists — it's frontend-only and independent of
the content and PvP work.

**The one rule:** do not start any netcode before the catalog loader, content
hash, and serialization round-trip tests exist (Phase 1 + the first step of
Phase 4). PvP without those is built on sand.

Status legend: ☐ todo ◐ in progress ☑ done

---

## Phase 0 — Make the public repo contributor-safe

The highest-leverage work, and the easiest to skip. A public sandbox lives or
dies on whether a stranger can contribute without you hand-reviewing every line.
CI is what makes the "fork → edit a spell → open a PR" path *safe to merge*.

### 0.1 ☑ Add a `TB_BUILD_GUI` CMake option (prerequisite for fast CI)

CI should build and test the headless core **without** dragging in Raylib (which
FetchContent would clone + compile — slow and brittle on a runner). This also
gives you a clean server/headless build, which the architecture already calls
for. Make the graphical frontend opt-out.

In `CMakeLists.txt`, near the top (after `project(...)`):

```cmake
option(TB_BUILD_GUI "Build the Raylib graphical client" ON)
```

Then wrap the entire Raylib + `tactical_battler` section (from
`find_package(raylib ...)` down through the final
`target_link_libraries(tactical_battler ...)`) in:

```cmake
if(TB_BUILD_GUI)
    # find_package(raylib ...) / FetchContent / add_executable(tactical_battler ...)
    # ... existing block unchanged ...
endif()
```

**Acceptance:** `cmake -S . -B build -DTB_BUILD_GUI=OFF && cmake --build build -j`
builds `tb_core` + all five headless binaries and **never fetches Raylib**.
`-DTB_BUILD_GUI=ON` (the default) still builds the full game.

### 0.2 ☑ Confirm every test fails loudly

CI gates on exit codes. Current state:

- `tb_spells_demo`, `tb_ai_demo` — ☑ already `return g_fails == 0 ? 0 : 1`.
- `tb_headless`, `tb_build_demo` — smoke tests (catch crashes/linker breakage),
  exit 0 on success. Fine as-is; optionally add a sanity assertion + non-zero
  return if you want them to guard behaviour, not just "it ran".

**Acceptance:** introducing a deliberate regression makes at least one binary
exit non-zero.

### 0.3 ☑ Add the CI workflow

Create `.github/workflows/ci.yml`:

```yaml
name: CI

on:
  push:
    branches: [main]
  pull_request:

jobs:
  headless:
    name: Headless core + tests
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4

      - name: Install toolchain
        run: sudo apt-get update && sudo apt-get install -y cmake g++ ninja-build

      - name: Configure (no GUI, no Raylib)
        run: cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DTB_BUILD_GUI=OFF

      - name: Build core + headless binaries
        run: cmake --build build -j

      - name: Smoke + assertion tests
        run: |
          set -e
          ./build/tb_headless
          ./build/tb_build_demo
          ./build/tb_spells_demo
          ./build/tb_ai_demo

      - name: Balance sanity (fixed seed, fast)
        run: ./build/tb_balance 200 1
```

Notes:
- `-DTB_BUILD_GUI=OFF` keeps the run to seconds — no Raylib, no GL/X11.
- `set -e` makes the test step fail on the first non-zero exit.
- `tb_balance 200 1` is a deterministic smoke of the simulator (fixed seed),
  not a balance gate — it just proves the harness still runs end to end.

**Acceptance:** the workflow is green on `main` and on a PR; a PR that breaks a
test goes red.

### 0.4 ☑ (Optional) GUI compile job

Catches `render/` breakage. Slower (compiles Raylib), so keep it a separate job;
consider caching `build/_deps` later if it drags.

```yaml
  gui-build:
    name: GUI compiles (Linux)
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Install GUI deps
        run: |
          sudo apt-get update
          sudo apt-get install -y cmake g++ ninja-build \
            libgl1-mesa-dev libx11-dev libxrandr-dev libxinerama-dev \
            libxcursor-dev libxi-dev libwayland-dev libxkbcommon-dev \
            wayland-protocols
      - name: Configure + build full client
        run: |
          cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
          cmake --build build --target tactical_battler -j
```

(The GUI can't *run* on a headless runner — `IsWindowReady()` will fail — so this
job is compile-only by design. `main.cpp` already bails cleanly when there's no
display.)

### 0.5 ☑ Turn on branch protection

A branch ruleset (**Settings → Rules → Rulesets → New branch ruleset**) is
active on the default branch. Recorded configuration:

- **Name:** `protect-main`; **Enforcement:** Active.
- **Target:** include default branch.
- **Rules:** restrict deletions ON; block force pushes ON; require a pull
  request before merging ON; require status checks to pass ON.
- **Pull request:** required approvals **0** (solo maintainer — can't self-approve;
  PR + green CI is the gate); require conversation resolution ON; merge method
  **squash**.
- **Required status check:** `Headless core + tests` (the CI job name), with
  "require branches up to date" ON.
- **Bypass:** Repository admin (Always) — the maintainer can push directly;
  external contributors are fully gated.

Note for future checks: a status check only appears in the selector after it has
reported at least once, so the workflow must have run before it can be required.

**Acceptance:** ☑ a red commit cannot be merged into `main` via PR; direct pushes
to `main` are blocked for non-admins.

### 0.6 ◐ `CONTRIBUTING.md` + good first issues

- ☑ `CONTRIBUTING.md` — build/test instructions (incl. `-DTB_BUILD_GUI=OFF`), the
  `core/` "no graphics, no DB" rule + determinism invariant, contribution-type
  table, PR/CI process, code style, and a **WebAssembly/itch.io** "help wanted"
  section (pure C++20 core + Raylib's Emscripten support → in-browser build).
- ☐ File 2–3 **good first issues** from the known backlog (do on GitHub):
  - *Fireball radius buff* — weakest attack (~43%); tune in `makeDefaultCatalog`.
  - *Teach the AI to use Portal* — currently unused (its step-on-entry mechanic
    needs deeper planning than the beam reaches; see `AI.cpp`).
  - *Add a new spell from existing effects* — pure data, no engine change.
- (Optional) issue + PR templates.

**Phase 0 status:** CI (0.1–0.4) and the branch ruleset (0.5) are live — PRs are
gated by green CI and `main` is protected. **Remaining: 0.6** (CONTRIBUTING.md +
good first issues) to finish the newcomer on-ramp.

---

## Phase 1 — The catalog loader (keystone refactor)

Unblocks content modding **and** is the trust anchor for PvP. The longer spells
live only as compiled C++, the more contributor habits ossify around the wrong
path — lock the data format early. See `ARCHITECTURE.md` §4.

**Decision: hand-rolled JSON** (no third-party dep), split into a reusable
generic layer + a catalog-specific mapper. Enum↔string mappings are centralised
so a new `core/` enum value is a *one-line* format change.

### 1.1 ☑ `data/Json.{h,cpp}` — minimal generic JSON reader/writer
A schema-agnostic `json::Value` (null/bool/number/string/array/object), `parse()`
→ value-or-error (with line:col), and a deterministic, insertion-ordered `dump()`
(pretty or compact). **Reused by the sprite-pack `pack.json` loader in Phase 2** —
this is the project's JSON layer, not throwaway. Numbers parse as double; integer
fields are range/integrality-checked by the mapper. Covered by `tb_json_demo`
(30 checks: types, escapes, `\u`/surrogates, strict errors, round-trip) and wired
into CI. Builds clean under `-Wall -Wextra`; `core/` untouched.

### 1.2 ☑ `data/SpellEnums.h` — the extension point
One `{enum, string}` table per enum (`TargetShape`, `Effect::Type`,
`StatusEffect::Kind`, `GroundKind`) + generic `constexpr toString` /
`fromString<E>` helpers. Adding a core enum value = one new row (covers future
`healOverTime`, new shapes, etc.). **Compile-time `static_assert`s** enforce
per-table integrity (round-trips, no duplicate keys/values, expected counts), so
an inconsistent table fails the build; `tb_enums_demo` adds runtime checks incl.
the unknown-string path (12 checks). Wired into CI; header-only, `core/`
untouched.

### 1.3 ☑ `data/CatalogJson.{h,cpp}` — map JSON ↔ `SpellCatalog`
`CatalogLoad loadCatalogFromString(...)` (→ `{ok, catalog, version, errors}`) and
`serializeCatalog(catalog, version)`. **Strict validation** collecting *all*
errors with context (`spells[2] "poison": effect[1]: ...`): unique/positive ids,
unique keys, non-negative costs/ranges, `minRange ≤ maxRange`, valid enums,
required-and-exclusive per-type effect payloads, integer/range checks,
**unknown fields rejected**. Optional fields default to the struct defaults
(name → capitalized key). `core/` untouched.

Schema versioning: `schema` (int, structural — bump only on breaking change) is
distinct from `version` (author content label). Adding enum strings is
backward-compatible and needs no `schema` bump.

Covered by `tb_catalog_demo` (28 checks): default-catalog byte-identical
round-trip (serialize→load→serialize), field-level fidelity, hand-authored
defaults, and 12 malformed-input rejections. Wired into CI; builds clean under
`-Wall -Wextra`. (`loadCatalogFromFile` + `sha256` land in 1.4.)

### 1.4 ☑ `makeDefaultCatalog()` becomes the generator + `sha256`
`tb_catalog_gen [out] [version]` emits `data/catalog.json` (top-level `data/` =
runtime content; `src/data/` = code) from `makeDefaultCatalog()`, so file and
code can't drift. Hand-rolled `data/Sha256.{h,cpp}` (`sha256Hex`, dep-free; the
server needs it too) hashes the exact bytes — the PvP handshake anchor (§5/§7) —
and `CatalogLoad.sha256` carries it. `loadCatalogFromFile(path)` added. The
committed `data/catalog.json` is generated and in sync.

### 1.5 ☑ Round-trip + validation tests, wired into CI
`tb_catalog_demo` (28 checks): byte-identical `serialize → load → serialize`,
field-level fidelity, hand-authored defaults, 12 malformed-input rejections, and
the file/`sha256` checks. `tb_sha256_demo` (known-answer vectors). Enum coverage
is compile-time (`static_assert`s, 1.2). CI also runs a **drift guard**:
regenerate `data/catalog.json` and `diff` it against the committed file.

### 1.6 ☑ Wire into the app + data-path resolution
`main.cpp` loads the catalog **before `InitWindow`** via the new
`render/ContentPaths.{h,cpp}` resolver (`$ATB_DATA_DIR` → `<exe>/data` →
`<exe>/../data` → `<exe>/../../data` → `./data`; uses Raylib's
`GetApplicationDirectory`, frontend-only). **Policy verified end-to-end:** valid
→ use it (logs version + sha256); *absent* → fall back to `makeDefaultCatalog()`
with a `WARNING`; *malformed* → log every contextual error and exit 1 before a
window opens (no silent fallback). The path resolver is **shared with sprite
packs** (Phase 2). `core/` untouched; headless build/tests unaffected.

**Acceptance:** ☑ the game runs off `data/catalog.json`; a bad file is rejected
with clear, contextual errors before launch; `core/` is untouched.

**Phase 1 complete.** ✅ The catalog is now data-driven: hand-rolled JSON layer
(1.1), enum tables (1.2), strict loader/validator (1.3), generator + `sha256` +
file loader (1.4), round-trip/validation tests + CI drift guard (1.5), and the
app wired to the file with a safe load policy (1.6). Content modding is unlocked
and the PvP trust anchor (`sha256`) is in place.

---

## Spells & economy (shipped alongside Phase 1)

### S.1 ☑ Rewind spell
New `StatusEffect::Kind::Rewind` + a small `Battle` mechanic: casting it snapshots
the target's **position + HP + statuses + cooldowns**; at the start of the
target's **2nd turn** it restores that snapshot (replacing current state). One
`SpellEnums` row, catalog entry `rewind` (id 12, range 0–6 so it self-casts,
cooldown 5). **Fizzles if the target is dead** (dead units never reach a turn, so
no revive). `tb_spells_demo` covers the snap-back (pos/HP/status) + the no-revive
case. Demonstrates the data/code boundary: the *mechanic* was the only code; a
Rewind-3-turns variant is now pure `catalog.json`.

### S.2 ☑ Initiative as a build buy
`StatAllocation.bonusInitiative` at `BuildRules.initCost = 1` pt each; wired into
`validateBuild`, `instantiate` (`baseInitiative + bonus`), and build
serialization (`init=` field). `tb_build_demo` verifies the round-trip + applied
initiative. **Remaining:** expose `±` for initiative in the build editor GUI
(small `BuildEditorScreen` follow-up; the economy/data model is done).

---

## Milestone: Roster entities (bombs & summons)

A cohesive `core/` arc — **entities that enter the roster mid-battle** — that
makes bombs and summons fall out as *content*. Independent of the catalog/PvP
critical path but a real (PR-reviewed) engine change. Confirmed design decisions
are baked in below.

**Shared foundation (the code) — ☑ done, `tb_roster_demo` (16 checks):**
- ☑ **`EntityKind { Champion, Summon, Object }`** on `Entity` (defaults to
  `Champion`, so all existing code is unchanged).
- ☑ **Mid-battle spawn** — `Battle::spawnEntity(Entity)`: appends to `units_`,
  inserts into `order_` **by initiative (ties by `EntityId`)**, adjusts
  `turnIdx_` so the active unit doesn't shift. Deterministic.
- ☑ **Control generalization** `Control { Player, AI, Inert }` + `controlOf(id)`
  (Object→Inert, Summon→AI, Champion→by team). *(GUI loop wiring — drive
  AI/inert turns by `controlOf`, auto-end inert — happens in the Bombs/Summons
  steps when entities actually enter normal play.)*
- ☑ **Death-triggered effects** — `Entity::onDeath` (a `Spell`) resolved at the
  dying unit's tile by `applyDamage`; reentrant + terminates (chain detonations
  work). `cast()`'s resolution was factored into `applySpellEffects()` and shared.
- ☑ **Victory = a team has no living *Champion*** — `checkVictory`/`winner` skip
  summons/objects. Backward-compatible (everything is a Champion today).
- ☑ **Creature catalog** — `Effect::Type::Summon` + `Battle::setCreatures`/
  `spawnCreature`, and the bestiary is now **data**: `data/creatures.json`
  (`data/CreatureJson.{h,cpp}` + `tb_creature_gen`), loaded by `main.cpp` with the
  same valid/absent/malformed policy as the catalog. The Spell/Effect/Status ↔
  JSON mapping was **factored into `data/SpellJson.{h,cpp}` + `data/JsonRead.h`**
  and shared by the catalog and creature loaders (no duplication). `EntityKind`
  got its `SpellEnums` table. `makeDefaultCreatures()` stays as the compiled seed
  + generator. Covered by `tb_creature_demo` (round-trip + field fidelity +
  malformed) with a CI drift guard; `core/` untouched.

**Bombs** ☑ — an `Object` template (`core/Creatures.cpp`): HP 12; ignition =
self-`DamageOverTime` 4/turn; `onDeath` = radius-1 `Damage` 20 (**friendly fire
on** — confirmed); `Entity::fuse` = 2 → detonates (dies → onDeath) on its 2nd
turn; reaching 0 HP (ignition, an attack, or a shove into a wall) detonates early.
Cast via the `bomb` spell (`Effect::Type::Summon` → spawns the "bomb" creature at
the target). Pushable/pullable/rewindable for free (it's an entity). Covered by
`tb_roster_demo` (summon → armed → 2-turn fuse → blast). Portal-on-forced-move
stays off for v1 (confirmed). *(AI doesn't cast Summon spells yet — like Portal,
expected; tune later.)*

**Summons** ☑ — three `Summon` templates in `core/Creatures.cpp`, each with one
innate ability and a **dead-simple, single-purpose AI** (`summonTakeOneAction` —
deliberately *not* the beam planner; summons aren't meant to be clever):
- **blocker** (HP 45) — a **self-centred Cross Pull, range 4** (the 4 straight
  lines): yanks every unit on its arms toward it; foes stop adjacent and take
  collision damage. Self-casts when a foe is on an arm, else advances.
- **healer** (HP 20) — heals the most-wounded ally within range, else closes in.
- **brute** (HP 30) — strikes the nearest foe, else closes in.

Cast via the `blocker`/`healer`/`brute` spells (`Effect::Type::Summon`). **Per-team
cap of 2** enforced in `spawnCreature`. Excluded from victory (foundation).
`main.cpp` now drives turns by **`controlOf`** (player inputs only their
Champions; summons run the AI; objects auto-pass). Covered by `tb_roster_demo`
(cap + the blocker pull). *(Two v1 notes: the Pull is friendly-fire like every
effect — it also drags allies on the arms; and champions' beam AI doesn't cast
Summon spells yet, like Portal — tune later.)*

**Milestone complete.** ✅ foundation ☑ → Bombs ☑ → Summons ☑ → datafy creatures ☑.
Both content types (spells + creatures) are now hand-editable JSON sharing one
validated mapping layer. **Optional follow-ups:** summon `duration`; per-*champion*
(vs per-team) cap; enemy-only Pull (the blocker is friendly-fire today); AI
heuristics so champions actually cast Summon spells.

---

## Phase 2 — Spell bar + sprite/asset packs (visible payoff)

Pure `render/` + `main.cpp`, zero authority risk, recruits a *different*
contributor pool (artists). Can overlap Phase 1. See `ARCHITECTURE.md` §6.

### 2.1 ☐ Clickable spell bar (do first — works with zero art)
- Single-source the per-slot rectangles in a `Layout` helper used by both draw
  and hit-test.
- `main.cpp` hit-tests bar rects **before** the grid move/cast logic so a HUD
  click selects a spell instead of being read as a board move.
- Visual states from the `Entity`: selected / affordable / on-cooldown(+turns) /
  unaffordable. Keep the hotkey digit drawn on each button (additive, not a
  replacement). Hover → existing `spellLabel()` tooltip.
- `ViewState` gains `selectedSpell` + parallel `spellIconKeys`.
- Icon-key resolution needs **no core change**:
  `slot → build.spellIds[slot] → catalog.find(id)->key`.

### 2.2 ☐ Sprite/asset pack seam — **atlas-based**
- **Atlas-first** (Raylib batches sub-rects of one bound texture — singular files
  would force a bind per sprite). A pack = `pack.json` + one or more atlas PNGs.
- `pack.json` (parsed by the **reused `data/Json` layer from 1.1**) maps each
  semantic key → `{ atlas, rect:[x,y,w,h], anchor, anim?, cast? }`. Keys reuse
  existing enums/slugs (`TileType`, `Faction`, `GroundKind`, `StatusEffect::Kind`,
  spell `key`); add the optional cosmetic `appearanceKey`.
- Route every renderer draw through a pack lookup with the fallback ladder:
  `atlas sprite → pack palette color → built-in primitive`. The current primitive
  renderer becomes the default/fallback pack and is never removed.
- **v1**: static `rect` per key + palette, one atlas page, hot-reload. Animation
  (`anim`/`cast`) fields are reserved in the manifest but optional.

### 2.3 ☐ Combat log + structured engine event stream
- The engine emits a typed, ordered **event stream** as it resolves a turn
  (`Move`, `Cast`, `Damage`, `Heal`, `Status`, `Death`, `Storm`, …) — pure data,
  deterministic, no I/O. A small `Battle` addition (append to a log vector); it
  does **not** change resolution outcomes. Replaces the ad-hoc status strings
  `main.cpp` builds today.
- GUI: a scrolling **combat log panel** renders events as text
  ("Pyromancer casts Fireball → 14 dmg to Bruiser", deaths, poison ticks, ring
  damage), with scrollback + autoscroll.
- **Shared infrastructure:** the same stream drives animation event-clips (2.4),
  and is the natural basis for **replays** (Phase 5) and **PvP state deltas**
  (Phase 4). Build it once, here.

### 2.4 ☐ Animations (after the static seam works)
- A **clip** = ordered atlas sub-rects + `fps` + `loop`. **Ambient** (`anim`,
  loops) vs **event** (`cast`, later `hit`/`death`, played once when the matching
  **engine event** from 2.3 arrives, then back to ambient). Cheap — no extra
  texture binds.

### 2.5 ☐ Ship a `packs/default/` example
A copy-able starter (an atlas + manifest, or palette-only) + a short
`docs/sprite-packs.md` authoring guide (incl. how to export an atlas from
TexturePacker / free-tex-packer or hand-author rects).

**Acceptance:** spells are clickable; a partial pack restyles only what it
defines and everything else falls back cleanly; the combat log narrates the
fight; no server/authority code touched.

---

## Parallel track — Web / WASM build

Not on the linear critical path: this can happen anytime now that the graphical
client exists, and it's a great, self-contained contribution. The payoff is a
**browser-playable build shareable on itch.io** (or any static host) — the
lowest-friction way for people to try the sandbox.

Why it's feasible with no engine changes: `core/` is pure, portable C++20 (no
platform syscalls in resolution) and Raylib has first-class Emscripten/HTML5
support, so the same code compiles to WebAssembly.

### W.1 ☐ Emscripten toolchain + web Raylib
Stand up `emsdk`; configure Raylib for the web target (`-DPLATFORM=Web`). Output
is `.wasm` + `.js` + `.html`.

### W.2 ☐ Adapt the frontend main loop
Move the per-frame body from the blocking `while (!WindowShouldClose())` loop into
an `emscripten_set_main_loop` callback (browsers can't block). `core/` is
untouched — this is purely `main.cpp`/`render/`.

### W.3 ☐ Package assets
Bundle sprite packs / data files via `--preload-file` into the Emscripten virtual
filesystem. The **atlas format helps here** — one `atlas.png` per pack instead of
hundreds of singular sprites means far fewer files to preload and fetch.

### W.4 ☐ Build-system + CI
Add a web/Emscripten path to `CMakeLists.txt` beside the native build, and wire a
web-build job into CI so it can't rot.

### W.5 ☐ Publish
Upload the `.html` / `.wasm` / `.js` bundle to itch.io as an HTML5 game.

**Acceptance:** the game runs in a browser from a static bundle; native builds are
unaffected.

---

## Phase 3 — Pluggable AI

Opens an AI-authoring lane; self-contained in `core/`.

### 3.1 ☐ Extract a `Brain` strategy interface
e.g. `decide(const Battle&, EntityId) -> PlannedAction` (or full-turn plan).
The current beam-search planner becomes the default `Brain`.

### 3.2 ☐ Registry / selection
Let a match pick a `Brain` by name so community AIs drop in without forking
`AI.cpp`. Keep `enemyTakeOneAction` / `runEnemyTurn` working as thin adapters.

**Acceptance:** a second toy `Brain` (e.g. greedy) can be selected and runs in
`tb_headless` / `tb_balance` without touching combat code.

---

## Phase 4 — Networked PvP

Only start once Phase 1 (catalog loader + hash) is done. Build order and trust
model are detailed in `ARCHITECTURE.md` §7. Each step is independently
verifiable.

### 4.1 ☐ Serialization + headless round-trip tests
Intent (`move`/`cast`/`endTurn`), state snapshot, build payload (reuse
`serializeBuild`). Prove deterministic offline — **no sockets yet**. Add to CI.

### 4.2 ☐ `MatchSource` refactor (local only)
Abstract `main.cpp`'s in-process `Battle` driving behind an interface; ship the
**local** impl first. Pure refactor — the game plays identically.

### 4.3 ☐ In-process loopback "server"
Run the intent/snapshot loop through an authoritative match runner with no real
socket, to prove the end-to-end loop is deterministic.

### 4.4 ☐ Real transport + 1v1 custom match
WebSocket/TCP, direct connect / join code. Server validates: handshake hash →
`validateBuild()` → per-intent ownership + `canCast` legality. Never trust
client outcomes.

### 4.5 ☐ Lobby, matchmaking, accounts, ranked MMR
SQLite (the `BuildRepository` / `schema.sql` seam) for accounts, results,
ratings. Custom lobbies pin a host catalog by hash.

### 4.6 ☐ Lobby & in-match chat
Text chat over the existing transport (4.4): lobby chat + in-match chat. The
server relays messages scoped to the lobby/match, with basic safety levers
(rate-limit, per-user mute, server-side length cap). Purely additive on the
netcode — no engine involvement. GUI: a chat panel sharing the HUD with the
combat log (2.3).

**Acceptance:** two clients play an authoritative match on the self-hosted
server; a tampered client cannot affect the outcome; players can chat in the
lobby and during the match.

---

## Phase 5 — Replays & spectate (mostly free)

### 5.1 ☐ Persist `seed + intents`; re-simulate to play back (no frame recording).
### 5.2 ☐ Spectate = subscribe to the same snapshot stream.
### 5.3 ☐ Ongoing balance backlog (fireball radius, portal AI, synergy tuning via `tb_balance`).

**Acceptance:** a finished match can be replayed deterministically from its
stored `seed + intents`.
