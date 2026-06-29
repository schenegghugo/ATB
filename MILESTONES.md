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

### 1.1 ☐ `data/Json.{h,cpp}` — minimal generic JSON reader/writer
A schema-agnostic `JsonValue` (null/bool/number/string/array/object), `parse()`
→ value-or-error, and a pretty `dump()`. **Reused by the sprite-pack `pack.json`
loader in Phase 2** — this is the project's JSON layer, not throwaway. Numbers
parse as double; integer fields are range/integrality-checked by the mapper.

### 1.2 ☐ `data/SpellEnums.h` — the extension point
One `{enum, string}` table per enum (`TargetShape`, `Effect::Type`,
`StatusEffect::Kind`, `GroundKind`) + generic `toString` / `fromString<E>`
helpers. Adding a core enum value = one new row (covers future `healOverTime`,
new shapes, etc.). A test asserts every enum value has a mapping.

### 1.3 ☐ `data/CatalogJson.{h,cpp}` — map JSON ↔ `SpellCatalog`
`CatalogLoad loadCatalogFromString/FromFile(...)` and
`serializeCatalog(catalog, version)`. **Strict validation** collecting *all*
errors with context (`spell "poison" → effect[1]: ...`): unique/positive ids,
unique keys, `minRange ≤ maxRange`, valid enums, required per-type payloads,
**unknown fields rejected**. `core/`'s `SpellCatalog` API is unchanged.

Schema versioning: `schema` (int, structural — bump only on breaking change) is
distinct from `version` (author content label). Adding enum strings is
backward-compatible and needs no `schema` bump.

### 1.4 ☐ `makeDefaultCatalog()` becomes the generator + `sha256`
A `tb_catalog_gen` target emits `data/catalog.json` from `makeDefaultCatalog()`
so file and code can't drift. Hand-rolled `sha256Hex(bytes)` (dep-free; the
server needs it too) hashes the exact file bytes — the PvP handshake anchor
(§5/§7).

### 1.5 ☐ Round-trip test (`catalog_demo`, wired into CI)
`serialize(makeDefaultCatalog()) → load → assert equal`. Plus the enum-coverage
test (1.2) and a clutch of malformed-input cases that must fail with clear
errors.

### 1.6 ☐ Wire into the app + data-path resolution
`Session` loads `data/catalog.json`. **Policy:** valid → use; *absent* → fall
back to `makeDefaultCatalog()` with a notice; *malformed* → fail loudly (never
silently fall back — that hides corruption). Resolve where `data/` lives
relative to the binary — **shared with sprite packs**, so solve it once.

**Acceptance:** the game and all demos run off `data/catalog.json`; a bad file is
rejected with a clear, contextual error; `core/` is untouched.

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
filesystem.

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

**Acceptance:** two clients play an authoritative match on the self-hosted
server; a tampered client cannot affect the outcome.

---

## Phase 5 — Replays & spectate (mostly free)

### 5.1 ☐ Persist `seed + intents`; re-simulate to play back (no frame recording).
### 5.2 ☐ Spectate = subscribe to the same snapshot stream.
### 5.3 ☐ Ongoing balance backlog (fireball radius, portal AI, synergy tuning via `tb_balance`).

**Acceptance:** a finished match can be replayed deterministically from its
stored `seed + intents`.
