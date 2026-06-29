#pragma once
//
// Sha256.h — Dependency-free SHA-256.
//
// Used as the catalog content hash (the trust anchor for the ranked/custom PvP
// handshake, ARCHITECTURE §5/§7) and reusable wherever a stable digest of bytes
// is needed. Hand-rolled so the headless core/server build pulls in no crypto
// dependency.
//
#include <string>

namespace tb {

// Lowercase hex SHA-256 (64 chars) of the given bytes.
[[nodiscard]] std::string sha256Hex(const std::string& bytes);

} // namespace tb
