# Contributing to ATB

Thanks for being here. ATB is an MIT-licensed, headless tactical-combat engine
built to be **hacked on** — new spells, sprites, AIs, even whole frontends. This
guide covers how to build it, how the codebase is organised, and how to get a
change merged.

Before touching the engine or adding content, skim
[`ARCHITECTURE.md`](ARCHITECTURE.md) — it's the design contract.
[`MILESTONES.md`](MILESTONES.md) shows what's built and what's planned.

## The one architectural rule

**Dependencies only point downward: `render/` → `data/` → `core/`.**

`core/` (the engine) has **zero graphics and zero database dependencies** — no
Raylib, no sockets, no file I/O in combat resolution. This is what lets the same
code run in the game, in tests, in the balance simulator, and (later) in a
match server. If you want to `#include` Raylib or a socket library from `core/`,
the thing you want belongs in a higher layer. PRs that breach this boundary will
be asked to refactor.

A related invariant: **combat resolution is deterministic.** Same initial state +
same actions ⇒ identical outcome (arena gen takes an explicit seed). Don't
introduce wall-clock or RNG reads into `core/` resolution — it's what makes
replays, server validation, and reproducible balance runs possible.

## Getting set up

Headless build (core + demos, no Raylib — fast, this is what CI runs):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DTB_BUILD_GUI=OFF
cmake --build build -j
```

Full graphical build (fetches Raylib automatically if not installed):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/tactical_battler
```

Run the test/demo suite — **do this before opening a PR**, it's the same gate CI
enforces:

```bash
./build/tb_headless && ./build/tb_build_demo && ./build/tb_spells_demo && ./build/tb_ai_demo
./build/tb_balance 200 1   # deterministic simulator smoke
```

## Ways to contribute

| Want to… | Where | Notes |
|----------|-------|-------|
| **Add / tweak a spell** | the catalog (`makeDefaultCatalog`, later `data/catalog.json`) | Pure data — combine existing `Effect`s. No engine change. |
| **Draw sprites / re-theme** | an atlas-based sprite pack (`render/`, see ARCHITECTURE §6) | One `atlas.png` + a `pack.json` mapping keys → atlas rects (+ optional animations). Client-only, no engine recompile. Great first contribution for artists. |
| **Write an AI** | `AI.cpp` (a pluggable `Brain` interface is planned) | Beam-search planner today; self-contained in `core/`. |
| **Build a new frontend** | against the `Battle` read API | The engine never needs to know your frontend exists. |
| **Tune balance** | `tb_balance` + spell costs | Back your change with before/after numbers from the simulator. |
| **Add a new _mechanic_** | extend the `Effect`/`StatusEffect`/`GroundKind` vocabulary in `core/`, then resolve it in `Battle` | A real engine change — expect closer review. |

### Good first issues

- **Fireball radius buff** — it's the weakest attack (~43% win rate); tune it in
  the catalog and confirm the shift with `tb_balance`.
- **Teach the AI to use Portal** — currently unused; its step-on-entry mechanic
  needs deeper planning than the beam search reaches (`AI.cpp`).
- **Add a new spell** from existing effects — pure catalog data, no engine change.

## Pull request process

1. **Branch** off `main` (direct pushes to `main` are blocked by a ruleset).
2. Make your change; keep it focused — one logical change per PR.
3. **Run the headless build + test suite locally** (commands above).
4. Open the PR. CI (`.github/workflows/ci.yml`) builds the headless core and runs
   the suite; **the `Headless core + tests` check must be green to merge.**
5. Resolve review conversations; PRs merge as a **squash** (one tidy commit on
   `main`).

If your change affects balance, include the relevant `tb_balance` numbers. If it
touches an architectural seam, note it against `ARCHITECTURE.md` so the contract
stays honest.

## Code style

- C++20. Match the surrounding code — naming, comment density, and idiom.
- `core/` must compile clean under `-Wall -Wextra` (it's on for `tb_core`).
- Prefer data over new code paths (a new spell should be data, not an `if`).
- Comments explain *why*, not *what*; the existing files are a good template.

## Web / WebAssembly builds (help wanted)

Because the **core is pure, portable C++20** (no platform syscalls in combat
resolution) and the **frontend uses Raylib** — which has first-class
**Emscripten / HTML5** support — ATB should compile to **WebAssembly** and run
**in a browser**, with no engine changes. That means a playable web build you can
drop straight onto **itch.io** (or any static host) as an HTML5 game — the
lowest-friction way for people to try the sandbox.

What this takes (a great, self-contained contribution):

- Build the Emscripten toolchain (`emsdk`) and configure Raylib for the web
  target (`-DPLATFORM=Web`); the build emits `.wasm` + `.js` + `.html`.
- Adapt the frontend main loop: browsers don't allow a blocking
  `while (!WindowShouldClose())`, so the per-frame body moves into a callback
  driven by `emscripten_set_main_loop`. The `core/` engine is untouched.
- Package assets (e.g. sprite packs) with `--preload-file` into the virtual FS.
- Add a `TB_PLATFORM_WEB` / Emscripten path to `CMakeLists.txt` alongside the
  existing native build, ideally wired into CI so the web build can't rot.

The headless core already proves the logic is platform-independent; the web
target is mostly a frontend + build-system exercise. If you want to take it on,
open an issue first so we can scope the CMake/toolchain shape together.

## License

By contributing, you agree your contributions are licensed under the project's
[MIT License](LICENSE).
