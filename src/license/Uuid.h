// UUIDv5 (RFC 4122 §4.3) — name-based UUID derived via SHA-1 of
// (namespace || name).  Used to produce the Idempotency-Key for the
// activation endpoint: the same (clientId, product, fingerprint,
// activationCode) tuple must always derive the same key so a retry from
// the same machine is recognised by the server as a duplicate (and
// returns the cached response instead of consuming another slot).
//
// `namespace_uuid`: any RFC 4122 UUID.  We use a fixed project-scoped
// UUID baked into the binary (Activation.cpp) — the namespace is
// arbitrary as long as it stays constant across builds.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace visio::license {

// Format a 16-byte UUID as the canonical hyphenated lowercase string
// (8-4-4-4-12).
std::string uuid_format(const std::array<uint8_t, 16>& bytes);

// Parse a canonical hyphenated UUID into 16 bytes.  Returns false on
// malformed input.  Accepts both lowercase and uppercase hex.
bool uuid_parse(std::string_view canonical, std::array<uint8_t, 16>& out);

// Generate a UUIDv5 from a namespace UUID and a name string.
// Algorithm (RFC 4122 §4.3):
//   1. Compute SHA-1(namespace_bytes || name).
//   2. Take the first 16 bytes.
//   3. Set the version field (high 4 bits of byte 6) to 0x5.
//   4. Set the variant field (high 2 bits of byte 8) to 0b10 (RFC 4122).
std::array<uint8_t, 16> uuid_v5(const std::array<uint8_t, 16>& namespace_uuid,
                                std::string_view name);

}  // namespace visio::license
