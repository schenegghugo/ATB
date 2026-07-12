//
// commit_demo.cpp — CR.6 slice 2: the decoy-reveal commitment layer.
//
// Records real games whose openings are scripted around a decoy cast, then
// checks the verifier's commitment rules: an honest commitment verifies (both
// stay-"a" and swap-"b"); a reveal contradicting its commitment fails; letting a
// "b" commitment expire fails (the dodge-the-damage cheat); tampered hashes,
// missing and surplus commitments fail; casual records may skip commitments
// explicitly. CI smoke test.
//
#include "core/AI.h"
#include "core/Battle.h"
#include "core/Build.h"
#include "core/Creatures.h"
#include "core/Match.h"
#include "core/Ruleset.h"
#include "core/Spells.h"
#include "data/Net.h"
#include "net/MatchRunner.h"
#include "net/Replay.h"

#include <cstdio>
#include <optional>
#include <string>

using namespace tb;

static int g_fails = 0;
#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) std::printf("  [PASS] %s\n", msg);                                               \
        else { std::printf("  [FAIL] %s\n", msg); ++g_fails; }                                     \
    } while (0)

namespace {

constexpr EntityId P = 0, Twin = 2;
constexpr int kDecoySlot = 1; // P's build: {Attack, Decoy}

// Drives a match through the MatchRunner while recording every submitted intent
// — the resulting GameRecord is exactly what the wire would carry.
struct Scripted {
    net::MatchRunner runner;
    replay::GameRecord rec;

    Scripted(const Ruleset& r, const SpellCatalog& cat, const std::vector<Entity>& cre,
             unsigned seed, const CharacterBuild& player, const CharacterBuild& enemy)
        : runner(buildMatch(r, {player}, {enemy}, cat, seed, cre), net::Seat::Human,
                 net::Seat::Human) {
        rec.catalogHash = replay::catalogHash(cat);
        rec.rulesetHash = replay::rulesetHash(r);
        rec.seed = seed;
        rec.player = player;
        rec.enemy = enemy;
    }

    bool submit(const net::Intent& in) {
        const std::optional<Faction> seat = runner.awaitingSeat();
        if (!seat) return false;
        rec.intents.push_back(in);
        return runner.submit(*seat, in);
    }

    // End turns until `id` holds the turn (bounded).
    void toActive(EntityId id) {
        for (int i = 0; i < 64 && !runner.finished() && runner.battle().activeUnit() != id; ++i)
            submit(net::Intent::endTurn());
    }

    // First tile (deterministic scan order) the actor can legally cast `slot` at.
    [[nodiscard]] std::optional<Vec2i> castableTile(EntityId actor, int slot) const {
        const Grid& g = runner.battle().grid();
        for (int y = 0; y < g.height(); ++y)
            for (int x = 0; x < g.width(); ++x)
                if (runner.battle().canCast(actor, slot, {x, y})) return Vec2i{x, y};
        return std::nullopt;
    }

    // Cast `slot` from the active unit at the first legal tile. False if none.
    bool castFromActive(int slot) {
        const EntityId me = runner.battle().activeUnit();
        const std::optional<Vec2i> t = castableTile(me, slot);
        if (!t) return false;
        return submit(net::Intent::cast(slot, *t));
    }

    // Play the rest of the game with the Brain — but never let it cast Decoy
    // again (the scripted scenarios own the commitment count).
    void finishWithBrain() {
        for (int guard = 0; guard < 4000 && !runner.finished(); ++guard) {
            const std::optional<Faction> seat = runner.awaitingSeat();
            if (!seat) break;
            const EntityId me = runner.battle().activeUnit();
            for (const PlannedAction& a : defaultBrain().planTurn(runner.battle(), me)) {
                if (runner.finished()) break;
                if (a.kind == PlannedAction::Kind::Cast) {
                    const auto& spells = runner.battle().unit(me).spells;
                    if (a.slot >= 0 && a.slot < static_cast<int>(spells.size()) &&
                        spells[a.slot].name == "Decoy")
                        continue; // scripted scenarios control decoy casts
                    submit(net::Intent::cast(a.slot, a.target));
                } else {
                    submit(net::Intent::move(a.target));
                }
            }
            if (!runner.finished()) submit(net::Intent::endTurn());
        }
    }
};

replay::DecoyCommit commit(const std::string& choice, const std::string& nonce) {
    return {replay::makeCommitment(choice, nonce), choice, nonce};
}

} // namespace

int main() {
    const SpellCatalog catalog = makeDefaultCatalog();
    const std::vector<Entity> creatures = makeDefaultCreatures();
    const Ruleset ruleset = makeDefaultRuleset();

    CharacterBuild pb; // {Attack, Decoy} — slots 0, 1
    pb.name = "Feint";
    pb.spellIds = {spellid::Attack, spellid::Decoy};
    CharacterBuild eb;
    eb.name = "Basic";
    eb.stats.hpPurchases = 2;
    eb.spellIds = {spellid::Attack};

    auto scripted = [&](unsigned seed) { return Scripted(ruleset, catalog, creatures, seed, pb, eb); };

    // ---- Honest game, choice "a": cast decoy, act later from the original ----
    std::printf("Honest \"a\" (stay the original)\n");
    replay::GameRecord honestA;
    {
        Scripted s = scripted(9001);
        s.toActive(P);
        CHECK(s.castFromActive(kDecoySlot), "decoy cast is scripted in");
        s.submit(net::Intent::endTurn()); // spend no more AP this turn
        s.toActive(P);                    // wait a full rotation with the pair open
        CHECK(s.runner.battle().isCloaked(P), "the pair is open across the opponent's turn");
        CHECK(s.castFromActive(0), "the original acts — reveal \"a\"");
        CHECK(!s.runner.battle().isCloaked(P), "acting revealed the pair");
        s.finishWithBrain();
        s.rec.commits.push_back(commit("a", "n0nce-A"));
        honestA = s.rec;

        replay::VerifyResult v = replay::verify(honestA, ruleset, catalog, creatures);
        if (!v.ok) std::printf("         · %s\n", v.error.c_str());
        CHECK(v.ok, "an honest \"a\" commitment verifies");
    }

    std::printf("The notation carries commitments round-trip\n");
    {
        const std::string wire = replay::serializeRecord(honestA);
        CHECK(wire.find(" #") != std::string::npos, "commit token present in the notation");
        replay::RecordParse p = replay::parseRecord(wire);
        CHECK(p.ok && p.record.commits.size() == 1 && p.record.commits[0].choice == "a",
              "commitment parses back");
        CHECK(p.ok && replay::serializeRecord(p.record) == wire, "round-trip is byte-identical");
        CHECK(p.ok && replay::verify(p.record, ruleset, catalog, creatures).ok,
              "the parsed record still verifies");
    }

    std::printf("Cheats against the honest record are rejected\n");
    {
        replay::GameRecord lied = honestA; // acted from "a" but committed "b"
        lied.commits[0] = commit("b", "n0nce-B");
        replay::VerifyResult v = replay::verify(lied, ruleset, catalog, creatures);
        CHECK(!v.ok && v.error.find("contradicts") != std::string::npos,
              "a reveal contradicting its commitment is rejected");

        replay::GameRecord badHash = honestA;
        badHash.commits[0].commit = std::string(64, 'e'); // not hash(choice:nonce)
        v = replay::verify(badHash, ruleset, catalog, creatures);
        CHECK(!v.ok && v.error.find("hash") != std::string::npos,
              "a commitment failing its hash is rejected");

        replay::GameRecord missing = honestA;
        missing.commits.clear();
        v = replay::verify(missing, ruleset, catalog, creatures);
        CHECK(!v.ok && v.error.find("no commitment") != std::string::npos,
              "a decoy cast without a commitment is rejected (ranked default)");
        CHECK(replay::verify(missing, ruleset, catalog, creatures, /*require=*/false).ok,
              "…but a casual replay may skip commitments explicitly");

        replay::GameRecord surplus = honestA;
        surplus.commits.push_back(commit("a", "extra"));
        v = replay::verify(surplus, ruleset, catalog, creatures);
        CHECK(!v.ok && v.error.find("more commitments") != std::string::npos,
              "surplus commitments are rejected");
    }

    // ---- Identity swap, choice "b": act from the twin ------------------------
    std::printf("Identity swap \"b\" (become the twin)\n");
    {
        Scripted s = scripted(9004);
        s.toActive(P);
        CHECK(s.castFromActive(kDecoySlot), "decoy cast is scripted in");
        s.toActive(Twin);
        CHECK(s.runner.battle().activeUnit() == Twin && s.runner.battle().isCloaked(Twin),
              "the twin holds a turn while cloaked");
        CHECK(s.castFromActive(0), "the twin acts — reveal \"b\"");
        s.finishWithBrain();

        replay::GameRecord swapped = s.rec;
        swapped.commits.push_back(commit("b", "sw4p"));
        CHECK(replay::verify(swapped, ruleset, catalog, creatures).ok,
              "an honest \"b\" commitment verifies");

        replay::GameRecord lied = s.rec;
        lied.commits.push_back(commit("a", "sw4p"));
        replay::VerifyResult v = replay::verify(lied, ruleset, catalog, creatures);
        CHECK(!v.ok && v.error.find("contradicts") != std::string::npos,
              "committing \"a\" then acting from the twin is rejected");
    }

    // ---- Expiry: reveals the original by rule --------------------------------
    std::printf("Expiry (never acted)\n");
    {
        Scripted s = scripted(9003);
        s.toActive(P);
        CHECK(s.castFromActive(kDecoySlot), "decoy cast is scripted in");
        for (int i = 0; i < 60 && !s.runner.battle().cloakPairs().empty() && !s.runner.finished(); ++i)
            s.submit(net::Intent::endTurn());
        CHECK(s.runner.battle().cloakPairs().empty(), "the pair expired unrevealed");
        s.finishWithBrain();

        replay::GameRecord stayed = s.rec;
        stayed.commits.push_back(commit("a", "st4y"));
        CHECK(replay::verify(stayed, ruleset, catalog, creatures).ok,
              "committing \"a\" and letting it expire verifies (expiry = original)");

        replay::GameRecord chicken = s.rec;
        chicken.commits.push_back(commit("b", "st4y"));
        replay::VerifyResult v = replay::verify(chicken, ruleset, catalog, creatures);
        CHECK(!v.ok && v.error.find("contradicts") != std::string::npos,
              "committing \"b\" then chickening out into expiry is rejected");
    }

    std::printf("\n%s (%d failure%s)\n", g_fails == 0 ? "ALL PASS" : "FAILURES", g_fails,
                g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
