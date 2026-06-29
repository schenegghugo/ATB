//
// enums_demo.cpp — Test for the enum <-> JSON string tables (data/SpellEnums).
//
// Including the header already fires its compile-time integrity static_asserts
// (round-trips, no duplicates, expected counts). This adds runtime checks of the
// lookup helpers, including the unknown-string path. Non-zero exit on failure.
//
#include "data/SpellEnums.h"

#include <cstdio>

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
    std::printf("enum -> string\n");
    CHECK(enums::toString(enums::kTargetShapes, TargetShape::Circle) == "circle", "shape Circle");
    CHECK(enums::toString(enums::kEffectTypes, Effect::Type::ApplyStatus) == "applyStatus",
          "effect ApplyStatus");
    CHECK(enums::toString(enums::kStatusKinds, StatusEffect::Kind::DamageOverTime) == "damageOverTime",
          "status DamageOverTime");
    CHECK(enums::toString(enums::kGroundKinds, GroundKind::Portal) == "portal", "ground Portal");

    std::printf("string -> enum\n");
    {
        auto s = enums::fromString(enums::kTargetShapes, "line");
        CHECK(s && *s == TargetShape::Line, "\"line\" -> Line");
        auto e = enums::fromString(enums::kEffectTypes, "spawn");
        CHECK(e && *e == Effect::Type::Spawn, "\"spawn\" -> Spawn");
        auto k = enums::fromString(enums::kStatusKinds, "shield");
        CHECK(k && *k == StatusEffect::Kind::Shield, "\"shield\" -> Shield");
    }

    std::printf("unknown strings\n");
    CHECK(!enums::fromString(enums::kTargetShapes, "blob"), "unknown shape -> nullopt");
    CHECK(!enums::fromString(enums::kStatusKinds, "dot"), "wrong slug -> nullopt (must be damageOverTime)");
    CHECK(!enums::fromString(enums::kEffectTypes, ""), "empty string -> nullopt");

    std::printf("round-trip every row (runtime mirror of the static_asserts)\n");
    {
        bool ok = true;
        for (const auto& [e, s] : enums::kEffectTypes)
            ok = ok && enums::fromString(enums::kEffectTypes, s) == std::optional{e};
        CHECK(ok, "every effect-type row round-trips");
    }

    std::printf(g_fails == 0 ? "\nALL PASS (0 failures)\n" : "\n%d FAILURE(S)\n", g_fails);
    return g_fails == 0 ? 0 : 1;
}
