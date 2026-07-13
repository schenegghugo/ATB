# Milestones

Open backlog for ATB — **what's left**, one line each. Shipped work has been
flushed from this file (it lives in git history and
[`ARCHITECTURE.md`](ARCHITECTURE.md)); only unresolved items remain.
Legend: **◐ in progress · ☐ todo**.

---

## 0.0.2 — Elemental surfaces (Divinity/BG3-style) — headline feature

**Status: code-complete, all slices validated (full CI green, determinism
fingerprint unchanged); pending commit + release.** Design locked in
[`docs/elements.md`](docs/elements.md).

Turn `Glyph` from a bespoke trap into a **neutral surface** that elemental
spells paint and combine (fire · electric · poison · water · ice · heal · oil).
Built on the existing `GroundEffect` + `tickGround()` + `onEnterTile()` substrate
(`Battle.cpp`): a new **element axis**, a deterministic **reaction table**, and
content. Every reaction re-sims identically from notation (verify-don't-host):
resolution order is canonical (row-major zone, ground spawn order, units by id)
and chains are capped one-per-tile-per-cast.

- ✅ **E.0 Design lock** — [`docs/elements.md`](docs/elements.md): roster, the
  `(surface × incoming) → (result, burst)` matrix, canonical resolution order,
  the one-reaction-per-tile chain cap, notation/determinism impact. All design
  calls closed (Oil in, Glyph repel retired, Electric = persistent surface,
  Frozen=root / Stunned=skip).
- ✅ **E.1 Surface substrate** — `enum Element` + `GroundSpec.element` +
  `GroundEffect.element/blocksLos`; `SpellEnums.h` rows + count bumps; `SpellJson`
  + `Snapshot` (`Net.cpp`) in/out. `element` is optional so pre-0.0.2 data
  round-trips byte-identical (no schema bump needed).
- ✅ **E.2 Passive behaviour** — `surfaceEnter`/`surfaceTick` per element; new
  statuses `Wet`/`Burning`/`Frozen`/`Stunned`/`Oiled`; `stepTo` gates on Frozen,
  `startTurnFor` ticks Burning + zeroes a Stunned turn. Deterministic.
- ✅ **E.3 Reaction engine** — the matrix in core, tile-local (one
  `GroundEffect` per painted tile), first-match spawn order, one reaction per
  tile per cast. Steam sets `blocksLos` (wired into LOS). Truth-table tested.
- ✅ **E.4 Effect vocabulary** — `Effect::Type::PaintSurface` (element + duration
  in `amount`) + `TargetShape::Cone` (reuses the 0.0.1 wheel facing via
  `affectedTiles` rotation). *Delayed second stage reuses the existing
  `onDeath`/`fuse` object — no `DelayedArea` type needed.*
- ✅ **E.5 Elemental spell set** — `Storm` (rain via `PaintSurface(Water)` +
  fused `stormcloud` creature whose `onDeath` paints Electric on the wet zone),
  `Blizzard` (Cone: damage + Frozen + Ice), and painters `Ignite`/`Puddle`/
  `Electrify`. Catalog regen'd (25 spells, v1.2.0), creatures v1.1.0.
- ✅ **E.6 AI awareness** — `element` into `featurize()`; evaluator prices
  Burning as DoT, Stunned/Frozen attackers as reduced threat, and a
  surface-hazard term (avoid fire, value a heal pool). *Painters read "AI-unused"
  in sims — the planner doesn't yet set up multi-turn combos (future work).*
- ✅ **E.7 Render + narration** — element palette + surface draw in
  `Renderer.cpp`, `statusWord` names the new statuses. (Client boots + renders
  the new catalog; in-battle surface tint not screenshot-verified — no input
  injection here. Theming the palette + a Storm telegraph are follow-ups.)
- ✅ **E.8 Ship gate** — `tb_balance` clean (Storm/Blizzard balanced), determinism
  fingerprint **unchanged** (no re-lock needed), elemental determinism guard +
  reaction truth-table tests, full `scripts/run_ci_checks.sh` green. Ready to tag
  0.0.2 (commit + release are Hugo's to drive).

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
