# Milestones

The execution plan for ATB, ordered by **what unblocks the most** and **what
makes the repo safe and attractive to contributors** — not by feature appeal.
Design rationale for each piece lives in [`ARCHITECTURE.md`](ARCHITECTURE.md);
this file is the *sequence* and the *step-by-step* for getting there.

**Phase order:** CI + contributing → catalog loader (with hash) → spell bar +
sprite packs → pluggable AI (→ self-teaching AI, optional depth item) → networked
PvP → replays.

Off the critical path, a **Web/WASM build** (see *Parallel track* below) can be
picked up anytime now that the GUI exists — it's frontend-only and independent of
the content and PvP work.

**Where we are:** Phases 0–2, the **Core split**, **Match rulesets**, and
**pluggable AI** (3.1/3.2) are done. Phase 4 is essentially complete: **4.1–4.4**
(wire formats → `MatchSource` seam → loopback runner → real TCP + GUI remote
client) plus **4.5**'s server slices (multi-match daemon, accounts + PBKDF2,
Elo/MMR, private lobbies, connect screen + main menu). The **correspondence-ranked
arc (CR.1–CR.5) is built end-to-end**: cross-platform determinism, the game
notation + verifier (= replays, §5.1), the mailbox relay, the double-submit
arbiter, and the official perfect-information ranked ruleset. **Open threads:**
GUI playtesting + async connect/waiting screen, SQLite behind the store seam,
TLS before any public non-VPN launch, 4.6 chat, Phase 5.2 spectate, and the
hidden-information ranked ideas (commit-reveal / decoys) parked under CR.6.

**The one rule (satisfied):** no netcode before the catalog loader, content hash,
and serialization round-trip tests exist (Phase 1 + 4.1) — all long since done.

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
`tb_catalog_gen` scaffolds `data/catalog.json` (top-level `data/` = runtime
content; `src/data/` = code) from `makeDefaultCatalog()`. **The data file is the
source of truth** (hand-editable); the compiled seed is the absent-file fallback +
a bootstrap scaffold — they may diverge, and that's fine. Hand-rolled
`data/Sha256.{h,cpp}` (`sha256Hex`, dep-free; the
server needs it too) hashes the exact bytes — the PvP handshake anchor (§5/§7) —
and `CatalogLoad.sha256` carries it. `loadCatalogFromFile(path)` added. The
committed `data/catalog.json` is generated and in sync.

### 1.5 ☑ Round-trip + validation tests, wired into CI
`tb_catalog_demo` (28 checks): byte-identical `serialize → load → serialize`,
field-level fidelity, hand-authored defaults, 12 malformed-input rejections, and
the file/`sha256` checks. `tb_sha256_demo` (known-answer vectors). Enum coverage
is compile-time (`static_assert`s, 1.2). CI runs a **validity check** —
`tb_catalog_gen --check data/catalog.json` (and the creatures equivalent) — so the
committed, hand-editable data files must load + validate. *(Originally a
regenerate-and-diff drift guard; flipped once the data file became canonical and
balance values are tuned by editing the JSON directly.)*

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
  got its `SpellEnums` table. `makeDefaultCreatures()` stays as the compiled
  fallback/scaffold (the data file is canonical). Covered by `tb_creature_demo`
  (round-trip + field fidelity + malformed) + a CI validity check
  (`tb_creature_gen --check`); `core/` untouched.

**Bombs** ☑ — an `Object` template (`core/Creatures.cpp`): HP 12; ignition =
self-`DamageOverTime` 4/turn; `onDeath` = radius-1 `Damage` 20 (**friendly fire
on** — confirmed); `Entity::fuse` = 2 → detonates (dies → onDeath) on its 2nd
turn; reaching 0 HP (ignition, an attack, or a shove into a wall) detonates early.
Cast via the `bomb` spell (`Effect::Type::Summon` → spawns the "bomb" creature at
the target). Pushable/pullable/rewindable for free (it's an entity). Covered by
`tb_roster_demo` (summon → armed → 2-turn fuse → blast). Portal-on-forced-move
stays off for v1 (confirmed). *(The champion beam planner now casts `Summon`
spells — `enumerateActions` offers empty in-range tiles as spawn targets — so
bombs/summons appear in AI play and in the balance sim.)*

**Summons** ☑ — three `Summon` templates in `core/Creatures.cpp`, each with one
innate ability and a **dead-simple, single-purpose AI** (`summonTakeOneAction` —
deliberately *not* the beam planner; summons aren't meant to be clever):
- **blocker** (HP 18) — a **self-centred Cross Pull, range 4** (the 4 straight
  lines): yanks every unit on its arms toward it; foes stop adjacent and take
  collision damage. Self-casts when a foe is on an arm, else advances.
- **healer** (HP 10) — heals the most-wounded ally within range, else closes in.
- **brute** (HP 18) — strikes the nearest foe, else closes in.

Cast via the `blocker`/`healer`/`brute` spells (`Effect::Type::Summon`). **Per-team
cap of 2** enforced in `spawnCreature`. Excluded from victory (foundation).
`main.cpp` now drives turns by **`controlOf`** (player inputs only their
Champions; summons run the AI; objects auto-pass). Covered by `tb_roster_demo`
(cap + the blocker pull). *(v1 note: the Pull is friendly-fire like every effect —
it also drags allies on the arms. The champion beam AI **does** now cast Summon
spells; only Portal remains AI-unused.)*

**Milestone complete.** ✅ foundation ☑ → Bombs ☑ → Summons ☑ → datafy creatures ☑.
Both content types (spells + creatures) are now hand-editable JSON sharing one
validated mapping layer.

**Fixes since:**
- *Bomb-aware AI* — the beam planner treated a foe-team bomb as a target (it
  harpooned bombs *toward* itself). Now bombs are excluded from targeting,
  nearest-foe, and the aggression field, and `evalState` adds an `expectedBlast`
  risk term so units avoid standing in a live bomb's blast radius. (`tb_roster_demo`)
- *Dead-unit HUD* — status panels now draw only for **living** units, so a dead
  summon/bomb's HP/AP/MP card disappears.

**Optional follow-ups:** summon `duration`; per-*champion* (vs per-team) cap;
enemy-only Pull (the blocker is friendly-fire today); AI heuristics so champions
actually *cast* Summon spells.

---

## Build editor revamp (spell tags + bigger, filterable picker)

### BE.0 ☑ Bugfix — balance sim crash on the grown catalog
`balance_sim` indexed fixed `std::array<,12>` by spell id; new ids (12–16)
overflowed → segfault (CI's `tb_balance 200 1`). Now sized from the catalog's max
id (vectors) with bounds guards, so it grows with the catalog. Verified across
seeds incl. seed 1.

### BE.1 ☑ Free-form spell `tags`
`SpellDef.tags` (`std::vector<std::string>`) — **free-form** (no controlled
vocabulary, fully moddable), serialized/validated by `CatalogJson` (array of
unique non-empty strings). Official catalog authored per the taxonomy
(category: `damage`/`support`/`summon` + modifiers `aoe`/`single`/`melee`/`ranged`/
`dot`/`buff`/`debuff`/`terrain`/`mobility`). A `tb_catalog_demo` **consistency
test** enforces that *derivable* tags can't lie (`aoe`⇒non-Single shape,
`single`⇒Single, `summon`⇒Summon effect, `dot`⇒DoT, `damage`⇒Damage/DoT);
intent tags (buff/debuff/support/…) are unchecked. `data/catalog.json` regenerated.

### BE.2 ☑ Editor UI: bigger window + filterable card grid
Window opens larger (≥1180×720) and is **resizable** (`FLAG_WINDOW_RESIZABLE`);
the editor reads the **live window size** (`GetScreenWidth/Height`) and lays out
responsively each frame — so it works under tiling WMs (Sway ignores fixed-size
*requests*, so per-screen `SetWindowSize` was the wrong approach). The vertical
list is replaced by a **card grid** (name + cost + AP/range + tags), with
**category filter chips** (All / Damage / Effects / Support / Summon → tag
predicates) and a right column of stat steppers — now including **+INIT**
(the deferred S.2 control) — plus the budget bar + validation errors. Built &
run-verified (no crash; harmless Wayland window-*position* warnings only).
*(Follow-ups: spell-card icons once Phase-2 atlas lands; grid scroll for very
large modded catalogs; secondary modifier-tag toggles.)*

---

## Core split — separate `core/` headers (do first)

`Battle.h` mixes three concerns; split for clarity. Mechanical,
**behaviour-preserving**, full-suite-verified.

### CS.1 ☑ Split `Battle.h` into Combat / Entity / Battle
- `core/Combat.h` — spell/effect **data model**: `Effect`, `TargetShape`, `Spell`,
  `StatusEffect`, `GroundKind`, `GroundSpec`, `DamageSource`.
- `core/Entity.h` — `EntityId`, `Faction`, `EntityKind`, `Control`, `Entity`,
  `EntitySnapshot` (includes `Combat.h` + `Grid.h`).
- `core/Battle.h` — the engine only: `Phase`, `GroundEffect`, `PendingRewind`,
  `Battle` (re-includes `Combat.h` + `Entity.h`, so existing `#include "Battle.h"`
  still resolves everything). `Grid.h` stays = the arena terrain.
- Data-layer headers narrowed to their real deps (`Spells.h`→`Combat.h`,
  `Build.h`/`Creatures.h`/`CreatureJson.h`→`Entity.h`, `SpellJson.h`→`Combat.h`,
  `SpellEnums.h`→`Combat.h`+`Entity.h`). **No behaviour change** — full suite,
  `--check`, balance determinism, and the GUI all pass unchanged. `StormConfig`
  still lives in `Battle.h`; it migrates into the `Ruleset` in R.1.

---

## Milestone: Match rulesets (datafied; unify game ↔ balance sim)

A `Ruleset` + `data/rules.json`, loaded by the **same** Json + JsonRead +
validation + generator + `sha256` + drift-guard machinery as the catalog /
creatures. Goal: the game and the balance sim build matches **the same way**, and
the match format is editable by modders / fixable for competitive play. Becomes
the **third pinned artifact** (catalog + creatures + ruleset) in the trust model
(`ARCHITECTURE.md` §5/§7).

Ruleset fields: `format.teamSize` (1/2/3 → NvN); `economy` (= `BuildRules`:
budget, base HP/AP/MP/Init + per-point costs); `bannedSpells` (by key);
`closingRing` (= `StormConfig`: enabled / startRound / damage); `arena`
(`random {w,h,coverage}` | `static {map}`).

### R.1 ☑ `Ruleset` + `data/RulesetJson.{h,cpp}` + `data/rules.json`
`core/Ruleset.h` (`teamSize`, `economy` = `BuildRules`, `bannedSpells`,
`closingRing` = `StormConfig`, `arena` {w,h,coverage}) + `makeDefaultRuleset()`.
`StormConfig` moved to `core/Storm.h` (config, not engine). Loader mirrors
`CatalogJson` (strict, all-errors-with-context; omitted blocks keep defaults);
`tb_ruleset_gen` (`--check` + `--force` scaffold); committed `data/rules.json`
(hashable, `--check`ed in CI). `tb_ruleset_demo` (20 checks: round-trip, partial
overrides, 7 malformed). Bonus: `json::dump` now uses `std::to_chars` so
`coverage` prints as `0.18` (shortest round-trip). **Wiring into game + sim is
R.2** — the `ATB_*` env-var HP overrides stay until then.

### R.2 ☑ Unified `buildMatch(ruleset, teams, seed, …)` → `Battle`
`core/Match.{h,cpp}` `buildMatch()` is the single construction path: arena from
`rules.arena`, economy from `rules.economy`, ring from `rules.closingRing`, roster
placed from the two teams' builds (`teamSpawns` spreads N champions; `teamSize:1`
yields the historical spawns, so balance stays byte-deterministic). **Both** the
game (`main.cpp` → `makeBattle` now one-lines `buildMatch`; `Session` carries a
`Ruleset`; window sized from `rules.arena`; editor validates against
`rules.economy`) and the balance sim load `data/rules.json` (same
valid/absent/fail-loud policy) and call `buildMatch`. The sim generates
`teamSize` builds per side (2v2 verified) and reports the ruleset source + format.
**Removed the `ATB_*` env-var HP overrides** — tune via `rules.json` now. *(The
editor still authors one build per side; teams for `teamSize>1` are R.3.)*

### R.3 ☑ Team formats 2v2 / 3v3
The game now honours `rules.json` `teamSize`. The editor authors a **team of
builds**: player **slot tabs** (`1 2 3`, tinted red when a slot is over budget) to
edit each champion in turn, and a row of **enemy slot pickers** (each cycles the
saved builds). `playerTeam()` / `enemyTeam()` feed the shared `buildMatch`, so
Fight only enables when *every* player slot is valid. The editor takes the whole
`Ruleset` (economy + teamSize). `core/` + sim already supported N champions
(victory = no living champion per team); the sim generates `teamSize` builds per
side. Verified: default 1v1 unchanged; a `teamSize:2` ruleset runs end-to-end in
both the sim (4 entities/match) and the GUI.

### R.4 ☑ Static maps
`data/MapJson.{h,cpp}` loads a map authored as char rows (`.` walkable, `#` wall,
`o` obstacle) → a `Grid`, with strict validation (equal-length rows, known chars,
≥3×3) and an **on-load reachability gate** (the canonical spawns must be walkable
+ connected). Ruleset `arena.map` (a key) selects it: non-empty → load
`data/maps/<map>.json` instead of generating. `buildMatch` takes an optional
`const Grid*` (static arena); the game (`Session.staticArena`, loaded once) and
the sim both resolve + load it (fail-loud on a bad map). Ships `data/maps/duel.json`
(20×15). `tb_map_demo` (13 checks incl. ragged/blocked-off rejection + the sample
validates), wired into CI. Verified end-to-end: static-map matches run in the sim
and the GUI; default (random arena) unchanged.

### R.5 ☑ Ban enforcement + competitive / custom
`validateBuild` gained a `bannedSpells` (catalog-key) param — it rejects any build
using a banned spell (the server's build-admission check too). The **editor**
greys out banned cards (`BANNED`, not selectable) and validates the team against
the bans; the **sim** drops banned spells from its random-build pool. Verified:
`tb_build_demo` (valid normally, rejected when banned) + a sim run with
`fireball`/`poison` banned shows them at 0% pick. Trust tie-in (documented,
`ARCHITECTURE.md` §5/§7): **ranked** pins the official ruleset; **custom** lobbies
pin an agreed `rules.json` by hash — functionally realized with Phase 4 netcode.

**Milestone complete.** ✅ Core split → R.1 (ruleset data) → R.2 (unified
`buildMatch`) → R.3 (2v2/3v3) → R.4 (static maps) → R.5 (bans). One `rules.json`
now drives format, economy, ring, arena, and bans across the game *and* the
balance sim — the third pinned, hand-editable, validity-gated artifact beside
catalog + creatures.

---

## Phase 2 — Spell bar + sprite/asset packs (visible payoff)

Pure `render/` + `main.cpp`, zero authority risk, recruits a *different*
contributor pool (artists). Can overlap Phase 1. See `ARCHITECTURE.md` §6.

### 2.1 ☑ Clickable spell bar (do first — works with zero art)
`render::spellSlotRect()` single-sources the geometry; `main.cpp` hit-tests it
before the board; `Renderer::drawSpellBar()` paints selected / affordable /
cooldown / unaffordable states with the hotkey digit; `ViewState` gained
`selectedSpell` + `spellIconKeys` (resolved by catalog name, ready for 2.2 icons).
Visually confirmed in-game.
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

### 2.2 ☑ Sprite/asset pack seam — **atlas-based**
*Seam shipped; manifest loader headless-tested (`tb_pack_demo`, in CI); palette
re-theme visually confirmed in-game.* `render/PackManifest.{h,cpp}`
(raylib-free parse + strict validation) + `render/SpritePack.{h,cpp}` (atlas load,
resolve, `drawSprite`/`paletteOr`). Renderer routes **tiles / units / spell-bar
icons** through the ladder (atlas sprite → palette colour → primitive); `main.cpp`
loads a pack from `ATB_PACK=<dir>` (absent ⇒ primitives, unchanged). Example
palette-only pack in `packs/example/`. *Remaining:* ground effects + status
markers wiring, the editor skill-dictionary icons, animation clips (2.4).
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

### 2.3 ☑ Combat log + structured engine event stream
*Engine stream shipped + headless-tested (`tb_event_demo`, in CI); GUI log panel
implemented and visually confirmed in-game.* `core/Event.h` (`BattleEvent`/`EventType`);
`Battle` appends via `emit()` at TurnStart/Move/Cast/Damage/Heal/Status/Death and
exposes `events()`. The AI disables recording on its planning clones
(`setEventRecording`) so they stay cheap. Renderer draws a scrolling log in the
column right of the board (`formatEvent`, wheel scrollback + autoscroll); Move is
emitted but hidden in the log (kept for animation/replay). *Remaining:* the
one-line `status` string still coexists (not yet fully replaced); forced-move
displacement isn't emitted as Move yet.
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

### 2.4 ☑ Animations (after the static seam works)
*Clip data model + parsing shipped and headless-tested (`tb_pack_demo`, in CI);
event-driven cast flash visually confirmed in-game.* A **clip** = ordered atlas
sub-rects (`rects`) + `fps` + `loop`, parsed/validated by `PackManifest`
(`Clip::frameAt`/`duration` pick the frame). **Ambient** (`anim`, loops) vs
**event** (`cast`, plays once then reverts). The one-shot trigger reuses the
§2.3 event stream: `render/Animator` consumes new `Cast` events off
`battle.events()` and stamps the actor's trigger time; `SpritePack::drawSprite`
gained a frame-aware overload (cast clip while running → ambient loop → static
rect); the renderer routes **unit** sprites through it (`main.cpp` syncs the
`Animator` each frame, resets it on new battle/rematch). Cheap — same single
atlas bind, no extra textures. The shipped `default` + `example_upscaled` packs
author a champion **cast flash** from existing atlas cells (no new art), and
`tools/gen_pack.py` emits it too. Artist-facing authoring guide added to
`packs/README.md` (*Editing the art* + *Animations*). *(Later `hit`/`death`
event clips + ambient idle loops are pure data once packs supply the frames.)*

### 2.5 ☑ Ship a `packs/default/` example
Copy-able starters ship: `packs/default/` (full atlas — tiles, unit tokens, a
distinct icon per spell, cast animations, generated by `tools/gen_pack.py`),
`packs/example_upscaled_to_96px/` (the same at 96 px), and the new
**`packs/example/`** — the minimal **palette-only** re-theme (no atlas, ~5 lines),
the smallest thing to copy. Full authoring guide added at
**`docs/sprite-packs.md`** (palette-only → repaint the atlas → author your own:
the `rect`/`anchor`/animation format, the semantic-key table, and how to export an
atlas from **TexturePacker / free-tex-packer** or hand-author rects); `packs/README.md`
slimmed to an in-directory quick reference that links to it. All three packs
validate through `loadPackManifestFromFile` (verified).

**Acceptance:** spells are clickable; a partial pack restyles only what it
defines and everything else falls back cleanly; the combat log narrates the
fight; no server/authority code touched.

**Phase 2 complete.** ✅ 2.1 spell bar → 2.2 atlas pack seam → 2.3 combat log +
event stream → 2.4 animations → 2.5 example packs + `docs/sprite-packs.md`. Spells
are clickable, the fight narrates itself, and artists have a copy-able starter
(palette-only through full atlas) plus a full authoring guide — all `render/` +
`main.cpp`, zero authority risk.

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

### 3.1 ☑ Extract a `Brain` strategy interface
`core/AI.h` now defines `Brain` — `planTurn(const Battle&, EntityId) ->
std::vector<PlannedAction>` (full-turn plan; the caller picks execution
granularity) + `name()`. `PlannedAction` was promoted to the public header. The
beam search is now `BeamSearchBrain`, exposed as the stable singleton
`defaultBrain()`. `enemyTakeOneAction` / `runEnemyTurn` gained `const Brain&`
overloads and the old signatures delegate to `defaultBrain()`, so every caller
(`main.cpp` + 7 tests) compiles unchanged. Summons stay deliberately off the Brain
seam (fixed `summonTakeOneAction`). **Behaviour-preserving:** full headless suite
passes and `tb_balance 400 7` is byte-identical to the pre-change AI apart from the
wall-clock timing line (determinism intact). *(Registry / by-name selection + a toy
Brain are 3.2.)*

### 3.2 ☑ Registry / selection
A match can now pick a `Brain` by name. `AI.h`/`AI.cpp` gained a small registry —
`brainByName(name)`, `brainNames()`, `registerBrain(brain)` — seeded with two
built-ins: **`beam`** (the default beam search, `defaultBrain()`) and a new
**`greedy`** toy (a deliberately weaker 1-ply hill-climb reusing
`enumerateActions`/`evalState`; deterministic, ~2× faster, and a worked template
for community AIs). Registration is non-owning (Brains are static singletons) and
selection stays **app-level** — `core/` reads no environment. `tb_headless` and
`tb_balance` select via **`ATB_BRAIN=<name>`** (matching the `ATB_MAP`/`ATB_TEAM`
convention; unknown name → lists the choices and exits 1); `scripts/balance.sh`
exposes it as `-b/--brain` and the report header prints the AI used.
`enemyTakeOneAction`/`runEnemyTurn` are unchanged thin adapters (the default
overloads still call `defaultBrain()`). **No combat code touched.**

**Acceptance:** ☑ `ATB_BRAIN=greedy` runs in both `tb_headless` and `tb_balance`
(distinct balance numbers from `beam`, deterministic across reruns modulo the
wall-clock line); the full headless suite + `tb_balance 200 1` stay green.

---

## Phase 3.5 — Self-teaching AI (learned evaluator, NNUE-style)

Branches off Phase 3; **off the PvP critical path** (a depth item, not a
blocker). Replaces the hand-tuned `evalState` with a *learned* scalar — the beam
search (`planTurn`) is untouched, exactly as chess NNUE swapped the leaf eval
without changing alpha-beta. The engine already has the two hard prerequisites: a
**deterministic, cloneable `Battle`** (`Battle s2 = n.state;`) and a **scalar eval
at a clean seam** (`evalState` + `EvalWeights`). `tb_balance` is already both the
self-play *data generator* (thousands of deterministic matches/sec) and the
*gauntlet* for accepting a new net. No policy net / MCTS needed — `enumerateActions`
does move selection; the net only judges positions (why NNUE fits better than
AlphaZero here). The eval it replaces lives in `AI.cpp` (`evalState`); the
pluggable-AI seam is `ARCHITECTURE.md` §9–10.

### 3.5.1 ☐ Split the seam: `Evaluator` (state → score) under `Brain`
Extract `evalState` behind an `Evaluator` interface; `HandcraftedEvaluator` wraps
today's `EvalWeights`. The default beam `Brain` (Phase 3) is composed from an
`Evaluator&`. Pure refactor, zero behaviour change — gate with `tb_balance`:
handcrafted-vs-handcrafted must stay ~50%.

### 3.5.2 ☐ Versioned feature encoder `featurize(Battle, Faction)`
Shared by data-gen **and** inference so they can't drift; version + hash the
layout. Hybrid: sparse `(side, EntityKind, tile)` one-hots (the
incrementally-updatable part) + pooled, order-invariant dense per-side scalars
(effHP / AP / MP / pending DoT / shields / #invisible / reachable offensive damage
/ `foeField` distance / #in-storm) + globals (round, storm radius/damage,
side-to-move).

### 3.5.3 ☐ Self-play data export
A flag on the `buildMatch` sim loop logs `(features, label)` per turn; label =
that side's eventual result (+1/−1), optionally blended with the beam score at
that node (bootstrapping, as NNUE trained on Stockfish evals — converges far
faster than pure win/loss). Reuses existing headless throughput.

### 3.5.4 ☐ Offline training + weights artifact
Python/PyTorch trains a small MLP (value in [−1, 1]); export to a flat,
**versioned, `sha256`-pinned** weights file — a *fourth pinned artifact* beside
catalog/creatures/rules, loaded with the same valid/absent/malformed policy
(**absent ⇒ fall back to `HandcraftedEvaluator`**, invalid ⇒ hard error). Add a
`tb_*_gen --check` CI gate. No Python at runtime.

### 3.5.5 ☐ C++ inference `LearnedEvaluator`
Dependency-free (a couple of matmuls + activation) behind the `Evaluator` seam.
Deterministic given `(weights, state)` — the eval only chooses the AI's move, not
game rules, so replays and server authority are unaffected.

### 3.5.6 ☐ Improvement loop + gating
Self-play with net *vN* → retrain → *vN+1*; promote **only if it beats vN
>~55%** over N sim matches (reuse the Wilson CI already in `tb_balance`). This is
AlphaZero's loop with the beam search standing in for MCTS.

### 3.5.7 ☐ (Optional) NNUE-grade speed
Sparse features + an **incrementally updatable accumulator** tied to the beam's
clone→apply (a *move* flips one `(side,kind,tile)` feature → subtract/add one
embedding row; AoE / summons / forced-moves recompute), plus int8 quantization.

**First vertical slice:** 3.5.1 + a minimal 3.5.2–3.5.5 whose training target is
the *handcrafted* score (imitation). Success = the learned eval reproduces
handcrafted play (~50% in the gauntlet), proving the whole
featurize→train→export→load→search pipeline at **zero strength risk**. *Then*
switch the target to game outcomes and let it surpass the hand weights.

**Acceptance:** `LearnedEvaluator` loads a checked-in weights file and plays a
full `tb_balance` run; on the imitation target it holds ~50% vs
`HandcraftedEvaluator`, and an outcome-trained net beats it >55% in the gauntlet.
Absent/invalid weights fall back to handcrafted with a loud message.
**Stretch (non-goal for v1):** a policy network + MCTS (full AlphaZero) — only if
the eval-only net plateaus.

---

## Phase 4 — Networked PvP

Build order and trust model are detailed in `ARCHITECTURE.md` §7. Each step is
independently verifiable. **The offline foundation (4.1–4.3) is done:** wire
formats + round-trip tests, the client `MatchSource` seam, and the authoritative
in-process loopback runner — all deterministic and headless-tested, with no
sockets yet. Remaining work (4.4+) adds the transport and lobby on top; it does
not change `core/`.

### 4.1 ☑ Serialization + headless round-trip tests
`data/Net.{h,cpp}` adds the three PvP wire payloads (§7), built on the existing
`Json`/`JsonRead`/`SpellJson`/`SpellEnums` layers, `core/` untouched:
- **`Intent`** — `move{dest}` / `cast{spellIdx,target}` / `endTurn`, with
  `applyIntent(Battle&, actor, intent)` — a thin, **legality-checked** dispatch
  over the public engine verbs (`moveToward`/`cast`/`endTurn`), exactly what the
  authoritative runner (4.3) will call per inbound intent. Strict compact-JSON
  (de)serialization, all-errors-with-context.
- **`Snapshot`** — the near-full renderable match state (phase / winner / active /
  round / ring + per-unit public state + ground effects), addressed by stable
  `EntityId`. `snapshotOf(const Battle&)` reads public accessors only.
  Deliberately omits each unit's full spell list (loadout is known from match
  setup) — a lean state delta, not a rebuildable Battle (spectator spells later).
- **Build payload** — reuses `serializeBuild`/`deserializeBuild` verbatim.

`tb_net_demo` (29 checks, in CI): byte-identical round-trips for all three;
malformed-input rejection (intent + snapshot); and a **determinism** proof —
driving a whole match purely by Intents is reproducible *and* byte-identical to
playing it through the engine verbs (`runEnemyTurn`). No sockets.

**Acceptance:** ☑ intents/snapshots/builds round-trip deterministically offline,
wired into CI; `core/` untouched.

### 4.2 ☑ `MatchSource` refactor (local only)
`render/MatchSource.{h,cpp}` introduces the **frontend seam** between the UI
(input + rendering) and *who drives the Battle*: `battle()` (the state to render),
`awaitingLocalInput()` (is it the local player's turn?), `submit(net::Intent)`
(apply a move/cast/endTurn), and `update(dt)` (advance AI/inert turns on the watch
timer). `LocalMatchSource` holds the in-process `Battle` — the exact turn-driving
logic `main.cpp` ran inline, moved behind the seam **verbatim**. `main.cpp` now
turns clicks/keys into **Intents** (the §4.1 vocabulary) and submits them; it
reads `source->battle()` to render and never mutates the Battle directly (it no
longer includes `core/AI.h` — the AI lives behind the seam). This is the shape a
`RemoteMatchSource` (4.6) fills: send Intents, mirror server snapshots; render
code unchanged.

`LocalMatchSource` is **raylib-free** (pure core + net), so `tb_matchsource_demo`
(12 checks, in CI) verifies the seam headless: the `awaitingLocalInput`/`submit`/
`update` contract (paced AI, illegal cast refused without mutating) and that a
full match driven through the seam is deterministic. The GUI compiles + launches
(catalog/creatures/ruleset load, editor runs); the battle path is the same calls
the headless test exercises.

**Acceptance:** ☑ pure refactor — the seam plays identically (verified headless +
GUI compiles/launches), `core/` untouched, and the interface is ready for a remote
impl.

### 4.3 ☑ In-process loopback "server"
A new **`net/` tier**: `net/MatchRunner.{h,cpp}` is the authoritative resolver
(§7 component #2) — it owns one `Battle` and is the *only* thing that mutates it.
Per-`Faction` **`Seat { Human, AI }`** decides who drives each side; summons and
inert objects are always server-driven. `submit(seat, intent)` enforces the trust
checks — **ownership** (the seat must own the active Champion) and **legality**
(the engine verbs refuse illegal casts/moves without mutating) — then applies the
intent via `net::applyIntent` and `advance()`s every server-controlled turn
(AI champions/summons via `runEnemyTurn`, inert passes) to the next human decision
point. `snapshot()` is the broadcast state. No new combat logic — a thin loop over
`tb_core`; no sockets.

`tb_loopback_demo` (14 checks, in CI) proves the goal — **end-to-end
determinism**: an all-AI runner and a fully intent-driven (`Human`×`Human`,
scripted by the Brain) run are each **byte-identical to the raw engine**
(`runEnemyTurn`) and reproducible across runs. Plus the authority checks
(wrong-seat + illegal intents rejected without mutation) and PvE (the AI seat is
never awaited — the server plays it).

**Acceptance:** ☑ the intent → apply → snapshot loop runs through the
authoritative runner in-process and is provably deterministic; `core/` untouched.

### 4.4 ☑ Real transport + 1v1 custom match
A new **`net/` transport** (kept out of `tb_core` so the engine stays portable +
socket-free): **hand-rolled, zero-dependency TCP** (`net/Socket.{h,cpp}` —
length-prefixed frames over Berkeley sockets, `MSG_NOSIGNAL`, non-blocking poll)
matching the project's ethos; a tiny JSON message layer (`net/Protocol.h`) whose
payloads reuse the §4.1 wire serializers verbatim; `net/GameServer.{h,cpp}`
(`serveOneMatch`) wrapping the §4.3 `MatchRunner` with the two admission
checkpoints — **handshake content hash** (`contentHashOf`) then **`validateBuild`
vs the ruleset** — and per-intent ownership + legality already enforced by the
runner (never trusts a client outcome). Ships a self-hosted **`tb_server`** binary
that serves `data/` content.

**Deterministic mirror (the client model):** the server sends match setup (both
builds + a concrete arena seed) in `welcome`, then broadcasts only the **applied
human intents**; each client (`net/MirrorSession`) rebuilds an identical
`MatchRunner` and replays that stream, reproducing everything else (AI/summon/inert
turns) locally — so the client renders a byte-identical mirror of the authoritative
Battle with minimal wire traffic. The **GUI remote client** is `render/`
`RemoteMatchSource` (a `MatchSource` over the mirror — same render path as local,
only the source of truth swaps); `main.cpp` joins a networked match when
`ATB_CONNECT=host[:port]` is set (else local), falling back to local on a failed
connect.

Verified: `tb_net_transport_demo` (two mirror clients) and `tb_remote_demo` (the
GUI's `RemoteMatchSource` vs a client) each play a **full authoritative 1v1 over a
real localhost socket** — opposite seats, run to a winner, **all parties agree on
the identical final snapshot** — plus **content-hash-mismatch rejection**; both in
CI and stress-run 20×+ clean. The real `tb_server` binary + two clients complete a
match end to end, and the GUI compiles/links the remote path. *(Two real bugs
fixed en route: a silent `SIGPIPE` on half-open writes → `MSG_NOSIGNAL`; and
`buildMatch(seed=0)` **time-seeding a different arena per process** → the server now
picks one concrete seed and ships it, so all mirrors match. Transport: TCP, zero-dep;
WebSocket can layer on later for the browser build.)*

**Configuring the address (today):** the client reads **`ATB_CONNECT=host[:port]`**
(default port 5555) — e.g. `ATB_CONNECT=192.168.1.50:5555 ./tactical_battler`;
unset ⇒ a local match. The server is **`tb_server [port]`** (serves one match, then
exits). ⚠️ *Current limitation:* `Listener::bind` binds `INADDR_LOOPBACK`
(127.0.0.1) — LAN/internet hosting needs it on `INADDR_ANY` (0.0.0.0) + a firewall
rule / port-forward. A one-line change, gated so tests stay loopback-only.
*(Follow-up: a small `[server] host/port` in a config file or an in-editor connect
field, once the lobby lands.)*

**Follow-ups (not blockers):** optimistic client-side prediction (today the mirror
waits for the server echo — imperceptible on localhost, matters under real latency);
a connect/join-code lobby UI (Phase 4.5 territory); teams >1 over the wire.

### 4.5 ◐ Lobby, matchmaking, accounts, ranked MMR
SQLite (the `BuildRepository` / `schema.sql` seam) for accounts, results, ratings.

**Slice 1 ☑ — persistent multi-match server + configurable bind.** `tb_server` is
now a **daemon**, not a one-shot: `net::serveMatches()` accepts players, admits
each (handshake + `validateBuild`), and pairs them **FIFO** — every two admitted
players start a match that runs on its own thread, so many matches play
concurrently (independent Battles share no mutable state). `serveOneMatch` was
refactored to share the same `runAdmittedMatch` core. **Bind is configurable**
(`Listener::bind(port, host)`; `tb_server [port] [bind]`) — default `127.0.0.1`
(safe/local, and tests stay loopback), `0.0.0.0` for all interfaces, or a specific
IP (e.g. a Tailscale address) — so it can be hosted on the EliteDesk. A generous
per-move read timeout doubles as a **turn clock** (idle past it → forfeit).
`tb_server_demo` (in CI, stress-run 15×): four clients are matched into **two
concurrent matches** that both finish, and a bad-hash player is dropped without
wedging the queue. Verified: the real `tb_server` serves back-to-back matches and
stays up. *(v1 admits sequentially — a slow handshaker briefly stalls the queue;
a real server would handshake off-thread. Daemon match threads are detached.)*

**Slice 2 ☑ — accounts (username + password) + ranked MMR.** Identity model
decided: **username + password, no email** (zero PII → sidesteps most GDPR),
hashed with **PBKDF2-HMAC-SHA256** built on the existing hand-rolled SHA-256
(`data/Password.{h,cpp}`; `sha256Raw` exposed for HMAC) — a standard KDF, no crypto
dependency; validated against **published RFC vectors** (`tb_password_demo`).
`net/AccountStore.{h,cpp}` is a **thread-safe, JSON-file-backed** store (username →
salted hash + Elo rating + W/L); the seam is a drop-in for SQLite later.
`MatchConfig.accounts` makes a server **ranked** (login required, auto-register on
first use, **Elo K=32 zero-sum** recorded on a decisive result) vs **custom** (no
store → no login), so the 4.4/custom paths are untouched. Matchmaking now pairs the
**closest-rated** waiter (degrades to FIFO unranked). Login flows through the
`hello` handshake (`user`/`pass`); the GUI reads `ATB_USER`/`ATB_PASS`; `tb_server`
persists `accounts.json`. Covered by `tb_account_demo` (auth + Elo + persistence),
`tb_ranked_demo` (auth + Elo over a real socket + wrong-password rejection), both in
CI + stress-run; the real `tb_server` recorded a live ranked result end-to-end.
⚠️ **Passwords cross the wire in the clear — put the server behind TLS (reverse
proxy) or a VPN before any public launch** (transport encryption is a separate
task).

**Slice 3 ☑ — private lobbies (join codes).** The `hello` handshake gained a
`lobby` field: **empty → open matchmaking** (rated on a ranked server); a **shared
code → a private match** with whoever else presents the same code (unrated, even on
a ranked server). `serveMatches` keeps the open pool *and* a `code → waiting host`
map; friends just agree on a code out-of-band (no server-issued code, no async), and
the client passes it (`MirrorSession`/`playClient` `lobby` param; GUI `ATB_LOBBY`).
`tb_lobby_demo` (in CI, stress-run 10×): code-sharers get a private match, and **two
rooms in flight never cross-pair** — proven by tagging each room's champions and
checking no foreign tag reaches a client's final snapshot.

**Slice 4 ◐ — GUI networking screen.** `render/ConnectScreen.{h,cpp}` (an
immediate-mode screen like the build editor): server `host:port`, optional ranked
`username`/`password` (masked), and an optional `lobby` code, with click/Tab focus
and live editing. The build editor gained a **"Play Online >"** button →
`AppState::Connect`; `main.cpp` splits the match source into `newLocalMatch()` /
`connectRemote(params)` and the fields are seeded from the `ATB_*` env vars (still
work as defaults). Built + launches; **the screen's input/visuals need in-GUI
playtesting** (can't be auto-tested). *Known gap:* `connectRemote` **blocks until
the server pairs you** — a "waiting…" screen / async connect is a follow-up.

**Main menu (mode-first).** `render/MainMenuScreen.{h,cpp}` is the landing screen
— **Local Match / Play Online / Build Editor / Settings / Quit** — and the app now
boots here (`AppState::Menu`). Local/Online carry an **intent** into the build
editor (`BuildEditorScreen::Mode`): the editor's header + primary button reflect it
(Fight vs Play Online), Edit mode is pure authoring, and a **"‹ Menu"** button
returns. A minimal read-only **`SettingsScreen`** shows content/pack/server info
(editable settings + a saved-defaults `config.json` are a follow-up). Shared UI
widgets factored into `render/Ui.h`. GUI builds + boots into the menu; **the screen
flow needs your in-GUI playtesting.**

**Remaining:** async connect + a "waiting for opponent" screen; editable Settings +
saved network defaults; move the store to **SQLite** for scale (behind the same
seam) + match-history rows; a widening-band **queue**; and **transport encryption
(TLS)** before any public, non-VPN ranked launch (passwords are currently in the
clear).

**Deployment & trust model (decided):** two tiers, one codebase.
- **Ranked → server-authoritative, self-hosted.** A persistent instance (the HP
  EliteDesk G3) is the *single source of truth*: it owns the `MatchRunner`, pins
  the **official** catalog/creatures/ruleset by hash, validates builds + every
  intent, records results/MMR, and arbitrates disputes/disconnects. Ranked *cannot*
  be P2P — self-reported results can't be trusted for rating, and an authority is
  needed to hide information (e.g. invisible units) and enforce official content.
  This extends `tb_server` into a **multi-match** daemon (accept loop → a runner +
  a DB row per match) + a queue/MMR pairing service.
- **Custom → P2P, no dedicated server needed.** Because the core is deterministic
  and the wire carries **intents, never outcomes**, each peer can run its own
  `MatchRunner` and *independently validate the other's intents* (ownership +
  `canCast`/range/AP/LOS) — neither side can force an illegal move, and both stay in
  lockstep. Worst case for a cheater is a desync/rage-quit or exploiting shared
  hidden info — acceptable when nothing is at stake (you're playing a friend).
  *Simplest first step:* **host-authoritative** ("listen server") — one player's
  client runs `serveOneMatch` in-process (loopback for itself + one remote peer),
  reusing today's code as-is; the host is trusted. *Symmetric mutual-validation*
  (both run a runner, each rejects the peer's illegal intents) is the natural
  hardening and a small delta on the existing `MatchRunner`/mirror. Custom lobbies
  pin an agreed `rules.json` by hash so both peers simulate identically.

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

## Correspondence ranked — "verify, don't host" (CR.1–CR.5 ☑ built; CR.6 ideas parked)

The live authoritative server (4.4/4.5) **works and stays** — it's the right thing
for custom lobbies, LAN, and as a low-latency relay. But for **ranked at scale**,
the deterministic core enables a lighter, "chess-like" model where the server
**verifies finished games instead of hosting live ones**. This reframes ranked and
makes replays (Phase 5) its shared foundation.

**The core idea.** A whole match is fully described by a compact **game notation**:
`ruleset hash + arena seed + both builds + the ordered intent list`. Because the
core is deterministic, that string *is* the match — anyone can re-simulate it to the
identical result. Unlike chess FEN we **don't encode the board**: the seed
regenerates the arena, so the string stays tiny (~1–2 KB, one copy-pasteable blob).
The same artifact is three things at once: a **scoresheet** (submit to rank), a
**replay** (§5.1), and a **shareable game** (paste to rewatch).

```
ATB/1  <rulesetHash>  <seed>  <format>
P: <playerBuild>
E: <enemyBuild>
1. P  m8,4  c1@10,6  .      # move; cast slot 1 @(10,6); end turn
2. E  m3,7  c0@8,4   .
...
```

**Why it dodges NAT.** A notation isn't a connectivity fix — it removes the need for
a *live* connection. Turns are short strings exchanged **correspondence-style**
through a **dumb "mailbox" relay** (POST a move to a game id, GET pending moves):
both clients connect *outward*, so NAT never applies, and the relay is a key-value
store of strings with **no game logic** — trivial to self-host (the EliteDesk), and
far lighter than a live match host.

**Trust = double-submit + re-simulation** (no crypto keys needed; fits the
username+password identity). Both players independently submit their copy of the
scoresheet, authenticated by login. The arbiter accepts a result only if the two
submissions **match** and **re-simulate cleanly**: content hash == official, both
builds legal (`validateBuild`), every intent legal (the re-sim refuses illegal
ones), and the claimed winner == the replayed winner. Then it updates MMR. A loser
can't fake a win (no matching second sheet); nobody can forge a game they didn't
play. Abandonment falls back to a start-of-game ping + timeout → forfeit.

**Two hard prerequisites:**
- **Cross-platform bit-determinism** (the linchpin — the arbiter must reproduce the
  players' result exactly). `mt19937` is portable but **`std::uniform_*_distribution`
  is not**, so two players on different C++ stdlibs regenerate *different arenas from
  the same seed*. Must hand-roll the arena RNG distribution (we hand-roll everything
  else) and pin it with a known-answer test. This is also a **latent bug today** in
  the live mirror model.
- **Perfect-information ranked.** In P2P both clients hold the full state to
  simulate, so a cheat client could reveal **invisible** units. Fix: the official
  ranked ruleset **bans invisibility-granting spells** — then the model is airtight.
  (We don't enforce hard fog today even server-side, so nothing is lost; only a live
  authoritative server with per-client filtered snapshots could ever enforce it.)

**Build order (each independently testable, headless):**
- **CR.1 ☑ Determinism lock-down.** `generateArena` now draws from `std::mt19937`
  (its uint32 sequence *is* standardised/portable) with **plain integer ops** —
  the non-portable `std::uniform_real_distribution` is gone, and the two float
  configs (coverage / obstacleRatio) become integer per-mille thresholds computed
  once, so no float touches a per-tile draw. It was the only rules-affecting
  distribution (engine resolution has no RNG). `tb_determinism_demo` (in CI) is a
  **known-answer test**: it pins `(seed → arena)` and `(seed + builds + intents →
  final snapshot)` to fixed sha256 fingerprints (captured x86-64/libstdc++) — a
  different stdlib/platform that diverges now fails loudly. Arenas re-baselined
  (no committed artifact depends on a specific one); full suite green + stable
  across runs. Hardens today's live mirror too.
- **CR.2 ☑ Game notation + verifier** (= **Phase 5.1**). `net/Replay.{h,cpp}`:
  a `GameRecord` = `{catalogHash, seed, player build, enemy build, ordered
  intents}` with a **compact single-line notation** — `ATB1 <hash> <seed>
  <playerB64> <enemyB64> m18,7 . c2@13,11 …` (moves human-readable PGN-style, builds
  base64 via `data/Base64.h`; the seat of each intent is **implicit** — the runner
  knows whose turn it is). `serializeRecord`/`parseRecord` round-trip byte-for-byte;
  `verify(rec, ruleset, catalog, creatures)` checks the content hash + build
  legality, then **re-runs `MatchRunner`** and returns `{ok, winner, finalSnapshot}`
  — illegal intents are refused by the engine, so they can't forge a result.
  `tb_replay_demo` (in CI): a recorded 20-intent match is a **303-char string**;
  `verify()` reproduces the **exact final state** of the live match (and again from
  the parsed string); wrong hash / illegal build / incomplete record are rejected.
  This one artifact is the replay, the scoresheet, and the shareable game.
- **CR.3 ☑ Mailbox relay.** `net/MailboxRelay.{h,cpp}`: a `Mailbox` (thread-safe,
  per-game **append-only log of opaque strings** — no game logic), a `serveRelay`
  TCP server (one thread per connection, post/poll requests), and a `RelayClient`
  (`post(game, sender, msg)` / `poll(game, from) → {entries, next}`). Two players
  post their move-strings and poll for the other's, so they **never open a direct
  P2P socket** — both connect *out* to the relay → NAT-immune — and it's trivial to
  self-host (the EliteDesk). `tb_mailbox_demo` (in CI): two clients relay moves over
  a real socket, a 10-message correspondence exchange stays **ordered + consistent
  across both clients**, and games are isolated. *(In-memory for now; a persistent
  store for multi-day correspondence games is a follow-up.)*
- **CR.4 ☑ Submit-to-arbiter + MMR.** `net/Arbiter.{h,cpp}`: each player submits
  `{user, opponent, seat, notation}`; the arbiter pairs the two by a stable
  `gameKey` (sorted users + catalog hash + seed), **requires the two scoresheets to
  agree** (double attestation — a loser can't submit a different sheet), re-runs
  `replay::verify()` for the authoritative winner, and records **Elo** via
  `AccountStore`. Thread-safe; decided games are remembered so they can't be
  re-ranked. **A single submission never changes a rating** (you can fabricate a
  whole notation yourself — both sides must agree). `tb_arbiter_demo` (in CI):
  agreeing double-submit ranks the game (winner +16/loser −16); disagreeing sheets,
  inconsistent seat claims, illegal (over-budget) records, and lone submissions are
  all rejected with no rating change. *(Auth of `user` is the network layer's job;
  trustworthy forfeit-on-abandon needs per-move signing — noted, out of scope.)*
- **CR.5 ☑ Perfect-info ranked ruleset.** Ships **`data/rules.ranked.json`** — the
  canonical economy with `bannedSpells: ["invisible"]` (CI `--check`ed like the
  other data files). `GameRecord` gained a **`rulesetHash`** (notation now `ATB1
  <catalogHash> <rulesetHash> <seed> …`), with `replay::rulesetHash()`
  **content-addressed** (a fixed version label, so a URL-fetched copy verifies by
  hash regardless of origin/bytes) and `verify()` rejecting a hash mismatch — so a
  scoresheet **pins which rules it was played under** and can't be cross-submitted
  (the arbiter's `gameKey` includes it too). `tb_server` gained a `[rules-file]`
  arg (`tb_server 5555 0.0.0.0 data/rules.ranked.json` hosts the official format;
  a missing explicit file fails loud) + line-buffered logs for journald.
  `tb_ranked_rules_demo` (in CI): ban loads + rejects invisibility builds (legal
  casually, rejected ranked, and at `verify()`), content-addressing holds,
  cross-ruleset submission is rejected, and a legal ranked game still ranks
  end-to-end through the arbiter. *(Gotcha caught: the canonical `data/rules.json`
  economy is stricter than the compiled fallback — ranked builds must budget
  against the data file.)*

**CR arc complete.** ✅ CR.1 determinism → CR.2 notation+verifier → CR.3 mailbox
relay → CR.4 double-submit arbiter → CR.5 official ranked ruleset. Ranked is
perfect-information v1: invisibility stays casual/custom until CR.6.

### CR.6 ◐ Hidden information in trustless ranked

**Slice 1 ☑ — the decoy mechanic (engine + content).** Option 1 below is now a
`core/` mechanic. `Effect::Type::Decoy` spawns a **full, publicly identical twin**
of the caster on a free tile and cloaks the pair (`CloakPair` in `Battle`): both
members stay Champion-kind, both block movement, both are player-driven —
**nothing in shared state says which is real**. While cloaked, damage to either
member **defers** into a hidden per-member pool (no HP change, no death, the
Damage event still narrates). **Casting from a member reveals it as the real one**
— so the reveal choice rides in the ordinary intent stream (replays/verification
need no format change); an unrevealed pair **expires to the original by rule**
(deterministic, no secret needed). At reveal the decoy quietly fades (no victory-
relevant death) and only the real member's pending damage lands (a lethal reveal
fizzles the cast and ends the match properly). Ships as the **`decoy` spell**
(id 20) — the ranked-legal stealth replacing the banned `invisible`. The beam AI
offers decoy targets like summons. `tb_decoy_demo` (36 checks, in CI) covers
spawn/indistinguishability, deferral, reveal-by-acting, the identity swap
(acting from the twin — the original fades, control + victory carry over),
expiry-defaults-to-original, and lethal reveals. Determinism KAT fingerprints
unchanged; 2000-match sim runs clean with decoys in play.

**Shipped alongside (new status-system powers + three spells, ids 17–19):**
- **`blind`** — new `StatusEffect::Kind::RangeDebuff`: `effMax = maxRange −
  (maxRange·pct)/100`, floored at `minRange` (pure integer math, stacks capped at
  100%). Blind is −60% for two victim turns.
- **`surge`** — new `StatusEffect.delay` (a status inert for N owner-turns, then
  active — a generic *delayed payload* for modders): +2 AP for 2 turns, **then**
  −6 AP for 1 turn (AP/MP resets now floor at 0).
- **`flux`** — new `Effect.polarized` flag: the magnitude flips sign against the
  caster's foes. One spell = +2 MP to an ally / −2 MP to an enemy (1 turn).
All three are data-driven (JSON: `status.delay`, effect `polarized`, `decoy`
type; strict validation; omitted-when-default so existing files are byte-stable).
*(Follow-ups: the beam AI never casts Blind/Surge/Flux — like Portal — and plays
Decoy naively, so all four sim "too weak"; Rewind on a cloaked member is an
untested edge; hard fog of the pending pools is inherently client-side.)*

**Slice 2 ☑ — the commitment layer (correspondence ranked).** The notation gained
**`#commit:choice:nonce` tokens** (one per decoy cast, in cast order; `commit =
sha256(choice ":" nonce)`, `choice` = `a` stay-original / `b` become-the-twin;
`replay::makeCommitment()` is the shared formula). `verify()` now re-simulates
while diffing the live `cloakPairs()` across intents: the Nth pair binds the Nth
commitment; **a member that acts must be the committed one**, and a pair left to
**expire reveals the original by rule — so a `b` commitment that expires FAILS**
(that is precisely the dodge-the-damage cheat: commit to the twin, watch where the
AoE lands, chicken out). Tampered hashes, missing commitments (ranked default:
required) and surplus commitments all fail; pairs still cloaked at game end are
choice-exempt (the secret never resolved) but must still hash-verify; casual
replays may pass `requireCommitments=false`. Engine + intent stream untouched —
the layer is entirely notation + verifier. **Timeliness** (that the commit was
shown at cast, not invented post-hoc) is attested by **double-submit**: the
opponent won't co-sign a scoresheet whose commitments they never saw in play.
`tb_commit_demo` (23 checks, in CI): honest `a`, honest swap-`b`, and honest
expiry all verify; the lie, the chicken-out, the bad hash, the missing and the
surplus commitment are all rejected; notation round-trips byte-identically.
*(Remaining for a full in-game flow: the client generating a commitment at cast +
shipping it with the move over the relay — protocol UX, not trust machinery.)*

#### Design options considered (for reference)

An altered client can never be prevented from *seeing* state it must simulate; the
options are to **remove** the secret (CR.5, done), make it **cryptographic**, or
make it **symmetric**. Candidate designs, roughly cheapest-first:

1. **Decoy invisibility (Scotland-Yard style) — likely the sweet spot.** Casting
   spawns a **real and a decoy entity, both visible**; the caster commits
   `hash(whichIsReal + nonce)` at cast time. Both are physical entities in the
   engine (both block movement/collide identically), so **the opponent's legal
   move-space needs no secret** — determinism and P2P simulation just work. Damage
   to either is **deferred** (tracked per-entity, applied at reveal — the
   "damage updates when invisibility ends" idea). Reveal = publish the nonce;
   commitment makes retroactive swapping impossible. Epistemic stealth, zero
   protocol change beyond one commitment token in the notation.
2. **Commit-reveal movement (Diplomacy-style).** True hidden movement: while
   invisible, broadcast `hash(move+nonce)` instead of the move; reveal at
   stealth-end and retro-resolve (opponent AoE vs committed path). Requires the
   hidden unit to **ghost** (not block/collide) while hidden — otherwise the
   opponent can't compute their own legal moves — i.e. a real gameplay change, and
   acting must break stealth.
3. **ZK proofs (the Dark Forest precedent).** Each hidden move ships a
   zero-knowledge proof of legality w.r.t. the secret position (zkSNARKs; the
   Ethereum game *Dark Forest* does exactly this for fog-of-war). The "correct"
   cryptographic answer; heavyweight dependency, real R&D.
4. **Fog-oracle relay.** The mailbox relay (CR.3) holds *only* the hidden bits and
   answers hit-queries — a minimal trusted component that can't decide outcomes
   (everything is committed + audited at reveal), only leak. Pragmatic middle
   ground on self-hosted infra.

**Adjacent anti-cheat (protocol can't fix, detection can):** engine assistance —
a client quietly consulting the beam search (chess's classic problem). We *have*
the reference engine: compute per-move loss vs `defaultBrain()`/`evalState` over
submitted scoresheets (chess.com-style accuracy screening), plus move-time stats
from relay timestamps. The relay's append-only log also doubles as the **match
clock + abandonment witness** (its attested log ranks a walkover). Collusion/
boosting remains a social/graph-detection problem — no protocol fixes it.

Relationship to what's built: 4.4/4.5's live server is retained (custom/LAN/relay);
CR.2 *is* Phase 5.1; `MatchRunner` is the verifier; `AccountStore` records the
ranking. No `core/` rewrite — this is packaging + a determinism fix.

---

## Phase 5 — Replays & spectate (mostly free)

### 5.1 ☑ Persist `seed + intents`; re-simulate to play back (no frame recording). **Done as CR.2 (`net/Replay.{h,cpp}`, `tb_replay_demo`) — one notation serves replay, ranked submission, and shareable games.** *(Remaining for a GUI replay viewer: step the re-sim through the renderer.)*
### 5.2 ☐ Spectate = subscribe to the same snapshot stream.
### 5.3 ☐ Ongoing balance backlog (fireball radius, portal AI, synergy tuning via `tb_balance`).

**Acceptance:** a finished match can be replayed deterministically from its
stored `seed + intents`.
