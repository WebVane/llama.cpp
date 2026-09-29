// SHA-1 over a byte buffer.  Only used to derive the Idempotency-Key
// (UUIDv5 per RFC 4122 §4.3) — never for cryptographic identity or .lic
// signing.  We tolerate SHA-1's collision weakness here because the
// server treats Idempotency-Key purely as an opaque correlation token.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace visio::license {

std::array<uint8_t, 20> sha1(const uint8_t* data, std::size_t len);

inline std::array<uint8_t, 20> sha1(std::string_view s) {
    return sha1(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

}  // namespace visio::license
