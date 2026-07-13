//
// ai_demo.cpp — Behavioural checks for the utility-aware AI. Crafts situations
// where the right play is a *utility* spell and asserts the AI picks it.
//
#include "core/AI.h"
#include "core/Battle.h"
#include "core/Evaluator.h"
#include "core/Intel.h"
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

Entity makeUnit(Faction team, Vec2i pos, std::vector<int> ids, int hp = 60, int mp = 4,
                int ap = 12) {
    Entity e;
    e.name = team == Faction::Player ? "P" : "E";
    e.team = team;
    e.pos = pos;
    e.maxHp = hp > 60 ? hp : 60;
    e.hp = hp;
    e.maxAp = e.ap = ap;
    e.maxMp = e.mp = mp;
    e.initiative = team == Faction::Player ? 10 : 5;
    for (int id : ids)
        if (const SpellDef* d = catalog.find(id)) e.spells.push_back(d->spell);
    return e;
}

int nearestFoeDist(const Battle& b, EntityId self) {
    const Entity& me = b.unit(self);
    int best = -1;
    for (EntityId i = 0; i < b.unitCount(); ++i) {
        const Entity& u = b.unit(i);
        if (!u.alive() || u.team == me.team) continue;
        const int d = manhattan(me.pos, u.pos);
        if (best < 0 || d < best) best = d;
    }
    return best;
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
        // Foe at distance 8: past our move+attack reach (mp 4 + range 3 = 7), so we
        // can't strike back this turn — the best play is to raise a shield.
        r.push_back(makeUnit(Faction::Enemy, {9, 2}, {spellid::Attack}, /*hp=*/60, /*mp=*/4));
        Battle b(std::move(g), std::move(r));
        (void)enemyTakeOneAction(b, P);
        check(b.unit(P).hasStatus(StatusEffect::Kind::Shield), "AI raised a shield against the threat");
    }

    // --- Prefers AoE on a cluster over single-target ------------------------
    // AP 7 buys Fireball (4) + Attack (3): the AoE line out-damages two single
    // Attacks (28 + 15 vs 30), so the planner should open with the Fireball.
    std::printf("Two clustered foes -> Fireball hits both:\n");
    {
        Grid g(12, 5);
        std::vector<Entity> r;
        r.push_back(makeUnit(Faction::Player, {2, 2}, {spellid::Attack, spellid::Fireball},
                             /*hp=*/60, /*mp=*/4, /*ap=*/7));
        r.push_back(makeUnit(Faction::Enemy, {7, 2}, {spellid::Attack}));
        r.push_back(makeUnit(Faction::Enemy, {7, 3}, {spellid::Attack}));
        Battle b(std::move(g), std::move(r));
        (void)enemyTakeOneAction(b, P);
        check(b.unit(1).hp < 60 && b.unit(2).hp < 60, "both foes took Fireball damage");
    }

    // --- Evaluator prices elemental hazards + lost turns (E.6) --------------
    std::printf("Evaluator values surfaces + stun/freeze:\n");
    {
        auto boardScore = [&](void (*setup)(Battle&)) {
            Grid g(12, 5);
            std::vector<Entity> r;
            r.push_back(makeUnit(Faction::Player, {2, 2}, {spellid::Attack}));
            r.push_back(makeUnit(Faction::Enemy, {8, 2}, {spellid::Attack}));
            Battle b(std::move(g), std::move(r));
            setup(b);
            return handcraftedEvaluator().evaluate(b, makeEvalContext(b, Faction::Player, false));
        };
        const double base = boardScore([](Battle&) {});
        const double stunned = boardScore(
            [](Battle& b) { b.unit(1).statuses.push_back({StatusEffect::Kind::Stunned, 0, 1, 0}); });
        const double burning = boardScore(
            [](Battle& b) { b.unit(1).statuses.push_back({StatusEffect::Kind::Burning, 5, 2, 0}); });
        check(stunned > base, "a stunned foe (lost turn) scores better for me");
        check(burning > base, "a burning foe (Fire DoT) scores better for me");

        // Painting Fire under a foe (clean A/B on one board) improves my eval.
        {
            Grid g(12, 5);
            std::vector<Entity> r;
            r.push_back(makeUnit(Faction::Player, {2, 2}, {spellid::Ignite}));
            r.push_back(makeUnit(Faction::Enemy, {6, 2}, {spellid::Attack})); // within Ignite range 5
            Battle b(std::move(g), std::move(r));
            const auto ctx = makeEvalContext(b, Faction::Player, false);
            const double before = handcraftedEvaluator().evaluate(b, ctx);
            const bool cast = b.cast(P, 0, {6, 2}); // Ignite paints Fire on the enemy's tile
            const double after = handcraftedEvaluator().evaluate(b, ctx);
            check(cast, "Ignite cast lands (sanity)");
            check(after > before, "painting Fire under a foe improves the eval for me");
        }
    }

    // --- Lookahead: finds the exact-lethal line ------------------------------
    // 12 AP = four Attacks = exactly 60 damage: the planner should simply kill
    // the foe this turn instead of trading (the old, duplicate-crowded beam
    // missed this line and settled for poison-and-cloak).
    std::printf("Lethal line available -> AI takes the kill:\n");
    {
        Grid g(12, 5);
        std::vector<Entity> r;
        r.push_back(makeUnit(Faction::Player, {3, 2},
                             {spellid::Attack, spellid::Poison, spellid::Invisible},
                             /*hp=*/30, /*mp=*/0));
        Entity foe = makeUnit(Faction::Enemy, {6, 2}, {spellid::Attack}, /*hp=*/60, /*mp=*/4);
        foe.spells.push_back(Spell{"bighit", 3, 1, 6, true, TargetShape::Single, 0, 0,
                                   {Effect{Effect::Type::Damage, 30, {}, {}}}});
        r.push_back(std::move(foe));
        Battle b(std::move(g), std::move(r));

        runEnemyTurn(b, /*autoEndTurn=*/false);
        check(!b.unit(1).alive(), "planner found the four-Attack kill");
    }

    // --- No kill available -> trade then vanish ------------------------------
    // 6 AP, foe too healthy to kill: Attack + Invisible (deal 15, take nothing)
    // beats Attack + Attack (deal 30, then eat a 30-point counter).
    std::printf("Fragile caster, no lethal -> Attack + Invisible:\n");
    {
        Grid g(12, 5);
        std::vector<Entity> r;
        r.push_back(makeUnit(Faction::Player, {3, 2}, {spellid::Attack, spellid::Invisible},
                             /*hp=*/20, /*mp=*/0, /*ap=*/6));
        Entity foe = makeUnit(Faction::Enemy, {6, 2}, {}, /*hp=*/100, /*mp=*/4);
        foe.spells.push_back(Spell{"bighit", 3, 1, 6, true, TargetShape::Single, 0, 0,
                                   {Effect{Effect::Type::Damage, 30, {}, {}}}});
        r.push_back(std::move(foe));
        Battle b(std::move(g), std::move(r));

        runEnemyTurn(b, /*autoEndTurn=*/false);
        check(b.unit(1).hp < 100, "AI still got its hit in");
        check(b.unit(P).invisible(), "AI cloaked instead of trading down");
    }

    // --- Blind then kite out of the shrunken envelope ------------------------
    // Foe: Fireball (range 6, mp 4) = threat radius 10 — our MP alone can't
    // escape it. Blind cuts the foe's reach to 3 (+4 mp = 7): cast Blind at
    // range 6, retreat to 10 > 7, and the AI takes zero expected damage.
    std::printf("Artillery foe -> Blind + retreat out of reach:\n");
    {
        Grid g(12, 5);
        std::vector<Entity> r;
        r.push_back(makeUnit(Faction::Player, {5, 2}, {spellid::Blind}, /*hp=*/30, /*mp=*/4));
        r.push_back(makeUnit(Faction::Enemy, {11, 2}, {spellid::Fireball}, /*hp=*/60, /*mp=*/4));
        Battle b(std::move(g), std::move(r));

        runEnemyTurn(b, /*autoEndTurn=*/false);
        check(b.unit(1).hasStatus(StatusEffect::Kind::RangeDebuff), "AI blinded the artillery");
        check(nearestFoeDist(b, P) > 7, "AI kited beyond the blinded threat range");
    }

    // --- Flux-slow the chaser, then disengage --------------------------------
    // Foe: Attack (range 6, mp 4) = threat radius 10; our 4 MP from distance 5
    // only reaches 9. Flux (polarized MpBuff) slows the foe to 2 MP -> radius 8,
    // and 9 > 8 is safe.
    std::printf("Chasing foe -> Flux slow + retreat:\n");
    {
        Grid g(14, 5);
        std::vector<Entity> r;
        r.push_back(makeUnit(Faction::Player, {8, 2}, {spellid::Flux}, /*hp=*/30, /*mp=*/4));
        r.push_back(makeUnit(Faction::Enemy, {13, 2}, {spellid::Attack}, /*hp=*/60, /*mp=*/4));
        Battle b(std::move(g), std::move(r));

        runEnemyTurn(b, /*autoEndTurn=*/false);
        bool slowed = false;
        for (const StatusEffect& s : b.unit(1).statuses)
            if (s.kind == StatusEffect::Kind::MpBuff && s.magnitude < 0) slowed = true;
        check(slowed, "AI slowed the chaser with Flux");
        check(nearestFoeDist(b, P) > 8, "AI disengaged beyond the slowed threat range");
    }

    // --- Surge: banked AP is worth the delayed crash --------------------------
    // Nothing to fight yet (foe far out of reach): the AI should invest in the
    // AP buff whose crash is discounted past the likely horizon.
    std::printf("Idle turn -> Surge (banked AP beats idling):\n");
    {
        Grid g(20, 5);
        std::vector<Entity> r;
        r.push_back(makeUnit(Faction::Player, {0, 2}, {spellid::Surge}, /*hp=*/60, /*mp=*/0));
        r.push_back(makeUnit(Faction::Enemy, {19, 2}, {spellid::Attack}));
        Battle b(std::move(g), std::move(r));

        runEnemyTurn(b, /*autoEndTurn=*/false);
        check(b.unit(P).hasStatus(StatusEffect::Kind::ApBuff), "AI banked the Surge AP buff");
    }

    // --- Intel model: knowledge is a fold over the event stream ---------------
    std::printf("Intel: revealed slots + observed turns from events:\n");
    {
        Grid g(12, 5);
        std::vector<Entity> r;
        r.push_back(makeUnit(Faction::Player, {1, 2}, {spellid::Attack, spellid::Mend}));
        r.push_back(makeUnit(Faction::Enemy, {7, 2}, {spellid::Attack, spellid::Fireball},
                             /*hp=*/60, /*mp=*/4));
        Battle b(std::move(g), std::move(r), StormConfig{false, 5, 8});
        b.endTurn();                             // -> foe's turn
        const bool cast = b.cast(1, 1, {1, 2});  // foe reveals Fireball (slot 1), dist 6
        b.endTurn();                             // -> back to us

        const Intel intel = buildIntel(b, Faction::Player);
        check(cast, "setup: foe actually cast Fireball");
        check(intel.byId[1].revealed(1), "the cast slot is revealed");
        check(!intel.byId[1].revealed(0), "the never-cast slot stays hidden");
        check(intel.byId[1].turnsObserved == 1, "one foe turn observed");
        check(intel.byId[0].revealedSlots.empty(), "own units are not tracked");
    }

    // --- Intel ("scout"): fear what you've SEEN, not the hidden hand ----------
    // Same board, two minds, storm off. The foe's Fireball (threat radius 11)
    // is never revealed. Omniscient "beam" reads the loadout and flees the
    // envelope; "scout" only carries the unknown prior, which has decayed over
    // ten quiet foe turns — so it holds its ground (and lets the aggression
    // gradient pull it in) where omniscience runs. (5 MP so the retreat can
    // actually clear the Fireball's 7+4 reach.)
    std::printf("Unrevealed artillery, long observed -> scout probes, beam flees:\n");
    {
        auto makeBoard = []() {
            Grid g(16, 5);
            std::vector<Entity> r;
            r.push_back(makeUnit(Faction::Player, {6, 2}, {}, /*hp=*/30, /*mp=*/5));
            r.push_back(makeUnit(Faction::Enemy, {14, 2}, {spellid::Fireball}, /*hp=*/60,
                                 /*mp=*/4)); // dist 8: inside the true 7+4 envelope
            return Battle(std::move(g), std::move(r), StormConfig{false, 5, 8});
        };
        auto passRounds = [](Battle& b, int rounds) {
            for (int i = 0; i < rounds * 2; ++i) b.endTurn(); // nobody acts
        };

        Battle forBeam = makeBoard();
        passRounds(forBeam, 10);
        runEnemyTurn(forBeam, /*autoEndTurn=*/false, *brainByName("beam"));
        const int beamDist = nearestFoeDist(forBeam, P);

        Battle forScout = makeBoard();
        passRounds(forScout, 10);
        runEnemyTurn(forScout, /*autoEndTurn=*/false, *brainByName("scout"));
        const int scoutDist = nearestFoeDist(forScout, P);

        check(beamDist > 11, "omniscient beam fled beyond the (secretly real) envelope");
        check(scoutDist <= 8, "scout, shown nothing for ten turns, holds or probes");
    }

    // --- Minimax ("deep"): refuse the bait the static eval swallows -----------
    // Me: 12 HP, one Attack. Foe: 60 HP, Attack, threat radius 10. From dist 7,
    // engaging nets +2.25 on the static eval (deal 15, risk term unchanged), so
    // "beam" walks in and swings — and dies to the reply it never modelled.
    // "deep" prices the exchange: every engaging line ends with the foe's
    // answer killing us (-lossPenalty), while kiting stays ahead of the
    // envelope for the whole horizon (the arena leaves room to run — in a
    // cramped box deep correctly concludes death is inevitable and trades).
    // Same board, opposite decisions.
    std::printf("Suicidal trade -> beam swings, deep disengages:\n");
    {
        auto makeBoard = []() {
            Grid g(30, 5);
            std::vector<Entity> r;
            r.push_back(makeUnit(Faction::Player, {12, 2}, {spellid::Attack}, /*hp=*/12,
                                 /*mp=*/4, /*ap=*/3));
            r.push_back(makeUnit(Faction::Enemy, {5, 2}, {spellid::Attack}, /*hp=*/60,
                                 /*mp=*/4));
            return Battle(std::move(g), std::move(r), StormConfig{false, 5, 8});
        };

        Battle forBeam = makeBoard();
        runEnemyTurn(forBeam, /*autoEndTurn=*/false, *brainByName("beam"));
        check(forBeam.unit(1).hp < 60, "beam takes the (statically profitable) swing");

        Battle forDeep = makeBoard();
        runEnemyTurn(forDeep, /*autoEndTurn=*/false, *brainByName("deep"));
        check(forDeep.unit(1).hp == 60 && nearestFoeDist(forDeep, P) > 10,
              "deep sees the reply and flees the envelope instead");
    }

    // --- Pure flight: no spells, inside a threat envelope ---------------------
    // Nothing to cast: the only good move is the retreat macro (the toward-foe
    // macros can't express it). Threat radius 7; fleeing reaches 9.
    std::printf("Unarmed + threatened -> flee out of range:\n");
    {
        Grid g(12, 5);
        std::vector<Entity> r;
        r.push_back(makeUnit(Faction::Player, {6, 2}, {}, /*hp=*/20, /*mp=*/4));
        r.push_back(makeUnit(Faction::Enemy, {11, 2}, {spellid::Attack}, /*hp=*/60, /*mp=*/4));
        Battle b(std::move(g), std::move(r));

        runEnemyTurn(b, /*autoEndTurn=*/false);
        check(nearestFoeDist(b, P) > 7, "AI fled beyond the foe's threat range");
    }

    std::printf("\n%s (%d failure%s)\n", g_fails == 0 ? "ALL PASS" : "FAILURES", g_fails,
                g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
