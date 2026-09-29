// SHA-256 over a byte buffer, using OpenSSL's libcrypto (already linked
// for the existing Trial HMAC).  Returns the 32-byte digest.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace visio::license {

std::array<uint8_t, 32> sha256(const uint8_t* data, std::size_t len);

inline std::array<uint8_t, 32> sha256(std::string_view s) {
    return sha256(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

}  // namespace visio::license
