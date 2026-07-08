//
// chat_demo.cpp — Phase 4.5 slice 6.4: in-match chat relay.
//
// Two clients in a live match exchange chat over the match transport. The server
// broadcasts each line tagged with the sender's seat (asynchronously — chat need
// not wait for a turn), so both mirrors show the same transcript with correct
// attribution. CI smoke test.
//
#include "core/Build.h"
#include "core/Creatures.h"
#include "core/Ruleset.h"
#include "core/Spells.h"
#include "net/GameServer.h"
#include "net/MirrorSession.h"
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
#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) std::printf("  [PASS] %s\n", msg);                                               \
        else { std::printf("  [FAIL] %s\n", msg); ++g_fails; }                                     \
    } while (0)

namespace {
CharacterBuild makeBuild(const char* n) {
    CharacterBuild b;
    b.name = n;
    b.stats.hpPurchases = 2;
    b.spellIds = {spellid::Attack};
    return b;
}
// Pump until the mirror's chat log has at least `n` lines (bounded wall-clock).
bool waitChat(MirrorSession& s, std::size_t n) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (s.chat().size() < n && std::chrono::steady_clock::now() < deadline) s.pump(100);
    return s.chat().size() >= n;
}
} // namespace

int main() {
    const SpellCatalog catalog = makeDefaultCatalog();
    const std::vector<Entity> creatures = makeDefaultCreatures();
    const Ruleset ruleset = makeDefaultRuleset();

    MatchConfig cfg;
    cfg.ruleset = ruleset;
    cfg.catalog = catalog;
    cfg.creatures = creatures;
    cfg.contentHash = contentHashOf(catalog); // casual (no accounts) → no login

    std::optional<Listener> listener = Listener::bind(0);
    const uint16_t port = listener->port();
    std::thread server([&] { serveMatches(*listener, cfg, /*maxMatches=*/1, /*readTimeoutSec=*/30); });

    {
        // Connect concurrently: the server sends `welcome` only once both are in.
        std::string ep, eq;
        std::unique_ptr<MirrorSession> p, q;
        std::thread tp([&] { p = MirrorSession::connect("127.0.0.1", port, cfg.contentHash, makeBuild("P"), ruleset, catalog, creatures, &ep); });
        std::thread tq([&] { q = MirrorSession::connect("127.0.0.1", port, cfg.contentHash, makeBuild("Q"), ruleset, catalog, creatures, &eq); });
        tp.join();
        tq.join();
        CHECK(p && q, "both clients join a live match");
        if (!p || !q) { server.join(); return 1; }

        // The server assigns seats by connection order (a concurrent-join race), so
        // attribute chat by each mirror's actual seat.
        const Faction pSeat = p->seat(), qSeat = q->seat();
        CHECK(pSeat != qSeat, "the two clients hold opposite seats");

        std::printf("Chat is relayed asynchronously to both, tagged by seat\n");
        // One side is to move; neither moves — chat must still flow both ways.
        CHECK(p->sendChat("hi from p"), "p sends a line");
        CHECK(waitChat(*q, 1) && q->chat()[0].seat == pSeat && q->chat()[0].text == "hi from p",
              "the opponent receives it, attributed to p's seat");
        CHECK(waitChat(*p, 1) && p->chat()[0].seat == pSeat,
              "the sender sees its own line too (echoed by the server)");

        CHECK(q->sendChat("hey from q"), "q sends a line (out of turn)");
        CHECK(waitChat(*p, 2) && p->chat()[1].seat == qSeat && p->chat()[1].text == "hey from q",
              "the other player receives the out-of-turn line");
        CHECK(waitChat(*q, 2), "both transcripts converge to the same two lines");
    }

    server.join(); // clients closed → the match ends (idle/disconnect) → thread returns

    std::printf("\n%s (%d failure%s)\n", g_fails == 0 ? "ALL PASS" : "FAILURES", g_fails,
                g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
