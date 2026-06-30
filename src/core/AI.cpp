#include "AI.h"

#include <algorithm>
#include <vector>

namespace tb {

namespace {

// --- Tunables ---------------------------------------------------------------
constexpr int kMaxPlies = 4;   // max actions looked ahead within a turn
constexpr int kBeamWidth = 6;  // states kept between plies

struct EvalWeights {
    double dotWeight = 0.9;   // banked damage-over-time, counted as near-dealt
    double riskWeight = 0.85; // fear of incoming damage next turn
    double killBonus = 45.0;
    double lossPenalty = 70.0;
    double aggression = 0.35; // reward closing on foes (breaks the standoff;
                              // threat terms alone are symmetric so wouldn't)
    double stormWeight = 1.1; // flee the closing ring (slightly > taking the hit)
};
constexpr EvalWeights EW{};

// --- Small spell helpers ----------------------------------------------------
Vec2i cardinalToward(Vec2i from, Vec2i to) {
    int dx = to.x - from.x, dy = to.y - from.y;
    if (std::abs(dx) >= std::abs(dy)) return Vec2i{dx == 0 ? 0 : (dx > 0 ? 1 : -1), 0};
    return Vec2i{0, dy > 0 ? 1 : -1};
}

int spellDamage(const Spell& s) {
    int d = 0;
    for (const Effect& fx : s.effects) {
        if (fx.type == Effect::Type::Damage) d += fx.amount;
        else if (fx.type == Effect::Type::ApplyStatus &&
                 fx.status.kind == StatusEffect::Kind::DamageOverTime)
            d += fx.status.magnitude;
    }
    return d;
}
bool has(const Spell& s, Effect::Type t) {
    for (const Effect& fx : s.effects)
        if (fx.type == t) return true;
    return false;
}
bool hasStatusEffect(const Spell& s, StatusEffect::Kind k) {
    for (const Effect& fx : s.effects)
        if (fx.type == Effect::Type::ApplyStatus && fx.status.kind == k) return true;
    return false;
}

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

// Worst single hit any foe could land on `victim` next turn (range + movement +
// LOS aware, cooldowns ignored as a deliberate over-estimate). An *invisible*
// victim can't be targeted, so its incoming damage is zero — this is the lever
// the planner uses to value going invisible.
int expectedIncoming(const Battle& b, EntityId victimId) {
    const Entity& v = b.unit(victimId);
    if (!v.alive() || v.invisible()) return 0;
    int worst = 0;
    for (EntityId i = 0; i < b.unitCount(); ++i) {
        const Entity& f = b.unit(i);
        if (!f.alive() || f.team == v.team) continue; // a foe's own invisibility doesn't blind it
        const int dist = manhattan(f.pos, v.pos);
        const bool los = b.clearLineOfSight(f.pos, v.pos);
        for (const Spell& sp : f.spells) {
            const int dmg = spellDamage(sp);
            if (dmg <= 0 || dist > sp.maxRange + f.maxMp) continue;
            if (sp.needsLineOfSight && !los) continue;
            worst = std::max(worst, dmg);
        }
    }
    return worst;
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

// How good the board is for `me` — higher is better. Captures banked DoT, shields
// and (crucially) the damage we expect to take, so defensive plans are valued.
// `foeField` is BFS walking distance from the nearest foe to every tile (foes
// are static during our turn), so closing in always lowers it even around walls.
double evalState(const Battle& b, Faction me, const std::vector<int>& foeField) {
    const int unreachable = b.grid().width() + b.grid().height();
    double score = 0.0;
    for (EntityId i = 0; i < b.unitCount(); ++i) {
        const Entity& u = b.unit(i);
        if (u.alive()) {
            const double effHp = u.hp + shieldPool(u) - EW.dotWeight * pendingDoT(u);
            const double risk = EW.riskWeight * (expectedIncoming(b, i) + expectedBlast(b, i));
            score += (u.team == me) ? (effHp - risk) : -(effHp - risk);
            // Asymmetric aggression: only *my* units are rewarded for closing in,
            // so the gradient pulls us toward combat instead of a mutual standoff.
            if (u.team == me) {
                int d = foeField[b.grid().index(u.pos)];
                if (d < 0) d = unreachable;
                score -= EW.aggression * d;
                if (b.inStorm(u.pos)) score -= EW.stormWeight * b.stormDamage(); // flee the ring
            }
        } else {
            score += (u.team == me) ? -EW.lossPenalty : EW.killBonus;
        }
    }
    return score;
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

// --- Action model -----------------------------------------------------------
struct PlannedAction {
    enum class Kind { Cast, Move } kind = Kind::Cast;
    int slot = -1;
    Vec2i target{};
};

void applyAction(Battle& b, EntityId self, const PlannedAction& a) {
    if (a.kind == PlannedAction::Kind::Cast) b.cast(self, a.slot, a.target);
    else b.moveToward(self, a.target);
}

std::vector<PlannedAction> enumerateActions(const Battle& b, EntityId self) {
    const Entity& me = b.unit(self);
    std::vector<PlannedAction> acts;

    std::vector<Vec2i> foeTiles, allyTiles;
    for (EntityId i = 0; i < b.unitCount(); ++i) {
        const Entity& u = b.unit(i);
        if (!u.alive() || u.kind == EntityKind::Object) continue; // don't target inert bombs
        if (u.team == me.team) allyTiles.push_back(u.pos);
        else if (!u.invisible()) foeTiles.push_back(u.pos); // can't target hidden foes
    }

    if (me.ap > 0) {
        for (int slot = 0; slot < static_cast<int>(me.spells.size()); ++slot) {
            const Spell& sp = me.spells[slot];
            const bool offensive = spellDamage(sp) > 0 || has(sp, Effect::Type::Push) ||
                                   has(sp, Effect::Type::Pull);
            const bool supportive = has(sp, Effect::Type::Heal) ||
                                    hasStatusEffect(sp, StatusEffect::Kind::Shield);
            const bool selfBuff = hasStatusEffect(sp, StatusEffect::Kind::Invisible);
            const bool placement = has(sp, Effect::Type::Spawn);

            std::vector<Vec2i> targets;
            if (offensive) targets.insert(targets.end(), foeTiles.begin(), foeTiles.end());
            if (supportive) targets.insert(targets.end(), allyTiles.begin(), allyTiles.end());
            if (selfBuff) targets.push_back(me.pos);
            if (placement && !foeTiles.empty()) {
                const Vec2i fp = foeTiles.front();
                const Vec2i dir = cardinalToward(me.pos, fp);
                const int step = std::min(sp.maxRange, std::max(1, manhattan(me.pos, fp) - 1));
                targets.push_back(Vec2i{me.pos.x + dir.x * step, me.pos.y + dir.y * step});
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
    }
    return acts;
}

// Beam search over action sequences; returns the path to the best end-state.
std::vector<PlannedAction> planTurn(const Battle& battle, EntityId self) {
    const Faction me = battle.unit(self).team;
    // Foes don't move during our turn, so their distance field is computed once.
    const std::vector<int> foeField = buildFoeField(battle, me);

    struct Node {
        Battle state;
        std::vector<PlannedAction> seq;
        double ev;
    };
    std::vector<Node> beam;
    beam.push_back({battle, {}, evalState(battle, me, foeField)});

    double bestEv = beam[0].ev;
    std::vector<PlannedAction> bestSeq;

    for (int ply = 0; ply < kMaxPlies; ++ply) {
        std::vector<Node> next;
        for (Node& n : beam) {
            if (n.state.phase() == Phase::Finished || !n.state.unit(self).alive()) continue;
            for (const PlannedAction& a : enumerateActions(n.state, self)) {
                Battle s2 = n.state; // clone + simulate
                applyAction(s2, self, a);
                const double e = evalState(s2, me, foeField);
                std::vector<PlannedAction> seq = n.seq;
                seq.push_back(a);
                if (e > bestEv) { bestEv = e; bestSeq = seq; }
                next.push_back({std::move(s2), std::move(seq), e});
            }
        }
        if (next.empty()) break;
        std::sort(next.begin(), next.end(), [](const Node& a, const Node& b) { return a.ev > b.ev; });
        if (static_cast<int>(next.size()) > kBeamWidth) // truncate (Node isn't default-constructible)
            next.erase(next.begin() + kBeamWidth, next.end());
        beam = std::move(next);
    }
    return bestSeq;
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
        has(sp, Effect::Type::Heal) || hasStatusEffect(sp, StatusEffect::Kind::Shield);
    const bool puller = has(sp, Effect::Type::Pull);

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

AIAction enemyTakeOneAction(Battle& battle, EntityId self) {
    if (battle.phase() == Phase::Finished || !battle.unit(self).alive()) return AIAction::Done;
    if (battle.unit(self).kind == EntityKind::Summon) return summonTakeOneAction(battle, self);
    const std::vector<PlannedAction> plan = planTurn(battle, self);
    if (plan.empty()) return AIAction::Done;
    // Execute just the first step (movement one tile at a time for animation);
    // the next call re-plans from the resulting state.
    return executeFirst(battle, self, plan.front());
}

void runEnemyTurn(Battle& battle, bool autoEndTurn) {
    const EntityId self = battle.activeUnit();
    if (battle.unit(self).kind == EntityKind::Summon) {
        // Simple summons act one step at a time until they're spent.
        for (int i = 0; i < 8 && battle.phase() != Phase::Finished; ++i)
            if (summonTakeOneAction(battle, self) == AIAction::Done) break;
    } else {
        // Plan once, execute the whole sequence (efficient for headless / sim).
        for (const PlannedAction& a : planTurn(battle, self)) {
            if (battle.phase() == Phase::Finished) break;
            applyAction(battle, self, a);
        }
    }
    if (autoEndTurn && battle.phase() != Phase::Finished) battle.endTurn();
}

} // namespace tb
