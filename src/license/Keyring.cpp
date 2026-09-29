#include "license/Keyring.h"

#include "license/Base64Url.h"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace visio::license {

bool Keyring::add(std::string kid, std::string_view pubkey_b64url) {
    auto bytes = base64url_decode(pubkey_b64url);
    if (!bytes || bytes->size() != 32) return false;
    std::array<uint8_t, 32> pk{};
    std::copy(bytes->begin(), bytes->end(), pk.begin());
    keys_[std::move(kid)] = pk;
    return true;
}

bool Keyring::load_json(std::string_view doc) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(doc);
    } catch (const std::exception&) {
        return false;
    }
    if (!j.is_object() || !j.contains("keys") || !j["keys"].is_array()) {
        return false;
    }
    for (const auto& entry : j["keys"]) {
        if (!entry.is_object()) return false;
        if (!entry.contains("kid") || !entry["kid"].is_string()) return false;
        if (!entry.contains("pubkey") || !entry["pubkey"].is_string()) return false;
        if (!add(entry["kid"].get<std::string>(),
                 entry["pubkey"].get_ref<const std::string&>())) {
            return false;
        }
    }
    return true;
}

std::optional<std::array<uint8_t, 32>> Keyring::find(std::string_view kid) const {
    auto it = keys_.find(std::string(kid));
    if (it == keys_.end()) return std::nullopt;
    return it->second;
}

std::vector<std::string> Keyring::kids() const {
    std::vector<std::string> out;
    out.reserve(keys_.size());
    for (const auto& [k, _] : keys_) out.push_back(k);
    std::sort(out.begin(), out.end());
    return out;
}

// Fallback for dev / test builds that do not link keyring_baked.cpp (the
// CI-generated table): an empty keyring, so every .lic verification fails
// and tests must call Keyring::load_json() on their own instance.  A
// production build defines PROVISION_LICENSE_HAS_BAKED_KEYRING for this TU
// and links the definition generated from keyring_baked.cpp.in instead.
#ifndef PROVISION_LICENSE_HAS_BAKED_KEYRING
const Keyring& Keyring::baked() {
    static const Keyring empty;
    return empty;
}
#endif

}  // namespace visio::license
