#include "Battle.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace tb {

namespace {

constexpr int kCollisionDamagePerCell = 5; // forced-move into a blocker
constexpr int kSummonCap = 2;              // max living summons per team

// Reduce an arbitrary delta to a single cardinal step (axis with larger span).
Vec2i cardinalStep(Vec2i from, Vec2i to) {
    int dx = to.x - from.x;
    int dy = to.y - from.y;
    if (std::abs(dx) >= std::abs(dy)) return Vec2i{dx == 0 ? 0 : (dx > 0 ? 1 : -1), 0};
    return Vec2i{0, dy > 0 ? 1 : -1};
}

} // namespace

// ---------------------------------------------------------------------------
Battle::Battle(Grid grid, std::vector<Entity> units, StormConfig storm)
    : grid_(std::move(grid)), units_(std::move(units)), storm_(storm) {
    stormCenter_ = Vec2i{grid_.width() / 2, grid_.height() / 2};
    const int dx = std::max(stormCenter_.x, grid_.width() - 1 - stormCenter_.x);
    const int dy = std::max(stormCenter_.y, grid_.height() - 1 - stormCenter_.y);
    stormMaxRadius_ = std::max(dx, dy);

    order_.resize(units_.size());
    for (EntityId i = 0; i < units_.size(); ++i) {
        order_[i] = i;
        if (units_[i].spellCooldowns.size() != units_[i].spells.size())
            units_[i].spellCooldowns.assign(units_[i].spells.size(), 0);
    }

    // Stable initiative sort: higher initiative acts first; ties keep input order.
    std::stable_sort(order_.begin(), order_.end(),
                     [&](EntityId a, EntityId b) { return units_[a].initiative > units_[b].initiative; });

    if (!order_.empty()) startTurnFor(order_[turnIdx_]);
}

// ---------------------------------------------------------------------------
EntityId Battle::spawnEntity(Entity e) {
    const EntityId id = static_cast<EntityId>(units_.size());
    if (e.spellCooldowns.size() != e.spells.size())
        e.spellCooldowns.assign(e.spells.size(), 0);
    units_.push_back(std::move(e));

    // Insert into initiative order: higher initiative first, ties by EntityId
    // ascending (matches the constructor's stable, id-ascending tie order). The
    // new id is the largest, so it lands after equal-initiative incumbents.
    const int newInit = units_[id].initiative;
    std::size_t insertAt = order_.size();
    for (std::size_t i = 0; i < order_.size(); ++i) {
        const EntityId other = order_[i];
        const int oi = units_[other].initiative;
        if (newInit > oi || (newInit == oi && id < other)) {
            insertAt = i;
            break;
        }
    }
    const bool hadUnits = !order_.empty();
    order_.insert(order_.begin() + static_cast<std::ptrdiff_t>(insertAt), id);
    // Keep the active unit pointed at: shift the cursor if we inserted at/before it.
    if (hadUnits && insertAt <= turnIdx_) ++turnIdx_;
    return id;
}

// ---------------------------------------------------------------------------
Phase Battle::phase() const {
    if (finished_ || order_.empty()) return Phase::Finished;
    return units_[activeUnit()].team == Faction::Player ? Phase::PlayerTurn : Phase::EnemyTurn;
}

int Battle::safeRadius() const {
    if (!storm_.enabled || round_ < storm_.startRound) return stormMaxRadius_;
    const int ringsClosed = round_ - storm_.startRound + 1;
    const int r = stormMaxRadius_ - ringsClosed;
    return r < 0 ? 0 : r;
}

bool Battle::inStorm(Vec2i p) const {
    if (!storm_.enabled || round_ < storm_.startRound) return false;
    const int cheby = std::max(std::abs(p.x - stormCenter_.x), std::abs(p.y - stormCenter_.y));
    return cheby > safeRadius();
}

std::optional<Faction> Battle::winner() const {
    if (!finished_) return std::nullopt;
    bool playerAlive = false, enemyAlive = false;
    for (const Entity& e : units_) {
        if (!e.alive() || !e.isChampion()) continue; // only Champions decide victory
        (e.team == Faction::Player ? playerAlive : enemyAlive) = true;
    }
    if (playerAlive == enemyAlive) return std::nullopt; // double-KO / none
    return playerAlive ? Faction::Player : Faction::Enemy;
}

std::optional<EntityId> Battle::unitAt(Vec2i tile) const {
    for (EntityId i = 0; i < units_.size(); ++i)
        if (units_[i].alive() && units_[i].pos == tile) return i;
    return std::nullopt;
}

std::optional<EntityId> Battle::nearestFoe(EntityId of) const {
    const Entity& me = units_[of];
    std::optional<EntityId> best;
    int bestDist = 0;
    for (EntityId i = 0; i < units_.size(); ++i) {
        const Entity& e = units_[i];
        if (!e.alive() || e.team == me.team) continue;
        if (e.invisible()) continue;                  // concealed foes can't be acquired
        if (e.kind == EntityKind::Object) continue;   // bombs aren't foes to fight
        int d = manhattan(me.pos, e.pos);
        if (!best || d < bestDist) { best = i; bestDist = d; }
    }
    return best;
}

std::vector<Vec2i> Battle::occupancy(std::optional<EntityId> exclude) const {
    std::vector<Vec2i> out;
    for (EntityId i = 0; i < units_.size(); ++i) {
        if (exclude && *exclude == i) continue;
        if (units_[i].alive()) out.push_back(units_[i].pos);
    }
    return out;
}

std::vector<Vec2i> Battle::wallTiles() const {
    std::vector<Vec2i> out;
    for (const GroundEffect& g : ground_)
        if (g.kind == GroundKind::Wall)
            out.insert(out.end(), g.tiles.begin(), g.tiles.end());
    return out;
}

std::vector<Vec2i> Battle::pathBlockers(EntityId mover) const {
    std::vector<Vec2i> out = occupancy(mover);
    const std::vector<Vec2i> walls = wallTiles();
    out.insert(out.end(), walls.begin(), walls.end());
    return out;
}

bool Battle::clearLineOfSight(Vec2i a, Vec2i b) const {
    return hasLineOfSight(grid_, a, b, wallTiles());
}

// ---------------------------------------------------------------------------
// Turn lifecycle
// ---------------------------------------------------------------------------
void Battle::startTurnFor(EntityId id) {
    Entity& e = units_[id];

    // A new round begins when the initiative leader starts its turn.
    if (!order_.empty() && id == order_[0]) ++round_;

    emit({EventType::TurnStart, id}); // before the start-of-turn ticks below fire

    // Cloak expiry: the pair is ticked on the ORIGINAL member's turn start; when
    // it runs out unrevealed, the original is real by rule (no secret needed —
    // deterministic for replays). An early reveal happens by casting instead.
    if (auto pi = pairIndexOf(id); pi && cloaks_[*pi].a == id && --cloaks_[*pi].turnsLeft <= 0)
        revealPair(*pi, id);

    // (Hook 2) Effect tick: buffs are "active" for the turn they tick, so we
    // apply them to the AP/MP reset *before* decrementing/expiring durations.
    // Delayed statuses (delay > 0) are inert until their delay has ticked out.
    int apBonus = 0, mpBonus = 0;
    for (const StatusEffect& s : e.statuses) {
        if (s.delay > 0) continue;
        if (s.kind == StatusEffect::Kind::ApBuff) apBonus += s.magnitude;
        else if (s.kind == StatusEffect::Kind::MpBuff) mpBonus += s.magnitude;
    }
    e.ap = std::max(0, e.maxAp + apBonus); // debuffs can push below zero — floor it
    e.mp = std::max(0, e.maxMp + mpBonus);

    for (const StatusEffect& s : e.statuses) {
        if (s.delay > 0) continue;
        if (s.kind == StatusEffect::Kind::DamageOverTime) applyDamage(id, s.magnitude);
    }

    // Fuse: an armed object (bomb) detonates when its countdown elapses — kill it
    // so its onDeath (the blast) fires. Skipped if ignition/an attack already did.
    if (e.alive() && e.fuse > 0 && --e.fuse == 0) applyDamage(id, e.hp);

    // Rewind owns its own countdown (and may replace this unit's state), so it is
    // ticked before — and excluded from — the generic status aging below.
    rewindTick(id);

    // Age and expire statuses (Rewind markers are managed by rewindTick). A
    // delayed status burns its delay first; its own duration starts after.
    for (StatusEffect& s : e.statuses) {
        if (s.kind == StatusEffect::Kind::Rewind) continue;
        if (s.delay > 0) --s.delay;
        else --s.remainingTurns;
    }
    e.statuses.erase(std::remove_if(e.statuses.begin(), e.statuses.end(),
                                    [](const StatusEffect& s) {
                                        return s.kind != StatusEffect::Kind::Rewind &&
                                               s.delay <= 0 && s.remainingTurns <= 0;
                                    }),
                     e.statuses.end());

    // Cool down this unit's spells.
    for (int& cd : e.spellCooldowns)
        if (cd > 0) --cd;

    // Age battlefield features once per turn.
    tickGround();

    // Closing ring: bleed any unit caught outside the shrinking safe square.
    if (e.alive() && inStorm(e.pos)) applyDamage(id, storm_.damage, DamageSource::Storm);
}

void Battle::tickGround() {
    for (GroundEffect& g : ground_) --g.remainingTurns;
    ground_.erase(std::remove_if(ground_.begin(), ground_.end(),
                                 [](const GroundEffect& g) { return g.remainingTurns <= 0; }),
                  ground_.end());
}

void Battle::endTurn() {
    if (finished_ || order_.empty()) return;
    // Advance to the next living unit in the initiative order.
    for (std::size_t i = 0; i < order_.size(); ++i) {
        turnIdx_ = (turnIdx_ + 1) % order_.size();
        if (units_[order_[turnIdx_]].alive()) break;
    }
    startTurnFor(order_[turnIdx_]);
}

// ---------------------------------------------------------------------------
// Damage / victory
// ---------------------------------------------------------------------------
void Battle::applyDamage(EntityId id, int amount, DamageSource src) {
    Entity& e = units_[id];
    if (amount <= 0 || !e.alive()) return;

    // Cloaked pair member: the hit DEFERS — accumulate into the member's pending
    // pool, no HP change, no death, no victory check. Only the real member's pool
    // lands at reveal (shields interact then, against the summed amount). The
    // Damage event still narrates (the attacker knows what they dealt; the HP
    // just doesn't visibly move — that ambiguity is the mechanic).
    if (auto pi = pairIndexOf(id)) {
        CloakPair& p = cloaks_[*pi];
        (id == p.a ? p.pendingA : p.pendingB) += amount;
        emit({EventType::Damage, /*actor=*/id, /*target=*/id, amount, -1, src});
        return;
    }

    // (Hook 2) Shields absorb first.
    for (StatusEffect& s : e.statuses) {
        if (amount <= 0) break;
        if (s.kind != StatusEffect::Kind::Shield) continue;
        int absorbed = std::min(s.magnitude, amount);
        s.magnitude -= absorbed;
        amount -= absorbed;
    }
    e.statuses.erase(std::remove_if(e.statuses.begin(), e.statuses.end(),
                                    [](const StatusEffect& s) {
                                        return s.kind == StatusEffect::Kind::Shield && s.magnitude <= 0;
                                    }),
                     e.statuses.end());

    e.hp -= amount;
    if (e.hp < 0) e.hp = 0;
    if (amount > 0) emit({EventType::Damage, /*actor=*/id, /*target=*/id, amount, -1, src});
    if (!e.alive()) {
        lastDeathSource_ = src;
        emit({EventType::Death, /*actor=*/id, /*target=*/id, 0, -1, src}); // before onDeath resolves
        // Death-triggered effects (e.g. a bomb's detonation). Copy first — the
        // resolution mutates units_ and can recurse (chain detonations); a dead
        // unit's applyDamage early-returns, so chains terminate.
        if (!e.onDeath.effects.empty()) {
            const Spell death = e.onDeath;
            const Faction team = e.team;
            const Vec2i at = e.pos;
            applySpellEffects(death, team, at, at);
        }
    }
    checkVictory();
}

void Battle::checkVictory() {
    bool playerAlive = false, enemyAlive = false;
    for (const Entity& e : units_) {
        if (!e.alive() || !e.isChampion()) continue; // summons/objects don't keep a team alive
        (e.team == Faction::Player ? playerAlive : enemyAlive) = true;
    }
    if (!playerAlive || !enemyAlive) finished_ = true;
}

// ---------------------------------------------------------------------------
// Movement
// ---------------------------------------------------------------------------
bool Battle::stepTo(EntityId who, Vec2i adjacent) {
    Entity& e = units_[who];
    if (phase() == Phase::Finished) return false;
    if (e.mp <= 0) return false;
    if (manhattan(e.pos, adjacent) != 1) return false;
    if (!grid_.isWalkable(adjacent)) return false;
    if (unitAt(adjacent)) return false; // tile occupied by another unit
    for (Vec2i w : wallTiles())
        if (w == adjacent) return false; // blocked by a Shelter wall

    e.pos = adjacent;
    e.mp -= 1;
    emit({EventType::Move, /*actor=*/who, /*target=*/0, /*amount=*/1, -1,
          DamageSource::Spell, StatusEffect::Kind::DamageOverTime, /*to=*/adjacent});
    onEnterTile(who);
    return true;
}

int Battle::moveToward(EntityId who, Vec2i dest) {
    Entity& e = units_[who];
    if (phase() == Phase::Finished || e.mp <= 0) return 0;

    std::vector<Vec2i> path = findPath(grid_, e.pos, dest, pathBlockers(who));
    if (path.size() < 2) return 0; // path[0] is the current tile

    int moved = 0;
    for (std::size_t i = 1; i < path.size() && e.mp > 0; ++i) {
        if (!stepTo(who, path[i])) break;
        ++moved;
    }
    return moved;
}

// ---------------------------------------------------------------------------
// Spellcasting
// ---------------------------------------------------------------------------
bool Battle::canCast(EntityId caster, int spellIdx, Vec2i target) const {
    if (phase() == Phase::Finished) return false;
    const Entity& c = units_[caster];
    if (spellIdx < 0 || spellIdx >= static_cast<int>(c.spells.size())) return false;
    if (!grid_.inBounds(target)) return false;

    const Spell& sp = c.spells[spellIdx];
    if (c.ap < sp.apCost) return false;
    if (spellIdx < static_cast<int>(c.spellCooldowns.size()) && c.spellCooldowns[spellIdx] > 0)
        return false;

    // RangeDebuff (e.g. Blind): shave a percentage off maxRange, floored at
    // minRange — `effMax = maxRange - (maxRange * pct) / 100` in pure integer
    // math (deterministic). Stacked debuffs add up, capped at 100%.
    int rangePct = 0;
    for (const StatusEffect& s : c.statuses)
        if (s.kind == StatusEffect::Kind::RangeDebuff && s.delay <= 0) rangePct += s.magnitude;
    int effMax = sp.maxRange;
    if (rangePct > 0) {
        effMax = sp.maxRange - (sp.maxRange * std::min(rangePct, 100)) / 100;
        if (effMax < sp.minRange) effMax = sp.minRange;
    }

    const int range = manhattan(c.pos, target);
    if (range < sp.minRange || range > effMax) return false;
    if (sp.needsLineOfSight && !clearLineOfSight(c.pos, target)) return false;
    return true;
}

std::vector<Vec2i> Battle::affectedTiles(const Spell& spell, Vec2i casterPos, Vec2i target) const {
    std::vector<Vec2i> tiles;
    auto push = [&](Vec2i p) {
        if (grid_.inBounds(p)) tiles.push_back(p);
    };

    switch (spell.shape) {
        case TargetShape::Single:
            push(target);
            break;
        case TargetShape::Circle:
            for (int dy = -spell.radius; dy <= spell.radius; ++dy)
                for (int dx = -spell.radius; dx <= spell.radius; ++dx)
                    if (std::abs(dx) + std::abs(dy) <= spell.radius)
                        push(Vec2i{target.x + dx, target.y + dy});
            break;
        case TargetShape::Cross:
            push(target);
            for (int r = 1; r <= spell.radius; ++r) {
                push(Vec2i{target.x + r, target.y});
                push(Vec2i{target.x - r, target.y});
                push(Vec2i{target.x, target.y + r});
                push(Vec2i{target.x, target.y - r});
            }
            break;
        case TargetShape::Line: {
            Vec2i dir = cardinalStep(casterPos, target);
            Vec2i p = target;
            for (int r = 0; r <= spell.radius; ++r) {
                push(p);
                p = Vec2i{p.x + dir.x, p.y + dir.y};
            }
            break;
        }
    }
    return tiles;
}

std::vector<EntityId> Battle::unitsAt(const std::vector<Vec2i>& tiles) const {
    std::vector<EntityId> ids;
    for (EntityId i = 0; i < units_.size(); ++i) {
        if (!units_[i].alive()) continue;
        for (Vec2i t : tiles) {
            if (units_[i].pos == t) { ids.push_back(i); break; }
        }
    }
    return ids;
}

void Battle::spawnGround(const GroundSpec& spec, Faction owner, Vec2i casterPos, Vec2i target,
                         const std::vector<Vec2i>& zone) {
    GroundEffect g;
    g.owner = owner;
    g.remainingTurns = spec.duration;
    g.magnitude = spec.magnitude;
    switch (spec.kind) {
        case GroundKind::Wall:
            g.kind = GroundKind::Wall;
            for (Vec2i t : zone)
                if (grid_.isWalkable(t) && !unitAt(t)) g.tiles.push_back(t);
            break;
        case GroundKind::Glyph:
            g.kind = GroundKind::Glyph;
            g.center = target;
            for (Vec2i t : zone)
                if (grid_.isWalkable(t)) g.tiles.push_back(t);
            break;
        case GroundKind::Portal:
            g.kind = GroundKind::Portal;
            g.tiles = {casterPos}; // entry sits on the caster's tile
            g.exit = target;       // exit at the targeted tile
            break;
    }
    if (!g.tiles.empty()) ground_.push_back(std::move(g));
}

bool Battle::cast(EntityId caster, int spellIdx, Vec2i target) {
    if (!canCast(caster, spellIdx, target)) return false;

    // Acting drops the guise: casting from a cloaked pair member declares THAT
    // member the real one (the reveal choice rides in the ordinary intent
    // stream). Deferred damage lands now — if it proves lethal, the reveal
    // consumed the action and the cast fizzles (the intent was legal and DID
    // mutate state, so this still returns true).
    if (auto pi = pairIndexOf(caster)) {
        revealPair(*pi, caster);
        if (!units_[caster].alive()) return true;
    }

    // Copy the spell: applying effects can move/kill units (incl. resizing
    // status vectors), so we must not hold a reference into a unit mid-resolve.
    const Spell sp = units_[caster].spells[spellIdx];
    const Faction casterTeam = units_[caster].team;
    const Vec2i casterPos = units_[caster].pos;

    units_[caster].ap -= sp.apCost;
    if (spellIdx < static_cast<int>(units_[caster].spellCooldowns.size()))
        units_[caster].spellCooldowns[spellIdx] = sp.cooldown;

    emit({EventType::Cast, /*actor=*/caster, /*target=*/0, 0, /*spellSlot=*/spellIdx});
    applySpellEffects(sp, casterTeam, casterPos, target);
    return true;
}

void Battle::applySpellEffects(const Spell& sp, Faction casterTeam, Vec2i casterPos, Vec2i target) {
    const std::vector<Vec2i> zone = affectedTiles(sp, casterPos, target);

    // Tile/ground/spawn effects resolve once for the cast (not per victim).
    for (const Effect& fx : sp.effects) {
        if (fx.type == Effect::Type::Spawn)
            spawnGround(fx.ground, casterTeam, casterPos, target, zone);
        else if (fx.type == Effect::Type::Summon)
            spawnCreature(fx.creature, casterTeam, target);
        else if (fx.type == Effect::Type::Decoy) {
            // The caster is whoever stands on casterPos (captured pre-resolve).
            if (std::optional<EntityId> who = unitAt(casterPos))
                spawnDecoy(*who, target, fx.amount);
        }
    }

    // Unit effects resolve per affected unit (friendly fire included).
    const std::vector<EntityId> hit = unitsAt(zone);
    for (EntityId victim : hit) {
        for (const Effect& fx : sp.effects) {
            switch (fx.type) {
                case Effect::Type::Damage:
                    applyDamage(victim, fx.amount);
                    break;
                case Effect::Type::Heal: {
                    Entity& v = units_[victim];
                    const int before = v.hp;
                    v.hp = std::min(v.maxHp, v.hp + fx.amount);
                    if (v.hp > before)
                        emit({EventType::Heal, victim, victim, v.hp - before});
                    break;
                }
                case Effect::Type::ApplyStatus: {
                    // Polarized: the same spell buffs allies and debuffs foes
                    // (magnitude sign flips against the caster's opponents).
                    Effect fxApplied = fx;
                    if (fx.polarized && units_[victim].team != casterTeam)
                        fxApplied.status.magnitude = -fxApplied.status.magnitude;
                    const Effect& fx = fxApplied; // shadow for the code below
                    if (fx.status.kind == StatusEffect::Kind::Rewind) {
                        Entity& v = units_[victim];
                        // Recast refreshes: drop any prior pending rewind + marker.
                        rewinds_.erase(std::remove_if(rewinds_.begin(), rewinds_.end(),
                                                      [&](const PendingRewind& r) {
                                                          return r.target == victim;
                                                      }),
                                       rewinds_.end());
                        v.statuses.erase(std::remove_if(v.statuses.begin(), v.statuses.end(),
                                                        [](const StatusEffect& s) {
                                                            return s.kind == StatusEffect::Kind::Rewind;
                                                        }),
                                         v.statuses.end());
                        // Snapshot the *current* state (marker excluded), then mark.
                        rewinds_.push_back({victim,
                                            EntitySnapshot{v.pos, v.hp, v.statuses, v.spellCooldowns},
                                            fx.status.remainingTurns});
                        v.statuses.push_back(fx.status);
                    } else {
                        units_[victim].statuses.push_back(fx.status);
                    }
                    emit({EventType::Status, victim, victim, fx.status.magnitude, -1,
                          DamageSource::Spell, fx.status.kind});
                    break;
                }
                case Effect::Type::Push:
                    applyForcedMove(victim, cardinalStep(casterPos, units_[victim].pos), fx.amount);
                    break;
                case Effect::Type::Pull:
                    applyForcedMove(victim, cardinalStep(units_[victim].pos, casterPos), fx.amount);
                    break;
                case Effect::Type::Spawn:
                case Effect::Type::Summon:
                case Effect::Type::Decoy:
                    break; // handled above, once
            }
        }
    }
}

void Battle::spawnCreature(const std::string& key, Faction team, Vec2i at) {
    if (!grid_.isWalkable(at) || unitAt(at).has_value()) return; // need a free tile
    for (const Entity& proto : creatures_) {
        if (proto.name != key) continue;
        // Per-team summon cap (objects like bombs are uncapped).
        if (proto.kind == EntityKind::Summon) {
            int living = 0;
            for (const Entity& u : units_)
                if (u.alive() && u.team == team && u.kind == EntityKind::Summon) ++living;
            if (living >= kSummonCap) return;
        }
        Entity e = proto;
        e.team = team;
        e.pos = at;
        spawnEntity(std::move(e));
        return;
    }
}

// ---------------------------------------------------------------------------
// Cloaked decoy pairs (see CloakPair in Battle.h)
// ---------------------------------------------------------------------------
std::optional<std::size_t> Battle::pairIndexOf(EntityId id) const {
    for (std::size_t i = 0; i < cloaks_.size(); ++i)
        if (cloaks_[i].a == id || cloaks_[i].b == id) return i;
    return std::nullopt;
}

void Battle::spawnDecoy(EntityId caster, Vec2i at, int duration) {
    if (!grid_.isWalkable(at) || unitAt(at).has_value()) return; // need a free tile
    const Entity& src = units_[caster];
    if (!src.alive() || duration <= 0) return;

    // The twin is a FULL copy — name, team, kind, stats, statuses, cooldowns —
    // so the two members are publicly indistinguishable. It joins the initiative
    // like any roster entrant and is driven by the same controller as the caster.
    Entity twin = src;
    twin.pos = at;
    const EntityId b = spawnEntity(std::move(twin));
    cloaks_.push_back({caster, b, duration, 0, 0});
}

void Battle::revealPair(std::size_t pairIdx, EntityId realId) {
    const CloakPair p = cloaks_[pairIdx]; // copy — applyDamage below may recurse
    cloaks_.erase(cloaks_.begin() + static_cast<std::ptrdiff_t>(pairIdx));

    const EntityId decoyId = realId == p.a ? p.b : p.a;
    const int pending = realId == p.a ? p.pendingA : p.pendingB;

    // The decoy vanishes quietly: HP to 0 directly — no onDeath, no death
    // attribution, no victory scan (the real champion is alive, so the team
    // stands either way). It stays on the append-only roster as a corpse.
    units_[decoyId].hp = 0;
    emit({EventType::Death, decoyId, decoyId, 0, -1, DamageSource::Spell});

    // Only the real member's deferred damage lands (now that it's no longer a
    // pair member, applyDamage runs the normal path: shields, death, victory).
    if (pending > 0) applyDamage(realId, pending);
}

void Battle::onEnterTile(EntityId who) {
    Entity& e = units_[who];
    if (!e.alive()) return;
    const Vec2i here = e.pos;

    for (const GroundEffect& g : ground_) {
        bool onIt = false;
        for (Vec2i t : g.tiles)
            if (t == here) { onIt = true; break; }
        if (!onIt) continue;

        if (g.kind == GroundKind::Glyph) {
            Vec2i dir = cardinalStep(g.center, here);
            if (dir.x == 0 && dir.y == 0) dir = Vec2i{1, 0}; // standing on the origin
            applyForcedMove(who, dir, g.magnitude);          // forced move does not re-trigger
            return;
        }
        if (g.kind == GroundKind::Portal) {
            if (grid_.isWalkable(g.exit) && !unitAt(g.exit)) e.pos = g.exit; // no chain
            return;
        }
    }
}

void Battle::rewindTick(EntityId id) {
    auto it = std::find_if(rewinds_.begin(), rewinds_.end(),
                           [&](const PendingRewind& r) { return r.target == id; });
    if (it == rewinds_.end()) return;
    if (--it->turnsLeft > 0) return; // not yet

    Entity& e = units_[id];
    if (e.alive()) {
        // Restore position + HP + statuses + cooldowns. Replacing the status
        // vector also drops the Rewind marker. (Rewind does not revive: a dead
        // unit never reaches its turn, so this branch only runs while alive.)
        e.pos = it->snap.pos;
        e.hp = it->snap.hp;
        e.statuses = it->snap.statuses;
        e.spellCooldowns = it->snap.spellCooldowns;
    } else {
        e.statuses.erase(std::remove_if(e.statuses.begin(), e.statuses.end(),
                                        [](const StatusEffect& s) {
                                            return s.kind == StatusEffect::Kind::Rewind;
                                        }),
                         e.statuses.end());
    }
    rewinds_.erase(it);
}

// ---------------------------------------------------------------------------
// Forced movement (out-of-turn)
// ---------------------------------------------------------------------------
void Battle::applyForcedMove(EntityId who, Vec2i dir, int distance) {
    if (dir.x == 0 && dir.y == 0) return;
    Entity& e = units_[who];
    const std::vector<Vec2i> walls = wallTiles();
    auto isWall = [&](Vec2i p) {
        for (Vec2i w : walls)
            if (w == p) return true;
        return false;
    };

    for (int step = 0; step < distance; ++step) {
        Vec2i next{e.pos.x + dir.x, e.pos.y + dir.y};
        const bool blocked = !grid_.isWalkable(next) || unitAt(next).has_value() || isWall(next);
        if (blocked) {
            const int remaining = distance - step;
            applyDamage(who, remaining * kCollisionDamagePerCell, DamageSource::Collision);
            return;
        }
        e.pos = next;
    }
}

} // namespace tb
