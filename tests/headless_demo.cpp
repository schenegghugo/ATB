//
// headless_demo.cpp — Proof the gameplay core is fully decoupled from Raylib.
// Builds two classless characters from the catalog, runs a deterministic
// AI-vs-AI battle to the death, and prints a turn log. CI smoke test.
//
#include "core/AI.h"
#include "core/Battle.h"
#include "core/Build.h"
#include "core/Grid.h"
#include "core/Spells.h"

#include <cstdio>
#include <cstdlib>

using namespace tb;

namespace {

// Drive whichever unit holds the turn with the selected Brain, one action at a
// time (mirrors how the GUI paces the AI).
void runTurn(Battle& b, const Brain& brain) {
    const EntityId self = b.activeUnit();
    const Entity& me = b.unit(self);
    const int cap = me.maxAp + me.maxMp + 4;
    for (int i = 0; i < cap; ++i) {
        if (enemyTakeOneAction(b, self, brain) == AIAction::Done) break;
    }
    if (b.phase() != Phase::Finished) b.endTurn();
}

// Resolve the AI from $ATB_BRAIN (default: the beam search). Unknown name →
// nullptr after listing the choices, so the caller can fail loud.
const Brain* selectBrain() {
    const char* want = std::getenv("ATB_BRAIN");
    if (!want || !*want) return &defaultBrain();
    if (const Brain* b = brainByName(want)) return b;
    std::fprintf(stderr, "headless: unknown ATB_BRAIN='%s'; available:", want);
    for (std::string_view n : brainNames()) std::fprintf(stderr, " %.*s", (int)n.size(), n.data());
    std::fprintf(stderr, "\n");
    return nullptr;
}

void printAscii(const Battle& b) {
    const Grid& g = b.grid();
    for (int y = 0; y < g.height(); ++y) {
        for (int x = 0; x < g.width(); ++x) {
            Vec2i p{x, y};
            char c = '.';
            if (g.at(p) == TileType::Wall) c = '#';
            else if (g.at(p) == TileType::Obstacle) c = 'o';
            if (auto id = b.unitAt(p)) c = b.controlledByPlayer(*id) ? 'P' : 'E';
            std::putchar(c);
        }
        std::putchar('\n');
    }
}

const Entity& team(const Battle& b, Faction t) {
    for (const Entity& e : b.units())
        if (e.team == t) return e;
    return b.units().front();
}

} // namespace

int main() {
    const Brain* brain = selectBrain();
    if (!brain) return 1;

    SpellCatalog catalog = makeDefaultCatalog();
    BuildRules rules{};

    CharacterBuild pyro;
    pyro.name = "Pyromancer";
    pyro.stats.bonusAp = 1;
    pyro.spellIds = {spellid::Attack, spellid::Fireball, spellid::Poison};

    CharacterBuild bruiser;
    bruiser.name = "Bruiser";
    bruiser.stats.hpPurchases = 4;
    bruiser.stats.bonusMp = 1;
    bruiser.spellIds = {spellid::Attack, spellid::Knockback, spellid::Harpoon};

    ArenaConfig cfg;
    cfg.seed = 1337;
    Grid grid = generateArena(cfg);

    std::vector<Entity> roster;
    roster.push_back(instantiate(pyro, catalog, Faction::Player, cfg.playerSpawn, rules));
    roster.push_back(instantiate(bruiser, catalog, Faction::Enemy, cfg.enemySpawn, rules));
    Battle battle(std::move(grid), std::move(roster));

    std::printf("Pyromancer (player) vs Bruiser (enemy) — classless point-buy builds.\n");
    std::printf("AI brain: %.*s\n\n", (int)brain->name().size(), brain->name().data());
    printAscii(battle);

    int round = 0;
    while (battle.phase() != Phase::Finished && round < 100) {
        ++round;
        runTurn(battle, *brain);
        const Entity& p = team(battle, Faction::Player);
        const Entity& e = team(battle, Faction::Enemy);
        std::printf("Round %2d | %-10s HP %2d @ (%2d,%2d) | %-8s HP %2d @ (%2d,%2d)\n", round,
                    p.name.c_str(), p.hp, p.pos.x, p.pos.y, e.name.c_str(), e.hp, e.pos.x, e.pos.y);
    }

    auto w = battle.winner();
    std::printf("\nResult: %s after %d half-turns.\n",
                (w && *w == Faction::Player) ? "PLAYER WINS" : "ENEMY WINS", round);
    return 0;
}
