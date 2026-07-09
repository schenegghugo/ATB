//
// lobby_chat_demo.cpp — Phase 4.6: lobby-wide chat + correspondence-game chat,
// with the safety levers (length cap, per-user rate limit, operator mute).
//
// Lobby chat is a capped rolling log every session can poll from an absolute
// cursor. Correspondence chat rides a per-game SIDE log (participants only), so
// the move log the verifier replays stays pure. CI smoke test.
//
#include "core/Build.h"
#include "core/Creatures.h"
#include "core/Ruleset.h"
#include "core/Spells.h"
#include "net/AccountStore.h"
#include "net/GameServer.h" // contentHashOf
#include "net/Lobby.h"
#include "net/Socket.h"

#include "lobby_test_util.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace tb;
using namespace tb::net;
using tbtest::makeBuild;
using tbtest::readyUp;

static int g_fails = 0;
#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) std::printf("  [PASS] %s\n", msg);                                               \
        else { std::printf("  [FAIL] %s\n", msg); ++g_fails; }                                     \
    } while (0)

namespace {
void waitRateWindow() { std::this_thread::sleep_for(std::chrono::milliseconds(600)); }
} // namespace

int main() {
    const SpellCatalog catalog = makeDefaultCatalog();
    const std::vector<Entity> creatures = makeDefaultCreatures();
    const Ruleset ruleset = makeDefaultRuleset();

    const std::string dbPath = "tb_lobby_chat_test.json";
    std::remove(dbPath.c_str());
    AccountStore accounts(dbPath);

    LobbyConfig cfg;
    cfg.catalog = catalog;
    cfg.creatures = creatures;
    cfg.casualRules = ruleset;
    cfg.rankedRules = ruleset;
    cfg.contentHash = contentHashOf(catalog);
    cfg.accounts = &accounts;
    cfg.chatMaxLen = 200;
    cfg.chatMinIntervalSec = 0.5f; // fast test cadence
    cfg.chatMuted = {"mallory"};

    std::optional<Listener> listener = Listener::bind(0);
    const uint16_t port = listener->port();
    constexpr int kConns = 3; // 3 sessions, no live match conns (corr is session-borne)
    std::thread lobby([&] { serveLobby(*listener, cfg, /*maxConns=*/kConns, /*readTimeoutSec=*/30); });

    {
        std::string e;
        std::unique_ptr<LobbySession> alice =
            LobbySession::connect("127.0.0.1", port, cfg.contentHash, "alice", "pw-a", &e);
        std::unique_ptr<LobbySession> bob =
            LobbySession::connect("127.0.0.1", port, cfg.contentHash, "bob", "pw-b", &e);
        std::unique_ptr<LobbySession> mallory =
            LobbySession::connect("127.0.0.1", port, cfg.contentHash, "mallory", "pw-m", &e);
        CHECK(alice && bob && mallory, "three players log in");
        if (!alice || !bob || !mallory) { lobby.join(); return 1; }

        std::printf("Lobby-wide chat\n");
        CHECK(alice->chatSend("hello lobby", &e), "alice says hello");
        std::optional<ChannelPoll> cp = bob->chatPoll(0);
        CHECK(cp && cp->entries.size() == 1 && cp->entries[0].sender == "alice" &&
                  cp->entries[0].msg == "hello lobby",
              "bob's poll sees it, tagged with the sender");
        const std::size_t bobCursor = cp ? cp->next : 0;

        std::printf("Safety levers\n");
        std::string rateErr;
        CHECK(!alice->chatSend("again!", &rateErr) && rateErr.find("fast") != std::string::npos,
              "an instant second message is rate-limited");
        waitRateWindow();
        std::string lenErr;
        CHECK(!alice->chatSend(std::string(300, 'x'), &lenErr) &&
                  lenErr.find("long") != std::string::npos,
              "an over-long message is rejected");
        std::string muteErr;
        CHECK(!mallory->chatSend("free me", &muteErr) && muteErr.find("muted") != std::string::npos,
              "a muted user's message is rejected");
        CHECK(!alice->chatSend("", &e), "an empty message is rejected");

        waitRateWindow();
        CHECK(bob->chatSend("hi alice", &e), "bob replies");
        cp = bob->chatPoll(bobCursor);
        CHECK(cp && cp->entries.size() == 1 && cp->entries[0].sender == "bob",
              "polling from a cursor returns only the new entries");

        std::printf("Correspondence-game chat (side log, participants only)\n");
        MatchFormat corr;
        corr.time = MatchFormat::Time::Unlimited;
        CHECK(alice->seek(corr, &e), "alice seeks an unlimited (correspondence) game");
        std::optional<std::vector<SeekInfo>> seeks = bob->listSeeks();
        std::optional<ReadyCheckInfo> bobRc =
            (seeks && !seeks->empty()) ? bob->acceptSeek((*seeks)[0].id, &e) : std::nullopt;
        LobbyEvent aliceEv = alice->poll();
        PairedInfo aPair, bPair;
        const bool paired = bobRc && aliceEv.kind == LobbyEvent::Kind::ReadyCheck &&
                            readyUp(*alice, aliceEv.readyCheck, makeBuild("Alice"), *bob, *bobRc,
                                    makeBuild("Bob"), aPair, bPair);
        CHECK(paired && !aPair.live && !aPair.game.empty(), "they pair into a correspondence game");
        if (!paired) { lobby.join(); return 1; }

        waitRateWindow();
        CHECK(alice->corrChatSend(aPair.game, "good luck!", &e), "alice chats in the game");
        std::optional<ChannelPoll> gc = bob->corrChatPoll(bPair.game, 0);
        CHECK(gc && gc->entries.size() == 1 && gc->entries[0].sender == "alice" &&
                  gc->entries[0].msg == "good luck!",
              "bob reads it from the game's chat log");
        std::string outsiderErr;
        CHECK(!mallory->corrChatSend(aPair.game, "let me in", &outsiderErr),
              "a non-participant can't chat in the game");
        CHECK(!mallory->corrChatPoll(aPair.game, 0).has_value(),
              "…nor read its chat log");
        std::optional<ChannelPoll> moves = alice->corrPoll(aPair.game, 0);
        CHECK(moves && moves->entries.empty(), "the MOVE log stays pure (chat rides a side log)");
    }

    lobby.join();
    std::remove(dbPath.c_str());

    std::printf("\n%s (%d failure%s)\n", g_fails == 0 ? "ALL PASS" : "FAILURES", g_fails,
                g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
