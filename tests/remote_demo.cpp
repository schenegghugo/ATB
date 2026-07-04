//
// remote_demo.cpp — Phase 4.4: the GUI's RemoteMatchSource, headless.
//
// RemoteMatchSource is raylib-free, so the exact code path the graphical client
// uses to play over the network is tested without a window: it drives one seat as
// a MatchSource (battle()/awaitingLocalInput()/submit()/update()) against the real
// server, opposite an ordinary GameClient. Proves the mirror the GUI renders ends
// byte-identical to the server. CI smoke test.
//
#include "core/Build.h"
#include "core/Creatures.h"
#include "core/Ruleset.h"
#include "core/Spells.h"
#include "net/GameClient.h"
#include "net/GameServer.h"
#include "net/MirrorSession.h"
#include "net/Socket.h"
#include "render/RemoteMatchSource.h"

#include <chrono>
#include <cstdio>
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

CharacterBuild pyromancer() {
    CharacterBuild b;
    b.name = "Pyromancer";
    b.spellIds = {spellid::Attack, spellid::Fireball, spellid::Poison};
    return b;
}
CharacterBuild bruiser() {
    CharacterBuild b;
    b.name = "Bruiser";
    b.stats.hpPurchases = 2;
    b.spellIds = {spellid::Attack, spellid::Knockback};
    return b;
}

} // namespace

int main() {
    const SpellCatalog catalog = makeDefaultCatalog();
    const std::vector<Entity> creatures = makeDefaultCreatures();
    const Ruleset ruleset = makeDefaultRuleset();
    const std::string hash = contentHashOf(catalog);
    const MatchConfig cfg{ruleset, catalog, creatures, hash};

    std::printf("The GUI's RemoteMatchSource plays a full match over a socket\n");

    std::optional<Listener> listener = Listener::bind(0);
    CHECK(listener.has_value(), "server binds an ephemeral port");
    const uint16_t port = listener->port();

    ServeResult server;
    std::thread srv([&] { server = serveOneMatch(*listener, cfg); });

    // Seat A: driven through the RemoteMatchSource seam exactly like main.cpp does.
    std::string remoteFinal;
    bool remoteFinished = false;
    std::string remoteErr;
    std::thread ta([&] {
        std::string err;
        std::unique_ptr<MirrorSession> ms = MirrorSession::connect(
            "127.0.0.1", port, hash, pyromancer(), ruleset, catalog, creatures, &err);
        if (!ms) { remoteErr = err; return; }
        const Faction seat = ms->seat();
        render::RemoteMatchSource src(std::move(ms));
        // Auto-play: submit our turn once per round. Keying on the round counter
        // (not an observed "not my turn" edge) is robust to update() draining a
        // whole round of the opponent's intents in one non-blocking pump — a human
        // driving the GUI has no such flag, they just click when it's their turn.
        int lastActedRound = -1;
        for (int guard = 0; guard < 200000 && !src.finished(); ++guard) {
            src.update(0.016f); // pump server messages (as the GUI does each frame)
            if (src.awaitingLocalInput() && src.battle().round() != lastActedRound) {
                for (const Intent& in : chasePolicy(seat, snapshotOf(src.battle())))
                    src.submit(in);
                lastActedRound = src.battle().round();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        remoteFinished = src.finished();
        remoteFinal = serializeSnapshot(snapshotOf(src.battle()));
    });

    // Seat B: an ordinary headless client.
    ClientResult cb;
    std::thread tb([&] {
        cb = playClient("127.0.0.1", port, hash, bruiser(), ruleset, catalog, creatures, chasePolicy);
    });

    ta.join();
    tb.join();
    srv.join();

    CHECK(remoteErr.empty(), "RemoteMatchSource connected + was admitted");
    CHECK(server.ok, "server ran the match to completion");
    CHECK(remoteFinished && cb.ok, "both the remote source and the plain client finished");
    CHECK(!server.finalSnapshot.empty() && remoteFinal == server.finalSnapshot &&
              cb.finalSnapshot == server.finalSnapshot,
          "the GUI mirror ends byte-identical to the server's authoritative state");

    std::printf("\n%s (%d failure%s)\n", g_fails == 0 ? "ALL PASS" : "FAILURES", g_fails,
                g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
