//
// brain_arena.cpp — head-to-head Brain A/B over a spread of deterministic
// boards (75 loadout/position configs, each played twice with sides swapped so
// first-mover advantage cancels). This is the promotion gate for AI work: a
// candidate Brain must beat the incumbent >~55% of decisive games (3.5.6 /
// H.4 in MILESTONES.md). Also double-runs one config to confirm determinism.
//
//   tb_brain_arena                # default card: deep/adaptive/scout vs beam
//   tb_brain_arena <A> <B>        # any two registered brain names
//
// Not CI-gated (a full card runs ~20s); run it when touching core/AI.cpp,
// core/Evaluator.cpp or core/Intel.cpp.
//
#include "core/AI.h"
#include "core/Battle.h"
#include "core/Spells.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace tb;

namespace {

SpellCatalog catalog = makeDefaultCatalog();

Entity makeUnit(Faction team, Vec2i pos, const std::vector<int>& ids, int hp, int mp, int ap,
                int init) {
    Entity e;
    e.name = team == Faction::Player ? "A" : "B";
    e.team = team;
    e.pos = pos;
    e.maxHp = e.hp = hp;
    e.maxAp = e.ap = ap;
    e.maxMp = e.mp = mp;
    e.initiative = init;
    for (int id : ids)
        if (const SpellDef* d = catalog.find(id)) e.spells.push_back(d->spell);
    return e;
}

struct Config {
    std::vector<int> loadA, loadB;
    Vec2i posA, posB;
};

// A spread of loadouts/positions: symmetric mirrors and asymmetric matchups.
std::vector<Config> makeConfigs() {
    using S = std::vector<int>;
    const S bruiser{spellid::Attack, spellid::Fireball, spellid::Bulwark};
    const S artillery{spellid::Fireball, spellid::Harpoon, spellid::Blind};
    const S trickster{spellid::Attack, spellid::Poison, spellid::Invisible, spellid::Flux};
    const S control{spellid::Attack, spellid::Knockback, spellid::Shelter, spellid::Surge};
    const S healerly{spellid::Attack, spellid::Mend, spellid::Bulwark, spellid::Blind};
    std::vector<Config> cfgs;
    const std::vector<S> loads{bruiser, artillery, trickster, control, healerly};
    const std::vector<std::pair<Vec2i, Vec2i>> spots{
        {{2, 2}, {13, 2}}, {{1, 1}, {14, 3}}, {{4, 2}, {11, 2}}};
    for (std::size_t i = 0; i < loads.size(); ++i)
        for (std::size_t j = 0; j < loads.size(); ++j)
            for (const auto& [pa, pb] : spots)
                cfgs.push_back({loads[i], loads[j], pa, pb});
    return cfgs; // 5*5*3 = 75 boards, x2 sides = 150 games per pairing
}

// Plays one match, brainP driving Player units and brainE driving Enemy units.
// Returns +1 (Player wins), -1 (Enemy wins), 0 (draw / turn-cap).
int playMatch(const Config& cfg, const Brain& brainP, const Brain& brainE,
              std::string* transcript = nullptr) {
    Grid g(16, 5);
    std::vector<Entity> r;
    r.push_back(makeUnit(Faction::Player, cfg.posA, cfg.loadA, 60, 4, 8, 10));
    r.push_back(makeUnit(Faction::Enemy, cfg.posB, cfg.loadB, 60, 4, 8, 5));
    Battle b(std::move(g), std::move(r), StormConfig{true, 5, 8});
    for (int turn = 0; turn < 200 && b.phase() != Phase::Finished; ++turn) {
        const Entity& u = b.unit(b.activeUnit());
        runEnemyTurn(b, /*autoEndTurn=*/true, u.team == Faction::Player ? brainP : brainE);
        if (transcript) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%d:%d,%d/%d;", turn, u.pos.x, u.pos.y, u.hp);
            *transcript += buf;
        }
    }
    auto w = b.winner();
    if (!w) return 0;
    return *w == Faction::Player ? 1 : -1;
}

void duel(const char* nameA, const char* nameB) {
    const Brain* A = brainByName(nameA);
    const Brain* B = brainByName(nameB);
    if (!A || !B) { std::printf("unknown brain\n"); return; }
    int winA = 0, winB = 0, draw = 0;
    for (const Config& cfg : makeConfigs()) {
        // A as Player, B as Enemy:
        int r1 = playMatch(cfg, *A, *B);
        // sides swapped (same board, B drives Player):
        int r2 = playMatch(cfg, *B, *A);
        winA += (r1 == 1) + (r2 == -1);
        winB += (r1 == -1) + (r2 == 1);
        draw += (r1 == 0) + (r2 == 0);
    }
    const int n = winA + winB + draw;
    std::printf("%-8s vs %-8s : %3d - %3d (%d draws)  -> %s %.1f%% of decisive\n", nameA, nameB,
                winA, winB, draw, winA >= winB ? nameA : nameB,
                100.0 * (winA >= winB ? winA : winB) / (winA + winB ? winA + winB : 1));
    (void)n;
}

} // namespace

int main(int argc, char** argv) {
    // Determinism spot-check: same config, same pairing, twice.
    {
        std::string t1, t2;
        const Config cfg = makeConfigs()[7];
        playMatch(cfg, *brainByName("deep"), *brainByName("adaptive"), &t1);
        playMatch(cfg, *brainByName("deep"), *brainByName("adaptive"), &t2);
        std::printf("determinism: %s\n", t1 == t2 ? "OK (identical transcripts)" : "FAIL");
    }
    if (argc > 2) { duel(argv[1], argv[2]); return 0; }
    duel("deep", "beam");
    duel("adaptive", "beam");
    duel("adaptive", "deep");
    duel("scout", "beam");
    return 0;
}
