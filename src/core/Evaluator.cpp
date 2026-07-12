#include "Evaluator.h"

#include "SpellTraits.h"

#include <algorithm>

namespace tb {

namespace {

int pendingDoT(const Entity& e) {
    int d = 0;
    for (const StatusEffect& s : e.statuses)
        if (s.kind == StatusEffect::Kind::DamageOverTime) d += s.magnitude * s.remainingTurns;
    return d;
}

int shieldPool(const Entity& e) {
    int p = 0;
    for (const StatusEffect& s : e.statuses)
        if (s.kind == StatusEffect::Kind::Shield) p += s.magnitude;
    return p;
}

// Active RangeDebuff percentage on `attacker` (e.g. Blind), 0..100.
int rangeDebuffPct(const Entity& attacker) {
    int pct = 0;
    for (const StatusEffect& s : attacker.statuses)
        if (s.kind == StatusEffect::Kind::RangeDebuff && s.delay <= 0) pct += s.magnitude;
    return std::min(pct, 100);
}

// The attacker's usable reach with `sp` next turn, honouring any active
// RangeDebuff — same integer math as Battle::canCast. This is what lets the
// planner see Blind's value: a blinded foe's threat envelope shrinks.
int effectiveMaxRange(const Entity& attacker, const Spell& sp) {
    const int pct = rangeDebuffPct(attacker);
    if (pct <= 0) return sp.maxRange;
    int effMax = sp.maxRange - (sp.maxRange * pct) / 100;
    return effMax < sp.minRange ? sp.minRange : effMax;
}

// The attacker's movement budget next turn: maxMp plus any active MpBuff — a
// Flux-slowed foe (negative magnitude) closes less distance.
int nextTurnMp(const Entity& attacker) {
    int bonus = 0;
    for (const StatusEffect& s : attacker.statuses)
        if (s.kind == StatusEffect::Kind::MpBuff && s.delay <= 0) bonus += s.magnitude;
    return std::max(0, attacker.maxMp + bonus);
}

// Worst single hit any foe could land on `victim` next turn — range, movement,
// LOS, and the attacker's range/movement debuffs aware (a Blinded or Flux-slowed
// attacker projects a genuinely smaller envelope; that's what makes those spells
// worth casting). An *invisible* victim can't be targeted, so its incoming
// damage is zero — the lever the planner uses to value cloaking.
//
// `nextTurnOnly` decides how cooldowns are read. For MY units' risk we want the
// accurate next-turn view: a spell threatens only if its cooldown will have
// elapsed (cooldowns tick at the owner's turn start, so cd <= 1 is castable) —
// this lets the planner punish a spent cooldown by advancing. For the pressure
// side (how menaced my FOES are) we deliberately stay cooldown-blind: a spell on
// cooldown comes back, and pricing my menace by momentary cooldowns would teach
// the planner that casting its best spell "weakens" it.
// `intel` (nullable) engages the observed-opponent model: only *revealed* slots
// of tracked foes count as concrete threats, plus a decaying unknown prior for
// what they might still be hiding (see EvalWeights::unknownThreatBase). The
// prior's range envelope honours the attacker's debuffs too — Blind also
// shrinks what an unknown spell could reach.
int expectedIncoming(const Battle& b, EntityId victimId, bool nextTurnOnly, const Intel* intel,
                     const EvalWeights& w) {
    const Entity& v = b.unit(victimId);
    if (!v.alive() || v.invisible()) return 0;
    int worst = 0;
    for (EntityId i = 0; i < b.unitCount(); ++i) {
        const Entity& f = b.unit(i);
        if (!f.alive() || f.team == v.team) continue; // a foe's own invisibility doesn't blind it
        const int dist = manhattan(f.pos, v.pos);
        const bool los = b.clearLineOfSight(f.pos, v.pos);
        const int mp = nextTurnMp(f);
        // Entities beyond the intel snapshot are mid-sim spawns (summons/bombs)
        // — public templates, never intel-tracked.
        const FoeIntel* fi =
            (intel && i < intel->byId.size() && intel->tracks(b, i)) ? &intel->byId[i] : nullptr;
        for (std::size_t slot = 0; slot < f.spells.size(); ++slot) {
            if (fi && !fi->revealed(slot)) continue; // unseen — priced by the prior below
            const Spell& sp = f.spells[slot];
            const int dmg = spellDamage(sp);
            if (dmg <= 0) continue;
            if (nextTurnOnly && slot < f.spellCooldowns.size() && f.spellCooldowns[slot] > 1)
                continue;
            if (dist > effectiveMaxRange(f, sp) + mp) continue;
            if (sp.needsLineOfSight && !los) continue;
            worst = std::max(worst, dmg);
        }
        if (fi) {
            double base = w.unknownThreatBase;
            for (int t = 0; t < fi->turnsObserved; ++t) base *= w.unknownThreatDecay;
            const int pct = rangeDebuffPct(f);
            const int effRange = w.unknownThreatRange - (w.unknownThreatRange * pct) / 100;
            if (base >= 1.0 && dist <= effRange + mp)
                worst = std::max(worst, static_cast<int>(base));
        }
    }
    return worst;
}

// Discounted value of a unit's banked AP/MP buffs (see EvalWeights::apValue).
// Statuses are stored with their applied sign (polarized debuffs arrive
// negative), so this term rewards buffing allies AND slowing foes symmetrically.
// A delayed status (Surge's crash) burns its delay before ticking, so its turns
// are discounted from further out — often past the horizon of a short fight.
double statusEconomy(const Entity& e, const EvalWeights& w) {
    double v = 0.0;
    for (const StatusEffect& s : e.statuses) {
        double perPoint = 0.0;
        if (s.kind == StatusEffect::Kind::ApBuff) perPoint = w.apValue;
        else if (s.kind == StatusEffect::Kind::MpBuff) perPoint = w.mpValue;
        else continue;
        double g = 1.0;
        for (int t = 0; t < s.delay; ++t) g *= w.futureDiscount;
        double turns = 0.0;
        for (int t = 0; t < s.remainingTurns; ++t) { g *= w.futureDiscount; turns += g; }
        v += perPoint * s.magnitude * turns;
    }
    return v;
}

// Blast damage threatening `victim` from any live bomb (an Object with a damaging
// onDeath) whose detonation footprint covers the victim's tile. Bombs are short-
// fused and also detonate when killed, so any in-range bomb is treated as a live
// threat — this pulls the planner out of blast zones (and stops it dragging a
// bomb toward its own units, since that raises this term for them).
int expectedBlast(const Battle& b, EntityId victimId) {
    const Entity& v = b.unit(victimId);
    if (!v.alive()) return 0;
    int worst = 0;
    for (EntityId i = 0; i < b.unitCount(); ++i) {
        const Entity& e = b.unit(i);
        if (!e.alive() || e.kind != EntityKind::Object || e.onDeath.effects.empty()) continue;
        int dmg = 0;
        for (const Effect& fx : e.onDeath.effects)
            if (fx.type == Effect::Type::Damage) dmg += fx.amount;
        if (dmg <= 0) continue;
        for (Vec2i t : b.affectedTiles(e.onDeath, e.pos, e.pos))
            if (t == v.pos) { worst = std::max(worst, dmg); break; }
    }
    return worst;
}

// BFS distance from the nearest foe of `me` to every tile (min across foes).
std::vector<int> buildFoeField(const Battle& b, Faction me) {
    const Grid& g = b.grid();
    std::vector<int> field(static_cast<std::size_t>(g.width()) * g.height(), -1);
    for (EntityId i = 0; i < b.unitCount(); ++i) {
        const Entity& f = b.unit(i);
        if (!f.alive() || f.team == me || f.kind == EntityKind::Object) continue; // not bombs
        std::vector<int> d = distanceField(g, f.pos);
        for (std::size_t k = 0; k < field.size(); ++k)
            if (d[k] >= 0 && (field[k] < 0 || d[k] < field[k])) field[k] = d[k];
    }
    return field;
}

} // namespace

EvalContext makeEvalContext(const Battle& battle, Faction me, bool withIntel) {
    EvalContext ctx{me, buildFoeField(battle, me), std::nullopt};
    if (withIntel) ctx.intel = buildIntel(battle, me);
    return ctx;
}

// Captures banked DoT, shields and (crucially) the damage we expect to take, so
// defensive plans are valued.
double HandcraftedEvaluator::evaluate(const Battle& b, const EvalContext& ctx) const {
    const Faction me = ctx.me;
    const int unreachable = b.grid().width() + b.grid().height();
    double score = 0.0;
    for (EntityId i = 0; i < b.unitCount(); ++i) {
        const Entity& u = b.unit(i);
        if (u.alive()) {
            const double effHp = u.hp + shieldPool(u) - w_.dotWeight * pendingDoT(u) +
                                 statusEconomy(u, w_);
            // Intel only shapes MY units' risk — my own loadout is no secret to
            // me, so the menace-to-foes side stays fully informed either way.
            const Intel* intel = ctx.intel ? &*ctx.intel : nullptr;
            const double risk = w_.riskWeight *
                                (expectedIncoming(b, i, /*nextTurnOnly=*/u.team == me,
                                                  u.team == me ? intel : nullptr, w_) +
                                 expectedBlast(b, i));
            score += (u.team == me) ? (effHp - risk) : -(effHp - risk);
            // Asymmetric aggression: only *my* units are rewarded for closing in,
            // so the gradient pulls us toward combat instead of a mutual standoff.
            if (u.team == me) {
                int d = ctx.foeField[b.grid().index(u.pos)];
                if (d < 0) d = unreachable;
                score -= w_.aggression * d;
                if (b.inStorm(u.pos)) score -= w_.stormWeight * b.stormDamage(); // flee the ring
            }
        } else {
            score += (u.team == me) ? -w_.lossPenalty : w_.killBonus;
        }
    }
    return score;
}

const Evaluator& handcraftedEvaluator() {
    static const HandcraftedEvaluator eval;
    return eval;
}

} // namespace tb
