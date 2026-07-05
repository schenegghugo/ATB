#pragma once
//
// Base64.h — Dependency-free base64 (standard alphabet). Header-only.
//
// Used to pack a build's text payload into a single delimiter-free token inside
// the space-separated game notation (net/Replay).
//
#include <optional>
#include <string>

namespace tb::base64 {

inline std::string encode(const std::string& in) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const unsigned n = (static_cast<unsigned char>(in[i]) << 16) |
                           (static_cast<unsigned char>(in[i + 1]) << 8) |
                           static_cast<unsigned char>(in[i + 2]);
        out.push_back(T[(n >> 18) & 63]);
        out.push_back(T[(n >> 12) & 63]);
        out.push_back(T[(n >> 6) & 63]);
        out.push_back(T[n & 63]);
    }
    if (const std::size_t rem = in.size() - i; rem == 1) {
        const unsigned n = static_cast<unsigned char>(in[i]) << 16;
        out.push_back(T[(n >> 18) & 63]);
        out.push_back(T[(n >> 12) & 63]);
        out += "==";
    } else if (rem == 2) {
        const unsigned n = (static_cast<unsigned char>(in[i]) << 16) |
                           (static_cast<unsigned char>(in[i + 1]) << 8);
        out.push_back(T[(n >> 18) & 63]);
        out.push_back(T[(n >> 12) & 63]);
        out.push_back(T[(n >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

inline std::optional<std::string> decode(const std::string& in) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    if (in.size() % 4 != 0) return std::nullopt;
    std::string out;
    out.reserve(in.size() / 4 * 3);
    for (std::size_t i = 0; i < in.size(); i += 4) {
        const bool p2 = in[i + 2] == '=', p3 = in[i + 3] == '=';
        const int a = val(in[i]), b = val(in[i + 1]);
        const int c = p2 ? 0 : val(in[i + 2]), d = p3 ? 0 : val(in[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0) return std::nullopt;
        const unsigned n = (a << 18) | (b << 12) | (c << 6) | d;
        out.push_back(static_cast<char>((n >> 16) & 0xff));
        if (!p2) out.push_back(static_cast<char>((n >> 8) & 0xff));
        if (!p3) out.push_back(static_cast<char>(n & 0xff));
    }
    return out;
}

} // namespace tb::base64
