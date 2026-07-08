# Milestones

Execution ledger for ATB — **what we did** and **what's left**, one line each.
Design rationale lives in [`ARCHITECTURE.md`](ARCHITECTURE.md); this file is the
*checklist*. Legend: **☑ done · ◐ in progress · ☐ todo**.

**Where we are:** the single-player game, content pipeline (data-driven catalog /
creatures / rulesets, all hash-pinned), pluggable AI, sprite packs, and the full
**networked PvP + Online Home** are built and playtest-confirmed — lobby daemon,
seek board + directed challenges, per-match ready check, live *and* correspondence
matches, idle-forfeit clock, in-match chat, and the "verify-don't-host"
correspondence-ranked arc (determinism → notation/verifier → relay → arbiter →
ranked ruleset → decoy hidden-info). What remains is hardening (TLS, persistence,
SQLite), reach (Web/WASM), and depth (self-teaching AI) — see below.

---

## What's left (quick view)

- **Online hardening (4.5/4.6/6 follow-ups)** — the tier that makes ranked
  publicly deployable:
  - ☐ **TLS / transport encryption** — *the* gate before any public, non-VPN launch
    (passwords cross the wire in the clear today; fine behind LAN/VPN).
  - ☐ **Persistence** — lobby store + `Mailbox` to disk (survive a server restart) +
    correspondence **cold-resume** (replay the log; client-side secret persistence
    for decoy games).
  - ☐ **SQLite** behind the account/store seam + match-history rows.
  - ☐ **Chat**: lobby (pre-match) + correspondence-game chat (4.6) — in-match *live*
    chat is done.
  - ☐ **True chess clock** — accumulating time bank (today "chess" = a per-move cap).
  - ☐ Async connect + "waiting for opponent" screen; widening-band **queue**; GUI
    **decoy-choice prompt**.
- **Parallel / depth (off the critical path)**
  - ☐ **Web/WASM build** (W.1–W.5) — browser-playable on itch.io.
  - ☐ **Self-teaching AI** (3.5.1–3.5.7) — NNUE-style learned evaluator.
  - ◐ **CR.6 deeper hidden-info** — commit-reveal movement / ZK (slices 1–3 done).
  - ☐ **Spectate** (5.2) — subscribe to the snapshot stream.
- **Polish / content**
  - ◐ **0.6** — file 2–3 good-first-issues (CONTRIBUTING.md done).
  - ☐ **Balance backlog** (5.3) — fireball radius; teach AI to cast
    Portal/Blind/Surge/Flux (unused); synergy tuning via `tb_balance`.
  - ☐ Initiative `±` in the build-editor GUI; sprite-pack ground-effect/status art;
    a GUI replay viewer.

---

## Phase 0 — Contributor-safe public repo

- ☑ **0.1** `TB_BUILD_GUI` CMake option — headless core builds without Raylib.
- ☑ **0.2** Every test fails loudly (non-zero exit gates CI).
- ☑ **0.3** CI workflow (`.github/workflows/ci.yml`) — headless build + all demos.
- ☑ **0.4** GUI compile job (Linux, compile-only).
- ☑ **0.5** Branch protection ruleset on `main` (PR + green CI required).
- ◐ **0.6** `CONTRIBUTING.md` ☑ (build/test, `core/` rules, WASM "help wanted");
  ☐ file 2–3 good-first-issues on GitHub.

## Phase 1 — Catalog loader (data-driven content)

- ☑ **1.1** `data/Json.{h,cpp}` — hand-rolled JSON reader/writer (`tb_json_demo`).
- ☑ **1.2** `data/SpellEnums.h` — enum↔string tables, compile-time-checked.
- ☑ **1.3** `data/CatalogJson.{h,cpp}` — strict JSON ↔ `SpellCatalog`, all-errors.
- ☑ **1.4** `makeDefaultCatalog()` → generator (`tb_catalog_gen`) + `data/Sha256`
  (the PvP trust anchor); `data/catalog.json` is canonical.
- ☑ **1.5** Round-trip + validation tests + CI validity gate (`tb_catalog_demo`).
- ☑ **1.6** App loads `data/catalog.json` via `render/ContentPaths` with a safe
  valid/absent/malformed policy.

## Spells & economy (alongside Phase 1)

- ☑ **S.1** Rewind spell (snapshot → restore at 2nd turn; no-revive).
- ☑ **S.2** Initiative as a build buy (`bonusInitiative`); ☐ expose `±` in editor GUI.

## Roster entities — bombs & summons

- ☑ Foundation: `EntityKind`, mid-battle `spawnEntity`, `Control`, `onDeath`,
  victory = no living Champion (`tb_roster_demo`).
- ☑ **Bombs** — `Object` template, fuse/ignition/blast, cast via `bomb` spell.
- ☑ **Summons** — blocker / healer / brute, single-purpose AI, per-team cap 2.
- ☑ **Creatures datafied** — `data/creatures.json` + `data/CreatureJson`, shared
  `data/SpellJson` mapping (`tb_creature_demo`).
- ☐ AI never casts Blind/Surge/Flux/Portal; enemy-only Pull; summon `duration`.

## Build editor revamp

- ☑ **BE.0** Balance-sim crash fix (catalog-sized vectors).
- ☑ **BE.1** Free-form spell `tags` + consistency test.
- ☑ **BE.2** Bigger resizable window, filterable card grid, `+INIT` stepper.

## Core split

- ☑ **CS.1** Split `Battle.h` → `core/Combat.h` / `Entity.h` / `Battle.h`
  (behaviour-preserving).

## Match rulesets (datafied)

- ☑ **R.1** `core/Ruleset.h` + `data/rules.json` (`tb_ruleset_demo`); `StormConfig`
  → `core/Storm.h`.
- ☑ **R.2** Unified `core/Match.buildMatch()` — game + balance sim build matches
  the same way.
- ☑ **R.3** Team formats 2v2 / 3v3 (editor slot tabs + enemy pickers).
- ☑ **R.4** Static maps (`data/MapJson`, `data/maps/duel.json`, reachability gate).
- ☑ **R.5** Ban enforcement (editor greys banned; sim drops them); ranked/custom
  trust tie-in.

## Phase 2 — Spell bar + sprite/asset packs

- ☑ **2.1** Clickable spell bar (`spellSlotRect`, visual states).
- ☑ **2.2** Atlas-based sprite-pack seam (`render/PackManifest` + `SpritePack`,
  `ATB_PACK`, `tb_pack_demo`).
- ☑ **2.3** Structured engine event stream + GUI combat log (`core/Event.h`,
  `tb_event_demo`).
- ☑ **2.4** Animations — clip data model + event-driven cast flash.
- ☑ **2.5** `packs/default/` + `example_upscaled` + palette-only `example/` +
  `docs/sprite-packs.md`.
- ☐ Ground-effect + status-marker art wiring; editor skill icons.

## Phase 3 — Pluggable AI

- ☑ **3.1** `core/AI.h` `Brain` interface; beam search = `BeamSearchBrain` /
  `defaultBrain()`.
- ☑ **3.2** Registry + by-name selection (`beam` / `greedy`, `ATB_BRAIN`).

## Phase 3.5 — Self-teaching AI (NNUE-style) ☐

- ☐ **3.5.1** Split `Evaluator` seam under `Brain` (`HandcraftedEvaluator`).
- ☐ **3.5.2** Versioned `featurize(Battle, Faction)`.
- ☐ **3.5.3** Self-play data export from the sim.
- ☐ **3.5.4** Offline PyTorch training → versioned, hash-pinned weights artifact.
- ☐ **3.5.5** Dependency-free C++ `LearnedEvaluator` inference.
- ☐ **3.5.6** Improvement loop + gating (promote only if >~55% vs prev).
- ☐ **3.5.7** (Optional) NNUE-grade incremental accumulator + int8.

## Phase 4 — Networked PvP

- ☑ **4.1** Wire formats — `Intent` / `Snapshot` / build, round-trip + determinism
  (`data/Net`, `tb_net_demo`).
- ☑ **4.2** `render/MatchSource` seam — UI ↔ who-drives-the-Battle
  (`tb_matchsource_demo`).
- ☑ **4.3** In-process authoritative `net/MatchRunner` (`tb_loopback_demo`).
- ☑ **4.4** Real TCP transport (`net/Socket` + `Protocol` + `GameServer`), `tb_server`,
  deterministic-mirror client (`net/MirrorSession` → `render/RemoteMatchSource`),
  `ATB_CONNECT`.

### Phase 4.5 — Lobby, accounts, Online Home ◐

- ☑ **Slice 1** Persistent multi-match server (`serveMatches`, FIFO pairing,
  configurable bind).
- ☑ **Slice 2** Accounts — username+password (PBKDF2, `data/Password`) + Elo,
  JSON-backed `net/AccountStore`; ranked vs custom.
- ☑ **Slice 3** Private lobbies via join codes.
- ☑ **Slice 4** GUI networking screen (`render/ConnectScreen`) — *superseded by 6.1*
  (now the lobby login).
- ☑ **Slice 5** Online Home: `net/Lobby` seek board + directed challenges, format
  (rated + clock), live routing (`tb_lobby_challenge_demo`).
- ☑ **Slice 5b** Unlimited → correspondence played + ranked over the lobby
  (`MoveChannel` seam, embedded `Mailbox` + `Arbiter`, reconnect resume;
  `tb_lobby_correspondence_demo`).
- ☑ **Slice 5c** GUI lobby + correspondence play (`render/LobbyScreen`,
  `CorrespondenceMatchSource`).
- ☑ **Slice 5d** `tb_lobby` daemon (self-hosted Online Home).
- ☐ **Remaining:** async connect + "waiting" screen; **SQLite** + match history;
  widening-band **queue**; **TLS**.

### Phase 4.5 Slice 6 — Online UX pass (playtest feedback)

- ☑ **6.1** Flow: menu → **login → lobby** (no forced pre-lobby editor); "Edit build"
  in lobby.
- ☑ **6.2** Per-match **ready check** (all games): build chosen at ready → READY,
  else cancel; server-validated; `tb_ready_check_demo` + `render/ReadyCheckScreen`.
- ◐ **6.3** Idle-clock **forfeit** ☑ (per-move / chess = `mainSec/5`;
  `tb_lobby_forfeit_demo`) + **visible two-clock HUD** ☑; ☐ true chess time bank.
- ◐ **6.4** In-match **chat** ☑ (async relay, split log column, `tb_chat_demo`); ☐
  lobby + correspondence chat.
- ☑ **6.5** BUG fix — online moves now register (pump every frame).
- ☑ **6.6** `tb_lobby` warns when run without `./data`.
- ☑ **6.7** End-of-match VICTORY/DEFEAT/DRAW screen → return to lobby; editor
  ‹Menu returns to where it was opened.

### Phase 4.6 — Chat ☐

- ◐ In-match live chat done (6.4); ☐ **lobby chat** + correspondence-game chat +
  safety levers (rate-limit / mute / length cap).

## Correspondence ranked — "verify, don't host"

- ☑ **CR.1** Cross-platform determinism lock-down (hand-rolled arena RNG,
  `tb_determinism_demo` KAT).
- ☑ **CR.2** Game notation + verifier = replays §5.1 (`net/Replay`, `tb_replay_demo`).
- ☑ **CR.3** Mailbox relay (`net/MailboxRelay`, NAT-immune, `tb_mailbox_demo`).
- ☑ **CR.4** Submit-to-arbiter + double-attestation + Elo (`net/Arbiter`,
  `tb_arbiter_demo`).
- ☑ **CR.5** Perfect-info ranked ruleset (`data/rules.ranked.json`, bans invisible,
  content-addressed hash; `tb_ranked_rules_demo`).
- ◐ **CR.6** Hidden info in trustless ranked:
  - ☑ Slice 1 — decoy mechanic (`Effect::Decoy`, cloak pairs, `decoy` spell;
    `tb_decoy_demo`) + `blind`/`surge`/`flux`.
  - ☑ Slice 2 — commitment layer in notation + verifier (`tb_commit_demo`).
  - ☑ Slice 3 — correspondence session generates commitments at cast
    (`net/Correspondence`, `MoveChannel`, `tb_correspondence_demo`).
  - ☐ Deeper options: commit-reveal movement / ZK proofs (parked R&D).

## Phase 5 — Replays & spectate

- ☑ **5.1** Persist `seed + intents`, re-simulate = replay/scoresheet/shareable
  (done as CR.2); ☐ GUI replay viewer.
- ☐ **5.2** Spectate — subscribe to the snapshot stream (a spectator is another
  mirror).
- ☐ **5.3** Balance backlog — fireball radius, portal/Blind/Surge/Flux AI, synergy
  tuning via `tb_balance`.

## Parallel track — Web / WASM ☐

- ☐ **W.1** Emscripten toolchain + web Raylib (`-DPLATFORM=Web`).
- ☐ **W.2** Adapt the frontend loop (`emscripten_set_main_loop`).
- ☐ **W.3** Package assets (`--preload-file`; the atlas format helps).
- ☐ **W.4** Web build path in CMake + CI job.
- ☐ **W.5** Publish `.html/.wasm/.js` to itch.io.
