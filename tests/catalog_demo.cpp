//
// catalog_demo.cpp — Test for the JSON catalog loader/serializer (data/CatalogJson).
//
// Proves: the compiled makeDefaultCatalog() round-trips through JSON byte-for-byte
// (serialize -> load -> serialize), field-level values survive, and malformed
// inputs are rejected with contextual errors. Non-zero exit on failure.
//
#include "core/Spells.h"
#include "data/CatalogJson.h"

#include <cstdio>
#include <string>

using namespace tb;

static int g_fails = 0;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            std::printf("  [PASS] %s\n", msg);                                                     \
        } else {                                                                                   \
            std::printf("  [FAIL] %s\n", msg);                                                     \
            ++g_fails;                                                                             \
        }                                                                                          \
    } while (0)

// True iff loading `json` fails and at least one error mentions `needle`.
static bool rejectsWith(const std::string& json, const std::string& needle) {
    CatalogLoad r = loadCatalogFromString(json);
    if (r.ok) return false;
    for (const std::string& e : r.errors)
        if (e.find(needle) != std::string::npos) return true;
    return false;
}

int main() {
    std::printf("Round-trip the default catalog\n");
    {
        const std::string a = serializeCatalog(makeDefaultCatalog(), "1.0.0");
        CatalogLoad load = loadCatalogFromString(a);
        CHECK(load.ok, "default catalog serializes -> loads cleanly");
        CHECK(load.version == "1.0.0", "version round-trips");
        CHECK(load.catalog.all().size() == 11, "all 11 spells survive");
        const std::string b = serializeCatalog(load.catalog, "1.0.0");
        CHECK(a == b, "serialize -> load -> serialize is byte-identical");
    }

    std::printf("Field-level fidelity (poison: damage + DoT)\n");
    {
        CatalogLoad load = loadCatalogFromString(serializeCatalog(makeDefaultCatalog(), "1.0.0"));
        const SpellDef* poison = load.catalog.findByKey("poison");
        CHECK(poison != nullptr, "find poison by key");
        if (poison) {
            const Spell& sp = poison->spell;
            CHECK(poison->id == spellid::Poison, "id preserved");
            CHECK(poison->buildCost == 3 && sp.apCost == 3, "costs preserved");
            CHECK(sp.effects.size() == 2, "two effects");
            CHECK(sp.effects[0].type == Effect::Type::Damage && sp.effects[0].amount == 4,
                  "effect[0] = damage 4");
            CHECK(sp.effects[1].type == Effect::Type::ApplyStatus &&
                      sp.effects[1].status.kind == StatusEffect::Kind::DamageOverTime &&
                      sp.effects[1].status.magnitude == 7 && sp.effects[1].status.remainingTurns == 3,
                  "effect[1] = DoT 7/turn x3");
        }
        // Spawn payload (shelter -> wall, duration 3).
        const SpellDef* shelter = load.catalog.findByKey("shelter");
        CHECK(shelter && shelter->spell.effects[0].type == Effect::Type::Spawn &&
                  shelter->spell.effects[0].ground.kind == GroundKind::Wall &&
                  shelter->spell.effects[0].ground.duration == 3,
              "shelter spawns a wall, duration 3");
    }

    std::printf("Hand-authored minimal spell + defaults\n");
    {
        const char* src = R"({
            "schema": 1, "version": "x",
            "spells": [
              { "id": 1, "key": "jab", "buildCost": 1, "apCost": 2,
                "effects": [ { "type": "damage", "amount": 5 } ] }
            ]
        })";
        CatalogLoad r = loadCatalogFromString(src);
        CHECK(r.ok, "minimal spell loads");
        const SpellDef* jab = r.ok ? r.catalog.findByKey("jab") : nullptr;
        CHECK(jab && jab->spell.name == "Jab", "name defaults to capitalized key");
        CHECK(jab && jab->spell.minRange == 1 && jab->spell.maxRange == 1, "range defaults");
        CHECK(jab && jab->spell.shape == TargetShape::Single, "shape defaults to single");
        CHECK(jab && jab->spell.needsLineOfSight, "LOS defaults to true");
    }

    std::printf("Strict validation rejects bad input\n");
    {
        auto wrap = [](const std::string& spell) {
            return std::string(R"({"schema":1,"version":"x","spells":[)") + spell + "]}";
        };
        CHECK(rejectsWith("{ not json", "parse error"), "malformed JSON -> parse error");
        CHECK(rejectsWith(R"({"version":"x","spells":[]})", "schema"), "missing schema");
        CHECK(rejectsWith(R"({"schema":9,"version":"x","spells":[{"id":1,"key":"a","buildCost":1,"apCost":1,"effects":[{"type":"damage","amount":1}]}]})",
                          "unsupported schema"),
              "wrong schema version");
        CHECK(rejectsWith(R"({"schema":1,"version":"x","spells":[]})", "empty"), "empty spells");
        CHECK(rejectsWith(wrap(R"({"id":1,"key":"a","buildCost":1,"apCost":1,"shape":"square","effects":[{"type":"damage","amount":1}]})"),
                          "unknown shape"),
              "unknown enum value");
        CHECK(rejectsWith(wrap(R"({"id":1,"key":"a","buildCost":1,"apCost":1,"wat":true,"effects":[{"type":"damage","amount":1}]})"),
                          "unknown field"),
              "unknown field rejected");
        CHECK(rejectsWith(wrap(R"({"key":"a","buildCost":1,"apCost":1,"effects":[{"type":"damage","amount":1}]})"),
                          "missing required field \"id\""),
              "missing id");
        CHECK(rejectsWith(wrap(R"({"id":1,"key":"a","buildCost":1,"apCost":1,"minRange":5,"maxRange":2,"effects":[{"type":"damage","amount":1}]})"),
                          "minRange"),
              "minRange > maxRange");
        CHECK(rejectsWith(wrap(R"({"id":1,"key":"a","buildCost":1,"apCost":1,"effects":[{"type":"damage","amount":1.5}]})"),
                          "must be an integer"),
              "non-integer amount");
        CHECK(rejectsWith(wrap(R"({"id":1,"key":"a","buildCost":1,"apCost":1,"effects":[{"type":"applyStatus","amount":3}]})"),
                          "requires a \"status\""),
              "applyStatus without status object");
        CHECK(rejectsWith(wrap(R"({"id":1,"key":"a","buildCost":1,"apCost":1,"effects":[{"type":"damage","amount":1,"ground":{"kind":"wall"}}]})"),
                          "not valid for effect type"),
              "stray payload for wrong type");
        CHECK(rejectsWith(std::string(R"({"schema":1,"version":"x","spells":[)") +
                              R"({"id":1,"key":"a","buildCost":1,"apCost":1,"effects":[{"type":"damage","amount":1}]},)" +
                              R"({"id":1,"key":"b","buildCost":1,"apCost":1,"effects":[{"type":"damage","amount":1}]}]})",
                          "duplicate id"),
              "duplicate id across spells");
    }

    std::printf(g_fails == 0 ? "\nALL PASS (0 failures)\n" : "\n%d FAILURE(S)\n", g_fails);
    return g_fails == 0 ? 0 : 1;
}
