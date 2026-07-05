//
// mailbox_demo.cpp — Phase 4.5 CR.3: the store-and-forward relay.
//
// Two clients exchange move-strings through the relay over a real socket (both
// connect OUT — NAT-immune), and the per-game log stays ordered + consistent and
// isolated between games. CI smoke test.
//
#include "net/MailboxRelay.h"
#include "net/Socket.h"

#include <cstdio>
#include <optional>
#include <string>
#include <thread>

using namespace tb::net;

static int g_fails = 0;
#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) std::printf("  [PASS] %s\n", msg);                                               \
        else { std::printf("  [FAIL] %s\n", msg); ++g_fails; }                                     \
    } while (0)

int main() {
    Mailbox box;
    std::optional<Listener> listener = Listener::bind(0);
    const uint16_t port = listener->port();
    // Two clients this run; serveRelay returns once both have connected + closed.
    std::thread relay([&] { serveRelay(*listener, box, /*maxConns=*/2); });

    { // clients live only in this scope so they close before we stop the relay
        std::printf("Two clients relay moves through the mailbox\n");
        std::optional<RelayClient> a = RelayClient::connect("127.0.0.1", port);
        std::optional<RelayClient> b = RelayClient::connect("127.0.0.1", port);
        CHECK(a && b, "both clients connect to the relay");

        const std::optional<std::size_t> p = a->post("g1", "alice", "m8,4");
        CHECK(p && *p == 1, "A's post returns log length 1");

        RelayClient::PollResult got = *b->poll("g1", 0);
        CHECK(got.entries.size() == 1 && got.entries[0].sender == "alice" &&
                  got.entries[0].msg == "m8,4" && got.next == 1,
              "B receives A's move (sender + payload + cursor)");

        (void)b->post("g1", "bob", ".");
        RelayClient::PollResult back = *a->poll("g1", *p); // A polls from index 1
        CHECK(back.entries.size() == 1 && back.entries[0].sender == "bob",
              "A receives B's reply, not its own move");

        std::printf("A full correspondence exchange stays ordered + consistent\n");
        for (int i = 0; i < 5; ++i) {
            (void)a->post("g2", "alice", "a" + std::to_string(i));
            (void)b->post("g2", "bob", "b" + std::to_string(i));
        }
        RelayClient::PollResult full = *a->poll("g2", 0);
        CHECK(full.entries.size() == 10, "all 10 messages are logged");
        bool ordered = full.entries.size() == 10;
        for (int i = 0; i < 5 && ordered; ++i)
            ordered = full.entries[2 * i].msg == "a" + std::to_string(i) &&
                      full.entries[2 * i + 1].msg == "b" + std::to_string(i);
        CHECK(ordered, "messages preserved in exact post order");
        RelayClient::PollResult fullB = *b->poll("g2", 0);
        CHECK(fullB.entries.size() == 10, "both clients see the identical log");

        std::printf("Games are isolated\n");
        RelayClient::PollResult empty = *a->poll("g3", 0);
        CHECK(empty.entries.empty(), "an untouched game is empty (no cross-talk)");
    }

    relay.join(); // returns once both clients (above) have closed

    std::printf("\n%s (%d failure%s)\n", g_fails == 0 ? "ALL PASS" : "FAILURES", g_fails,
                g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
