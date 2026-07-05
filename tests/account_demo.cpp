//
// account_demo.cpp — Phase 4.5: the AccountStore (auth + Elo + persistence).
//
#include "net/AccountStore.h"

#include <cstdio>
#include <string>

using namespace tb::net;

static int g_fails = 0;
#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) std::printf("  [PASS] %s\n", msg);                                               \
        else { std::printf("  [FAIL] %s\n", msg); ++g_fails; }                                     \
    } while (0)

int main() {
    const std::string path = "tb_account_test.json";
    std::remove(path.c_str());

    std::printf("Auto-register + password auth\n");
    {
        AccountStore store(path);
        AuthResult a = store.authenticate("alice", "s3cret");
        CHECK(a.ok && a.created, "unknown user auto-registers");
        CHECK(a.account.rating == kDefaultRating, "new account starts at the default rating");

        AuthResult again = store.authenticate("alice", "s3cret");
        CHECK(again.ok && !again.created, "known user with correct password logs in (no re-create)");

        CHECK(!store.authenticate("alice", "wrong").ok, "wrong password rejected");
        CHECK(!store.authenticate("", "x").ok && !store.authenticate("bob", "").ok,
              "empty username/password rejected");

        CHECK(store.authenticate("bob", "hunter2").created, "second user registers");
        CHECK(store.size() == 2, "two accounts stored");
    }

    std::printf("Elo update on a recorded result (zero-sum)\n");
    {
        AccountStore store(path); // reloads alice + bob from disk
        const int a0 = store.ratingOf("alice"), b0 = store.ratingOf("bob");
        CHECK(a0 == kDefaultRating && b0 == kDefaultRating, "ratings persisted across reload");

        store.recordResult(/*winner=*/"alice", /*loser=*/"bob");
        const int a1 = store.ratingOf("alice"), b1 = store.ratingOf("bob");
        CHECK(a1 > a0 && b1 < b0, "winner gains rating, loser drops");
        CHECK((a1 - a0) == (b0 - b1), "Elo is zero-sum (equal ratings ⇒ ±16)");
        CHECK(a1 - a0 == 16, "K=32 at equal ratings gives +16/-16");

        auto av = store.get("alice");
        CHECK(av && av->wins == 1 && av->losses == 0, "winner W/L updated");
        auto bv = store.get("bob");
        CHECK(bv && bv->wins == 0 && bv->losses == 1, "loser W/L updated");

        store.recordResult("alice", "nobody"); // unknown loser -> no-op
        CHECK(store.ratingOf("alice") == a1, "recording against an unknown player is a no-op");
    }

    std::printf("Rating + password survive a reload\n");
    {
        AccountStore store(path);
        CHECK(store.ratingOf("alice") > kDefaultRating, "updated rating persisted");
        CHECK(store.authenticate("alice", "s3cret").ok, "password hash persisted (still verifies)");
    }

    std::remove(path.c_str());
    std::printf("\n%s (%d failure%s)\n", g_fails == 0 ? "ALL PASS" : "FAILURES", g_fails,
                g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
