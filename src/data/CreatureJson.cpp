#include "CreatureJson.h"

#include "JsonRead.h"
#include "Sha256.h"
#include "SpellEnums.h"
#include "SpellJson.h"

#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace tb {

namespace {

using Errors = std::vector<std::string>;

// A creature's innate spell and its onDeath share the bare-Spell layout.
void readSpellObject(const json::Value& obj, const std::string& ctx, Spell& out, Errors& e) {
    if (!obj.isObject()) {
        e.push_back(ctx + ": expected an object");
        return;
    }
    jsonread::checkAllowed(obj,
                           {"name", "apCost", "minRange", "maxRange", "needsLineOfSight", "shape",
                            "radius", "cooldown", "effects"},
                           ctx, e);
    spelljson::readSpellFields(obj, ctx, out, e);
}

void parseCreature(const json::Value& cv, std::size_t idx, Entity& out,
                   std::set<std::string>& seenKeys, Errors& e) {
    std::string ctx = "creatures[" + std::to_string(idx) + "]";
    if (!cv.isObject()) {
        e.push_back(ctx + ": expected an object");
        return;
    }
    jsonread::checkAllowed(cv,
                           {"key", "kind", "hp", "ap", "mp", "initiative", "fuse", "statuses",
                            "spells", "onDeath"},
                           ctx, e);

    const json::Value* kv = cv.find("key");
    if (!kv || !kv->isString() || kv->asString().empty()) {
        e.push_back(ctx + ": \"key\" must be a non-empty string");
    } else {
        out.name = kv->asString();
        if (!seenKeys.insert(out.name).second)
            e.push_back(ctx + ": duplicate key \"" + out.name + "\"");
        ctx += " \"" + out.name + "\"";
    }

    if (const json::Value* kindv = cv.find("kind")) {
        if (!kindv->isString()) {
            e.push_back(ctx + ": \"kind\" must be a string");
        } else if (auto k = enums::fromString(enums::kEntityKinds, kindv->asString())) {
            out.kind = *k;
        } else {
            e.push_back(ctx + ": unknown kind \"" + kindv->asString() + "\"");
        }
    } else {
        e.push_back(ctx + ": missing required field \"kind\"");
    }

    int hp = 0;
    if (jsonread::wantInt(cv, "hp", ctx, hp, e)) {
        if (hp <= 0) e.push_back(ctx + ": \"hp\" must be positive");
        out.maxHp = out.hp = hp;
    }
    out.maxAp = out.ap = jsonread::optInt(cv, "ap", 0, ctx, e);
    out.maxMp = out.mp = jsonread::optInt(cv, "mp", 0, ctx, e);
    out.initiative = jsonread::optInt(cv, "initiative", 0, ctx, e);
    out.fuse = jsonread::optInt(cv, "fuse", 0, ctx, e);
    if (out.fuse < 0) e.push_back(ctx + ": \"fuse\" must be >= 0");

    if (const json::Value* sv = cv.find("statuses")) {
        if (!sv->isArray()) {
            e.push_back(ctx + ": \"statuses\" must be an array");
        } else {
            const json::Value::Array& arr = sv->asArray();
            for (std::size_t i = 0; i < arr.size(); ++i) {
                StatusEffect st;
                spelljson::parseStatus(arr[i], ctx + ".statuses[" + std::to_string(i) + "]", st, e);
                out.statuses.push_back(st);
            }
        }
    }

    if (const json::Value* sp = cv.find("spells")) {
        if (!sp->isArray()) {
            e.push_back(ctx + ": \"spells\" must be an array");
        } else {
            const json::Value::Array& arr = sp->asArray();
            for (std::size_t i = 0; i < arr.size(); ++i) {
                Spell s;
                readSpellObject(arr[i], ctx + ".spells[" + std::to_string(i) + "]", s, e);
                out.spells.push_back(std::move(s));
            }
        }
    }

    if (const json::Value* od = cv.find("onDeath"))
        readSpellObject(*od, ctx + ".onDeath", out.onDeath, e);
}

json::Value creatureToJson(const Entity& c) {
    using json::Value;
    Value o = Value::makeObject();
    o.set("key", Value(c.name));
    o.set("kind", Value(std::string(enums::toString(enums::kEntityKinds, c.kind))));
    o.set("hp", Value(c.maxHp));
    o.set("ap", Value(c.maxAp));
    o.set("mp", Value(c.maxMp));
    o.set("initiative", Value(c.initiative));
    o.set("fuse", Value(c.fuse));

    Value statuses = Value::makeArray();
    for (const StatusEffect& s : c.statuses) statuses.push_back(spelljson::statusToJson(s));
    o.set("statuses", std::move(statuses));

    Value spells = Value::makeArray();
    for (const Spell& sp : c.spells) {
        Value so = Value::makeObject();
        spelljson::writeSpellFields(so, sp);
        spells.push_back(std::move(so));
    }
    o.set("spells", std::move(spells));

    if (!c.onDeath.effects.empty()) {
        Value od = Value::makeObject();
        spelljson::writeSpellFields(od, c.onDeath);
        o.set("onDeath", std::move(od));
    }
    return o;
}

} // namespace

CreatureLoad loadCreaturesFromString(const std::string& text) {
    CreatureLoad result;
    result.sha256 = sha256Hex(text);

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
    jsonread::checkAllowed(root, {"schema", "version", "creatures"}, "root", result.errors);

    int schema = 0;
    if (jsonread::wantInt(root, "schema", "root", schema, result.errors) &&
        schema != kCreatureSchemaVersion)
        result.errors.push_back("root: unsupported schema " + std::to_string(schema) +
                                " (this build supports " + std::to_string(kCreatureSchemaVersion) +
                                ")");

    if (const json::Value* v = root.find("version"); v && v->isString())
        result.version = v->asString();
    else
        result.errors.push_back("root: \"version\" must be a string");

    const json::Value* creatures = root.find("creatures");
    if (!creatures || !creatures->isArray()) {
        result.errors.push_back("root: \"creatures\" must be an array");
    } else if (creatures->asArray().empty()) {
        result.errors.push_back("root: \"creatures\" is empty");
    } else {
        std::set<std::string> seenKeys;
        const json::Value::Array& arr = creatures->asArray();
        for (std::size_t i = 0; i < arr.size(); ++i) {
            const std::size_t before = result.errors.size();
            Entity e;
            parseCreature(arr[i], i, e, seenKeys, result.errors);
            if (result.errors.size() == before) result.creatures.push_back(std::move(e));
        }
    }

    result.ok = result.errors.empty();
    return result;
}

std::string serializeCreatures(const std::vector<Entity>& creatures, const std::string& version) {
    using json::Value;
    Value root = Value::makeObject();
    root.set("schema", Value(kCreatureSchemaVersion));
    root.set("version", Value(version));
    Value arr = Value::makeArray();
    for (const Entity& c : creatures) arr.push_back(creatureToJson(c));
    root.set("creatures", std::move(arr));
    return json::dump(root, /*pretty=*/true);
}

CreatureLoad loadCreaturesFromFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        CreatureLoad result;
        result.errors.push_back("could not open creatures file: " + path);
        return result;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return loadCreaturesFromString(ss.str());
}

} // namespace tb
