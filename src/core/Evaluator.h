#pragma once
//
// Evaluator.h — the Brain's position-scoring seam (Phase 3.5.1).
//
// A Brain searches; an Evaluator says how good a resulting Battle state is for
// one faction. Splitting the two means search improvements (deeper look-ahead,
// opponent modelling) and evaluation improvements (better heuristics, learned
// weights, eventually the NNUE-style LearnedEvaluator of Phase 3.5) land
// independently. Implementations must be deterministic and side-effect-free —
// replays and the balance gauntlet depend on it.
//
// `EvalContext` carries per-turn precomputed inputs that are constant while one
// unit plans (foes don't move during our turn): today the foe distance field,
// later the observed-opponent intel (H.3). Build it once per planTurn with
// `makeEvalContext` and share it across every candidate-state evaluation.
//
#include "Battle.h"
#include "Intel.h"

#include <optional>
#include <string_view>
#include <vector>

namespace tb {

// Precomputed, plan-wide evaluation inputs for one faction's turn.
struct EvalContext {
    Faction me = Faction::Enemy;
    // BFS walking distance from the nearest foe of `me` to every tile (min
    // across foes; -1 = unreachable). Closing in always lowers it, even around
    // walls — the aggression gradient reads it.
    std::vector<int> foeField;
    // Observed-opponent knowledge (see Intel.h). Engaged: the risk term prices
    // only *revealed* enemy spells plus a decaying unknown prior — the AI plays
    // what it has seen, not the opponent's hand. Disengaged (nullopt): the
    // classic omniscient evaluation.
    std::optional<Intel> intel;
};

// Builds the context for `me` planning on `battle` (computes the foe field;
// folds the event stream into intel when `withIntel`).
[[nodiscard]] EvalContext makeEvalContext(const Battle& battle, Faction me,
                                          bool withIntel = false);

// How good `battle` is for `ctx.me` — higher is better. Zero-sum by design:
// what's good for one faction is bad for the other, so a turn-level minimax
// search can negate it at opponent nodes.
class Evaluator {
public:
    virtual ~Evaluator() = default;
    [[nodiscard]] virtual double evaluate(const Battle& battle, const EvalContext& ctx) const = 0;
    [[nodiscard]] virtual std::string_view name() const = 0;
};

// The hand-tuned linear evaluation the beam search has always used. Weights are
// public so the offline tuning loop (H.4 / 3.5.6) can fit them from self-play
// and construct a re-weighted instance.
struct EvalWeights {
    double dotWeight = 0.9;   // banked damage-over-time, counted as near-dealt
    double riskWeight = 0.85; // fear of incoming damage next turn
    // Death bounties are kind-aware. Champions carry the full amounts — only
    // they decide victory. A summon is worth its body (effHp) and its remaining
    // output (the risk term), so its bounty is a nudge, not a verdict: a summon
    // that dies after soaking an enemy turn was a won trade. Objects (bombs)
    // get no bounty at all — detonating is their job, and their menace is
    // already priced by expectedBlast.
    double killBonus = 45.0;
    double lossPenalty = 70.0;
    double summonKillBonus = 10.0;
    double summonLossPenalty = 10.0;
    double aggression = 0.35; // reward closing on foes (breaks the standoff;
                              // threat terms alone are symmetric so wouldn't)
    double stormWeight = 1.1; // flee the closing ring (slightly > taking the hit)
    // AP/MP buffs only land at the owner's NEXT turn start (they feed the AP/MP
    // reset — see Battle::startTurnFor), so a within-turn simulation can never
    // observe their payoff. These price a banked buff directly: one point of AP
    // is worth ~a third of an attack, one MP about a tile of tempo, and each
    // future turn is discounted (the fight may end before a delayed crash bites
    // — this is what makes Surge's +AP-now / -AP-later gamble worth taking).
    double apValue = 3.0;
    double mpValue = 1.0;
    double futureDiscount = 0.7; // per-turn discount on not-yet-ticked buff turns
    // The unknown-threat prior (intel mode only): how hard an *unrevealed*
    // enemy spell is assumed to hit, decaying per turn the foe has been
    // observed not showing it (if they had it, they'd likely have used it).
    // Yields emergent probing: cautious early, confident once foes have shown
    // their hand. Range envelope of the hypothetical spell in tiles.
    double unknownThreatBase = 12.0;
    double unknownThreatDecay = 0.8;
    int unknownThreatRange = 6;
};

class HandcraftedEvaluator final : public Evaluator {
public:
    explicit HandcraftedEvaluator(EvalWeights w = {}) : w_(w) {}
    [[nodiscard]] double evaluate(const Battle& battle, const EvalContext& ctx) const override;
    [[nodiscard]] std::string_view name() const override { return "handcrafted"; }
    [[nodiscard]] const EvalWeights& weights() const { return w_; }

private:
    EvalWeights w_;
};

// Default-weight singleton (stateless), the Evaluator brains use when a caller
// doesn't supply one.
[[nodiscard]] const Evaluator& handcraftedEvaluator();

} // namespace tb
