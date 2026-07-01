# Tactical Battler — POC

A Dofus-style tactical turn-based combat loop in **C++20** + **Raylib**.
Gameplay logic is fully **headless** and decoupled from rendering.

## Architecture

```
src/
  core/              ← headless engine (no Raylib, no DB, cache-friendly data)
    Grid.{h,cpp}     ← TileType, Grid, procedural gen + BFS validation,
                       BFS pathfinding, reachable-flood, Bresenham LOS
    Battle.{h,cpp}   ← roster of Entities by EntityId, initiative order, status
                       effects, data-driven Spell/Effect pipeline, forced movement
    Spells.{h,cpp}   ← the skill DICTIONARY (catalog) + build-point costs
    Build.{h,cpp}    ← classless point-buy CharacterBuild, validation, hydrate
    AI.{h,cpp}       ← enemy decision logic (scores its loadout, single-step)
  data/              ← persistence seam (still no DB dependency)
    BuildRepository.h        ← abstract store + In-Memory + flat-File impls
    schema.sql               ← SQLite/Postgres design the seam targets
  render/
    Renderer.{h,cpp}          ← Raylib battle rendering (primitives)
    BuildEditorScreen.{h,cpp} ← Raylib build-editor UI (immediate-mode)
  main.cpp           ← app state machine: Editor screen ⇄ Battle screen
tests/
  headless_demo.cpp  ← AI-vs-AI battle (builds from catalog), CI smoke test
  build_demo.cpp     ← catalog→validate→persist→instantiate round-trip
```

`core/` + `data/` build into a static lib (`tb_core`) with **zero graphics and
zero database deps**, so they run on a server or in tests unchanged.

### Skills & classless builds

Spells are **data** in a catalog (the "dictionary of skills"), each with a
build-point cost. Characters are **point-buy and classless**: a budget is spent
across skills *and* stat upgrades, so a glass-cannon caster and a durable bruiser
are just different spends — no classes. The POC ships 7 spells (attack, fireball
AoE, poison DoT, knockback push, harpoon pull, bulwark shield, mend heal).

```
Battle ── uses ── Spell (combat data only)
  ▲
  │ instantiate(build, catalog)
  │
CharacterBuild ── ids ──▶ SpellCatalog (SpellDef = Spell + cost)   [DB: spells]
   (budget, stats)                                                 [DB: builds]
        │
        └── persisted via BuildRepository ──▶ In-Memory │ File │ (SQLite)
```

The gameplay core never sees the database; it depends only on the
`BuildRepository` interface. Swapping the flat-file store for SQLite means adding
one implementation against `data/schema.sql` — **no changes to `core/`**.

### Combat model (roster edition)

Entities live in an **append-only `std::vector<Entity>`** addressed by a stable
`EntityId` (a raw index — never erased, so it's permanent). Turn order is an
**initiative list**; `Phase` is derived from the active unit's team. The four
extension hooks are in place for the skills system:

1. **Turn order** — `order_` + `turnIdx_`; advances to the next living unit.
2. **Status effects** — `StatusEffect` container per entity, ticked at turn start
   (DoT applies, buffs feed the AP/MP reset, shields absorb damage).
3. **Shared targeting** — `affectedTiles(spell, caster, target)` → `unitsAt(...)`,
   covering Single / Line / Cross / Circle shapes.
4. **Forced movement** — `applyForcedMove(id, dir, dist)` for push/pull, with
   collision damage; out-of-turn by design.

Spells are **data** (`Spell` = AP cost + range + shape + a list of `Effect`s), so
new content needs no state-machine changes. The POC ships an 11-spell catalog
(see `makeDefaultCatalog`) exercising every `Effect` type — e.g. the basic
attack is 3 AP, range 1–6, LOS, 15 damage.

## Build & Run

### CMake (recommended — fetches Raylib automatically if not installed)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

./build/tactical_battler   # graphical game
./build/tb_headless        # headless deterministic battle log
./build/tb_build_demo      # catalog + build validation + persistence round-trip
./build/tb_spells_demo     # asserts cooldowns + shelter/glyph/portal/invisible
./build/tb_balance [N] [seed]       # per-spell Monte-Carlo balance report (txt + html + csv)
./build/tb_team_balance [N] [seed]  # NvN team-composition report (set ATB_TEAM=2/3)

scripts/balance.sh -n 30000 --map duel     # friendlier wrapper (builds + runs)
scripts/team_balance.sh --team 2 -n 8000   # 2v2 composition analysis
```

For a **headless / server / CI build**, add `-DTB_BUILD_GUI=OFF`: this skips
Raylib entirely (no fetch, no GL/X11) and builds `tb_core` plus the demos in
seconds. This is exactly what the CI pipeline runs.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DTB_BUILD_GUI=OFF
cmake --build build -j
```

### Direct compile (headless core only, no Raylib needed)

```bash
g++ -std=c++20 -O2 -Isrc \
    src/core/*.cpp src/data/*.cpp tests/headless_demo.cpp -o tb_headless
./tb_headless
```

## Screens

The app opens on the **Build Editor** and flips to the **Battle** when you hit
*Fight* (Tab returns to the editor).

### Build Editor

Click skills in the dictionary to add/remove them, use `-`/`+` to buy stat
upgrades, and watch the budget bar turn red when you overspend (invalid builds
can't be saved or fought). Click the name field to rename, **Save** to persist
through the repository, cycle **Enemy** to choose the opponent preset, and
**Fight** to start.

### Battle controls

| Input         | Action                                              |
|---------------|-----------------------------------------------------|
| Left click    | Move active unit along shortest path (1 MP / tile)  |
| `1`–`9`       | Select a spell from the active unit's loadout       |
| Right click   | Cast the **selected** spell at the hovered tile     |
| Space / Enter | End player turn                                     |
| R             | Rematch (new arena, same builds)                    |
| Tab           | Return to the build editor                          |
| Esc           | Quit                                                |

Blue tint = reachable tiles. Orange tint = the selected spell's blast zone (red
if it can't be cast there). The sightline to the cursor is gold when clear, red
when a Wall blocks it. Pink ticks above a unit are active status effects.

## Rules implemented

- **Grid** 20×15, tiles: `Walkable`, `Wall` (blocks move + LOS), `Obstacle`
  (blocks move, transparent to LOS).
- **Procedural gen** ~18% coverage, with a **BFS reachability gate** that
  regenerates instantly until player↔enemy are connected.
- **Resources**: HP / max-AP / max-MP, reset at the start of each unit's turn.
- **Turn loop**: `PlayerTurn ⇄ EnemyTurn → Finished`.
- **Movement**: 1 MP per tile, BFS shortest path.
- **Spells**: data-driven (AP cost + cooldown + Manhattan range + LOS + shape +
  effect list); shapes Single / Line / Cross / Circle; effects Damage / Heal /
  Push / Pull / ApplyStatus / Spawn. Forced movement deals collision damage.
- **Cooldowns**: a spell with cooldown N can't be recast by the same caster for N
  of its turns — so power spells can't be spammed every turn.
- **Status effects**: damage-over-time, shields (absorb), AP/MP buffs, and
  invisibility (hides you from enemy AI), ticked at the owner's turn start.
- **Ground effects** (persistent, duration-based battlefield features): **Shelter**
  conjures temporary walls (block movement + LOS), **Glyph** lays a repel-on-enter
  trap zone, **Portal** teleports anyone stepping on the entry to the exit.
- **Builds**: classless point-buy from the skill catalog + stat upgrades,
  validated against a budget, persisted via a swappable repository.
- **Closing ring**: from round 5 the safe square around the arena centre shrinks
  one ring per round; units caught outside bleed each turn, forcing conflict. The
  planner is ring-aware and flees inward, so stalemates collapse into fights.

## Balance tuning

`tb_balance [matches] [seed] [outfile]` runs thousands of AI-vs-AI matches
between randomly generated point-buy builds — the loop for tuning costs and the
stat economy. Each run writes three siblings from `<outfile>` (default
`balance_report.txt`): the **text report** (also echoed to stdout), a
self-contained **`.html`** with charts (inline SVG, no dependencies), and
machine-readable **`.csv`** tables (`.spells` / `.pairs` / `.length` /
`.outcomes`) you can open in a spreadsheet. It uses the **same data the game
does**: `data/catalog.json`, `data/creatures.json`, and `data/rules.json`
(format, economy, banned spells, arena), so editing those files drives the
report. Point it at a different content set with `ATB_DATA_DIR=<dir>`, a specific
battlefield with `ATB_MAP=<key|path.json>` (or `''` for random), and a team size
with `ATB_TEAM=<n>` (1v1, 2v2, 3v3 — the per-spell stats hold for any format).

The friendlier wrapper `scripts/balance.sh` builds the binary and exposes these
as flags — `scripts/balance.sh --help`:

```bash
scripts/balance.sh -n 30000 -s 42            # 30k matches on the ruleset's arena
scripts/balance.sh --map duel                # test a specific static map
scripts/balance.sh --team 2 -n 10000         # 2v2 instead of 1v1
scripts/balance.sh --data /tmp/mymod --map arena -o mymod.txt
```

The report opens with a plain-English **SUMMARY** (which spells look too strong /
too weak, the first-move edge, typical game length), then:

- **Outcomes** — first- vs second-actor win rate + draw rate (95% Wilson CIs).
- **How decisive matches end** — spell kill / ring (storm) / collision split.
- **Match length** — min/median/mean/p90/max + a histogram.
- **Per-spell** — pick rate, win rate ± CI, `lift` (vs 50%), `val/pt` (cost
  efficiency), and a plain-English **verdict** (`balanced` / `TOO STRONG` /
  `too weak` / `niche` / `AI-unused`); non-overlapping CIs mean a real gap.
- **Stat investment** — win rate by stat-point spend and per stat bought
  (HP / AP / MP / **Init**).
- **Top synergies** — spell pairs by joint win rate and lift `vs solo-avg`
  (surfaces emergent combos the planner finds).

Re-run it after any balance change to see the effect, deterministically per seed.

### Team composition (2v2 / 3v3)

`tb_team_balance` (wrapper `scripts/team_balance.sh`) is the NvN sibling. Instead
of per-spell stats it **auto-classifies each build into an archetype** — Aggro /
Control / Summoner / Support / Evasion — and reports win rate by **team
make-up** plus a composition-vs-composition **matchup matrix** (HTML heatmap up to
2v2; CSV always). Answers "does Summoner+Support beat two Aggro builds?"

```bash
scripts/team_balance.sh --team 2 -n 8000     # 2v2 composition report
scripts/team_balance.sh --team 3 --map duel  # 3v3 on a fixed map
```

The AI is a **turn-level planner**, not a greedy picker: it beam-searches
sequences of actions within the AP/MP budget (cloning the `Battle` and
simulating each), scoring the *end-of-turn state* with `evalState()`. That state
heuristic banks DoT as near-dealt damage and drops a unit's incoming-damage term
to zero when it's invisible — so the planner discovers combos like **poison →
go invisible** (deal damage-over-time while taking none) when the foe out-threats
your follow-up attack. An asymmetric **aggression** term (BFS path distance to
the nearest foe) pulls units into combat instead of a mutual standoff.

Sample signal (3000 matches, fireball repriced 4→3 pt, closing ring on):

```
bulwark  62.7% 2pt   mend      58.8% 2pt   invisible 55.7% 3pt   knockback 54.4% 2pt
attack   53.3% 1pt   poison    50.6% 3pt   harpoon   46.0% 2pt   fireball  43.4% 3pt
glyph    42.0% 3pt   shelter   41.7% 3pt   portal    41.1% 3pt
```

Reads: the **closing ring** crushed the draw rate (7.6% → 0.5%) and shortened
matches, and its chip damage lifts survival tools — **bulwark/mend** now top the
table. **Fireball** at 3 pt recovered from ~35% to ~43% (still the weakest
attack, a candidate for a radius buff next). **Portal** stays AI-unused — its
step-on-entry mechanic needs deeper planning than the beam chases.

## Design & contributing

[`ARCHITECTURE.md`](ARCHITECTURE.md) is the design contract: the headless-core
boundary, the data-driven content model, the trust model for moddable
content, sprite/asset packs, and the server-authoritative PvP plan. Read it
before extending the engine or adding content.
[`MILESTONES.md`](MILESTONES.md) is the phased execution plan and the current
status of each piece.

Every pull request is gated by CI (`.github/workflows/ci.yml`): it builds the
headless core (`-DTB_BUILD_GUI=OFF`) and runs the demo/assertion suite, so a
change that breaks a test can't be merged. Run the same checks locally before
opening a PR:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DTB_BUILD_GUI=OFF
cmake --build build -j
./build/tb_headless && ./build/tb_build_demo && ./build/tb_spells_demo && ./build/tb_ai_demo
```

## License

[MIT](LICENSE) © 2026 Hugo Schenegg.
