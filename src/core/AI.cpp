#include "AI.h"

#include "Evaluator.h"
#include "SpellTraits.h"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <set>
#include <vector>

namespace tb {

namespace {

// --- Tunables ---------------------------------------------------------------
constexpr int kMaxPlies = 4;   // max actions looked ahead within a turn
constexpr int kBeamWidth = 6;  // states kept between plies

// --- Small spell helpers ----------------------------------------------------
Vec2i cardinalToward(Vec2i from, Vec2i to) {
    int dx = to.x - from.x, dy = to.y - from.y;
    if (std::abs(dx) >= std::abs(dy)) return Vec2i{dx == 0 ? 0 : (dx > 0 ? 1 : -1), 0};
    return Vec2i{0, dy > 0 ? 1 : -1};
}

// --- Action model -----------------------------------------------------------
// (PlannedAction is public — see AI.h — so Brains can express plans.)

void applyAction(Battle& b, EntityId self, const PlannedAction& a) {
    if (a.kind == PlannedAction::Kind::Cast) b.cast(self, a.slot, a.target);
    else b.moveToward(self, a.target);
}

// `slotMask` (nullable) restricts which spell slots may be cast — the minimax
// search uses it to model a *believed* opponent: when planning an enemy reply
// under intel, only the slots that foe has actually revealed are offered (see
// Intel.h). Unmasked callers plan with the unit's full loadout.
std::vector<PlannedAction> enumerateActions(const Battle& b, EntityId self,
                                            const std::vector<char>* slotMask = nullptr) {
    const Entity& me = b.unit(self);
    std::vector<PlannedAction> acts;

    std::vector<Vec2i> foeTiles, allyTiles, objectTiles;
    for (EntityId i = 0; i < b.unitCount(); ++i) {
        const Entity& u = b.unit(i);
        if (!u.alive()) continue;
        if (u.kind == EntityKind::Object) { objectTiles.push_back(u.pos); continue; } // bombs:
        // not attack targets, but portals can deliver them (see below)
        if (u.team == me.team) allyTiles.push_back(u.pos);
        else if (!u.invisible()) foeTiles.push_back(u.pos); // can't target hidden foes
    }

    if (me.ap > 0) {
        for (int slot = 0; slot < static_cast<int>(me.spells.size()); ++slot) {
            if (slotMask && (slot >= static_cast<int>(slotMask->size()) || !(*slotMask)[slot]))
                continue; // not part of the believed loadout
            const Spell& sp = me.spells[slot];
            // Status spells cut both ways: a RangeDebuff (Blind), a polarized
            // buff (Flux slows foes / hastens allies) or a plain negative swing
            // is worth aiming at foes; a positive AP/MP buff (Surge) at allies.
            // Over-offering is fine — the evaluator arbitrates.
            bool statusVsFoe = false, statusVsAlly = false;
            for (const Effect& fx : sp.effects) {
                if (fx.type != Effect::Type::ApplyStatus) continue;
                const StatusEffect& st = fx.status;
                if (st.kind == StatusEffect::Kind::RangeDebuff && st.magnitude > 0)
                    statusVsFoe = true;
                if (st.kind == StatusEffect::Kind::ApBuff || st.kind == StatusEffect::Kind::MpBuff) {
                    if (fx.polarized || st.magnitude < 0) statusVsFoe = true;
                    if (st.magnitude > 0) statusVsAlly = true;
                }
                // Rewind is insurance on an ally (snapshot now, restore in N
                // turns). Its payoff is invisible to a static eval — only the
                // minimax brains see the revert fire inside the search — but it
                // must be offered here or no brain can ever consider it.
                if (st.kind == StatusEffect::Kind::Rewind) statusVsAlly = true;
            }
            const bool offensive = spellDamage(sp) > 0 || hasEffect(sp, Effect::Type::Push) ||
                                   hasEffect(sp, Effect::Type::Pull) || statusVsFoe;
            const bool supportive = hasEffect(sp, Effect::Type::Heal) ||
                                    hasStatusEffect(sp, StatusEffect::Kind::Shield) ||
                                    statusVsAlly;
            const bool selfBuff = hasStatusEffect(sp, StatusEffect::Kind::Invisible);
            const bool placement = hasEffect(sp, Effect::Type::Spawn);
            // A portal transports whatever stands on its entry (enemies too)
            // straight to the traced exit, so the entry tile is a *target*:
            // offer foes (8-tile displacement) and objects (bomb delivery) and
            // let the simulation show where everyone lands.
            bool portalSpawn = false;
            for (const Effect& fx : sp.effects)
                if (fx.type == Effect::Type::Spawn && fx.ground.kind == GroundKind::Portal)
                    portalSpawn = true;
            // Summons and decoys both want a free tile to spawn onto.
            const bool summon = hasEffect(sp, Effect::Type::Summon) ||
                                hasEffect(sp, Effect::Type::Decoy);

            std::vector<Vec2i> targets;
            if (offensive) {
                targets.insert(targets.end(), foeTiles.begin(), foeTiles.end());
                // Objects too: a bomb detonates on death, so shooting or
                // poisoning one is fuse control — place a bomb and blow it the
                // same turn, or pre-detonate a foe's. The simulation shows the
                // blast; bad detonations refute themselves in the eval.
                targets.insert(targets.end(), objectTiles.begin(), objectTiles.end());
            }
            if (supportive) targets.insert(targets.end(), allyTiles.begin(), allyTiles.end());
            if (selfBuff) targets.push_back(me.pos);
            if (portalSpawn) {
                targets.insert(targets.end(), foeTiles.begin(), foeTiles.end());
                targets.insert(targets.end(), objectTiles.begin(), objectTiles.end());
            }
            if (placement) {
                // Ground features are position plays; a single hardcoded offer
                // starves the search (shelter/glyph verdicts were built on one
                // toward-foe candidate). Per foe, offer: as far as range allows
                // toward them (cut LOS mid-approach / lay a corridor), their own
                // tile (a glyph's movement tax; walls box their free neighbours),
                // and the caster's next tile toward them (a defensive wall/moat).
                auto offerPlacement = [&](Vec2i t) {
                    for (Vec2i o : targets)
                        if (o == t) return; // skip duplicates — clones cost budget
                    targets.push_back(t);
                };
                for (Vec2i fp : foeTiles) {
                    const Vec2i dir = cardinalToward(me.pos, fp);
                    const int step = std::min(sp.maxRange, std::max(1, manhattan(me.pos, fp) - 1));
                    offerPlacement(Vec2i{me.pos.x + dir.x * step, me.pos.y + dir.y * step});
                    offerPlacement(fp);
                    offerPlacement(Vec2i{me.pos.x + dir.x, me.pos.y + dir.y});
                }
            }
            // Summons spawn onto their target tile, which must be free and walkable
            // (an occupied tile silently no-ops the spawn but still spends AP). Offer
            // the caster's empty neighbours plus a spot stepping toward the nearest
            // foe, and let the beam search pick — the evaluator rewards the extra unit.
            if (summon) {
                auto offer = [&](Vec2i t) {
                    if (b.grid().isWalkable(t) && !b.unitAt(t)) targets.push_back(t);
                };
                for (Vec2i d : {Vec2i{1, 0}, Vec2i{-1, 0}, Vec2i{0, 1}, Vec2i{0, -1}})
                    offer(Vec2i{me.pos.x + d.x, me.pos.y + d.y});
                if (!foeTiles.empty()) {
                    const Vec2i fp = foeTiles.front();
                    const Vec2i dir = cardinalToward(me.pos, fp);
                    const int step = std::min(sp.maxRange, std::max(1, manhattan(me.pos, fp) - 1));
                    offer(Vec2i{me.pos.x + dir.x * step, me.pos.y + dir.y * step});
                }
            }
            for (Vec2i t : targets)
                if (b.canCast(self, slot, t)) acts.push_back({PlannedAction::Kind::Cast, slot, t});
        }
    }

    // Repositioning: advance toward each visible foe (a macro that spends MP).
    if (me.mp > 0) {
        for (Vec2i ft : foeTiles) {
            auto path = findPath(b.grid(), me.pos, ft, b.pathBlockers(self));
            if (path.size() >= 2) acts.push_back({PlannedAction::Kind::Move, -1, ft});
        }
        // Retreat: the reachable tile this turn's MP can buy that maximises
        // distance to the nearest visible foe (ties: first BFS order, so
        // deterministic). The toward-foe macros can't express disengaging, and
        // kiting — Blind then step out of the shrunken threat envelope — needs
        // it. Offered, not forced: the evaluator arbitrates via the risk term.
        if (!foeTiles.empty()) {
            auto nearestFoeDist = [&](Vec2i p) {
                int d = manhattan(p, foeTiles[0]);
                for (Vec2i ft : foeTiles) d = std::min(d, manhattan(p, ft));
                return d;
            };
            Vec2i best = me.pos;
            int bestDist = nearestFoeDist(me.pos);
            for (Vec2i t : reachableWithin(b.grid(), me.pos, me.mp, b.pathBlockers(self))) {
                const int d = nearestFoeDist(t);
                if (d > bestDist) { best = t; bestDist = d; }
            }
            if (!(best == me.pos))
                acts.push_back({PlannedAction::Kind::Move, -1, best});
        }
        // Ride a live portal: step onto its entry and the engine teleports you
        // to the exit — an escape/reposition verb the toward-foe and retreat
        // macros can't express. The teleport ends the move (later path steps
        // stop matching), so the plan is re-evaluated from the far side.
        for (const GroundEffect& ge : b.groundEffects()) {
            if (ge.kind != GroundKind::Portal || ge.tiles.empty()) continue;
            auto path = findPath(b.grid(), me.pos, ge.tiles[0], b.pathBlockers(self));
            if (path.size() >= 2) acts.push_back({PlannedAction::Kind::Move, -1, ge.tiles[0]});
        }
    }
    return acts;
}

// A cheap identity key for a simulated state: everything the evaluator can see.
// Cast-then-move and move-then-cast reach the same position — without dedup such
// permutations crowd the beam and it degenerates to one line explored kMaxPlies
// ways. (H.5's transposition table will grow this into a proper hash.)
std::vector<int> stateKey(const Battle& b) {
    std::vector<int> k;
    for (EntityId i = 0; i < b.unitCount(); ++i) {
        const Entity& u = b.unit(i);
        k.push_back(u.pos.x); k.push_back(u.pos.y);
        k.push_back(u.hp); k.push_back(u.ap); k.push_back(u.mp);
        for (int cd : u.spellCooldowns) k.push_back(cd);
        k.push_back(static_cast<int>(u.statuses.size()));
        for (const StatusEffect& s : u.statuses) {
            k.push_back(static_cast<int>(s.kind)); k.push_back(s.magnitude);
            k.push_back(s.remainingTurns); k.push_back(s.delay);
        }
    }
    k.push_back(static_cast<int>(b.groundEffects().size()));
    for (const GroundEffect& g : b.groundEffects()) {
        k.push_back(static_cast<int>(g.kind)); k.push_back(g.remainingTurns);
        k.push_back(g.exit.x); k.push_back(g.exit.y);
        for (Vec2i t : g.tiles) { k.push_back(t.x); k.push_back(t.y); }
    }
    return k;
}

// One candidate whole-turn plan: the action sequence, the state it reaches
// (already simulated — callers reuse it instead of re-applying), and its eval.
struct PlanCandidate {
    std::vector<PlannedAction> seq;
    Battle state;
    double ev;
};

// Beam search over one unit's turn; returns the K best *distinct* end-states
// (every visited state is a legal stopping point, so the pool holds them all —
// including the do-nothing root). `budget` counts simulated clones and caps the
// work: it is shared across a whole minimax search so total effort stays
// bounded and deterministic. Results are sorted best-first.
//
// `diversify` guarantees the selection first covers each distinct *opening*
// action (best line per opening, ev-ordered) before filling with runners-up.
// A minimax ROOT needs this: the static top-K is often K near-identical
// permutations of the same engage, and the line the look-ahead would actually
// choose (e.g. disengage) never enters the tree. Refutation nodes don't — an
// opponent only needs its best few replies.
std::vector<PlanCandidate> topKPlans(const Battle& battle, EntityId self, const Evaluator& eval,
                                     const EvalContext& ctx, int k, int& budget,
                                     const std::vector<char>* slotMask = nullptr,
                                     bool diversify = false) {
    struct Node {
        Battle state;
        std::vector<PlannedAction> seq;
        double ev;
    };
    std::vector<Node> beam;
    Node root{battle, {}, eval.evaluate(battle, ctx)};
    root.state.setEventRecording(false); // throwaway sims don't narrate (and stay cheap to clone)
    std::vector<Node> pool; // every distinct state visited, root included
    pool.push_back(root);
    beam.push_back(std::move(root));

    for (int ply = 0; ply < kMaxPlies && budget > 0; ++ply) {
        std::vector<Node> next;
        for (Node& n : beam) {
            if (n.state.phase() == Phase::Finished || !n.state.unit(self).alive()) continue;
            for (const PlannedAction& a : enumerateActions(n.state, self, slotMask)) {
                if (--budget < 0) break;
                Battle s2 = n.state; // clone + simulate
                applyAction(s2, self, a);
                const double e = eval.evaluate(s2, ctx);
                std::vector<PlannedAction> seq = n.seq;
                seq.push_back(a);
                next.push_back({std::move(s2), std::move(seq), e});
            }
        }
        if (next.empty()) break;
        // Stable: among equal evals the earlier-enumerated node wins, keeping
        // tie-breaks deterministic and identical across refactors.
        std::stable_sort(next.begin(), next.end(),
                         [](const Node& a, const Node& b) { return a.ev > b.ev; });
        // Keep the best kBeamWidth *distinct* states (equal states have equal
        // evals, so dropping later duplicates never loses a better line).
        std::vector<Node> pruned;
        std::set<std::vector<int>> seen;
        for (Node& n : next) {
            if (!seen.insert(stateKey(n.state)).second) continue;
            pruned.push_back(std::move(n));
            if (static_cast<int>(pruned.size()) >= kBeamWidth) break;
        }
        beam = std::move(pruned);
        pool.insert(pool.end(), beam.begin(), beam.end());
    }

    // Pool -> K best distinct plans (stable: first-found wins ties, so a
    // fireball-then-move line isn't arbitrarily swapped for its permutation).
    std::stable_sort(pool.begin(), pool.end(),
                     [](const Node& a, const Node& b) { return a.ev > b.ev; });
    std::vector<PlanCandidate> out;
    std::set<std::vector<int>> seen;
    std::vector<char> taken(pool.size(), 0);
    auto take = [&](std::size_t i) {
        taken[i] = 1;
        out.push_back({std::move(pool[i].seq), std::move(pool[i].state), pool[i].ev});
    };
    if (diversify) {
        std::set<std::array<int, 4>> openings; // (kind, slot, target) of seq[0]
        for (std::size_t i = 0; i < pool.size() && static_cast<int>(out.size()) < k; ++i) {
            const std::vector<PlannedAction>& seq = pool[i].seq;
            const std::array<int, 4> opening =
                seq.empty() ? std::array<int, 4>{-1, 0, 0, 0} // "pass" is its own opening
                            : std::array<int, 4>{static_cast<int>(seq[0].kind), seq[0].slot,
                                                 seq[0].target.x, seq[0].target.y};
            if (!openings.insert(opening).second) continue;
            if (!seen.insert(stateKey(pool[i].state)).second) continue;
            take(i);
        }
    }
    for (std::size_t i = 0; i < pool.size() && static_cast<int>(out.size()) < k; ++i) {
        if (taken[i]) continue;
        if (!seen.insert(stateKey(pool[i].state)).second) continue;
        take(i);
    }
    return out;
}

// Beam search over action sequences; returns the path to the best end-state.
std::vector<PlannedAction> planTurn(const Battle& battle, EntityId self, const Evaluator& eval,
                                    bool useIntel) {
    const Faction me = battle.unit(self).team;
    // Foes don't move during our turn, so the context (foe field + any intel)
    // is computed once, from the REAL battle — clones have recording off, but
    // intel is already folded by then.
    const EvalContext ctx = makeEvalContext(battle, me, useIntel);
    int budget = kMaxPlies * kBeamWidth * 64; // ample for a single-turn beam
    std::vector<PlanCandidate> plans = topKPlans(battle, self, eval, ctx, 1, budget);
    return plans.empty() ? std::vector<PlannedAction>{} : std::move(plans[0].seq);
}

// --- Turn-level minimax (the "deep" / "adaptive" Brains) ---------------------
// The beam brains answer "what's my best turn against a frozen board"; the
// minimax brains answer "can I win the exchange": my candidate turn vs the
// opponent's best reply vs mine again, alternating along the real initiative
// order (allies are max nodes too, so 2v2/3v3 fall out naturally). A node's
// branches are whole turn-plans (topKPlans) — the opponent never interjects
// mid-sequence, which is what makes turn-level (not action-level) alternation
// the correct granularity. Alpha-beta prunes lines the opponent refutes.
//
// Horizons are even (whole exchanges): every line is priced *after* the
// opponent's reply, and the leaf evaluator keeps its static threat term, so
// exposure at the horizon is still felt (no "I fireballed and the game ended").
// Work is capped by a shared clone budget — deterministic by construction, no
// wall clocks — and iterative deepening adopts only fully-completed rounds.
// Under intel ("adaptive"), enemy replies are generated from *revealed* slots
// only (the believed opponent), and leaves are valued with the root's
// knowledge: what the AI hasn't seen can't steer its play.

AIAction summonTakeOneAction(Battle& b, EntityId self); // defined below

constexpr int kDeepK = 4;          // candidate turn-plans per reply node
constexpr int kDeepRootK = 8;      // root candidates (diversified — see topKPlans)
constexpr int kDeepBudget = 24000; // simulated clones per decision
constexpr int kDeepMaxDepth = 6;   // unit-turn horizon cap (iterated 2, 4, 6)

const double kInf = std::numeric_limits<double>::infinity();

// Value of a settled position from the root player's perspective. The foe
// field is rebuilt for the leaf position (units have moved); intel is the
// root's — knowledge doesn't change inside a hypothetical.
double leafEval(const Battle& b, Faction root, const Evaluator& eval, const Intel* rootIntel) {
    EvalContext ctx = makeEvalContext(b, root, false);
    if (rootIntel) ctx.intel = *rootIntel;
    return eval.evaluate(b, ctx);
}

double searchTurns(const Battle& state, Faction root, int turnsLeft, double alpha, double beta,
                   const Evaluator& eval, const Intel* rootIntel, int& budget) {
    if (state.phase() == Phase::Finished || turnsLeft <= 0 || budget <= 0)
        return leafEval(state, root, eval, rootIntel);
    const EntityId self = state.activeUnit();
    const Entity& u = state.unit(self);

    // Scripted units (summons act simply, objects just tick) don't branch:
    // play the fixed behaviour and pass through without charging depth — the
    // horizon counts champion *decisions*.
    if (u.kind != EntityKind::Champion) {
        Battle s2 = state;
        --budget;
        if (u.kind == EntityKind::Summon)
            for (int i = 0; i < 8 && s2.phase() != Phase::Finished; ++i)
                if (summonTakeOneAction(s2, self) == AIAction::Done) break;
        if (s2.phase() != Phase::Finished) s2.endTurn();
        return searchTurns(s2, root, turnsLeft, alpha, beta, eval, rootIntel, budget);
    }

    const bool maxNode = u.team == root;
    // Candidates are generated from the acting unit's own perspective; the
    // root side keeps its intel, and an intel-tracked enemy champion is masked
    // to its revealed slots — the search plays the believed opponent, not the
    // actual hand.
    EvalContext ctx = makeEvalContext(state, u.team, false);
    if (rootIntel && maxNode) ctx.intel = *rootIntel;
    const std::vector<char>* mask = nullptr;
    if (rootIntel && !maxNode && self < rootIntel->byId.size() && rootIntel->tracks(state, self))
        mask = &rootIntel->byId[self].revealedSlots;
    std::vector<PlanCandidate> plans = topKPlans(state, self, eval, ctx, kDeepK, budget, mask);

    double best = maxNode ? -kInf : kInf;
    for (PlanCandidate& p : plans) { // never empty: the do-nothing root is always a candidate
        Battle s2 = std::move(p.state);
        if (s2.phase() != Phase::Finished) s2.endTurn();
        const double v = searchTurns(s2, root, turnsLeft - 1, alpha, beta, eval, rootIntel, budget);
        if (maxNode) { best = std::max(best, v); alpha = std::max(alpha, best); }
        else         { best = std::min(best, v); beta = std::min(beta, best); }
        if (alpha >= beta) break; // refuted — the other side won't allow this line
    }
    return best;
}

class DeepBrain final : public Brain {
public:
    explicit DeepBrain(std::string_view name, bool useIntel,
                       const Evaluator& eval = handcraftedEvaluator())
        : name_(name), useIntel_(useIntel), eval_(eval) {}

    [[nodiscard]] std::vector<PlannedAction> planTurn(const Battle& battle,
                                                      EntityId self) const override {
        const Faction me = battle.unit(self).team;
        const EvalContext ctx = makeEvalContext(battle, me, useIntel_);
        const Intel* rootIntel = ctx.intel ? &*ctx.intel : nullptr;
        int budget = kDeepBudget;
        std::vector<PlanCandidate> plans = topKPlans(battle, self, eval_, ctx, kDeepRootK,
                                                     budget, nullptr, /*diversify=*/true);
        if (plans.empty()) return {};

        std::vector<PlannedAction> best = plans[0].seq; // depth-0 fallback = beam's answer
        for (int depth = 2; depth <= kDeepMaxDepth && budget > 0; depth += 2) {
            double bestV = -kInf;
            std::size_t pick = 0;
            double alpha = -kInf;
            bool complete = true;
            for (std::size_t i = 0; i < plans.size(); ++i) {
                Battle s2 = plans[i].state; // copy — candidates are reused per iteration
                if (s2.phase() != Phase::Finished) s2.endTurn();
                const double v =
                    searchTurns(s2, me, depth - 1, alpha, kInf, eval_, rootIntel, budget);
                if (v > bestV) { bestV = v; pick = i; }
                alpha = std::max(alpha, v);
                if (budget <= 0 && i + 1 < plans.size()) { complete = false; break; }
            }
            if (complete) best = plans[pick].seq; // adopt only finished iterations
        }
        return best;
    }
    [[nodiscard]] std::string_view name() const override { return name_; }

private:
    std::string_view name_;
    bool useIntel_;
    const Evaluator& eval_;
};

// The default Brain: a thin wrapper over the beam search above, scoring states
// through its Evaluator (the handcrafted one by default — see Evaluator.h).
// Two registered flavours share this class: "beam" (omniscient, the default)
// and "scout" (intel mode — plays what it has *seen* of enemy loadouts plus a
// decaying unknown prior, see Intel.h). Stateless, so shared instances are safe.
class BeamSearchBrain final : public Brain {
public:
    explicit BeamSearchBrain(std::string_view name = "beam", bool useIntel = false,
                             const Evaluator& eval = handcraftedEvaluator())
        : name_(name), useIntel_(useIntel), eval_(eval) {}
    [[nodiscard]] std::vector<PlannedAction> planTurn(const Battle& battle,
                                                      EntityId self) const override {
        return tb::planTurn(battle, self, eval_, useIntel_);
    }
    [[nodiscard]] std::string_view name() const override { return name_; }

private:
    std::string_view name_;
    bool useIntel_;
    const Evaluator& eval_;
};

// A deliberately weaker toy: a greedy 1-ply hill-climb. Each step picks the
// single action that most improves the evaluation (ties keep the first
// enumerated, so it's deterministic) and stops once nothing improves. No
// look-ahead — a foil for the beam search and a worked template for community
// Brains (Phase 3.2).
class GreedyBrain final : public Brain {
public:
    explicit GreedyBrain(const Evaluator& eval = handcraftedEvaluator()) : eval_(eval) {}
    [[nodiscard]] std::vector<PlannedAction> planTurn(const Battle& battle,
                                                      EntityId self) const override {
        const Faction me = battle.unit(self).team;
        const EvalContext ctx = makeEvalContext(battle, me);
        Battle state = battle;
        state.setEventRecording(false); // throwaway sims don't narrate (and stay cheap to clone)
        std::vector<PlannedAction> seq;
        double cur = eval_.evaluate(state, ctx);
        for (int step = 0; step < kMaxPlies; ++step) {
            if (state.phase() == Phase::Finished || !state.unit(self).alive()) break;
            double best = cur;
            std::optional<PlannedAction> pick;
            Battle picked = state;
            for (const PlannedAction& a : enumerateActions(state, self)) {
                Battle s2 = state; // clone + simulate
                applyAction(s2, self, a);
                const double e = eval_.evaluate(s2, ctx);
                if (e > best) { best = e; pick = a; picked = std::move(s2); }
            }
            if (!pick) break; // no improving move — stop (greedy has no look-ahead)
            seq.push_back(*pick);
            state = std::move(picked);
            cur = best;
        }
        return seq;
    }
    [[nodiscard]] std::string_view name() const override { return "greedy"; }

private:
    const Evaluator& eval_;
};

// The Brains known to brainByName()/selection. Built-ins are inserted on first
// access; registerBrain() appends. Pointers are non-owning — every Brain here is
// a static singleton that outlives the program.
std::vector<const Brain*>& brainRegistry() {
    static std::vector<const Brain*> reg = [] {
        static const GreedyBrain greedy;
        static const BeamSearchBrain scout("scout", /*useIntel=*/true);
        static const DeepBrain deep("deep", /*useIntel=*/false);
        static const DeepBrain adaptive("adaptive", /*useIntel=*/true);
        return std::vector<const Brain*>{&defaultBrain(), &greedy, &scout, &deep, &adaptive};
    }();
    return reg;
}

AIAction executeFirst(Battle& battle, EntityId self, const PlannedAction& a) {
    if (a.kind == PlannedAction::Kind::Cast) {
        battle.cast(self, a.slot, a.target);
        return AIAction::Attacked;
    }
    const Entity& me = battle.unit(self);
    auto path = findPath(battle.grid(), me.pos, a.target, battle.pathBlockers(self));
    if (path.size() < 2 || !battle.stepTo(self, path[1])) return AIAction::Done;
    return AIAction::Moved;
}

// One step of shortest-path movement toward `goal`.
AIAction stepToward(Battle& b, EntityId self, Vec2i goal) {
    if (b.unit(self).mp <= 0) return AIAction::Done;
    auto path = findPath(b.grid(), b.unit(self).pos, goal, b.pathBlockers(self));
    if (path.size() < 2 || !b.stepTo(self, path[1])) return AIAction::Done;
    return AIAction::Moved;
}

// Dead-simple, single-purpose summon behaviour — summons "do one thing": a
// support unit heals the most-wounded ally, a puller (blocker) yanks foes onto
// itself, anything else strikes the nearest foe; each closes the distance when it
// can't act yet. Deliberately not the beam planner — summons aren't meant to be
// clever.
AIAction summonTakeOneAction(Battle& b, EntityId self) {
    const Entity& me = b.unit(self);
    if (me.spells.empty()) {
        if (auto foe = b.nearestFoe(self)) return stepToward(b, self, b.unit(*foe).pos);
        return AIAction::Done;
    }
    const Spell& sp = me.spells[0];
    const bool support =
        hasEffect(sp, Effect::Type::Heal) || hasStatusEffect(sp, StatusEffect::Kind::Shield);
    const bool puller = hasEffect(sp, Effect::Type::Pull);

    if (support) {
        std::optional<EntityId> best;
        double worst = 1.0;
        for (EntityId i = 0; i < b.unitCount(); ++i) {
            const Entity& u = b.unit(i);
            if (!u.alive() || u.team != me.team || u.maxHp <= 0) continue;
            const double frac = static_cast<double>(u.hp) / u.maxHp;
            if (frac < 1.0 && (!best || frac < worst)) { best = i; worst = frac; }
        }
        if (!best) return AIAction::Done; // nobody hurt — hold position
        const Vec2i tp = b.unit(*best).pos;
        if (b.canCast(self, 0, tp)) { b.cast(self, 0, tp); return AIAction::Attacked; }
        return stepToward(b, self, tp);
    }

    if (puller) {
        // Self-cast the cross pull when any foe sits on its arms (4 cardinal
        // lines, range 4); otherwise advance to drag more foes into range.
        if (b.canCast(self, 0, me.pos)) {
            for (EntityId id : b.unitsAt(b.affectedTiles(sp, me.pos, me.pos)))
                if (b.unit(id).team != me.team) {
                    b.cast(self, 0, me.pos);
                    return AIAction::Attacked;
                }
        }
        if (auto foe = b.nearestFoe(self)) return stepToward(b, self, b.unit(*foe).pos);
        return AIAction::Done;
    }

    // Default (damage): strike the nearest foe, or close in.
    if (auto foe = b.nearestFoe(self)) {
        const Vec2i fp = b.unit(*foe).pos;
        if (b.canCast(self, 0, fp)) { b.cast(self, 0, fp); return AIAction::Attacked; }
        return stepToward(b, self, fp);
    }
    return AIAction::Done;
}

} // namespace

const Brain& defaultBrain() {
    static const BeamSearchBrain brain;
    return brain;
}

const Brain* brainByName(std::string_view name) {
    for (const Brain* b : brainRegistry())
        if (b->name() == name) return b;
    return nullptr;
}

std::vector<std::string_view> brainNames() {
    std::vector<std::string_view> out;
    for (const Brain* b : brainRegistry()) out.push_back(b->name());
    return out;
}

bool registerBrain(const Brain& brain) {
    if (brainByName(brain.name())) return false; // name taken — keep the first
    brainRegistry().push_back(&brain);
    return true;
}

AIAction enemyTakeOneAction(Battle& battle, EntityId self, const Brain& brain) {
    if (battle.phase() == Phase::Finished || !battle.unit(self).alive()) return AIAction::Done;
    if (battle.unit(self).kind == EntityKind::Summon) return summonTakeOneAction(battle, self);
    const std::vector<PlannedAction> plan = brain.planTurn(battle, self);
    if (plan.empty()) return AIAction::Done;
    // Execute just the first step (movement one tile at a time for animation);
    // the next call re-plans from the resulting state.
    return executeFirst(battle, self, plan.front());
}

void runEnemyTurn(Battle& battle, bool autoEndTurn, const Brain& brain) {
    const EntityId self = battle.activeUnit();
    if (battle.unit(self).kind == EntityKind::Summon) {
        // Simple summons act one step at a time until they're spent. (Summons are
        // deliberately not Brain-driven — they run a fixed, simple behaviour.)
        for (int i = 0; i < 8 && battle.phase() != Phase::Finished; ++i)
            if (summonTakeOneAction(battle, self) == AIAction::Done) break;
    } else {
        // Plan once, execute the whole sequence (efficient for headless / sim).
        for (const PlannedAction& a : brain.planTurn(battle, self)) {
            if (battle.phase() == Phase::Finished) break;
            applyAction(battle, self, a);
        }
    }
    if (autoEndTurn && battle.phase() != Phase::Finished) battle.endTurn();
}

} // namespace tb
