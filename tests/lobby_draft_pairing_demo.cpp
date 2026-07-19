//
// lobby_draft_pairing_demo.cpp — Slice 2: TEAM seek/challenge → a DRAFT.
//
// Two full parties (2v2) pair into a draft instead of a ready check. A party leader
// posts a team seek (or directed challenge); the other leader accepts AS A TEAM. All
// four players receive the same DraftInfo — a 4-seat map across two factions plus the
// snake pick order (A0, B0, B1, A1) — each stamped with their own seat. Also covers
// the gates (need a full party of the right size; only the leader can accept) and
// disconnect cancellation. The pick/lock/reveal loop is slice 3.
//
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
std::unique_ptr<LobbySession> login(uint16_t port, const std::string& hash, const char* user,
                                    const char* pass) {
    std::string e;
    return LobbySession::connect("127.0.0.1", port, hash, user, pass, &e);
}

// Form a party led by `leader` with `mate` as its second member.
bool formPair(LobbySession& leader, LobbySession& mate) {
    std::string e;
    if (!leader.partyInvite(mate.account().user, &e)) return false;
    std::optional<std::vector<PartyInviteInfo>> inv = mate.listPartyInvites();
    if (!inv || inv->empty()) return false;
    return mate.partyAccept((*inv)[0].id, &e);
}

// A DraftInfo is well-formed for a 2v2: 4 seats (2 Player, 2 Enemy) with the four
// expected users, and the snake order [A0, B0, B1, A1] = seat indices [0, 2, 3, 1].
bool draftShapeOk(const DraftInfo& d, const std::vector<std::string>& teamA,
                  const std::vector<std::string>& teamB) {
    if (d.seats.size() != 4) return false;
    if (d.seats[0].faction != Faction::Player || d.seats[0].user != teamA[0]) return false;
    if (d.seats[1].faction != Faction::Player || d.seats[1].user != teamA[1]) return false;
    if (d.seats[2].faction != Faction::Enemy || d.seats[2].user != teamB[0]) return false;
    if (d.seats[3].faction != Faction::Enemy || d.seats[3].user != teamB[1]) return false;
    const std::vector<int> want{0, 2, 3, 1};
    return d.pickOrder == want;
}
} // namespace

int main() {
    const SpellCatalog catalog = makeDefaultCatalog();
    const std::vector<Entity> creatures = makeDefaultCreatures();
    const Ruleset ruleset = makeDefaultRuleset();

    const std::string dbPath = "tb_draft_pairing_test.json";
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
    // 4 sessions (ann, bea, cid, dan), all kept open for the whole run.
    constexpr int kConns = 4;
    std::thread lobby([&] { serveLobby(*listener, cfg, /*maxConns=*/kConns, /*readTimeoutSec=*/30); });

    MatchFormat fmt2v2;
    fmt2v2.time = MatchFormat::Time::PerMove;
    fmt2v2.perMoveSec = 30;
    fmt2v2.teamSize = 2;

    {
        std::unique_ptr<LobbySession> ann = login(port, hash, "ann", "pw");   // team A leader
        std::unique_ptr<LobbySession> bea = login(port, hash, "bea", "pw");   // team A mate
        std::unique_ptr<LobbySession> cid = login(port, hash, "cid", "pw");   // team B leader
        std::unique_ptr<LobbySession> dan = login(port, hash, "dan", "pw");   // team B mate
        CHECK(ann && bea && cid && dan, "four players log in");

        std::printf("A 2v2 team seek needs a full party\n");
        std::string e;
        CHECK(!ann->seek(fmt2v2, &e), "ann can't post a 2v2 seek with no party");
        CHECK(e.find("party") != std::string::npos, "…told she needs a party");

        CHECK(formPair(*ann, *bea), "party A = [ann, bea]");
        CHECK(formPair(*cid, *dan), "party B = [cid, dan]");

        std::printf("Team seek → accepted as a team → a draft for all four\n");
        CHECK(ann->seek(fmt2v2, &e), "ann (leader) posts the 2v2 seek");
        std::optional<std::vector<SeekInfo>> seeks = cid->listSeeks();
        CHECK(seeks && seeks->size() == 1 && (*seeks)[0].format.teamSize == 2 &&
                  (*seeks)[0].team.size() == 2,
              "cid sees the team seek with its 2-member roster");

        // A lone player can't accept a team seek.
        std::string se;
        CHECK(!bea->acceptSeekTeam(seeks ? (*seeks)[0].id : 0, &se).has_value(),
              "a non-leader / partyless player can't accept a team seek");

        std::optional<DraftInfo> cidDraft = cid->acceptSeekTeam((*seeks)[0].id, &e);
        const std::vector<std::string> tA{"ann", "bea"}, tB{"cid", "dan"};
        CHECK(cidDraft && draftShapeOk(*cidDraft, tA, tB),
              "cid (acceptor) gets the draft: 4 seats, 2 factions, snake order");
        CHECK(cidDraft && cidDraft->seats[cidDraft->mySeat].user == "cid",
              "…stamped with cid's own seat (Enemy 0)");

        // The other three learn via poll, each stamped with their own seat.
        auto pollDraft = [&](LobbySession& s) -> std::optional<DraftInfo> {
            for (int i = 0; i < 20; ++i) {
                const LobbyEvent ev = s.poll();
                if (ev.kind == LobbyEvent::Kind::Draft) return ev.draft;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            return std::nullopt;
        };
        std::optional<DraftInfo> annDraft = pollDraft(*ann);
        std::optional<DraftInfo> beaDraft = pollDraft(*bea);
        std::optional<DraftInfo> danDraft = pollDraft(*dan);
        CHECK(annDraft && draftShapeOk(*annDraft, tA, tB) &&
                  annDraft->seats[annDraft->mySeat].user == "ann",
              "ann learns via poll, seat Player 0");
        CHECK(beaDraft && beaDraft->seats.size() == 4 &&
                  beaDraft->seats[beaDraft->mySeat].user == "bea",
              "bea learns via poll, seat Player 1");
        CHECK(danDraft && danDraft->seats[danDraft->mySeat].user == "dan",
              "dan learns via poll, seat Enemy 1");
        CHECK(annDraft && cidDraft && annDraft->id == cidDraft->id,
              "all four share one draft id");

        std::printf("A team challenge routes to a draft too\n");
        CHECK(ann->challenge("cid", fmt2v2, &e), "ann's party challenges cid");
        std::optional<std::vector<ChallengeInfo>> inc = cid->listChallenges();
        CHECK(inc && inc->size() == 1 && (*inc)[0].team.size() == 2,
              "cid sees the team challenge + roster");
        std::optional<DraftInfo> cidDraft2 = inc ? cid->acceptChallengeTeam((*inc)[0].id, &e)
                                                 : std::nullopt;
        CHECK(cidDraft2 && draftShapeOk(*cidDraft2, tA, tB), "accepting the challenge opens a draft");
        // Drain the poll events this second draft queued for the others.
        (void)pollDraft(*ann);
        (void)pollDraft(*bea);
        (void)pollDraft(*dan);

        std::printf("A disconnect during a pending draft cancels it for everyone\n");
        CHECK(ann->seek(fmt2v2, &e), "ann re-posts the seek");
        std::optional<std::vector<SeekInfo>> s2 = cid->listSeeks();
        std::optional<DraftInfo> d3 = (s2 && !s2->empty()) ? cid->acceptSeekTeam((*s2)[0].id, &e)
                                                           : std::nullopt;
        CHECK(d3.has_value(), "a fresh draft opens");
        // Drain the draft poll events first (cid got its draft as the accept reply): a
        // single poll() coalesces a batch and a Draft outranks a Cancelled within it, so
        // the cancels must arrive in a later, draft-free batch.
        (void)pollDraft(*ann);
        (void)pollDraft(*bea);
        dan.reset(); // dan disconnects mid-draft
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        bool annCancelled = false;
        for (int i = 0; i < 20 && !annCancelled; ++i) {
            const LobbyEvent ev = ann->poll();
            if (ev.kind == LobbyEvent::Kind::Cancelled) annCancelled = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        CHECK(annCancelled, "ann is told the draft was cancelled when dan dropped");
    }

    lobby.join();
    std::remove(dbPath.c_str());

    std::printf("\n%s (%d failure%s)\n", g_fails == 0 ? "ALL PASS" : "FAILURES", g_fails,
                g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
