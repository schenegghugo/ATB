#include "RulesetJson.h"

#include "JsonRead.h"
#include "Sha256.h"

#include <fstream>
#include <set>
#include <sstream>
#include <string>

namespace tb {

namespace {

using Errors = std::vector<std::string>;
using jsonread::checkAllowed;
using jsonread::optBool;
using jsonread::optDouble;
using jsonread::optInt;

// Parse an optional nested object: returns it (or nullptr) and errors if the
// field exists but isn't an object.
const json::Value* optObject(const json::Value& obj, const char* key, const std::string& ctx,
                             Errors& e) {
    const json::Value* v = obj.find(key);
    if (!v) return nullptr;
    if (!v->isObject()) {
        e.push_back(ctx + ": \"" + key + "\" must be an object");
        return nullptr;
    }
    return v;
}

void parseEconomy(const json::Value& ev, BuildRules& r, Errors& e) {
    const std::string ctx = "economy";
    checkAllowed(ev,
                 {"pointBudget", "baseHp", "hpStep", "hpCost", "baseAp", "apCost", "baseMp",
                  "mpCost", "baseInitiative", "initCost"},
                 ctx, e);
    r.pointBudget = optInt(ev, "pointBudget", r.pointBudget, ctx, e);
    r.baseHp = optInt(ev, "baseHp", r.baseHp, ctx, e);
    r.hpStep = optInt(ev, "hpStep", r.hpStep, ctx, e);
    r.hpCost = optInt(ev, "hpCost", r.hpCost, ctx, e);
    r.baseAp = optInt(ev, "baseAp", r.baseAp, ctx, e);
    r.apCost = optInt(ev, "apCost", r.apCost, ctx, e);
    r.baseMp = optInt(ev, "baseMp", r.baseMp, ctx, e);
    r.mpCost = optInt(ev, "mpCost", r.mpCost, ctx, e);
    r.baseInitiative = optInt(ev, "baseInitiative", r.baseInitiative, ctx, e);
    r.initCost = optInt(ev, "initCost", r.initCost, ctx, e);

    if (r.pointBudget < 0) e.push_back(ctx + ": pointBudget must be >= 0");
    if (r.baseHp < 1) e.push_back(ctx + ": baseHp must be >= 1");
    if (r.baseAp < 0 || r.baseMp < 0 || r.baseInitiative < 0)
        e.push_back(ctx + ": base AP/MP/initiative must be >= 0");
    if (r.hpStep < 0) e.push_back(ctx + ": hpStep must be >= 0");
    if (r.hpCost < 1 || r.apCost < 1 || r.mpCost < 1 || r.initCost < 1)
        e.push_back(ctx + ": stat-point costs must be >= 1");
}

} // namespace

RulesetLoad loadRulesetFromString(const std::string& text) {
    RulesetLoad result;
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
    Errors& e = result.errors;
    checkAllowed(root,
                 {"schema", "version", "format", "economy", "bannedSpells", "closingRing", "arena"},
                 "root", e);

    int schema = 0;
    if (jsonread::wantInt(root, "schema", "root", schema, e) && schema != kRulesetSchemaVersion)
        e.push_back("root: unsupported schema " + std::to_string(schema) + " (this build supports " +
                    std::to_string(kRulesetSchemaVersion) + ")");

    if (const json::Value* v = root.find("version"); v && v->isString())
        result.version = v->asString();
    else
        e.push_back("root: \"version\" must be a string");

    Ruleset rs; // defaults; overridden by present fields

    if (const json::Value* fmt = optObject(root, "format", "root", e)) {
        checkAllowed(*fmt, {"teamSize"}, "format", e);
        rs.teamSize = optInt(*fmt, "teamSize", rs.teamSize, "format", e);
        if (rs.teamSize < 1 || rs.teamSize > 8)
            e.push_back("format: teamSize must be 1-8");
    }

    if (const json::Value* econ = optObject(root, "economy", "root", e))
        parseEconomy(*econ, rs.economy, e);

    if (const json::Value* bans = root.find("bannedSpells")) {
        if (!bans->isArray()) {
            e.push_back("bannedSpells: must be an array");
        } else {
            std::set<std::string> seen;
            for (const json::Value& b : bans->asArray()) {
                if (!b.isString() || b.asString().empty())
                    e.push_back("bannedSpells: each entry must be a non-empty string");
                else if (!seen.insert(b.asString()).second)
                    e.push_back("bannedSpells: duplicate \"" + b.asString() + "\"");
                else
                    rs.bannedSpells.push_back(b.asString());
            }
        }
    }

    if (const json::Value* ring = optObject(root, "closingRing", "root", e)) {
        checkAllowed(*ring, {"enabled", "startRound", "damage"}, "closingRing", e);
        rs.closingRing.enabled = optBool(*ring, "enabled", rs.closingRing.enabled, "closingRing", e);
        rs.closingRing.startRound = optInt(*ring, "startRound", rs.closingRing.startRound, "closingRing", e);
        rs.closingRing.damage = optInt(*ring, "damage", rs.closingRing.damage, "closingRing", e);
        if (rs.closingRing.startRound < 1) e.push_back("closingRing: startRound must be >= 1");
        if (rs.closingRing.damage < 0) e.push_back("closingRing: damage must be >= 0");
    }

    if (const json::Value* arena = optObject(root, "arena", "root", e)) {
        checkAllowed(*arena, {"width", "height", "coverage", "map"}, "arena", e);
        rs.arena.width = optInt(*arena, "width", rs.arena.width, "arena", e);
        rs.arena.height = optInt(*arena, "height", rs.arena.height, "arena", e);
        rs.arena.coverage = optDouble(*arena, "coverage", rs.arena.coverage, "arena", e);
        if (const json::Value* mv = arena->find("map")) {
            if (mv->isString()) rs.arena.map = mv->asString();
            else e.push_back("arena: \"map\" must be a string");
        }
        if (rs.arena.width < 1 || rs.arena.height < 1) e.push_back("arena: width/height must be >= 1");
        if (rs.arena.coverage < 0.0 || rs.arena.coverage > 1.0)
            e.push_back("arena: coverage must be between 0 and 1");
    }

    result.ruleset = std::move(rs);
    result.ok = result.errors.empty();
    return result;
}

std::string serializeRuleset(const Ruleset& r, const std::string& version) {
    using json::Value;
    Value root = Value::makeObject();
    root.set("schema", Value(kRulesetSchemaVersion));
    root.set("version", Value(version));

    Value format = Value::makeObject();
    format.set("teamSize", Value(r.teamSize));
    root.set("format", std::move(format));

    Value econ = Value::makeObject();
    econ.set("pointBudget", Value(r.economy.pointBudget));
    econ.set("baseHp", Value(r.economy.baseHp));
    econ.set("hpStep", Value(r.economy.hpStep));
    econ.set("hpCost", Value(r.economy.hpCost));
    econ.set("baseAp", Value(r.economy.baseAp));
    econ.set("apCost", Value(r.economy.apCost));
    econ.set("baseMp", Value(r.economy.baseMp));
    econ.set("mpCost", Value(r.economy.mpCost));
    econ.set("baseInitiative", Value(r.economy.baseInitiative));
    econ.set("initCost", Value(r.economy.initCost));
    root.set("economy", std::move(econ));

    Value bans = Value::makeArray();
    for (const std::string& b : r.bannedSpells) bans.push_back(Value(b));
    root.set("bannedSpells", std::move(bans));

    Value ring = Value::makeObject();
    ring.set("enabled", Value(r.closingRing.enabled));
    ring.set("startRound", Value(r.closingRing.startRound));
    ring.set("damage", Value(r.closingRing.damage));
    root.set("closingRing", std::move(ring));

    Value arena = Value::makeObject();
    arena.set("width", Value(r.arena.width));
    arena.set("height", Value(r.arena.height));
    arena.set("coverage", Value(r.arena.coverage));
    if (!r.arena.map.empty()) arena.set("map", Value(r.arena.map));
    root.set("arena", std::move(arena));

    return json::dump(root, /*pretty=*/true);
}

RulesetLoad loadRulesetFromFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        RulesetLoad result;
        result.errors.push_back("could not open ruleset file: " + path);
        return result;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return loadRulesetFromString(ss.str());
}

} // namespace tb
