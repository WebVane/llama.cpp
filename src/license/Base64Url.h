// Base64url (RFC 4648 §5) decoder, no-padding variant — the form used by the
// license server for both `signature` (64-byte Ed25519 sig) and `pubkey`
// (32-byte Ed25519 raw public key) fields.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace visio::license {

// Returns the decoded bytes on success, std::nullopt on any malformed input
// (illegal character, length not in {len%4 == 0, 2, 3}, etc.).  No padding
// expected; if `=` chars are present they are tolerated and treated as the
// canonical padding terminator (defensive — the server never emits them).
std::optional<std::vector<uint8_t>> base64url_decode(std::string_view in);

}  // namespace visio::license
