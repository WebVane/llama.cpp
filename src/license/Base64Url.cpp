#include "license/Base64Url.h"

#include <array>

namespace visio::license {
namespace {

// Inverse alphabet: index by byte, value = 6-bit chunk or 0xFF for invalid.
constexpr std::array<uint8_t, 256> build_dec_table() {
    std::array<uint8_t, 256> t{};
    for (auto& v : t) v = 0xFF;
    constexpr std::string_view alpha =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    for (uint8_t i = 0; i < 64; ++i) {
        t[static_cast<uint8_t>(alpha[i])] = i;
    }
    return t;
}

}  // namespace

std::optional<std::vector<uint8_t>> base64url_decode(std::string_view in) {
    static const auto dec = build_dec_table();

    // Strip optional padding `=` from the tail (server emits none, but tolerate).
    while (!in.empty() && in.back() == '=') in.remove_suffix(1);

    const std::size_t len = in.size();
    // Valid no-padding lengths: 4k (full triples), 4k+2 (1-byte tail), 4k+3 (2-byte tail).
    // 4k+1 cannot encode a whole byte boundary.
    if (len % 4 == 1) return std::nullopt;

    std::vector<uint8_t> out;
    out.reserve((len * 3) / 4);

    std::size_t i = 0;
    while (i + 4 <= len) {
        uint8_t a = dec[static_cast<uint8_t>(in[i])];
        uint8_t b = dec[static_cast<uint8_t>(in[i + 1])];
        uint8_t c = dec[static_cast<uint8_t>(in[i + 2])];
        uint8_t d = dec[static_cast<uint8_t>(in[i + 3])];
        if ((a | b | c | d) == 0xFF) return std::nullopt;
        out.push_back(static_cast<uint8_t>((a << 2) | (b >> 4)));
        out.push_back(static_cast<uint8_t>((b << 4) | (c >> 2)));
        out.push_back(static_cast<uint8_t>((c << 6) | d));
        i += 4;
    }
    const std::size_t rem = len - i;
    if (rem == 2) {
        uint8_t a = dec[static_cast<uint8_t>(in[i])];
        uint8_t b = dec[static_cast<uint8_t>(in[i + 1])];
        if ((a | b) == 0xFF) return std::nullopt;
        out.push_back(static_cast<uint8_t>((a << 2) | (b >> 4)));
    } else if (rem == 3) {
        uint8_t a = dec[static_cast<uint8_t>(in[i])];
        uint8_t b = dec[static_cast<uint8_t>(in[i + 1])];
        uint8_t c = dec[static_cast<uint8_t>(in[i + 2])];
        if ((a | b | c) == 0xFF) return std::nullopt;
        out.push_back(static_cast<uint8_t>((a << 2) | (b >> 4)));
        out.push_back(static_cast<uint8_t>((b << 4) | (c >> 2)));
    }
    return out;
}

}  // namespace visio::license
