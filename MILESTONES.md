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

## Online onboarding — the "flight check" for Play Online

**Status: todo (planning).** Self-hosting over Tailscale works
([`CONNECT.md`](CONNECT.md)), but it's a *power-user* path: a non-power-user who
clicks **Play Online** today can hit raw Tailscale/CLI states — e.g. the service
still coming up reads as a cryptic `NoState` — with no in-game guidance. Replace
that with a **dynamic flight check**: a preflight shown the first time Play Online
is clicked that scans the machine and, **by process of elimination**, walks the
user into the tailnet and on into a lobby. Each gate has exactly one cause and one
fix, so the panel shows only the first red row's remediation and keeps the rest
inert until it clears.

**The hard floor (named, not hidden).** The client **cannot** install Tailscale or
sign a user into a tailnet — that's a privileged system service (admin rights) with
per-user browser auth. The flight check *detects and guides* those two steps
(opens the download page / launches the app) but can't perform them. So "drive
everything from the client" tops out at **guidance** for install+login; every other
step is automated. Non-power-users who shouldn't touch Tailscale at all are the
[hosted-relay path](memory) (`pvp-deployment-model`) — a VPS + TLS lobby needing
only a username/password; Tailscale stays the free self-host option this flow eases.

**Mechanism — one probe, no new deps.** Everything reads from a single
`tailscale status --json` (parsed via the repo's own `src/data/Json.h`) plus one
TCP poke at the host port. Live-verified fields: `BackendState` (the whole state
ladder: absent/`NoState`/`Starting` → `NeedsLogin` → `Stopped` → `Running`),
`CurrentTailnet.Name`, `Self`, and the `Peer` map (each `TailscaleIPs` + `Online`).
The robust **"on my network"** signal is *"the host address appears as an online
peer,"* **not** a tailnet-name match — it answers the only question that matters
(can this client reach the server *now*), survives shared-node invites, and doubles
as **auto-detecting the host IP** to hand the lobby (no "type the server address").
The host address is an **input** (pasted share code / `ATB_CONNECT` / ConnectScreen
field), never hardcoded, so the friend-hosted model keeps working.

The ladder: **1** installed (binary found) · **2** service up · **3** signed in
(`Running`) · **4** on the host's network (host is a peer) · **5** host online ·
**6** game reachable (TCP to `host:port`) · **✅** all-green → **Enter Lobby**
(auto-fills the detected IP). The **[Check again]** re-run loop *is* the UX: the
user leaves to install/sign-in, returns, re-probes, advances.

- ✅ **O.1 — `TailscaleProbe` (core, pure, test-covered).** `net/TailscaleProbe`:
  binary discovery (PATH + well-known per-OS paths) + `status --json` parse (via
  `data/Json`, no new dep) into `TsStatus { installed, daemonUp, backend, tailnet,
  peers[], findPeer() }`; `flightStepFor()` reduces it to the first unmet step.
  Fixture tests (`tests/tailscale_demo.cpp`) over a captured live tailnet + a
  `NoState` regression + a `NeedsLogin` case. In `tb_transport`; wired into the CI
  gate. Verified live on Linux (probe → peer resolve → port poke → Ready).
- ✅ **O.2 — Subprocess helper.** `net/Subprocess`: shell-free capture (fork/exec
  on POSIX, `CreateProcess` + **`CREATE_NO_WINDOW`** on Windows) + `openUrl` /
  `openTailscaleApp`. Runs off the render thread (via O.3's future). *POSIX path
  compiled + run here; the Windows branch is written to the Socket.cpp winsock
  idiom but not yet compiled on Windows.*
- ✅ **O.3 — `FlightCheckScreen` ladder.** `render/FlightCheckScreen`: immediate-mode
  ladder, probe in a `std::future` so the frame never blocks, stop at first red with
  a per-OS fix button (open page / launch app / copy command), later gates dimmed.
  Screenshot-verified in Ready / HostUnreachable / JoinNetwork states.
- ◐ **O.4 — Route Play Online through it.** Shown automatically on first click
  (`Prefs.onlineChecked`, round-trip tested); thereafter straight to login, with a
  persistent "Trouble connecting?" link on the Connect screen to re-summon it. On
  all-green it auto-detects the host IP and prefills Connect. *Still TODO: automatic
  re-open on a failed connect (today it's the manual link).*
- ☐ **O.5 — Host-side auto-IP "share code."** Reuse O.1 on the host to show
  *"Share this with friends: `<hostname>` / `100.x.y.z`"* with a copy button —
  the value a joiner pastes into gate 4. Pairs with the in-GUI host button (§0.1.0
  follow-on).

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

## Champion identity — archetype art

**Status: todo (planning).** Give a build a *face*: derive an **archetype** from
the tags of the spells the player picked, and show that archetype's character art
in the build editor and in the match. A time-heavy build reads as a *chronomancer*;
a fire-heavy build as a *pyromancer*; etc. Presentation-only and derived — no new
gameplay state, no extra build field, so it can't desync a re-sim (cosmetic, like
[nameplates / cosmetic name styles](memory)).

**Design principle — extend the resolution the pack already does; add no new
subsystem.** `pack.json` is already a dictionary of `namespace.key → {atlas, rect}`
coordinates. The *namespace* is the type (`tiles.*`, `units.*`, `spells.*`) and the
*key* is a stable string key into `catalog.json` / `creatures.json`
(`spells.fireball`, `units.enemy`) — a by-key link, not by numeric id, which is
why it survives reordering and works for `creatures.json` (which has no id). Rather
than a `type + id` table or a separate `archetypes` rules block, make **`tags` a
first-class namespace in that same dictionary** and resolve every drawable through
a **key ladder**, most-specific → least:

- champion face: `units.<name>` → **`archetype.<dominant-tag>`** → `units.<faction>`
- spell icon:   `spells.<key>` → **`tags.<first-tag>`** → generic (free bonus tier)

The elegant consequence: **the pack's set of `archetype.*` / `tags.*` keys _is_ the
vocabulary.** No rules list, no thresholds, no `priority` field — "which tags are
archetypes" simply means "which `archetype.*` coordinates the modder drew."
Presence of a coordinate is the opt-in. Creating a new archetype is: add a tag to
some spells in `catalog.json`, and add one `archetype.<tag>` line to `pack.json`.
No recompile, no schema change.

```jsonc
// data/catalog.json — a spell just carries the tag (already supported today):
{ "key": "rewind", "tags": ["school:time", "support"], ... }

// packs/<pack>/pack.json — bind that tag to coordinates, same dict, new prefix:
"archetype.school:time": { "atlas": "main", "rect": [0, 384, 96, 96] }
```

The only C++ logic is a tiny, deterministic tally — pick the build's most-frequent
tag *that the pack has an `archetype.*` sprite for*, stable tie-break, else fall to
faction art. Everything else is data the modder controls.

- ☐ **A.1 — Thematic tags in the catalog/creatures.** Tag spells (and creatures)
  with their school(s) via namespaced tags (`school:time`, `school:fire`, …).
  Reuse the existing `tags` vector; round-trips through `CatalogJson` for free and
  leaves editor filtering (`matchesFilter`) untouched. Add a `tags` array to
  `creatures.json` too so summons/objects can theme the same way. Data-only.
- ☐ **A.2 — `tags.*` / `archetype.*` sprite namespaces.** No manifest schema change
  needed — these are ordinary dotted sprite keys the existing loader already
  accepts. Just document the two reserved prefixes and confirm colon-bearing keys
  (`archetype.school:time`) round-trip through `loadPackManifestFromString`. Add a
  parse test if the current key charset is stricter than expected.
- ☐ **A.3 — Dominant-tag resolver (core, pure, test-covered).** A pure fn
  `f(build tags, pack) -> optional<key>`: tally the build's tags, keep only those
  with an `archetype.<tag>` sprite present in the pack, return the most frequent
  (deterministic tie-break: catalog tag order, then lexical), else `nullopt`.
  Deterministic so editor and sim agree. Unit test in `tests/`.
- ☐ **A.4 — Generalize the sprite ladder.** Fold the ladders above into the resolve
  path so both champions (`Renderer.cpp` ~L541) and spell icons (~L177) try the
  tag/archetype tier before their generic fallback. One shared helper, so the
  renderer stays a pure consumer.
- ☐ **A.5 — Build-editor display.** In `BuildEditorScreen`, resolve the current
  slot's archetype (A.3) and draw `archetype.<tag>` as a portrait beside the
  name/stat column; updates live as spells are toggled. Neutral look when
  unresolved.
- ☐ **A.6 — In-match display.** Carry the resolved archetype key on the champion's
  `ViewState` (compute once at build/spawn, like `nameP/nameE`) so A.4's ladder can
  draw it; faction tint still applies.
- ☐ **A.7 — Content pass + worked example.** Author the first `archetype.*` art
  (chronomancer + a couple more) into the default pack's atlas, and document the
  "add a tag → add a coordinate" flow in the pack/modding docs so it doubles as the
  contributor example.

Open questions: multi-school builds (dominant only, or a blended look?); whether
`archetype.*` (build face) and `tags.*` (icon fallback) should really be one
namespace or stay two; whether the archetype key should also flavor the nameplate;
interaction with future admin-assigned cosmetic name styles.

## Hidden-info R&D

- ◐ **CR.6** — deeper options: commit-reveal movement / ZK proofs (parked R&D;
  slices 1–3 shipped).

## Contributor onboarding

- ☐ **0.6** — file 2–3 good-first-issues on GitHub (`CONTRIBUTING.md` shipped).

## Web / WASM

- ☐ **W.5** Publish `.html` / `.wasm` / `.js` to itch.io (W.1–W.4 browser client
  shipped).
