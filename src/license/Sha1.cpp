#include "license/Sha1.h"

#include <openssl/sha.h>

namespace visio::license {

std::array<uint8_t, 20> sha1(const uint8_t* data, std::size_t len) {
    std::array<uint8_t, 20> out{};
    SHA1(data, len, out.data());
    return out;
}

}  // namespace visio::license
