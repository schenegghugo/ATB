#pragma once
//
// Password.h — Password hashing for account auth (Phase 4.5).
//
// PBKDF2-HMAC-SHA256 built on the project's hand-rolled SHA-256 (Sha256.h) — a
// *standard* KDF (RFC 2898 / RFC 8018), not home-grown crypto — so the server
// stores salted, iterated hashes and no plaintext, with zero crypto dependency.
// (argon2id via libsodium would be stronger against GPU cracking; PBKDF2 with a
// high iteration count is the OWASP-approved dependency-free option.)
//
#include <cstdint>
#include <string>

namespace tb {

// HMAC-SHA256(key, msg) -> raw 32 bytes.
[[nodiscard]] std::string hmacSha256(const std::string& key, const std::string& msg);

// PBKDF2-HMAC-SHA256 -> `dkLen` derived bytes, hex-encoded. Exposed for
// known-answer tests; password storage goes through hashPassword/verifyPassword.
[[nodiscard]] std::string pbkdf2Sha256Hex(const std::string& password, const std::string& salt,
                                          std::uint32_t iterations, std::size_t dkLen);

// Hash a password for storage: generates a random salt, runs PBKDF2, and returns
// a self-describing string `pbkdf2_sha256$<iters>$<saltHex>$<hashHex>`.
[[nodiscard]] std::string hashPassword(const std::string& password);

// Verify `password` against a string produced by hashPassword(). False on any
// mismatch or malformed record.
[[nodiscard]] bool verifyPassword(const std::string& password, const std::string& stored);

} // namespace tb
