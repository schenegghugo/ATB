# Milestones

Execution ledger for ATB — phases broken into one-line steps.
Design rationale lives in [`ARCHITECTURE.md`](ARCHITECTURE.md).
Legend: **☑ done · ◐ in progress · ☐ todo**.

---

## What's left (quick view)

- ☐ TLS transport encryption — the gate before public launch.
- ☐ SQLite behind the account-store seam; match-history rows.
- ◐ W.5 itch.io release — web bundle built + browser-verified; upload pending.
- ☐ Self-teaching AI (3.5.1–3.5.7) — NNUE-style learned evaluator.
- ◐ CR.6d deeper hidden info — commit-reveal movement, ZK (parked).
- ☐ 5.3 balance backlog — fireball radius, unused-spell AI, synergies.
- ☐ Sprite art: ground effects, status markers, editor skill icons.
- ☐ Roster follow-ups: enemy-only Pull; summon `duration`.

---

## Phase 0 — Contributor-safe public repo

- ☑ **0.1** `TB_BUILD_GUI` option — headless core builds without Raylib.
- ☑ **0.2** Every test exits non-zero on failure, gating CI.
- ☑ **0.3** CI workflow: headless build plus every demo binary.
- ☑ **0.4** GUI compile-only job on Linux.
- ☑ **0.5** Branch protection on `main`: PR plus green CI.

## Phase 1 — Data-driven catalog

- ☑ **1.1** Hand-rolled JSON reader/writer (`data/Json`, `tb_json_demo`).
- ☑ **1.2** Enum↔string tables, compile-time-checked (`data/SpellEnums.h`).
- ☑ **1.3** Strict JSON ↔ `SpellCatalog`, all errors reported (`tb_catalog_demo`).
- ☑ **1.4** Catalog generator plus SHA-256 pin; `data/catalog.json` canonical.
- ☑ **1.5** Round-trip and validation tests; CI validity gate.
- ☑ **1.6** App loads catalog under a valid/absent/malformed policy.

## Spells & economy

- ☑ **S.1** Rewind spell — snapshot, restore two turns later, no-revive.
- ☑ **S.2** Initiative as a build buy; editor `±` stepper.

## Roster — bombs & summons

- ☑ Entity foundation: kinds, mid-battle spawns, victory rule (`tb_roster_demo`).
- ☑ Bombs — fuse, ignition, blast; cast via `bomb` spell.
- ☑ Summons — blocker/healer/brute, single-purpose AI, per-team cap two.
- ☑ Creatures datafied in `data/creatures.json` (`tb_creature_demo`).
- ☐ Enemy-only Pull; summon `duration`. (Unused-spell AI: see 5.3.)

## Build editor

- ☑ **BE.0** Balance-sim crash fix (catalog-sized vectors).
- ☑ **BE.1** Free-form spell `tags` plus a consistency test.
- ☑ **BE.2** Resizable window, filterable card grid, `+INIT` stepper.

## Core split

- ☑ **CS.1** `Battle.h` split into Combat/Entity/Battle, behaviour-preserving.

## Match rulesets (datafied)

- ☑ **R.1** `core/Ruleset` plus `data/rules.json`; storm config datafied.
- ☑ **R.2** Unified `buildMatch()` for game and balance sim.
- ☑ **R.3** Team formats 2v2 / 3v3 in the editor.
- ☑ **R.4** Static maps (`data/maps/`) with a reachability gate.
- ☑ **R.5** Ban enforcement in editor and sim; ranked tie-in.

## Phase 2 — Spell bar + sprite packs

- ☑ **2.1** Clickable spell bar with visual states.
- ☑ **2.2** Atlas-based sprite-pack seam (`ATB_PACK`, `tb_pack_demo`).
- ☑ **2.3** Structured event stream plus GUI combat log (`tb_event_demo`).
- ☑ **2.4** Animation clip model; event-driven cast flash.
- ☑ **2.5** Default and example packs; `docs/sprite-packs.md`.
- ☐ Ground-effect and status-marker art; editor skill icons.

## Phase 3 — Pluggable AI

- ☑ **3.1** `Brain` interface; beam search is the default.
- ☑ **3.2** Brain registry with by-name selection (`ATB_BRAIN`).

## Phase 3.5 — Self-teaching AI ☐

- ☐ **3.5.1** `Evaluator` seam under `Brain` (`HandcraftedEvaluator`).
- ☐ **3.5.2** Versioned `featurize(Battle, Faction)` feature extraction.
- ☐ **3.5.3** Self-play training-data export from the sim.
- ☐ **3.5.4** Offline PyTorch training; hash-pinned weights artifact.
- ☐ **3.5.5** Dependency-free C++ `LearnedEvaluator` inference.
- ☐ **3.5.6** Gated improvement loop — promote above ~55% win-rate.
- ☐ **3.5.7** Optional NNUE incremental accumulator; int8 quantization.

## Phase 4 — Networked PvP

- ☑ **4.1** Wire formats: Intent/Snapshot/build round-trips (`tb_net_demo`).
- ☑ **4.2** `MatchSource` seam between UI and battle driver.
- ☑ **4.3** In-process authoritative `MatchRunner` (`tb_loopback_demo`).
- ☑ **4.4** TCP transport, `tb_server`, deterministic-mirror client, `ATB_CONNECT`.

### Phase 4.5 — Lobby, accounts, Online Home

- ☑ Multi-match server: FIFO pairing, configurable bind address.
- ☑ Accounts: PBKDF2 passwords, Elo, JSON store; ranked/custom.
- ☑ Private lobbies via shared join codes.
- ☑ GUI connect screen — now the lobby login.
- ☑ Seek board plus directed challenges, live routing (`tb_lobby_challenge_demo`).
- ☑ Correspondence over the lobby: Mailbox, Arbiter, reconnect resume.
- ☑ GUI lobby and correspondence play (`render/LobbyScreen`).
- ☑ `tb_lobby` daemon — the self-hosted Online Home.
- ☑ Persistence: game registry and Mailbox journal survive restarts.
- ☑ Cold resume: `myCorrGames`, log replay, client-side decoy secrets.
- ☑ Quick-match queue on a widening Elo band (`tb_queue_demo`).
- ☑ Async connect; cancellable waiting-for-opponent screen.
- ☐ SQLite store plus match-history rows.
- ☐ TLS transport encryption.
- ☑ **UX** Menu → login → lobby; "Edit build" inside the lobby.
- ☑ **UX** Server-validated per-match ready check (`tb_ready_check_demo`).
- ☑ **UX** Idle forfeit, two-clock HUD, true chess bank (`tb_chess_clock_demo`).
- ☑ **UX** In-match chat: async relay, split log column (`tb_chat_demo`).
- ☑ Bug fix — pump every frame so moves register.
- ☑ **UX** `tb_lobby` warns when started without `./data`.
- ☑ **UX* End-of-match screen; editor ‹Menu returns where opened.

### Phase 4.6 — Chat ☑

- ☑ Lobby-wide chat: capped rolling log plus GUI panel.
- ☑ Correspondence chat in a side log — move log stays pure.
- ☑ Safety levers: rate limit, mute, length cap (`tb_lobby_chat_demo`).

## Correspondence ranked — "verify, don't host"

- ☑ **CR.1** Cross-platform determinism lock-down (`tb_determinism_demo` KAT).
- ☑ **CR.2** Game notation plus verifier — replays by re-simulation.
- ☑ **CR.3** NAT-immune store-and-forward mailbox relay (`tb_mailbox_demo`).
- ☑ **CR.4** Arbiter: double-attestation submits move Elo (`tb_arbiter_demo`).
- ☑ **CR.5** Perfect-info ranked ruleset, content-addressed hash (`tb_ranked_rules_demo`).
- ☑ **CR.6a** Decoy mechanic: cloak pairs, blind/surge/flux (`tb_decoy_demo`).
- ☑ **CR.6b** Commitment layer in notation and verifier (`tb_commit_demo`).
- ☑ **CR.6c** Commitments minted at cast; GUI choice prompt (`tb_correspondence_demo`).
- ☐ **CR.6d** Deeper: commit-reveal movement, ZK proofs (parked R&D).

## Phase 5 — Replays & spectate

- ☑ **5.1** Replays: seed plus intents re-simulated; GUI viewer.
- ☑ **5.2** Spectate: watchers mirror the logged stream (`tb_spectate_demo`).

## Parallel track — Web / WASM ◐

- ☑ **W.1** Emscripten toolchain (emsdk 6.0.2); web Raylib (`PLATFORM=Web`).
- ☑ **W.2** Frontend loop → `emscripten_set_main_loop_arg`; fixed-size canvas
  (raylib's resizable-canvas path skews mouse coords on web).
- ☑ **W.3** `--preload-file` bakes `data/` + `packs/default` into the bundle.
- ☑ **W.4** `emcmake` CMake path (online play compiled out — no TCP in browsers);
  `web-build` CI job packages the itch.io zip artifact.
- ◐ **W.5** itch.io publish: `build-web/tactical-battler-web.zip` built and
  verified playable headless (menu → editor → battle vs AI). Upload pending —
  HTML5 project, viewport 1180×720.
