//
// lobby_draft_finalize_demo.cpp — Slice 4: a completed draft → a live TEAM pairing.
//
// After the last pick of a 2v2 draft, the server mints a join token per seat and hands
// every player a PairedInfo carrying: their faction, which champion they PILOT
// (controllerSeat, orthogonal to faction), a unique token, and both sides' full rosters
// by seat index. This is the seat/controller model the live match (slice 5) consumes.
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
bool formPair(LobbySession& leader, LobbySession& mate) {
    std::string e;
    if (!leader.partyInvite(mate.account().user, &e)) return false;
    std::optional<std::vector<PartyInviteInfo>> inv = mate.listPartyInvites();
    return inv && !inv->empty() && mate.partyAccept((*inv)[0].id, &e);
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
// The rosters are the same for every recipient: playerTeam [Ann,Bea], enemyTeam [Cid,Dan].
bool rostersOk(const PairedInfo& p) {
    return p.live && p.playerTeam.size() == 2 && p.enemyTeam.size() == 2 &&
           p.playerTeam[0].name == "Ann" && p.playerTeam[1].name == "Bea" &&
           p.enemyTeam[0].name == "Cid" && p.enemyTeam[1].name == "Dan";
}
} // namespace

int main() {
    const SpellCatalog catalog = makeDefaultCatalog();
    const std::vector<Entity> creatures = makeDefaultCreatures();
    const Ruleset ruleset = makeDefaultRuleset();

    const std::string dbPath = "tb_draft_finalize_test.json";
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
    constexpr int kConns = 4;
    std::thread lobby([&] { serveLobby(*listener, cfg, /*maxConns=*/kConns, /*readTimeoutSec=*/30); });

    MatchFormat fmt;
    fmt.time = MatchFormat::Time::PerMove;
    fmt.perMoveSec = 30;
    fmt.teamSize = 2;

    {
        std::unique_ptr<LobbySession> ann = login(port, hash, "ann"); // P0
        std::unique_ptr<LobbySession> bea = login(port, hash, "bea"); // P1
        std::unique_ptr<LobbySession> cid = login(port, hash, "cid"); // E0
        std::unique_ptr<LobbySession> dan = login(port, hash, "dan"); // E1
        CHECK(ann && bea && cid && dan && formPair(*ann, *bea) && formPair(*cid, *dan),
              "four players log in and form two parties");

        std::string e;
        CHECK(ann->seek(fmt, &e), "ann posts the 2v2 seek");
        std::optional<std::vector<SeekInfo>> seeks = cid->listSeeks();
        std::optional<DraftInfo> d = (seeks && !seeks->empty())
                                         ? cid->acceptSeekTeam((*seeks)[0].id, &e)
                                         : std::nullopt;
        CHECK(d.has_value(), "cid accepts as a team → a draft opens");
        const int id = d ? d->id : 0;

        // Drive the snake picks: ann, cid, dan, bea.
        CHECK(ann->draftLock(id, named("Ann"), &e).status == DraftLockResult::Status::Locked, "ann locks");
        CHECK(cid->draftLock(id, named("Cid"), &e).status == DraftLockResult::Status::Locked, "cid locks");
        CHECK(dan->draftLock(id, named("Dan"), &e).status == DraftLockResult::Status::Locked, "dan locks");
        CHECK(bea->draftLock(id, named("Bea"), &e).status == DraftLockResult::Status::Complete,
              "bea's lock completes the draft");

        std::printf("Every player receives a team pairing (token + faction + controller seat)\n");
        std::optional<PairedInfo> pa = pollPaired(*ann);
        std::optional<PairedInfo> pb = pollPaired(*bea);
        std::optional<PairedInfo> pc = pollPaired(*cid);
        std::optional<PairedInfo> pd = pollPaired(*dan);
        CHECK(pa && pb && pc && pd, "all four learn of the pairing via poll");

        CHECK(pa && pa->seat == Faction::Player && pa->controllerSeat == 0, "ann: Player, pilots champ 0");
        CHECK(pb && pb->seat == Faction::Player && pb->controllerSeat == 1, "bea: Player, pilots champ 1");
        CHECK(pc && pc->seat == Faction::Enemy && pc->controllerSeat == 0, "cid: Enemy, pilots champ 0");
        CHECK(pd && pd->seat == Faction::Enemy && pd->controllerSeat == 1, "dan: Enemy, pilots champ 1");

        CHECK(pa && rostersOk(*pa) && pd && rostersOk(*pd),
              "both sides' full rosters ride along, by seat index");

        std::set<std::string> tokens;
        for (const std::optional<PairedInfo>* p : {&pa, &pb, &pc, &pd})
            if (*p) tokens.insert((*p)->token);
        CHECK(tokens.size() == 4 && tokens.count("") == 0, "each seat got a distinct, non-empty token");

        std::printf("The draft is consumed (no longer pollable)\n");
        CHECK(!ann->draftPoll(id).has_value(), "the completed draft is gone from the draft board");
    }

    lobby.join();
    std::remove(dbPath.c_str());

    std::printf("\n%s (%d failure%s)\n", g_fails == 0 ? "ALL PASS" : "FAILURES", g_fails,
                g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
