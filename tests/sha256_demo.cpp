//
// sha256_demo.cpp — Validates the hand-rolled SHA-256 against known vectors.
// Non-zero exit on failure.
//
#include "data/Sha256.h"

#include <cstdio>
#include <string>

using namespace tb;

static int g_fails = 0;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            std::printf("  [PASS] %s\n", msg);                                                     \
        } else {                                                                                   \
            std::printf("  [FAIL] %s\n", msg);                                                     \
            ++g_fails;                                                                             \
        }                                                                                          \
    } while (0)

int main() {
    std::printf("SHA-256 known-answer tests\n");
    CHECK(sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "empty string");
    CHECK(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "\"abc\"");
    CHECK(sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
          "56-byte message (two-block boundary)");
    // Stability + sensitivity.
    CHECK(sha256Hex("ATB").size() == 64, "digest is 64 hex chars");
    CHECK(sha256Hex("ATB") != sha256Hex("ATC"), "one-byte change flips the digest");
    CHECK(sha256Hex("ATB") == sha256Hex("ATB"), "deterministic");

    std::printf(g_fails == 0 ? "\nALL PASS (0 failures)\n" : "\n%d FAILURE(S)\n", g_fails);
    return g_fails == 0 ? 0 : 1;
}
