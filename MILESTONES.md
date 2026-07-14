# Milestones

Open backlog for ATB — **what's left**, one line each. Shipped work has been
flushed from this file (it lives in git history and
[`ARCHITECTURE.md`](ARCHITECTURE.md)); only unresolved items remain.
Legend: **◐ in progress · ☐ todo**.

---

## 0.1.0 — Team Play (2v2 / 3v3) — headline feature

**Status: in progress.** 0.0.2 (elemental surfaces) shipped. Numbered **0.1.0**
(not 0.0.3): the ground-tick fix changes combat timing, so 0.0.2 replays /
scoresheets no longer re-sim identically — a compatibility break, which bumps the
minor in 0.x. This release adds multi-human team matches (backend in progress),
plus two control/feedback UX wins and the terrain-lifetime fix, already landed on
the working branch.

The crux: the engine is 2-faction (`Faction{Player,Enemy}`) and the whole net
stack authorizes intents on a single `Faction seat`. 2v2/3v3 stays **two sides**
but needs a **controller-seat identity orthogonal to `Faction`** — a human owns a
subset of a faction's champions; the Arbiter authorizes by seat-ownership, not
faction. NOT a factions overhaul: `Ruleset.teamSize` (1–8) + `buildMatch` already
build N champions per side, and `MatchFormat.teamSize` already rides the wire.
Design decisions locked: **design-for-N** (ship 2v2 + 3v3 together), **separate
2v2 / 3v3 Elo ladders**, ready-check **shows partners' builds**.

- ✅ **Lobby GUI + matchmaking surface** — `1v1 / 2v2 / 3v3` selector in the
  format row (`LobbyScreen`); seeks/challenges/queue post & pair by team size
  (`sameFormat` keys on `teamSize`, already on the wire).
- ☐ **Engine seat-ownership** — per-unit owner tag, ownership-aware `controlFor` +
  intent authorization, generalized win check. Core-only, test-covered, defaults
  from `Faction` so 1v1 is unchanged. *Next step.*
- ☐ **Protocol/Arbiter/MirrorSession** carry `seat = (faction, slot)` not bare
  `Faction`.
- ☐ **Lobby party formation** — group N players, assign seats, group ready-check.
- ☐ **`AccountStore`** per-format ratings (1v1/2v2/3v3) + `schema.sql` migration.
- ☐ **GUI** — party UI, team-aware `ReadyCheckScreen`, board ownership tinting.
- ☐ **Ground aging → true per-round model** (with a deliberate balance pass) once
  team turn order lands — supersedes the interim champion-only tick below.
- ✅ **Unified Dofus-style controls** — left-click driven: no spell selected →
  move; select (bar click / `1`–`9`) → green castable tiles → left-click to cast;
  right-click / Esc / re-press digit → deselect (`main.cpp`). Pending release.
- ✅ **Floating combat numbers** — `Damage`/`Heal`/`Shield` events become rising,
  fading `-N`/`+N`/`N` popups over the board (`Animator` + `Renderer`), so hits
  read without watching the log. Pending release.
- ✅ **Ground-effect lifetime fix** — terrain (portals/glyphs/walls/surfaces) aged
  on *every* unit-turn, so bombs/summons joining the initiative order silently
  shortened it (NOT the summon cap). `tickGround` now runs on **champion** turns
  only — balance-neutral in 1v1. Regression test in `tests/spells_demo.cpp`.

## Online hardening — the gate to a public, non-VPN launch

- ☐ **TLS / transport encryption** — *the* blocker before any public launch;
  passwords cross the wire in the clear today (fine behind LAN/VPN).
- ☐ **SQLite** behind the account/store seam + match-history rows.

## AI depth

- ☐ **H.4** Weight tuning from self-play (Texel/SPSA on `EvalWeights` via the
  gauntlet, >55% promotion gate) — the bridge into 3.5.3 / 3.5.6.
- **Phase 3.5 — Self-teaching AI (NNUE-style)** — 3.5.1 shipped as H.1:
  - ☐ **3.5.2** Versioned `featurize(Battle, Faction)`.
  - ☐ **3.5.3** Self-play data export from the sim.
  - ☐ **3.5.4** Offline PyTorch training → versioned, hash-pinned weights artifact.
  - ☐ **3.5.5** Dependency-free C++ `LearnedEvaluator` inference.
  - ☐ **3.5.6** Improvement loop + gating (promote only if >~55% vs prev).
  - ☐ **3.5.7** (Optional) NNUE-grade incremental accumulator + int8.
- ☐ **Portal-aware AI** — the AI still never casts Portal (needs teleport-aware
  pathing); plus enemy-only Pull and summon `duration`.

## Balance & content

- ◐ **5.3 Balance backlog** — fireball radius, Portal AI, synergy tuning via
  `tb_balance` (Blind / Surge / Flux AI shipped as H.2).
- ☐ Sprite-pack **ground-effect + status-marker art** wiring; editor skill icons.

## Hidden-info R&D

- ◐ **CR.6** — deeper options: commit-reveal movement / ZK proofs (parked R&D;
  slices 1–3 shipped).

## Contributor onboarding

- ☐ **0.6** — file 2–3 good-first-issues on GitHub (`CONTRIBUTING.md` shipped).

## Web / WASM

- ☐ **W.5** Publish `.html` / `.wasm` / `.js` to itch.io (W.1–W.4 browser client
  shipped).
