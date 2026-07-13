# Milestones

Open backlog for ATB — **what's left**, one line each. Shipped work has been
flushed from this file (it lives in git history and
[`ARCHITECTURE.md`](ARCHITECTURE.md)); only unresolved items remain.
Legend: **◐ in progress · ☐ todo**.

---

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
