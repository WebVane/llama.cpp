#include "license/Sha256.h"

#include <openssl/sha.h>

namespace visio::license {

std::array<uint8_t, 32> sha256(const uint8_t* data, std::size_t len) {
    std::array<uint8_t, 32> out{};
    SHA256(data, len, out.data());
    return out;
}

}  // namespace visio::license
