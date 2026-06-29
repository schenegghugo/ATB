//
// ai_demo.cpp — Behavioural checks for the utility-aware AI. Crafts situations
// where the right play is a *utility* spell and asserts the AI picks it.
//
#include "core/AI.h"
#include "core/Battle.h"
#include "core/Spells.h"

#include <cstdio>
#include <vector>

using namespace tb;

namespace {

int g_fails = 0;
void check(bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) ++g_fails;
}

SpellCatalog catalog = makeDefaultCatalog();

Entity makeUnit(Faction team, Vec2i pos, std::vector<int> ids, int hp = 60, int mp = 4) {
    Entity e;
    e.name = team == Faction::Player ? "P" : "E";
    e.team = team;
    e.pos = pos;
    e.maxHp = 60;
    e.hp = hp;
    e.maxAp = e.ap = 12;
    e.maxMp = e.mp = mp;
    e.initiative = team == Faction::Player ? 10 : 5;
    for (int id : ids)
        if (const SpellDef* d = catalog.find(id)) e.spells.push_back(d->spell);
    return e;
}

} // namespace

int main() {
    constexpr EntityId P = 0;

    // --- Heals when wounded and unable to attack ----------------------------
    std::printf("Wounded + no attack available -> Mend:\n");
    {
        Grid g(12, 5);
        std::vector<Entity> r;
        r.push_back(makeUnit(Faction::Player, {1, 2}, {spellid::Attack, spellid::Mend}, /*hp=*/20));
        r.push_back(makeUnit(Faction::Enemy, {10, 2}, {spellid::Attack})); // dist 9, out of range
        Battle b(std::move(g), std::move(r));
        (void)enemyTakeOneAction(b, P);
        check(b.unit(P).hp > 20, "AI healed itself instead of flailing");
    }

    // --- Shields when threatened and unable to attack -----------------------
    std::printf("Threatened + no attack available -> Bulwark:\n");
    {
        Grid g(12, 5);
        std::vector<Entity> r;
        r.push_back(makeUnit(Faction::Player, {1, 2}, {spellid::Attack, spellid::Bulwark}));
        // Foe at distance 7: beyond our attack range (6) but it can close + hit us.
        r.push_back(makeUnit(Faction::Enemy, {8, 2}, {spellid::Attack}, /*hp=*/60, /*mp=*/4));
        Battle b(std::move(g), std::move(r));
        (void)enemyTakeOneAction(b, P);
        check(b.unit(P).hasStatus(StatusEffect::Kind::Shield), "AI raised a shield against the threat");
    }

    // --- Prefers AoE on a cluster over single-target ------------------------
    std::printf("Two clustered foes -> Fireball hits both:\n");
    {
        Grid g(12, 5);
        std::vector<Entity> r;
        r.push_back(makeUnit(Faction::Player, {2, 2}, {spellid::Attack, spellid::Fireball}));
        r.push_back(makeUnit(Faction::Enemy, {7, 2}, {spellid::Attack}));
        r.push_back(makeUnit(Faction::Enemy, {7, 3}, {spellid::Attack}));
        Battle b(std::move(g), std::move(r));
        (void)enemyTakeOneAction(b, P);
        check(b.unit(1).hp < 60 && b.unit(2).hp < 60, "both foes took Fireball damage");
    }

    // --- Lookahead: poison THEN go invisible (deal DoT while taking none) ----
    std::printf("Planner combo vs a hard-hitting foe -> Poison + Invisible:\n");
    {
        Grid g(12, 5);
        std::vector<Entity> r;
        // Fragile caster, foe in range, no need to move (mp 0).
        r.push_back(makeUnit(Faction::Player, {3, 2},
                             {spellid::Attack, spellid::Poison, spellid::Invisible},
                             /*hp=*/30, /*mp=*/0));
        Entity foe = makeUnit(Faction::Enemy, {6, 2}, {spellid::Attack}, /*hp=*/60, /*mp=*/4);
        // Give the foe a heavy hit, so cloaking is worth more than a second attack.
        foe.spells.push_back(Spell{"bighit", 3, 1, 6, true, TargetShape::Single, 0, 0,
                                   {Effect{Effect::Type::Damage, 30, {}, {}}}});
        r.push_back(std::move(foe));
        Battle b(std::move(g), std::move(r));

        runEnemyTurn(b, /*autoEndTurn=*/false); // plan + execute the player's whole turn
        const bool poisoned = b.unit(1).hasStatus(StatusEffect::Kind::DamageOverTime);
        const bool cloaked = b.unit(P).invisible();
        check(poisoned && cloaked, "planner applied DoT and then went invisible");
    }

    std::printf("\n%s (%d failure%s)\n", g_fails == 0 ? "ALL PASS" : "FAILURES", g_fails,
                g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
