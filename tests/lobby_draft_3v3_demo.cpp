//
// lobby_draft_3v3_demo.cpp — the draft flow at teamSize 3 (the 2N plumbing at N≠2).
//
// Two parties of THREE draft in the 6-seat snake order. Seats lay out [P0,P1,P2,E0,E1,
// E2] = indices [0..5]; the snake pickOrder is [0,3,4,1,2,5] (A0,B0,B1,A1,A2,B2). Drives
// all six picks in that order (checking the scout reveals grow), then verifies the
// completed draft hands each of the six players the right faction + controller seat and
// both full 3-champion rosters. Complements the 2v2 suite: this is what would break if
// snakeOrder / the seat maps didn't generalize past N=2.
//
#include "core/Build.h"
#include "core/Creatures.h"
#include "core/Ruleset.h"
#include "core/Spells.h"
#include "net/AccountStore.h"
#include "net/GameServer.h" // contentHashOf
#include "net/Lobby.h"
#include "net/Socket.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace tb;
using namespace tb::net;

static int g_fails = 0;
#define CHECK(cond, msg)                                                                            \
    do {                                                                                            \
        if (cond) std::printf("  [PASS] %s\n", msg);                                                \
        else { std::printf("  [FAIL] %s\n", msg); ++g_fails; }                                      \
    } while (0)

namespace {
std::unique_ptr<LobbySession> login(uint16_t port, const std::string& hash, const char* user) {
    std::string e;
    return LobbySession::connect("127.0.0.1", port, hash, user, "pw", &e);
}
// Form a party of three: leader invites m1 and m2, each accepts (joins in order).
bool formTrio(LobbySession& leader, LobbySession& m1, LobbySession& m2) {
    std::string e;
    if (!leader.partyInvite(m1.account().user, &e)) return false;
    std::optional<std::vector<PartyInviteInfo>> i1 = m1.listPartyInvites();
    if (!i1 || i1->empty() || !m1.partyAccept((*i1)[0].id, &e)) return false;
    if (!leader.partyInvite(m2.account().user, &e)) return false;
    std::optional<std::vector<PartyInviteInfo>> i2 = m2.listPartyInvites();
    return i2 && !i2->empty() && m2.partyAccept((*i2)[0].id, &e);
}
CharacterBuild named(const char* name) {
    CharacterBuild b;
    b.name = name;
    b.spellIds = {spellid::Attack};
    return b;
}
std::optional<PairedInfo> pollPaired(LobbySession& s) {
    for (int i = 0; i < 25; ++i) {
        const LobbyEvent ev = s.poll();
        if (ev.kind == LobbyEvent::Kind::Paired) return ev.paired;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return std::nullopt;
}
// How many of the OTHER faction's seats are revealed (locked) in `ds`.
int scoutedFoes(const DraftState& ds, Faction mine) {
    int n = 0;
    for (const DraftSeatState& s : ds.seats)
        if (s.faction != mine && s.locked) ++n;
    return n;
}
} // namespace

int main() {
    const SpellCatalog catalog = makeDefaultCatalog();
    const std::vector<Entity> creatures = makeDefaultCreatures();
    const Ruleset ruleset = makeDefaultRuleset();

    const std::string dbPath = "tb_draft_3v3_test.json";
    std::remove(dbPath.c_str());
    AccountStore accounts(dbPath);

    LobbyConfig cfg;
    cfg.catalog = catalog;
    cfg.creatures = creatures;
    cfg.casualRules = ruleset;
    cfg.rankedRules = ruleset;
    cfg.contentHash = contentHashOf(catalog);
    cfg.accounts = &accounts;

    std::optional<Listener> listener = Listener::bind(0);
    const uint16_t port = listener->port();
    const std::string hash = cfg.contentHash;
    constexpr int kConns = 6; // six sessions (no match play — this test is the draft)
    std::thread lobby([&] { serveLobby(*listener, cfg, /*maxConns=*/kConns, /*readTimeoutSec=*/30); });

    MatchFormat fmt;
    fmt.time = MatchFormat::Time::PerMove;
    fmt.perMoveSec = 30;
    fmt.teamSize = 3;

    {
        // Party A (Player): alice=P0, bob=P1, cara=P2. Party B (Enemy): dave=E0, eve=E1, finn=E2.
        std::unique_ptr<LobbySession> alice = login(port, hash, "alice");
        std::unique_ptr<LobbySession> bob = login(port, hash, "bob");
        std::unique_ptr<LobbySession> cara = login(port, hash, "cara");
        std::unique_ptr<LobbySession> dave = login(port, hash, "dave");
        std::unique_ptr<LobbySession> eve = login(port, hash, "eve");
        std::unique_ptr<LobbySession> finn = login(port, hash, "finn");
        CHECK(alice && bob && cara && dave && eve && finn, "six players log in");
        CHECK(formTrio(*alice, *bob, *cara) && formTrio(*dave, *eve, *finn),
              "two parties of three form: [alice,bob,cara] and [dave,eve,finn]");

        std::string e;
        CHECK(alice->seek(fmt, &e), "alice posts the 3v3 team seek");
        std::optional<std::vector<SeekInfo>> seeks = dave->listSeeks();
        std::optional<DraftInfo> d = (seeks && !seeks->empty())
                                         ? dave->acceptSeekTeam((*seeks)[0].id, &e)
                                         : std::nullopt;
        CHECK(d.has_value(), "dave accepts as a team → a 3v3 draft opens");
        const int id = d ? d->id : 0;
        CHECK(d && d->seats.size() == 6, "the draft has 6 seats");
        const std::vector<int> wantOrder{0, 3, 4, 1, 2, 5}; // A0,B0,B1,A1,A2,B2
        CHECK(d && d->pickOrder == wantOrder, "snake pick order is [0,3,4,1,2,5]");

        // Drive the six picks in snake order, watching the scout reveals grow.
        std::printf("Six snake picks, with the scout board revealing each lock\n");
        const Faction P = Faction::Player, E = Faction::Enemy;
        CHECK(alice->draftLock(id, named("Alice"), &e).status == DraftLockResult::Status::Locked,
              "pick 1: alice (P0) locks");
        std::optional<DraftState> s1 = dave->draftPoll(id);
        CHECK(s1 && scoutedFoes(*s1, E) == 1, "dave (next) scouts 1 enemy so far");

        CHECK(dave->draftLock(id, named("Dave"), &e).status == DraftLockResult::Status::Locked,
              "pick 2: dave (E0) locks");
        CHECK(eve->draftLock(id, named("Eve"), &e).status == DraftLockResult::Status::Locked,
              "pick 3: eve (E1) locks");
        std::optional<DraftState> s3 = bob->draftPoll(id);
        CHECK(s3 && scoutedFoes(*s3, P) == 2, "bob (P1) now scouts 2 enemies (dave, eve)");

        CHECK(bob->draftLock(id, named("Bob"), &e).status == DraftLockResult::Status::Locked,
              "pick 4: bob (P1) locks");
        CHECK(cara->draftLock(id, named("Cara"), &e).status == DraftLockResult::Status::Locked,
              "pick 5: cara (P2) locks");
        std::optional<DraftState> s5 = finn->draftPoll(id);
        CHECK(s5 && scoutedFoes(*s5, E) == 3, "finn (last, E2) scouts all 3 enemies before picking");

        CHECK(finn->draftLock(id, named("Finn"), &e).status == DraftLockResult::Status::Complete,
              "pick 6: finn (E2) locks last → draft complete");

        std::printf("All six receive a team pairing with the right seat/controller map\n");
        std::optional<PairedInfo> ps[6] = {pollPaired(*alice), pollPaired(*bob), pollPaired(*cara),
                                           pollPaired(*dave),  pollPaired(*eve), pollPaired(*finn)};
        CHECK(ps[0] && ps[1] && ps[2] && ps[3] && ps[4] && ps[5], "all six learn of the pairing");
        CHECK(ps[0] && ps[0]->seat == P && ps[0]->controllerSeat == 0, "alice: Player, champ 0");
        CHECK(ps[1] && ps[1]->seat == P && ps[1]->controllerSeat == 1, "bob: Player, champ 1");
        CHECK(ps[2] && ps[2]->seat == P && ps[2]->controllerSeat == 2, "cara: Player, champ 2");
        CHECK(ps[3] && ps[3]->seat == E && ps[3]->controllerSeat == 0, "dave: Enemy, champ 0");
        CHECK(ps[4] && ps[4]->seat == E && ps[4]->controllerSeat == 1, "eve: Enemy, champ 1");
        CHECK(ps[5] && ps[5]->seat == E && ps[5]->controllerSeat == 2, "finn: Enemy, champ 2");
        CHECK(ps[0] && ps[0]->playerTeam.size() == 3 && ps[0]->enemyTeam.size() == 3 &&
                  ps[0]->playerTeam[2].name == "Cara" && ps[0]->enemyTeam[0].name == "Dave",
              "both 3-champion rosters ride along, by seat index");

        std::set<std::string> tokens;
        for (const std::optional<PairedInfo>& p : ps)
            if (p) tokens.insert(p->token);
        CHECK(tokens.size() == 6 && tokens.count("") == 0, "each of the 6 seats got a distinct token");
    }

    lobby.join();
    std::remove(dbPath.c_str());

    std::printf("\n%s (%d failure%s)\n", g_fails == 0 ? "ALL PASS" : "FAILURES", g_fails,
                g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
