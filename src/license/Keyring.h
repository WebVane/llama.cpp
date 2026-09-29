// Trusted Ed25519 public keys, keyed by `kid` (a server-side identifier
// for a signing key).  Multiple kids may be active simultaneously
// (key-rotation overlap): a .lic is accepted iff its `kid` matches ANY
// entry — "accept-if-any" semantics.
//
// Production builds bake the keyring at CI time (a CMake-substituted
// header populated from a vault-provisioned secret).  For interop /
// conformance tests, the in-tree golden keyring at
// `src/license/docs/vectors/keyring.json` is loaded by the test harness
// via Keyring::from_json_string().
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace visio::license {

struct TrustedKey {
    std::string kid;
    std::array<uint8_t, 32> pubkey{};
};

class Keyring {
public:
    Keyring() = default;

    // Add a single trusted entry.  `pubkey_b64url` is the raw 32-byte
    // Ed25519 public key in base64url (no padding) — same form the
    // server publishes in /docs/vectors/keyring.json.  Returns false
    // if the encoding is malformed or the decoded length != 32.
    bool add(std::string kid, std::string_view pubkey_b64url);

    // Bulk-load from a JSON document of the form
    //   { "keys": [ { "kid": "...", "pubkey": "<base64url>" }, ... ] }
    // — the shape of src/license/docs/vectors/keyring.json.  Existing
    // entries are kept; duplicates overwrite.  Returns false on parse
    // failure or any malformed entry.
    bool load_json(std::string_view doc);

    // Lookup; std::nullopt when kid is unknown.
    std::optional<std::array<uint8_t, 32>> find(std::string_view kid) const;

    // Number of trusted entries.  Useful for sanity-asserts ("at least
    // one trusted key was baked into this build").
    std::size_t size() const noexcept { return keys_.size(); }

    // Sorted list of every kid currently trusted.  Used by LicenseLib at
    // startup to log what made it into the baked keyring — so an
    // operator can see at a glance whether a CI keyring secret was
    // actually injected (and which kid the LS must sign with for the
    // verifier to accept).  Order is alphabetical for stable log output.
    std::vector<std::string> kids() const;

    // Construct the singleton keyring used in production.  Backed by a
    // CMake-generated table at src/license/keyring_baked.cpp.  In dev /
    // test builds without that table, returns an empty Keyring — tests
    // call load_json() to populate it.
    static const Keyring& baked();

private:
    std::unordered_map<std::string, std::array<uint8_t, 32>> keys_;
};

}  // namespace visio::license
