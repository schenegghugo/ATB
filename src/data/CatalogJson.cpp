#include "CatalogJson.h"

#include "JsonRead.h"
#include "Sha256.h"
#include "SpellJson.h"

#include <cctype>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace tb {

namespace {

using Errors = std::vector<std::string>;

std::string capitalize(std::string s) {
    if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

// A catalog entry: the SpellDef wrapper (id/key/buildCost) flattened alongside
// the Spell's gameplay fields (handled by spelljson).
void parseSpell(const json::Value& sp, std::size_t idx, SpellDef& out, std::set<int>& seenIds,
                std::set<std::string>& seenKeys, Errors& e) {
    std::string ctx = "spells[" + std::to_string(idx) + "]";
    if (!sp.isObject()) {
        e.push_back(ctx + ": expected an object");
        return;
    }
    jsonread::checkAllowed(sp,
                           {"id", "key", "name", "buildCost", "apCost", "minRange", "maxRange",
                            "needsLineOfSight", "shape", "radius", "cooldown", "effects"},
                           ctx, e);

    int id = 0;
    if (jsonread::wantInt(sp, "id", ctx, id, e)) {
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
    if (jsonread::wantInt(sp, "buildCost", ctx, buildCost, e)) {
        if (buildCost < 0) e.push_back(ctx + ": \"buildCost\" must be >= 0");
        out.buildCost = buildCost;
    }

    Spell spell;
    spell.name = capitalize(out.key);            // default; overridden if "name" present
    spelljson::readSpellFields(sp, ctx, spell, e); // apCost, ranges, shape, cooldown, effects
    out.spell = std::move(spell);
}

} // namespace

CatalogLoad loadCatalogFromString(const std::string& text) {
    CatalogLoad result;
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
    jsonread::checkAllowed(root, {"schema", "version", "spells"}, "root", result.errors);

    int schema = 0;
    if (jsonread::wantInt(root, "schema", "root", schema, result.errors) &&
        schema != kCatalogSchemaVersion)
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
        s.set("buildCost", Value(d.buildCost));
        spelljson::writeSpellFields(s, d.spell); // name, apCost, ranges, …, effects
        spells.push_back(std::move(s));
    }
    root.set("spells", std::move(spells));
    return json::dump(root, /*pretty=*/true);
}

CatalogLoad loadCatalogFromFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        CatalogLoad result;
        result.errors.push_back("could not open catalog file: " + path);
        return result;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return loadCatalogFromString(ss.str());
}

} // namespace tb
