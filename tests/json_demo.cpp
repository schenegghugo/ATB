//
// json_demo.cpp — Unit/smoke test for the hand-rolled JSON layer (data/Json).
//
// Exercises parsing (types, numbers, escapes, \u/surrogates, nesting), strict
// error reporting, accessors, and parse->dump->parse round-trip equality.
// Returns non-zero on any failure so CI catches regressions.
//
#include "data/Json.h"

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

int main() {
    using json::Value;

    std::printf("Scalars & types\n");
    {
        CHECK(json::parse("true").value.isBool(), "true -> bool");
        CHECK(json::parse("null").value.isNull(), "null -> null");
        CHECK(json::parse("\"hi\"").value.isString(), "string -> string");
        auto neg = json::parse("-12.5");
        CHECK(neg.ok && neg.value.isNumber() && neg.value.asNumber() == -12.5, "negative float");
        auto exp = json::parse("6.02e2");
        CHECK(exp.ok && exp.value.asNumber() == 602.0, "exponent number");
        auto zero = json::parse("0");
        CHECK(zero.ok && zero.value.asNumber() == 0.0, "bare zero");
    }

    std::printf("Strings & escapes\n");
    {
        auto r = json::parse(R"("a\t\"b\"\nAé")");
        CHECK(r.ok && r.value.asString() == "a\t\"b\"\nA\xc3\xa9", "escapes + \\u (incl. UTF-8)");
        auto surro = json::parse(R"("😀")"); // U+1F600 grinning face
        CHECK(surro.ok && surro.value.asString() == "\xf0\x9f\x98\x80", "surrogate pair -> 4-byte UTF-8");
    }

    std::printf("Objects, arrays, lookup\n");
    {
        const char* src = R"({
            "schema": 1,
            "name": "demo",
            "nested": { "a": [1, 2, 3], "b": true },
            "empty_obj": {}, "empty_arr": []
        })";
        auto r = json::parse(src);
        CHECK(r.ok, "parse nested document");
        const Value& v = r.value;
        CHECK(v.isObject(), "root is object");
        CHECK(v.find("schema") && v.find("schema")->asNumber() == 1.0, "find scalar member");
        CHECK(v.find("missing") == nullptr, "find absent member -> nullptr");
        const Value* nested = v.find("nested");
        CHECK(nested && nested->find("a") && nested->find("a")->isArray(), "nested array");
        CHECK(nested->find("a")->asArray().size() == 3, "array size");
        CHECK(nested->find("a")->asArray()[2].asNumber() == 3.0, "array element");
        CHECK(v.find("empty_arr")->asArray().empty(), "empty array");
        CHECK(v.find("empty_obj")->asObject().empty(), "empty object");
    }

    std::printf("Strict error reporting\n");
    {
        CHECK(!json::parse("").ok, "empty input fails");
        CHECK(!json::parse("{ \"a\": 1 ").ok, "unterminated object fails");
        CHECK(!json::parse("[1, 2,]").ok, "trailing comma fails");
        CHECK(!json::parse("{ \"a\" 1 }").ok, "missing colon fails");
        CHECK(!json::parse("01").ok, "leading-zero number fails");
        CHECK(!json::parse("nul").ok, "bad literal fails");
        auto trailing = json::parse("{} garbage");
        CHECK(!trailing.ok, "trailing characters fail");
        CHECK(!trailing.error.empty() && trailing.error.find("line") != std::string::npos,
              "error message carries a position");
    }

    std::printf("Builders & round-trip\n");
    {
        Value obj = Value::makeObject();
        obj.set("id", Value(7));
        obj.set("key", Value("fireball"));
        Value arr = Value::makeArray();
        arr.push_back(Value(1));
        arr.push_back(Value(2));
        obj.set("rect", std::move(arr));
        obj.set("id", Value(8)); // set() replaces existing key
        CHECK(obj.find("id")->asNumber() == 8.0, "set() replaces existing key");
        CHECK(obj.asObject().size() == 3, "no duplicate keys after replace");

        const char* catalogish = R"({
            "schema": 1, "version": "1.0.0",
            "spells": [
              { "id": 1, "key": "attack", "buildCost": 1, "apCost": 3,
                "shape": "single", "needsLineOfSight": true,
                "effects": [ { "type": "damage", "amount": 15 } ] },
              { "id": 3, "key": "poison", "buildCost": 3,
                "effects": [ { "type": "applyStatus",
                  "status": { "kind": "damageOverTime", "magnitude": 7, "turns": 3 } } ] }
            ]
        })";
        auto a = json::parse(catalogish);
        CHECK(a.ok, "parse catalog-shaped doc");
        std::string dumped = json::dump(a.value);
        auto b = json::parse(dumped);
        CHECK(b.ok && b.value == a.value, "parse -> dump -> parse round-trips equal");

        // Compact and pretty must parse back to the same value.
        auto c = json::parse(json::dump(a.value, /*pretty=*/false));
        CHECK(c.ok && c.value == a.value, "compact dump round-trips equal");

        // Integers serialize cleanly (no 15.0).
        CHECK(json::dump(Value(15), false) == "15", "integral number prints without '.0'");
    }

    std::printf(g_fails == 0 ? "\nALL PASS (0 failures)\n" : "\n%d FAILURE(S)\n", g_fails);
    return g_fails == 0 ? 0 : 1;
}
