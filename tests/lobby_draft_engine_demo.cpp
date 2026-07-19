//
// lobby_draft_engine_demo.cpp — Slice 3: the SNAKE DRAFT ENGINE (pick loop).
//
// Two 2v2 parties draft one champion at a time in snake order (ann, cid, dan, bea for
// seats [P0,P1,E0,E1] → pickOrder [0,2,3,1]). Each lock REVEALS that build to everyone
// via draftPoll, so a later picker can scout and counter-build. Covers: withhold before
// lock, turn enforcement (only the current seat may lock), reveal-on-lock, illegal-build
// rejection, completion after the last pick, and per-pick timeout → auto-lock a minimal
// legal default. Turning a completed draft into a playable match is slice 4.
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
// Post a fresh 2v2 team seek from party A and accept it from party B → a draft id.
int openDraft(LobbySession& aLeader, LobbySession& bLeader, const MatchFormat& fmt) {
    std::string e;
    if (!aLeader.seek(fmt, &e)) return 0;
    std::optional<std::vector<SeekInfo>> seeks = bLeader.listSeeks();
    if (!seeks || seeks->empty()) return 0;
    std::optional<DraftInfo> d = bLeader.acceptSeekTeam((*seeks)[0].id, &e);
    return d ? d->id : 0;
}
// Is `seat` (0..3) locked in the polled state, and (optionally) with build name `name`?
bool lockedAs(const DraftState& ds, int seat, const char* name) {
    return seat < static_cast<int>(ds.seats.size()) && ds.seats[seat].locked &&
           ds.seats[seat].build.name == name;
}
} // namespace

int main() {
    const SpellCatalog catalog = makeDefaultCatalog();
    const std::vector<Entity> creatures = makeDefaultCreatures();
    const Ruleset ruleset = makeDefaultRuleset();

    const std::string dbPath = "tb_draft_engine_test.json";
    std::remove(dbPath.c_str());
    AccountStore accounts(dbPath);

    LobbyConfig cfg;
    cfg.catalog = catalog;
    cfg.creatures = creatures;
    cfg.casualRules = ruleset;
    cfg.rankedRules = ruleset;
    cfg.contentHash = contentHashOf(catalog);
    cfg.accounts = &accounts;
    cfg.draftPickSec = 3; // short enough to exercise a timeout without a long wait

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
        std::unique_ptr<LobbySession> ann = login(port, hash, "ann"); // seat 0 (P0), picks 1st
        std::unique_ptr<LobbySession> bea = login(port, hash, "bea"); // seat 1 (P1), picks 4th
        std::unique_ptr<LobbySession> cid = login(port, hash, "cid"); // seat 2 (E0), picks 2nd
        std::unique_ptr<LobbySession> dan = login(port, hash, "dan"); // seat 3 (E1), picks 3rd
        CHECK(ann && bea && cid && dan, "four players log in");
        CHECK(formPair(*ann, *bea) && formPair(*cid, *dan), "parties [ann,bea] and [cid,dan] form");

        const int id = openDraft(*ann, *cid, fmt);
        CHECK(id != 0, "a 2v2 draft opens (order: ann, cid, dan, bea)");

        std::printf("Withhold + turn enforcement before the first pick\n");
        std::optional<DraftState> st0 = cid->draftPoll(id);
        CHECK(st0 && st0->currentPick == 0 && !st0->complete, "draft starts at pick 0");
        CHECK(st0 && st0->seats.size() == 4 && !st0->seats[0].locked,
              "nothing is revealed yet (ann's seat is unlocked)");
        std::string e;
        DraftLockResult early = cid->draftLock(id, named("CidEarly"), &e);
        CHECK(early.status == DraftLockResult::Status::NotYourTurn,
              "cid can't lock out of turn (it's ann's pick)");

        std::printf("Pick 1: ann locks (illegal first, then legal) → revealed\n");
        CharacterBuild tooBig = named("AnnGreedy");
        tooBig.stats.hpPurchases = 9999; // blows the point budget
        DraftLockResult bad = ann->draftLock(id, tooBig, &e);
        CHECK(bad.status == DraftLockResult::Status::Rejected, "an over-budget build is rejected");
        DraftLockResult r1 = ann->draftLock(id, named("Ann"), &e);
        CHECK(r1.status == DraftLockResult::Status::Locked && r1.currentPick == 1,
              "ann locks a legal build → pick advances to 1");
        std::optional<DraftState> st1 = cid->draftPoll(id);
        CHECK(st1 && lockedAs(*st1, 0, "Ann"), "cid can now SCOUT ann's revealed build");

        std::printf("Picks 2-3: cid then dan counter-build with full info\n");
        // cid sees ann before locking (the whole point of the scout draft).
        DraftLockResult r2 = cid->draftLock(id, named("Cid"), &e);
        CHECK(r2.status == DraftLockResult::Status::Locked && r2.currentPick == 2, "cid locks (pick 2)");
        std::optional<DraftState> st2 = dan->draftPoll(id);
        CHECK(st2 && lockedAs(*st2, 0, "Ann") && lockedAs(*st2, 2, "Cid"),
              "dan sees BOTH ann and cid before picking");
        DraftLockResult r3 = dan->draftLock(id, named("Dan"), &e);
        CHECK(r3.status == DraftLockResult::Status::Locked && r3.currentPick == 3, "dan locks (pick 3)");

        std::printf("Pick 4: bea locks last → draft complete\n");
        std::optional<DraftState> st3 = bea->draftPoll(id);
        CHECK(st3 && lockedAs(*st3, 0, "Ann") && lockedAs(*st3, 2, "Cid") && lockedAs(*st3, 3, "Dan"),
              "bea (last) sees all three locked builds");
        DraftLockResult r4 = bea->draftLock(id, named("Bea"), &e);
        CHECK(r4.status == DraftLockResult::Status::Complete && r4.currentPick == 4,
              "bea's lock completes the draft");
        // Completion FINALIZES the draft into a team pairing (slice 4), consuming it —
        // the resulting PairedInfo is verified in lobby_draft_finalize_demo.
        CHECK(!ann->draftPoll(id).has_value(), "the completed draft is consumed (no longer pollable)");

        std::printf("A pick that runs out the clock auto-locks a default\n");
        const int id2 = openDraft(*ann, *cid, fmt); // fresh draft; ann picks first again
        CHECK(id2 != 0 && id2 != id, "a second draft opens");
        // Nobody locks. After the per-pick window elapses, a poll auto-locks ann's seat.
        std::this_thread::sleep_for(std::chrono::milliseconds(cfg.draftPickSec * 1000 + 400));
        std::optional<DraftState> to = cid->draftPoll(id2);
        CHECK(to && to->currentPick == 1 && to->seats[0].locked,
              "ann's timed-out pick auto-advanced to pick 1");
        CHECK(to && to->seats[0].build.spellIds.size() == 1 &&
                  to->seats[0].build.spellIds[0] == spellid::Attack,
              "…with the minimal legal default (a bare Attack)");
    }

    lobby.join();
    std::remove(dbPath.c_str());

    std::printf("\n%s (%d failure%s)\n", g_fails == 0 ? "ALL PASS" : "FAILURES", g_fails,
                g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
