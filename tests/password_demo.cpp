//
// password_demo.cpp — Phase 4.5: PBKDF2-HMAC-SHA256 password hashing.
//
// Known-answer tests against published HMAC-SHA256 / PBKDF2-HMAC-SHA256 vectors,
// plus the hashPassword/verifyPassword round-trip. CI smoke test.
//
#include "data/Password.h"
#include "data/Sha256.h"

#include <cstdio>
#include <string>

using namespace tb;

static int g_fails = 0;
#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) std::printf("  [PASS] %s\n", msg);                                               \
        else { std::printf("  [FAIL] %s\n", msg); ++g_fails; }                                     \
    } while (0)

static std::string toHex(const std::string& b) {
    static const char* h = "0123456789abcdef";
    std::string o;
    for (unsigned char c : b) { o.push_back(h[c >> 4]); o.push_back(h[c & 0xf]); }
    return o;
}

int main() {
    std::printf("SHA-256 raw/hex still agree (refactor sanity)\n");
    {
        CHECK(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
              "sha256Hex(\"abc\") vector");
        CHECK(toHex(sha256Raw("abc")) == sha256Hex("abc"), "sha256Raw matches sha256Hex");
    }

    std::printf("HMAC-SHA256 known-answer\n");
    {
        // RFC 4231-style: HMAC-SHA256(key=\"key\", \"The quick brown fox ...\").
        CHECK(toHex(hmacSha256("key", "The quick brown fox jumps over the lazy dog")) ==
                  "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8",
              "HMAC-SHA256(key, fox) vector");
    }

    std::printf("PBKDF2-HMAC-SHA256 known-answer vectors\n");
    {
        CHECK(pbkdf2Sha256Hex("password", "salt", 1, 32) ==
                  "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b",
              "PBKDF2(password, salt, 1, 32)");
        CHECK(pbkdf2Sha256Hex("password", "salt", 2, 32) ==
                  "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43",
              "PBKDF2(password, salt, 2, 32)");
        CHECK(pbkdf2Sha256Hex("password", "salt", 4096, 32) ==
                  "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a",
              "PBKDF2(password, salt, 4096, 32)");
        CHECK(pbkdf2Sha256Hex("passwordPASSWORDpassword", "saltSALTsaltSALTsaltSALTsaltSALTsalt", 4096,
                              40) ==
                  "348c89dbcbd32b2f32d814b8116e84cf2b17347ebc1800181c4e2a1fb8dd53e1c635518c7dac47e9",
              "PBKDF2(longer inputs, 40 bytes)");
    }

    std::printf("hashPassword / verifyPassword round-trip\n");
    {
        const std::string rec = hashPassword("hunter2");
        CHECK(rec.rfind("pbkdf2_sha256$", 0) == 0, "record is self-describing");
        CHECK(verifyPassword("hunter2", rec), "correct password verifies");
        CHECK(!verifyPassword("Hunter2", rec), "wrong password rejected");
        CHECK(!verifyPassword("hunter2", "garbage"), "malformed record rejected");
        // Two hashes of the same password differ (random salt).
        CHECK(hashPassword("hunter2") != rec, "salted: same password hashes differently");
    }

    std::printf("\n%s (%d failure%s)\n", g_fails == 0 ? "ALL PASS" : "FAILURES", g_fails,
                g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
