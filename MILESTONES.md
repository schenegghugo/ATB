# Milestones

The execution plan for ATB, ordered by **what unblocks the most** and **what
makes the repo safe and attractive to contributors** — not by feature appeal.
Design rationale for each piece lives in [`ARCHITECTURE.md`](ARCHITECTURE.md);
this file is the *sequence* and the *step-by-step* for getting there.

**Phase order:** CI + contributing → catalog loader (with hash) → spell bar +
sprite packs → pluggable AI → networked PvP → replays.

**The one rule:** do not start any netcode before the catalog loader, content
hash, and serialization round-trip tests exist (Phase 1 + the first step of
Phase 4). PvP without those is built on sand.

Status legend: ☐ todo ◐ in progress ☑ done

---

## Phase 0 — Make the public repo contributor-safe

The highest-leverage work, and the easiest to skip. A public sandbox lives or
dies on whether a stranger can contribute without you hand-reviewing every line.
CI is what makes the "fork → edit a spell → open a PR" path *safe to merge*.

### 0.1 ☐ Add a `TB_BUILD_GUI` CMake option (prerequisite for fast CI)

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

### 0.2 ☐ Confirm every test fails loudly

CI gates on exit codes. Current state:

- `tb_spells_demo`, `tb_ai_demo` — ☑ already `return g_fails == 0 ? 0 : 1`.
- `tb_headless`, `tb_build_demo` — smoke tests (catch crashes/linker breakage),
  exit 0 on success. Fine as-is; optionally add a sanity assertion + non-zero
  return if you want them to guard behaviour, not just "it ran".

**Acceptance:** introducing a deliberate regression makes at least one binary
exit non-zero.

### 0.3 ☐ Add the CI workflow

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

### 0.4 ☐ (Optional) GUI compile job

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

### 0.5 ☐ Turn on branch protection

On GitHub: **Settings → Branches → Add branch ruleset** for `main` →

1. Require a pull request before merging.
2. Require status checks to pass → select **`Headless core + tests`**.
3. (Optional) require the check be up to date with `main`.

**Acceptance:** you cannot push a red commit straight to `main`.

### 0.6 ☐ `CONTRIBUTING.md` + good first issues

- `CONTRIBUTING.md`: how to build (`cmake -S . -B build && cmake --build build`),
  how to run the tests, the `core/` "no graphics, no DB" rule, and a pointer to
  `ARCHITECTURE.md` before extending the engine or adding content.
- File 2–3 **good first issues** from the known backlog:
  - *Fireball radius buff* — weakest attack (~43%); tune in `makeDefaultCatalog`.
  - *Teach the AI to use Portal* — currently unused (its step-on-entry mechanic
    needs deeper planning than the beam reaches; see `AI.cpp`).
  - *Add a new spell from existing effects* — pure data, no engine change.
- (Optional) issue + PR templates.

**Phase 0 done when:** PRs are gated by green CI on `main`, and a newcomer has a
documented build/test path and a labelled on-ramp.

---

## Phase 1 — The catalog loader (keystone refactor)

Unblocks content modding **and** is the trust anchor for PvP. The longer spells
live only as compiled C++, the more contributor habits ossify around the wrong
path — lock the data format early. See `ARCHITECTURE.md` §4.

### 1.1 ☐ Define the JSON schema
Mirror `SpellDef` (id, key, buildCost) + `Spell` (apCost, ranges, LOS, shape,
radius, cooldown, effects[]). Effects carry their `type` + payload
(`status` / `ground`). Document it; reject unknown fields.

### 1.2 ☐ Write the loader in `data/`
`loadCatalog(path) -> SpellCatalog` with **strict validation** — malformed or
unknown-field files fail *loudly* at load, never silently coerced. `core/`'s
`SpellCatalog` API is unchanged; only the source of entries moves.

### 1.3 ☐ `makeDefaultCatalog()` becomes the generator
Keep it as the compiled fallback, and add a path to *emit* `data/catalog.json`
from it so the official file and the code can't drift.

### 1.4 ☐ Catalog version + `sha256` content hash
Add a declared version and a hash of the exact file bytes. Cheap to add now; it
is the handshake anchor for ranked/custom PvP later (§5/§7).

### 1.5 ☐ Round-trip test
`generate → load → compare` proves the file and `makeDefaultCatalog()` agree.
Add to CI.

**Acceptance:** the game and all demos run off `data/catalog.json`; a bad file is
rejected with a clear error; `core/` is untouched.

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

### 2.2 ☐ Sprite/asset pack seam
- Route every renderer draw through a pack lookup with the fallback ladder:
  `pack sprite → pack palette color → built-in primitive`. The current primitive
  renderer becomes the default/fallback pack and is never removed.
- Keys reuse existing enums/slugs (`TileType`, `Faction`, `GroundKind`,
  `StatusEffect::Kind`, spell `key`); add the optional cosmetic `appearanceKey`.
- `pack.json` manifest + image files; hot-reload for iteration.

### 2.3 ☐ Ship a `packs/default/` example
A copy-able starter (re-declaring the current palette) + a short
`docs/sprite-packs.md` authoring guide.

**Acceptance:** spells are clickable; a partial pack restyles only what it
defines and everything else falls back cleanly; no server/authority code touched.

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

**Acceptance:** two clients play an authoritative match on the self-hosted
server; a tampered client cannot affect the outcome.

---

## Phase 5 — Replays & spectate (mostly free)

### 5.1 ☐ Persist `seed + intents`; re-simulate to play back (no frame recording).
### 5.2 ☐ Spectate = subscribe to the same snapshot stream.
### 5.3 ☐ Ongoing balance backlog (fireball radius, portal AI, synergy tuning via `tb_balance`).

**Acceptance:** a finished match can be replayed deterministically from its
stored `seed + intents`.
