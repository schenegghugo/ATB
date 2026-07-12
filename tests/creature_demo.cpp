//
// creature_demo.cpp — Test for the bestiary loader/serializer (data/CreatureJson).
// Proves makeDefaultCreatures() round-trips through JSON byte-for-byte, that the
// bomb/blocker templates survive field-for-field, and that bad input is rejected.
//
#include "core/Creatures.h"
#include "data/CreatureJson.h"

#include <cstdio>
#include <string>

using namespace tb;

static int g_fails = 0;
#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) std::printf("  [PASS] %s\n", msg);                                               \
        else { std::printf("  [FAIL] %s\n", msg); ++g_fails; }                                     \
    } while (0)

static const Entity* find(const CreatureLoad& l, const std::string& key) {
    for (const Entity& e : l.creatures)
        if (e.name == key) return &e;
    return nullptr;
}

static bool rejectsWith(const std::string& json, const std::string& needle) {
    CreatureLoad r = loadCreaturesFromString(json);
    if (r.ok) return false;
    for (const std::string& e : r.errors)
        if (e.find(needle) != std::string::npos) return true;
    return false;
}

int main() {
    std::printf("Round-trip the default bestiary\n");
    {
        const std::string a = serializeCreatures(makeDefaultCreatures(), "1.0.0");
        CreatureLoad load = loadCreaturesFromString(a);
        CHECK(load.ok, "bestiary serializes -> loads cleanly");
        CHECK(load.version == "1.0.0", "version round-trips");
        CHECK(load.creatures.size() == 4, "all 4 creatures survive (bomb + 3 summons)");
        CHECK(a == serializeCreatures(load.creatures, "1.0.0"), "serialize -> load -> serialize byte-identical");
    }

    std::printf("Field fidelity (bomb object + blocker pull)\n");
    {
        CreatureLoad load = loadCreaturesFromString(serializeCreatures(makeDefaultCreatures(), "1.0.0"));
        const Entity* bomb = find(load, "bomb");
        CHECK(bomb && bomb->kind == EntityKind::Object, "bomb is an Object");
        CHECK(bomb && bomb->fuse == 2, "bomb fuse = 2");
        CHECK(bomb && bomb->hasStatus(StatusEffect::Kind::DamageOverTime), "bomb keeps its ignition DoT");
        CHECK(bomb && bomb->onDeath.shape == TargetShape::Circle && !bomb->onDeath.effects.empty(),
              "bomb onDeath is a radius blast");

        const Entity* blocker = find(load, "blocker");
        CHECK(blocker && blocker->kind == EntityKind::Summon, "blocker is a Summon");
        CHECK(blocker && blocker->spells.size() == 1 &&
                  blocker->spells[0].shape == TargetShape::Cross && blocker->spells[0].radius == 1 &&
                  blocker->spells[0].effects[0].type == Effect::Type::Pull,
              "blocker keeps its Cross-pull (radius 1)");
    }

    std::printf("Strict validation\n");
    {
        auto wrap = [](const std::string& c) {
            return std::string(R"({"schema":1,"version":"x","creatures":[)") + c + "]}";
        };
        CHECK(rejectsWith(R"({"schema":1,"version":"x","creatures":[]})", "empty"), "empty bestiary");
        CHECK(rejectsWith(wrap(R"({"key":"x","hp":10})"), "missing required field \"kind\""), "missing kind");
        CHECK(rejectsWith(wrap(R"({"key":"x","kind":"goblin","hp":10})"), "unknown kind"), "unknown kind");
        CHECK(rejectsWith(wrap(R"({"key":"x","kind":"object"})"), "missing required field \"hp\""), "missing hp");
        CHECK(rejectsWith(wrap(R"({"key":"x","kind":"object","hp":10,"wat":1})"), "unknown field"), "unknown field");
        CHECK(rejectsWith(wrap(R"({"key":"x","kind":"object","hp":10,"spells":[{"shape":"single"}]})"),
                          "apCost"),
              "creature spell still validated (missing apCost)");
    }

    std::printf(g_fails == 0 ? "\nALL PASS (0 failures)\n" : "\n%d FAILURE(S)\n", g_fails);
    return g_fails == 0 ? 0 : 1;
}
