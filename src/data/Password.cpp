//
// Password.cpp — see Password.h.
//
#include "Password.h"

#include "Sha256.h"

#include <array>
#include <cstdio>
#include <random>
#include <string>

namespace tb {
namespace {

constexpr std::size_t kBlock = 64;   // SHA-256 block size
constexpr std::size_t kHashLen = 32; // SHA-256 output
constexpr std::uint32_t kIterations = 200000;

std::string toHex(const std::string& bytes) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (unsigned char b : bytes) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0xf]);
    }
    return out;
}

// Constant-time-ish string compare (avoids leaking length-of-match via timing).
bool equalsCt(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

} // namespace

std::string hmacSha256(const std::string& key, const std::string& msg) {
    std::string k = key.size() > kBlock ? sha256Raw(key) : key;
    k.resize(kBlock, '\0'); // zero-pad to the block size

    std::string ipad(kBlock, '\0'), opad(kBlock, '\0');
    for (std::size_t i = 0; i < kBlock; ++i) {
        ipad[i] = static_cast<char>(static_cast<unsigned char>(k[i]) ^ 0x36);
        opad[i] = static_cast<char>(static_cast<unsigned char>(k[i]) ^ 0x5c);
    }
    return sha256Raw(opad + sha256Raw(ipad + msg));
}

std::string pbkdf2Sha256Hex(const std::string& password, const std::string& salt,
                            std::uint32_t iterations, std::size_t dkLen) {
    std::string dk;
    dk.reserve(dkLen);
    std::uint32_t block = 1;
    while (dk.size() < dkLen) {
        // U1 = HMAC(P, salt || INT32_BE(block))
        std::string prefixed = salt;
        prefixed.push_back(static_cast<char>((block >> 24) & 0xff));
        prefixed.push_back(static_cast<char>((block >> 16) & 0xff));
        prefixed.push_back(static_cast<char>((block >> 8) & 0xff));
        prefixed.push_back(static_cast<char>(block & 0xff));
        std::string u = hmacSha256(password, prefixed);
        std::string t = u;
        for (std::uint32_t i = 1; i < iterations; ++i) {
            u = hmacSha256(password, u);
            for (std::size_t j = 0; j < kHashLen; ++j) t[j] = static_cast<char>(t[j] ^ u[j]);
        }
        dk += t;
        ++block;
    }
    dk.resize(dkLen);
    return toHex(dk);
}

std::string hashPassword(const std::string& password) {
    // 16 random salt bytes from a nondeterministic source.
    std::random_device rd;
    std::string salt(16, '\0');
    for (char& c : salt) c = static_cast<char>(rd() & 0xff);

    const std::string hashHex = pbkdf2Sha256Hex(password, salt, kIterations, kHashLen);
    return "pbkdf2_sha256$" + std::to_string(kIterations) + "$" + toHex(salt) + "$" + hashHex;
}

bool verifyPassword(const std::string& password, const std::string& stored) {
    // Parse "pbkdf2_sha256$<iters>$<saltHex>$<hashHex>".
    const auto d1 = stored.find('$');
    if (d1 == std::string::npos || stored.compare(0, d1, "pbkdf2_sha256") != 0) return false;
    const auto d2 = stored.find('$', d1 + 1);
    if (d2 == std::string::npos) return false;
    const auto d3 = stored.find('$', d2 + 1);
    if (d3 == std::string::npos) return false;

    const std::uint32_t iters =
        static_cast<std::uint32_t>(std::strtoul(stored.substr(d1 + 1, d2 - d1 - 1).c_str(), nullptr, 10));
    const std::string saltHex = stored.substr(d2 + 1, d3 - d2 - 1);
    const std::string hashHex = stored.substr(d3 + 1);
    if (iters == 0 || saltHex.size() % 2 != 0) return false;

    // Decode the salt hex back to bytes.
    std::string salt;
    salt.reserve(saltHex.size() / 2);
    for (std::size_t i = 0; i + 1 < saltHex.size(); i += 2)
        salt.push_back(static_cast<char>(std::strtoul(saltHex.substr(i, 2).c_str(), nullptr, 16)));

    const std::string got = pbkdf2Sha256Hex(password, salt, iters, hashHex.size() / 2);
    return equalsCt(got, hashHex);
}

} // namespace tb
