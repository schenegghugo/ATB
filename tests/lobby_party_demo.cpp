//
// lobby_party_demo.cpp — Slice 1 of the team pre-game redesign: PARTY FORMATION.
//
// Over real sockets: players form a party (one side of a future NvN match). The
// leader invites; invitees see a polled invite board and accept to take a seat, in
// join order. Covers the refusals (non-leader invite, already-in-a-party, bad id),
// a member leaving, the leader disbanding, and disconnect cleanup (a departing
// leader disbands the party for everyone). No draft/pairing yet — that's slice 2+.
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
} // namespace

int main() {
    const SpellCatalog catalog = makeDefaultCatalog();
    const std::vector<Entity> creatures = makeDefaultCreatures();
    const Ruleset ruleset = makeDefaultRuleset();

    const std::string dbPath = "tb_party_test.json";
    std::remove(dbPath.c_str());
    AccountStore accounts(dbPath);

    LobbyConfig cfg;
    cfg.catalog = catalog;
    cfg.creatures = creatures;
    cfg.casualRules = ruleset;
    cfg.rankedRules = ruleset;
    cfg.contentHash = contentHashOf(catalog);
    cfg.accounts = &accounts;
    cfg.version = "0.2.0-test"; // echoed in the login hello for the client's version check

    std::optional<Listener> listener = Listener::bind(0);
    const uint16_t port = listener->port();
    const std::string hash = cfg.contentHash;
    // 4 sessions: alice, bob, carol (kept open), + dora (nested disconnect test).
    constexpr int kConns = 4;
    std::thread lobby([&] { serveLobby(*listener, cfg, /*maxConns=*/kConns, /*readTimeoutSec=*/30); });

    {
        std::unique_ptr<LobbySession> alice = login(port, hash, "alice", "pw-a");
        std::unique_ptr<LobbySession> bob = login(port, hash, "bob", "pw-b");
        std::unique_ptr<LobbySession> carol = login(port, hash, "carol", "pw-c");
        CHECK(alice && bob && carol, "three players log in");
        CHECK(alice && alice->serverVersion() == "0.2.0-test",
              "the login hello carries the server's build version (for the client's version check)");

        std::printf("Leader invites, invitees accept in join order\n");
        std::string e;
        CHECK(alice->partyInvite("bob", &e), "alice invites bob (auto-creates her party)");
        CHECK(alice->partyInvite("carol", &e), "alice invites carol");

        std::optional<std::vector<PartyInviteInfo>> bobInv = bob->listPartyInvites();
        CHECK(bobInv && bobInv->size() == 1 && (*bobInv)[0].from == "alice",
              "bob sees one invite from alice");
        CHECK(bobInv && !bobInv->empty() && (*bobInv)[0].members.size() == 1 &&
                  (*bobInv)[0].members[0] == "alice",
              "…showing the party roster he'd join (just alice)");

        CHECK(bobInv && bob->partyAccept((*bobInv)[0].id, &e), "bob accepts");
        std::optional<PartyInfo> ap = alice->partyInfo();
        CHECK(ap && ap->has() && ap->members.size() == 2 && ap->leader == "alice" &&
                  ap->members[1] == "bob",
              "alice's party is now [alice, bob], alice leading");

        std::optional<std::vector<PartyInviteInfo>> carolInv = carol->listPartyInvites();
        CHECK(carolInv && carolInv->size() == 1 && (*carolInv)[0].members.size() == 2,
              "carol's invite now shows the 2-member roster");
        CHECK(carolInv && carol->partyAccept((*carolInv)[0].id, &e), "carol accepts");
        ap = alice->partyInfo();
        CHECK(ap && ap->members.size() == 3 && ap->members[2] == "carol",
              "party is [alice, bob, carol]");

        std::printf("Refusals\n");
        std::string re;
        CHECK(!bob->partyInvite("alice", &re), "a non-leader (bob) can't invite");
        CHECK(re.find("leader") != std::string::npos, "…with a 'leader only' reason");
        re.clear();
        // carol is already in a party → inviting her is refused.
        CHECK(!alice->partyInvite("carol", &re), "can't invite someone already in a party");
        CHECK(re.find("already") != std::string::npos, "…with an 'already in a party' reason");
        CHECK(!carol->partyAccept(999999, &re), "accepting a bogus invite id fails");

        std::printf("A member leaves; then the leader disbands\n");
        CHECK(carol->partyLeave(), "carol leaves the party");
        ap = alice->partyInfo();
        CHECK(ap && ap->members.size() == 2 && ap->members[1] == "bob",
              "party drops back to [alice, bob]");
        std::optional<PartyInfo> cp = carol->partyInfo();
        CHECK(cp && !cp->has(), "carol is now party-free");

        CHECK(alice->partyLeave(), "alice (leader) leaves → disband");
        std::optional<PartyInfo> bp = bob->partyInfo();
        CHECK(bp && !bp->has(), "bob's party is disbanded when the leader leaves");

        std::printf("A leader who DISCONNECTS disbands the party\n");
        {
            std::unique_ptr<LobbySession> dora = login(port, hash, "dora", "pw-d");
            CHECK(dora && dora->partyInvite("bob", &e), "dora invites bob");
            std::optional<std::vector<PartyInviteInfo>> bi = bob->listPartyInvites();
            CHECK(bi && bi->size() == 1 && bob->partyAccept((*bi)[0].id, &e), "bob joins dora's party");
            std::optional<PartyInfo> dp = dora->partyInfo();
            CHECK(dp && dp->members.size() == 2, "party is [dora, bob]");
            // dora disconnects at scope end.
        }
        // Give the server a beat to process dora's disconnect (withdrawUser).
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::optional<PartyInfo> bp2 = bob->partyInfo();
        CHECK(bp2 && !bp2->has(), "bob is party-free after the leader disconnected");
    }

    lobby.join();
    std::remove(dbPath.c_str());

    std::printf("\n%s (%d failure%s)\n", g_fails == 0 ? "ALL PASS" : "FAILURES", g_fails,
                g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
