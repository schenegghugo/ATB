//
// ruleset_demo.cpp — Test for the ruleset loader/serializer (data/RulesetJson).
// Round-trip the default ruleset, field fidelity, and strict validation.
//
#include "core/Ruleset.h"
#include "data/RulesetJson.h"

#include <cstdio>
#include <string>

using namespace tb;

static int g_fails = 0;
#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) std::printf("  [PASS] %s\n", msg);                                               \
        else { std::printf("  [FAIL] %s\n", msg); ++g_fails; }                                     \
    } while (0)

static bool rejectsWith(const std::string& json, const std::string& needle) {
    RulesetLoad r = loadRulesetFromString(json);
    if (r.ok) return false;
    for (const std::string& e : r.errors)
        if (e.find(needle) != std::string::npos) return true;
    return false;
}

int main() {
    std::printf("Round-trip the default ruleset\n");
    {
        const std::string a = serializeRuleset(makeDefaultRuleset(), "1.0.0");
        RulesetLoad load = loadRulesetFromString(a);
        CHECK(load.ok, "default ruleset serializes -> loads cleanly");
        CHECK(load.version == "1.0.0", "version round-trips");
        CHECK(a == serializeRuleset(load.ruleset, "1.0.0"), "serialize -> load -> serialize identical");
        CHECK(load.ruleset.teamSize == 1, "default teamSize = 1");
        CHECK(load.ruleset.economy.baseHp == 30 && load.ruleset.economy.pointBudget == 12,
              "default economy preserved");
        CHECK(load.ruleset.closingRing.enabled && load.ruleset.closingRing.startRound == 5,
              "default closing ring preserved");
    }

    std::printf("Hand-authored overrides + defaults for omitted blocks\n");
    {
        const char* src = R"({
            "schema": 1, "version": "custom",
            "format": { "teamSize": 3 },
            "economy": { "baseHp": 40, "hpStep": 8 },
            "bannedSpells": ["portal", "bomb"],
            "arena": { "width": 24, "height": 18, "coverage": 0.25 }
        })";
        RulesetLoad r = loadRulesetFromString(src);
        CHECK(r.ok, "partial ruleset loads");
        CHECK(r.ruleset.teamSize == 3, "teamSize override");
        CHECK(r.ruleset.economy.baseHp == 40 && r.ruleset.economy.hpStep == 8, "economy overrides");
        CHECK(r.ruleset.economy.apCost == 3, "omitted economy field keeps default");
        CHECK(r.ruleset.bannedSpells.size() == 2, "bans parsed");
        CHECK(r.ruleset.arena.width == 24 && r.ruleset.arena.coverage == 0.25, "arena overrides");
        CHECK(r.ruleset.closingRing.enabled, "omitted closingRing block keeps defaults");
    }

    std::printf("Strict validation\n");
    {
        CHECK(rejectsWith(R"({"version":"x"})", "schema"), "missing schema");
        CHECK(rejectsWith(R"({"schema":9,"version":"x"})", "unsupported schema"), "wrong schema");
        CHECK(rejectsWith(R"({"schema":1,"version":"x","format":{"teamSize":0}})", "teamSize"),
              "teamSize 0 rejected");
        CHECK(rejectsWith(R"({"schema":1,"version":"x","economy":{"hpCost":0}})", "costs must be >= 1"),
              "free HP (hpCost 0) rejected");
        CHECK(rejectsWith(R"({"schema":1,"version":"x","arena":{"coverage":2}})", "coverage"),
              "coverage > 1 rejected");
        CHECK(rejectsWith(R"({"schema":1,"version":"x","wat":true})", "unknown field"),
              "unknown top-level field rejected");
        CHECK(rejectsWith(R"({"schema":1,"version":"x","bannedSpells":[""]})", "non-empty"),
              "empty ban string rejected");
    }

    std::printf(g_fails == 0 ? "\nALL PASS (0 failures)\n" : "\n%d FAILURE(S)\n", g_fails);
    return g_fails == 0 ? 0 : 1;
}
