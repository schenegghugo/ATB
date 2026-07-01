//
// event_demo.cpp — Deterministic checks for the combat event stream (§2.3).
// Scripts the Battle API directly and asserts the ordered BattleEvents it emits,
// plus that the AI's planning clones don't record. Headless (no GL context).
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

Entity makeUnit(std::string name, Faction team, Vec2i pos, std::vector<int> spellIds) {
    Entity e;
    e.name = std::move(name);
    e.team = team;
    e.pos = pos;
    e.maxHp = e.hp = 60;
    e.maxAp = e.ap = 30;
    e.maxMp = e.mp = 30;
    e.initiative = team == Faction::Player ? 10 : 5;
    for (int id : spellIds)
        if (const SpellDef* d = catalog.find(id)) e.spells.push_back(d->spell);
    return e;
}

Battle makeArena(std::vector<int> playerSpells, Vec2i pPos = {1, 3}, Vec2i ePos = {2, 3}) {
    Grid grid(14, 7);
    std::vector<Entity> roster;
    roster.push_back(makeUnit("P", Faction::Player, pPos, std::move(playerSpells)));
    roster.push_back(makeUnit("E", Faction::Enemy, ePos, {spellid::Attack}));
    return Battle(std::move(grid), std::move(roster));
}

int slotOf(const Battle& b, EntityId who, int spellId) {
    const SpellDef* def = catalog.find(spellId);
    const auto& spells = b.unit(who).spells;
    for (int i = 0; i < static_cast<int>(spells.size()); ++i)
        if (def && spells[i].name == def->spell.name) return i;
    return -1;
}

// First event of `type` at or after index `from` (nullptr if none).
const BattleEvent* firstOf(const Battle& b, EventType type, std::size_t from = 0) {
    const auto& ev = b.events();
    for (std::size_t i = from; i < ev.size(); ++i)
        if (ev[i].type == type) return &ev[i];
    return nullptr;
}

} // namespace

int main() {
    constexpr EntityId P = 0, E = 1;

    std::printf("Construction emits the first unit's TurnStart\n");
    {
        Battle b = makeArena({spellid::Attack});
        check(!b.events().empty() && b.events().front().type == EventType::TurnStart,
              "first event is TurnStart");
        check(b.events().front().actor == P, "TurnStart actor is the initiative leader");
    }

    std::printf("A voluntary step emits Move with the destination\n");
    {
        Battle b = makeArena({spellid::Attack}, {1, 3}, {6, 3});
        const std::size_t before = b.events().size();
        check(b.stepTo(P, {1, 2}), "stepTo succeeds");
        const BattleEvent* mv = firstOf(b, EventType::Move, before);
        check(mv && mv->actor == P, "Move event for the mover");
        check(mv && mv->to.x == 1 && mv->to.y == 2, "Move records the destination tile");
    }

    std::printf("A cast emits Cast, then Damage on the victim\n");
    {
        Battle b = makeArena({spellid::Attack}); // enemy adjacent at (2,3)
        const int atk = slotOf(b, P, spellid::Attack);
        const std::size_t before = b.events().size();
        check(b.cast(P, atk, {2, 3}), "attack casts");
        const BattleEvent* c = firstOf(b, EventType::Cast, before);
        const BattleEvent* d = firstOf(b, EventType::Damage, before);
        check(c && c->actor == P && c->spellSlot == atk, "Cast event names caster + slot");
        check(d && d->target == E && d->amount > 0, "Damage event on the enemy, amount > 0");
        check(d && d->source == DamageSource::Spell, "spell damage tagged Spell");
        // Ordering: the Cast precedes the Damage it caused.
        std::size_t ci = 0, di = 0;
        for (std::size_t i = 0; i < b.events().size(); ++i) {
            if (b.events()[i].type == EventType::Cast) ci = i;
            if (b.events()[i].type == EventType::Damage) di = i;
        }
        check(ci < di, "Cast is ordered before its Damage");
    }

    std::printf("A lethal blow emits Damage then Death\n");
    {
        Battle b = makeArena({spellid::Attack});
        b.unit(E).hp = 1; // one hit kills
        const int atk = slotOf(b, P, spellid::Attack);
        const std::size_t before = b.events().size();
        check(b.cast(P, atk, {2, 3}), "killing blow casts");
        const BattleEvent* dmg = firstOf(b, EventType::Damage, before);
        const BattleEvent* death = firstOf(b, EventType::Death, before);
        check(dmg != nullptr, "Damage emitted");
        check(death && death->target == E, "Death emitted for the victim");
        check(!b.unit(E).alive(), "victim is actually dead");
    }

    std::printf("Heal and Status effects narrate\n");
    {
        Battle b = makeArena({spellid::Mend, spellid::Bulwark});
        b.unit(P).hp = 20; // wounded so Mend does something
        const int mend = slotOf(b, P, spellid::Mend);
        if (mend >= 0 && b.canCast(P, mend, b.unit(P).pos)) {
            const std::size_t before = b.events().size();
            b.cast(P, mend, b.unit(P).pos);
            const BattleEvent* h = firstOf(b, EventType::Heal, before);
            check(h && h->target == P && h->amount > 0, "Heal event with positive amount");
        } else {
            check(false, "Mend self-cast unavailable (test setup)");
        }
        const int bul = slotOf(b, P, spellid::Bulwark);
        if (bul >= 0 && b.canCast(P, bul, b.unit(P).pos)) {
            const std::size_t before = b.events().size();
            b.cast(P, bul, b.unit(P).pos);
            const BattleEvent* s = firstOf(b, EventType::Status, before);
            check(s && s->status == StatusEffect::Kind::Shield, "Status event tags the Shield kind");
        } else {
            check(false, "Bulwark self-cast unavailable (test setup)");
        }
    }

    std::printf("endTurn emits the next unit's TurnStart\n");
    {
        Battle b = makeArena({spellid::Attack});
        const std::size_t before = b.events().size();
        b.endTurn();
        const BattleEvent* ts = firstOf(b, EventType::TurnStart, before);
        check(ts && ts->actor == E, "TurnStart for the next unit in initiative");
    }

    std::printf("AI planning does not record on its throwaway clones\n");
    {
        Battle b = makeArena({spellid::Attack}, {1, 3}, {3, 3});
        // The enemy (AI) acts on the *real* battle: events should grow.
        const std::size_t before = b.events().size();
        b.endTurn(); // hand the turn to E
        (void)enemyTakeOneAction(b, E);
        check(b.events().size() > before, "real AI action was recorded");

        // A clone with recording disabled must stay silent even as it resolves.
        Battle sim = b;
        sim.setEventRecording(false);
        const std::size_t simBefore = sim.events().size();
        check(simBefore == 0, "disabling recording clears the inherited log");
        sim.stepTo(E, {2, 3});
        check(sim.events().empty(), "no events recorded while recording is off");
    }

    if (g_fails == 0) std::printf("\nALL PASS (0 failures)\n");
    else std::printf("\n%d FAILURE(S)\n", g_fails);
    return g_fails == 0 ? 0 : 1;
}
