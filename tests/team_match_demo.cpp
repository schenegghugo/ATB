//
// team_match_demo.cpp — Slice 5: a live 2v2 played over the network to a finish.
//
// The whole team pre-game + match, end to end: two parties draft (party → team seek →
// snake picks), then all FOUR players join the resulting team pairing and play the 2v2
// to completion. The match runs one authoritative Battle with turns routed to the pilot
// of the ACTIVE champion (not just its faction) — verified by each mirror only acting
// (awaitingMe) when ITS champion holds the turn. A default Brain drives every seat.
//
#include "core/AI.h"
#include "core/Build.h"
#include "core/Creatures.h"
#include "core/Ruleset.h"
#include "core/Spells.h"
#include "net/AccountStore.h"
#include "net/GameServer.h" // contentHashOf
#include "net/Lobby.h"
#include "net/MirrorSession.h"
#include "net/Socket.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
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
    b.stats.hpPurchases = 3;
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

// Drive four mirrors of one live 2v2 to a finish. At any instant exactly one champion is
// active, so exactly one mirror reports awaitingMe(); its Brain acts, then all pump the
// authoritative echo to stay in lockstep.
bool playTeam(std::vector<std::unique_ptr<MirrorSession>>& ms) {
    auto allDone = [&] {
        for (const std::unique_ptr<MirrorSession>& m : ms)
            if (!m->finished()) return false;
        return true;
    };
    for (int guard = 0; guard < 400000 && !allDone(); ++guard) {
        for (std::unique_ptr<MirrorSession>& m : ms) m->pump(16);
        MirrorSession* mv = nullptr;
        int actors = 0;
        for (std::unique_ptr<MirrorSession>& m : ms)
            if (m->awaitingMe()) { ++actors; if (!mv) mv = m.get(); }
        if (actors > 1) { std::printf("         · two pilots awaited at once!\n"); return false; }
        if (!mv) continue;
        const EntityId me = mv->battle().activeUnit();
        const std::vector<PlannedAction> plan = defaultBrain().planTurn(mv->battle(), me);
        if (plan.empty()) {
            mv->send(net::Intent::endTurn());
        } else {
            const PlannedAction& act = plan.front();
            mv->send(act.kind == PlannedAction::Kind::Cast ? net::Intent::cast(act.slot, act.target)
                                                           : net::Intent::move(act.target));
        }
        for (std::unique_ptr<MirrorSession>& m : ms) m->pump(2000);
    }
    return allDone();
}
} // namespace

int main() {
    const SpellCatalog catalog = makeDefaultCatalog();
    const std::vector<Entity> creatures = makeDefaultCreatures();
    const Ruleset ruleset = makeDefaultRuleset();

    const std::string dbPath = "tb_team_match_test.json";
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
    // 4 lobby sessions + 4 team-match join conns = 8.
    constexpr int kConns = 8;
    std::thread lobby([&] { serveLobby(*listener, cfg, /*maxConns=*/kConns, /*readTimeoutSec=*/60); });

    MatchFormat fmt;
    fmt.time = MatchFormat::Time::PerMove;
    fmt.perMoveSec = 30;
    fmt.teamSize = 2;

    {
        std::unique_ptr<LobbySession> ann = login(port, hash, "ann");
        std::unique_ptr<LobbySession> bea = login(port, hash, "bea");
        std::unique_ptr<LobbySession> cid = login(port, hash, "cid");
        std::unique_ptr<LobbySession> dan = login(port, hash, "dan");
        CHECK(ann && bea && cid && dan && formPair(*ann, *bea) && formPair(*cid, *dan),
              "four players log in and form two parties");

        std::string e;
        ann->seek(fmt, &e);
        std::optional<std::vector<SeekInfo>> seeks = cid->listSeeks();
        std::optional<DraftInfo> d = (seeks && !seeks->empty())
                                         ? cid->acceptSeekTeam((*seeks)[0].id, &e)
                                         : std::nullopt;
        CHECK(d.has_value(), "team seek → draft opens");
        const int id = d ? d->id : 0;
        // Snake picks: ann, cid, dan, bea.
        (void)ann->draftLock(id, named("Ann"), &e);
        (void)cid->draftLock(id, named("Cid"), &e);
        (void)dan->draftLock(id, named("Dan"), &e);
        const DraftLockResult last = bea->draftLock(id, named("Bea"), &e);
        CHECK(last.status == DraftLockResult::Status::Complete, "all four lock → draft complete");

        std::optional<PairedInfo> pa = pollPaired(*ann);
        std::optional<PairedInfo> pb = pollPaired(*bea);
        std::optional<PairedInfo> pc = pollPaired(*cid);
        std::optional<PairedInfo> pd = pollPaired(*dan);
        CHECK(pa && pb && pc && pd, "all four receive a team pairing with a token");

        std::printf("All four join their tokens and play the 2v2 to a finish\n");
        // The server sends welcomeTeam only once all 2N match conns arrive, so join
        // concurrently (a sequential join would deadlock waiting on a welcome).
        std::vector<std::unique_ptr<MirrorSession>> ms(4);
        const std::string toks[4] = {pa ? pa->token : "", pb ? pb->token : "", pc ? pc->token : "",
                                     pd ? pd->token : ""};
        std::vector<std::thread> joins;
        for (int i = 0; i < 4; ++i)
            joins.emplace_back([&, i] {
                std::string je;
                ms[i] = MirrorSession::joinToken("127.0.0.1", port, toks[i], ruleset, catalog,
                                                 creatures, &je);
            });
        for (std::thread& t : joins) t.join();
        CHECK(ms[0] && ms[1] && ms[2] && ms[3], "all four mirrors join the match");

        // Each mirror pilots a distinct champion of its faction.
        CHECK(ms[0] && ms[0]->seat() == Faction::Player && ms[0]->controllerSeat() == 0,
              "ann mirrors Player champion 0");
        CHECK(ms[3] && ms[3]->seat() == Faction::Enemy && ms[3]->controllerSeat() == 1,
              "dan mirrors Enemy champion 1");

        const bool finished = ms[0] && ms[1] && ms[2] && ms[3] && playTeam(ms);
        CHECK(finished, "the 2v2 plays to a finish over the network, in lockstep");
        if (finished)
            CHECK(ms[0]->battle().winner().has_value() || ms[0]->forfeitWinner().has_value(),
                  "the match resolved to a winning team");
    }

    lobby.join();
    std::remove(dbPath.c_str());

    std::printf("\n%s (%d failure%s)\n", g_fails == 0 ? "ALL PASS" : "FAILURES", g_fails,
                g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
