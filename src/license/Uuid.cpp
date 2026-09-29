#include "license/Uuid.h"

#include "license/Sha1.h"

#include <cctype>
#include <cstdio>
#include <vector>

namespace visio::license {
namespace {

int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

std::string uuid_format(const std::array<uint8_t, 16>& b) {
    char buf[37];
    std::snprintf(buf, sizeof(buf),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  b[0], b[1], b[2], b[3],
                  b[4], b[5],
                  b[6], b[7],
                  b[8], b[9],
                  b[10], b[11], b[12], b[13], b[14], b[15]);
    return std::string{buf, 36};
}

bool uuid_parse(std::string_view s, std::array<uint8_t, 16>& out) {
    if (s.size() != 36) return false;
    static constexpr int kDashes[] = {8, 13, 18, 23};
    for (int i : kDashes) {
        if (s[i] != '-') return false;
    }
    std::size_t bi = 0, si = 0;
    while (bi < 16) {
        if (si == 8 || si == 13 || si == 18 || si == 23) {
            ++si;
            continue;
        }
        int hi = hex_nibble(s[si]);
        int lo = hex_nibble(s[si + 1]);
        if (hi < 0 || lo < 0) return false;
        out[bi++] = static_cast<uint8_t>((hi << 4) | lo);
        si += 2;
    }
    return true;
}

std::array<uint8_t, 16> uuid_v5(const std::array<uint8_t, 16>& ns,
                                std::string_view name) {
    std::vector<uint8_t> buf;
    buf.reserve(16 + name.size());
    buf.insert(buf.end(), ns.begin(), ns.end());
    buf.insert(buf.end(), name.begin(), name.end());
    const auto h = sha1(buf.data(), buf.size());
    std::array<uint8_t, 16> out{};
    for (std::size_t i = 0; i < 16; ++i) out[i] = h[i];
    // Version 5 in the high nibble of byte 6.
    out[6] = static_cast<uint8_t>((out[6] & 0x0F) | 0x50);
    // RFC 4122 variant (10xx) in the high two bits of byte 8.
    out[8] = static_cast<uint8_t>((out[8] & 0x3F) | 0x80);
    return out;
}

}  // namespace visio::license
