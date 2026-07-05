#include "SpellJson.h"

#include "JsonRead.h"
#include "SpellEnums.h"

#include <optional>
#include <string>

namespace tb::spelljson {

using jsonread::checkAllowed;
using jsonread::forbid;
using jsonread::optInt;
using jsonread::wantInt;

namespace {

template <class E, std::size_t N>
void enumField(const json::Value& obj, const char* key, const enums::Row<E> (&table)[N],
               const std::string& ctx, bool required, E& out, Errors& e) {
    const json::Value* v = obj.find(key);
    if (!v) {
        if (required) e.push_back(ctx + ": missing required field \"" + key + "\"");
        return; // optional & absent: leave `out` at its default
    }
    if (!v->isString()) {
        e.push_back(ctx + ": \"" + key + "\" must be a string");
        return;
    }
    const std::optional<E> r = enums::fromString(table, v->asString());
    if (!r) {
        e.push_back(ctx + ": unknown " + key + " \"" + v->asString() + "\"");
        return;
    }
    out = *r;
}

void parseGround(const json::Value& gv, const std::string& ctx, GroundSpec& out, Errors& e) {
    checkAllowed(gv, {"kind", "duration", "magnitude"}, ctx, e);
    enumField(gv, "kind", enums::kGroundKinds, ctx, true, out.kind, e);
    out.duration = optInt(gv, "duration", 2, ctx, e); // matches GroundSpec default
    out.magnitude = optInt(gv, "magnitude", 0, ctx, e);
}

} // namespace

void parseStatus(const json::Value& sv, const std::string& ctx, StatusEffect& out, Errors& e) {
    checkAllowed(sv, {"kind", "magnitude", "turns", "delay"}, ctx, e);
    enumField(sv, "kind", enums::kStatusKinds, ctx, true, out.kind, e);
    out.magnitude = optInt(sv, "magnitude", 0, ctx, e);
    out.remainingTurns = optInt(sv, "turns", 0, ctx, e);
    out.delay = optInt(sv, "delay", 0, ctx, e); // inert for N owner turns, then active
    if (out.delay < 0) e.push_back(ctx + ": \"delay\" must be >= 0");
    if (out.kind == StatusEffect::Kind::RangeDebuff &&
        (out.magnitude < 1 || out.magnitude > 100))
        e.push_back(ctx + ": rangeDebuff magnitude is a percent (1..100)");
}

json::Value statusToJson(const StatusEffect& s) {
    json::Value v = json::Value::makeObject();
    v.set("kind", json::Value(std::string(enums::toString(enums::kStatusKinds, s.kind))));
    v.set("magnitude", json::Value(s.magnitude));
    v.set("turns", json::Value(s.remainingTurns));
    if (s.delay != 0) v.set("delay", json::Value(s.delay)); // omitted when default
    return v;
}

Effect parseEffect(const json::Value& ev, const std::string& ctx, Errors& e) {
    Effect out{};
    if (!ev.isObject()) {
        e.push_back(ctx + ": expected an object");
        return out;
    }
    checkAllowed(ev, {"type", "amount", "status", "ground", "creature", "polarized"}, ctx, e);

    const std::size_t before = e.size();
    enumField(ev, "type", enums::kEffectTypes, ctx, true, out.type, e);
    if (e.size() != before) return out; // bad/missing type — can't validate payload

    const std::string typeName(enums::toString(enums::kEffectTypes, out.type));
    if (out.type != Effect::Type::ApplyStatus) forbid(ev, "polarized", typeName, ctx, e);
    switch (out.type) {
        case Effect::Type::Damage:
        case Effect::Type::Heal:
        case Effect::Type::Push:
        case Effect::Type::Pull: {
            forbid(ev, "status", typeName, ctx, e);
            forbid(ev, "ground", typeName, ctx, e);
            forbid(ev, "creature", typeName, ctx, e);
            int amount = 0;
            if (wantInt(ev, "amount", ctx, amount, e)) out.amount = amount;
            break;
        }
        case Effect::Type::ApplyStatus: {
            forbid(ev, "amount", typeName, ctx, e);
            forbid(ev, "ground", typeName, ctx, e);
            forbid(ev, "creature", typeName, ctx, e);
            out.polarized = jsonread::optBool(ev, "polarized", false, ctx, e);
            const json::Value* sv = ev.find("status");
            if (!sv || !sv->isObject())
                e.push_back(ctx + ": \"applyStatus\" requires a \"status\" object");
            else
                parseStatus(*sv, ctx + ".status", out.status, e);
            break;
        }
        case Effect::Type::Spawn: {
            forbid(ev, "amount", typeName, ctx, e);
            forbid(ev, "status", typeName, ctx, e);
            forbid(ev, "creature", typeName, ctx, e);
            const json::Value* gv = ev.find("ground");
            if (!gv || !gv->isObject())
                e.push_back(ctx + ": \"spawn\" requires a \"ground\" object");
            else
                parseGround(*gv, ctx + ".ground", out.ground, e);
            break;
        }
        case Effect::Type::Summon: {
            forbid(ev, "amount", typeName, ctx, e);
            forbid(ev, "status", typeName, ctx, e);
            forbid(ev, "ground", typeName, ctx, e);
            const json::Value* cv = ev.find("creature");
            if (!cv || !cv->isString() || cv->asString().empty())
                e.push_back(ctx + ": \"summon\" requires a non-empty \"creature\" key");
            else
                out.creature = cv->asString();
            break;
        }
        case Effect::Type::Decoy: {
            // amount = how many of the caster's turns the pair stays cloaked.
            forbid(ev, "status", typeName, ctx, e);
            forbid(ev, "ground", typeName, ctx, e);
            forbid(ev, "creature", typeName, ctx, e);
            int amount = 0;
            if (wantInt(ev, "amount", ctx, amount, e)) {
                if (amount < 1) e.push_back(ctx + ": \"decoy\" duration must be >= 1");
                out.amount = amount;
            }
            break;
        }
    }
    return out;
}

json::Value effectToJson(const Effect& fx) {
    using json::Value;
    Value e = Value::makeObject();
    e.set("type", Value(std::string(enums::toString(enums::kEffectTypes, fx.type))));
    switch (fx.type) {
        case Effect::Type::Damage:
        case Effect::Type::Heal:
        case Effect::Type::Push:
        case Effect::Type::Pull:
        case Effect::Type::Decoy:
            e.set("amount", Value(fx.amount));
            break;
        case Effect::Type::ApplyStatus: {
            e.set("status", statusToJson(fx.status));
            if (fx.polarized) e.set("polarized", Value(true)); // omitted when default
            break;
        }
        case Effect::Type::Spawn: {
            Value g = Value::makeObject();
            g.set("kind", Value(std::string(enums::toString(enums::kGroundKinds, fx.ground.kind))));
            g.set("duration", Value(fx.ground.duration));
            g.set("magnitude", Value(fx.ground.magnitude));
            e.set("ground", std::move(g));
            break;
        }
        case Effect::Type::Summon:
            e.set("creature", Value(fx.creature));
            break;
    }
    return e;
}

void readSpellFields(const json::Value& obj, const std::string& ctx, Spell& out, Errors& e) {
    if (const json::Value* nv = obj.find("name")) {
        if (!nv->isString()) e.push_back(ctx + ": \"name\" must be a string");
        else out.name = nv->asString();
    }
    int apCost = 0;
    if (wantInt(obj, "apCost", ctx, apCost, e)) {
        if (apCost < 0) e.push_back(ctx + ": \"apCost\" must be >= 0");
        out.apCost = apCost;
    }
    out.minRange = optInt(obj, "minRange", out.minRange, ctx, e);
    out.maxRange = optInt(obj, "maxRange", out.maxRange, ctx, e);
    out.needsLineOfSight = jsonread::optBool(obj, "needsLineOfSight", out.needsLineOfSight, ctx, e);
    enumField(obj, "shape", enums::kTargetShapes, ctx, false, out.shape, e);
    out.radius = optInt(obj, "radius", out.radius, ctx, e);
    out.cooldown = optInt(obj, "cooldown", out.cooldown, ctx, e);

    if (out.minRange < 0 || out.maxRange < 0)
        e.push_back(ctx + ": ranges must be >= 0");
    else if (out.minRange > out.maxRange)
        e.push_back(ctx + ": minRange (" + std::to_string(out.minRange) + ") > maxRange (" +
                    std::to_string(out.maxRange) + ")");
    if (out.radius < 0) e.push_back(ctx + ": \"radius\" must be >= 0");
    if (out.cooldown < 0) e.push_back(ctx + ": \"cooldown\" must be >= 0");

    const json::Value* effs = obj.find("effects");
    if (!effs || !effs->isArray() || effs->asArray().empty()) {
        e.push_back(ctx + ": \"effects\" must be a non-empty array");
    } else {
        const json::Value::Array& arr = effs->asArray();
        for (std::size_t i = 0; i < arr.size(); ++i)
            out.effects.push_back(parseEffect(arr[i], ctx + ": effect[" + std::to_string(i) + "]", e));
    }
}

void writeSpellFields(json::Value& obj, const Spell& sp) {
    using json::Value;
    obj.set("name", Value(sp.name));
    obj.set("apCost", Value(sp.apCost));
    obj.set("minRange", Value(sp.minRange));
    obj.set("maxRange", Value(sp.maxRange));
    obj.set("needsLineOfSight", Value(sp.needsLineOfSight));
    obj.set("shape", Value(std::string(enums::toString(enums::kTargetShapes, sp.shape))));
    obj.set("radius", Value(sp.radius));
    obj.set("cooldown", Value(sp.cooldown));
    Value effects = Value::makeArray();
    for (const Effect& fx : sp.effects) effects.push_back(effectToJson(fx));
    obj.set("effects", std::move(effects));
}

} // namespace tb::spelljson
