#include "CatalogJson.h"

#include "Json.h"
#include "SpellEnums.h"

#include <cctype>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <optional>
#include <set>
#include <string>

namespace tb {

namespace {

using Errors = std::vector<std::string>;

// --- Small typed field readers (append a contextual error on mismatch) ------

bool toInt(const json::Value& v, int& out) {
    if (!v.isNumber()) return false;
    const double d = v.asNumber();
    if (d != std::floor(d)) return false; // reject 15.5
    if (d < static_cast<double>(std::numeric_limits<int>::min()) ||
        d > static_cast<double>(std::numeric_limits<int>::max()))
        return false;
    out = static_cast<int>(d);
    return true;
}

// Required integer. Returns false (and records an error) if missing/non-integer.
bool wantInt(const json::Value& obj, const char* key, const std::string& ctx, int& out, Errors& e) {
    const json::Value* v = obj.find(key);
    if (!v) {
        e.push_back(ctx + ": missing required field \"" + key + "\"");
        return false;
    }
    if (!toInt(*v, out)) {
        e.push_back(ctx + ": \"" + key + "\" must be an integer");
        return false;
    }
    return true;
}

// Optional integer with default; records an error only if present-but-wrong.
int optInt(const json::Value& obj, const char* key, int def, const std::string& ctx, Errors& e) {
    const json::Value* v = obj.find(key);
    if (!v) return def;
    int out = def;
    if (!toInt(*v, out)) {
        e.push_back(ctx + ": \"" + key + "\" must be an integer");
        return def;
    }
    return out;
}

bool optBool(const json::Value& obj, const char* key, bool def, const std::string& ctx, Errors& e) {
    const json::Value* v = obj.find(key);
    if (!v) return def;
    if (!v->isBool()) {
        e.push_back(ctx + ": \"" + key + "\" must be a boolean");
        return def;
    }
    return v->asBool();
}

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

void checkAllowed(const json::Value& obj, std::initializer_list<const char*> allowed,
                  const std::string& ctx, Errors& e) {
    for (const json::Value::Member& m : obj.asObject()) {
        bool ok = false;
        for (const char* a : allowed)
            if (m.first == a) {
                ok = true;
                break;
            }
        if (!ok) e.push_back(ctx + ": unknown field \"" + m.first + "\"");
    }
}

void forbid(const json::Value& obj, const char* key, const std::string& typeName,
            const std::string& ctx, Errors& e) {
    if (obj.find(key))
        e.push_back(ctx + ": \"" + key + "\" is not valid for effect type \"" + typeName + "\"");
}

std::string capitalize(std::string s) {
    if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

// --- Effect / status / ground -----------------------------------------------

void parseStatus(const json::Value& sv, const std::string& ctx, StatusEffect& out, Errors& e) {
    checkAllowed(sv, {"kind", "magnitude", "turns"}, ctx, e);
    enumField(sv, "kind", enums::kStatusKinds, ctx, true, out.kind, e);
    out.magnitude = optInt(sv, "magnitude", 0, ctx, e);
    out.remainingTurns = optInt(sv, "turns", 0, ctx, e);
}

void parseGround(const json::Value& gv, const std::string& ctx, GroundSpec& out, Errors& e) {
    checkAllowed(gv, {"kind", "duration", "magnitude"}, ctx, e);
    enumField(gv, "kind", enums::kGroundKinds, ctx, true, out.kind, e);
    out.duration = optInt(gv, "duration", 2, ctx, e); // matches GroundSpec default
    out.magnitude = optInt(gv, "magnitude", 0, ctx, e);
}

// Returns the parsed effect; validity is signalled by whether `e` grew.
Effect parseEffect(const json::Value& ev, const std::string& ctx, Errors& e) {
    Effect out{};
    if (!ev.isObject()) {
        e.push_back(ctx + ": expected an object");
        return out;
    }
    checkAllowed(ev, {"type", "amount", "status", "ground"}, ctx, e);

    const std::size_t before = e.size();
    enumField(ev, "type", enums::kEffectTypes, ctx, true, out.type, e);
    if (e.size() != before) return out; // bad/missing type — can't validate payload

    const std::string typeName(enums::toString(enums::kEffectTypes, out.type));
    switch (out.type) {
        case Effect::Type::Damage:
        case Effect::Type::Heal:
        case Effect::Type::Push:
        case Effect::Type::Pull: {
            forbid(ev, "status", typeName, ctx, e);
            forbid(ev, "ground", typeName, ctx, e);
            int amount = 0;
            if (wantInt(ev, "amount", ctx, amount, e)) out.amount = amount;
            break;
        }
        case Effect::Type::ApplyStatus: {
            forbid(ev, "amount", typeName, ctx, e);
            forbid(ev, "ground", typeName, ctx, e);
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
            const json::Value* gv = ev.find("ground");
            if (!gv || !gv->isObject())
                e.push_back(ctx + ": \"spawn\" requires a \"ground\" object");
            else
                parseGround(*gv, ctx + ".ground", out.ground, e);
            break;
        }
    }
    return out;
}

// --- Spell -------------------------------------------------------------------

void parseSpell(const json::Value& sp, std::size_t idx, SpellDef& out, std::set<int>& seenIds,
                std::set<std::string>& seenKeys, Errors& e) {
    std::string ctx = "spells[" + std::to_string(idx) + "]";
    if (!sp.isObject()) {
        e.push_back(ctx + ": expected an object");
        return;
    }
    checkAllowed(sp,
                 {"id", "key", "name", "buildCost", "apCost", "minRange", "maxRange",
                  "needsLineOfSight", "shape", "radius", "cooldown", "effects"},
                 ctx, e);

    int id = 0;
    if (wantInt(sp, "id", ctx, id, e)) {
        if (id <= 0) e.push_back(ctx + ": \"id\" must be positive");
        else if (!seenIds.insert(id).second) e.push_back(ctx + ": duplicate id " + std::to_string(id));
        out.id = id;
    }

    const json::Value* kv = sp.find("key");
    if (!kv || !kv->isString() || kv->asString().empty()) {
        e.push_back(ctx + ": \"key\" must be a non-empty string");
    } else {
        out.key = kv->asString();
        if (!seenKeys.insert(out.key).second)
            e.push_back(ctx + ": duplicate key \"" + out.key + "\"");
        ctx += " \"" + out.key + "\""; // richer context for the remaining fields
    }

    int buildCost = 0;
    if (wantInt(sp, "buildCost", ctx, buildCost, e)) {
        if (buildCost < 0) e.push_back(ctx + ": \"buildCost\" must be >= 0");
        out.buildCost = buildCost;
    }

    Spell spell; // struct defaults cover any optional field that errors out
    if (const json::Value* nv = sp.find("name")) {
        if (!nv->isString()) e.push_back(ctx + ": \"name\" must be a string");
        else spell.name = nv->asString();
    } else {
        spell.name = capitalize(out.key);
    }

    int apCost = 0;
    if (wantInt(sp, "apCost", ctx, apCost, e)) {
        if (apCost < 0) e.push_back(ctx + ": \"apCost\" must be >= 0");
        spell.apCost = apCost;
    }

    spell.minRange = optInt(sp, "minRange", 1, ctx, e);
    spell.maxRange = optInt(sp, "maxRange", 1, ctx, e);
    spell.needsLineOfSight = optBool(sp, "needsLineOfSight", true, ctx, e);
    enumField(sp, "shape", enums::kTargetShapes, ctx, false, spell.shape, e);
    spell.radius = optInt(sp, "radius", 0, ctx, e);
    spell.cooldown = optInt(sp, "cooldown", 0, ctx, e);

    if (spell.minRange < 0 || spell.maxRange < 0)
        e.push_back(ctx + ": ranges must be >= 0");
    else if (spell.minRange > spell.maxRange)
        e.push_back(ctx + ": minRange (" + std::to_string(spell.minRange) + ") > maxRange (" +
                    std::to_string(spell.maxRange) + ")");
    if (spell.radius < 0) e.push_back(ctx + ": \"radius\" must be >= 0");
    if (spell.cooldown < 0) e.push_back(ctx + ": \"cooldown\" must be >= 0");

    const json::Value* effs = sp.find("effects");
    if (!effs || !effs->isArray() || effs->asArray().empty()) {
        e.push_back(ctx + ": \"effects\" must be a non-empty array");
    } else {
        const json::Value::Array& arr = effs->asArray();
        for (std::size_t i = 0; i < arr.size(); ++i)
            spell.effects.push_back(parseEffect(arr[i], ctx + ": effect[" + std::to_string(i) + "]", e));
    }

    out.spell = std::move(spell);
}

} // namespace

CatalogLoad loadCatalogFromString(const std::string& text) {
    CatalogLoad result;

    json::ParseResult pr = json::parse(text);
    if (!pr.ok) {
        result.errors.push_back("JSON parse error: " + pr.error);
        return result;
    }
    const json::Value& root = pr.value;
    if (!root.isObject()) {
        result.errors.push_back("root: expected an object");
        return result;
    }
    checkAllowed(root, {"schema", "version", "spells"}, "root", result.errors);

    int schema = 0;
    if (wantInt(root, "schema", "root", schema, result.errors) && schema != kCatalogSchemaVersion)
        result.errors.push_back("root: unsupported schema " + std::to_string(schema) +
                                " (this build supports " + std::to_string(kCatalogSchemaVersion) + ")");

    if (const json::Value* v = root.find("version"); v && v->isString())
        result.version = v->asString();
    else
        result.errors.push_back("root: \"version\" must be a string");

    const json::Value* spells = root.find("spells");
    if (!spells || !spells->isArray()) {
        result.errors.push_back("root: \"spells\" must be an array");
    } else if (spells->asArray().empty()) {
        result.errors.push_back("root: \"spells\" is empty");
    } else {
        std::set<int> seenIds;
        std::set<std::string> seenKeys;
        const json::Value::Array& arr = spells->asArray();
        for (std::size_t i = 0; i < arr.size(); ++i) {
            const std::size_t before = result.errors.size();
            SpellDef def;
            parseSpell(arr[i], i, def, seenIds, seenKeys, result.errors);
            if (result.errors.size() == before) result.catalog.add(std::move(def));
        }
    }

    result.ok = result.errors.empty();
    return result;
}

std::string serializeCatalog(const SpellCatalog& catalog, const std::string& version) {
    using json::Value;
    Value root = Value::makeObject();
    root.set("schema", Value(kCatalogSchemaVersion));
    root.set("version", Value(version));

    Value spells = Value::makeArray();
    for (const SpellDef& d : catalog.all()) {
        Value s = Value::makeObject();
        s.set("id", Value(d.id));
        s.set("key", Value(d.key));
        s.set("name", Value(d.spell.name));
        s.set("buildCost", Value(d.buildCost));
        s.set("apCost", Value(d.spell.apCost));
        s.set("minRange", Value(d.spell.minRange));
        s.set("maxRange", Value(d.spell.maxRange));
        s.set("needsLineOfSight", Value(d.spell.needsLineOfSight));
        s.set("shape", Value(std::string(enums::toString(enums::kTargetShapes, d.spell.shape))));
        s.set("radius", Value(d.spell.radius));
        s.set("cooldown", Value(d.spell.cooldown));

        Value effects = Value::makeArray();
        for (const Effect& fx : d.spell.effects) {
            Value e = Value::makeObject();
            e.set("type", Value(std::string(enums::toString(enums::kEffectTypes, fx.type))));
            switch (fx.type) {
                case Effect::Type::Damage:
                case Effect::Type::Heal:
                case Effect::Type::Push:
                case Effect::Type::Pull:
                    e.set("amount", Value(fx.amount));
                    break;
                case Effect::Type::ApplyStatus: {
                    Value st = Value::makeObject();
                    st.set("kind", Value(std::string(enums::toString(enums::kStatusKinds, fx.status.kind))));
                    st.set("magnitude", Value(fx.status.magnitude));
                    st.set("turns", Value(fx.status.remainingTurns));
                    e.set("status", std::move(st));
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
            }
            effects.push_back(std::move(e));
        }
        s.set("effects", std::move(effects));
        spells.push_back(std::move(s));
    }
    root.set("spells", std::move(spells));
    return json::dump(root, /*pretty=*/true);
}

} // namespace tb
