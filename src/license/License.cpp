#include "license/License.h"

#include "license/Base64Url.h"
#include "license/Ed25519.h"
#include "license/Jcs.h"
#include "license/Keyring.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdio>
#include <ctime>

namespace visio::license {
namespace {

using json = nlohmann::json;

// Parse RFC 3339 with fixed-form `YYYY-MM-DDTHH:MM:SSZ` (no fractional
// seconds, always UTC).  The server contract pins this form
// (docs/vectors/golden/README.md §2), so we hard-code the parse rather
// than dragging in a general-purpose RFC 3339 grammar.  We tolerate the
// `.<digits>Z` fractional form as a defensive bonus (timegm + fraction
// discarded) — never relied upon, but it's one branch.
//
// Returns std::nullopt on any deviation from the documented form.
std::optional<std::chrono::system_clock::time_point>
parse_rfc3339_utc(std::string_view s) {
    if (s.size() < 20) return std::nullopt;  // "YYYY-MM-DDTHH:MM:SSZ" = 20 chars
    // Accept "...Z" or "...<frac>Z"; reject any timezone offset (server
    // promises 'Z' only — so does the description in metadata.json).
    if (s.back() != 'Z') return std::nullopt;

    int y, mo, d, h, mi, se;
    char sep1, sep2, tsep, csep1, csep2;
    // The trailing 'Z' is not part of sscanf because of the optional fraction
    // between SS and Z.  We consume up to seconds, then look at the remainder.
    if (std::sscanf(std::string(s.substr(0, 19)).c_str(),
                    "%4d%c%2d%c%2d%c%2d%c%2d%c%2d",
                    &y, &sep1, &mo, &sep2, &d, &tsep, &h, &csep1, &mi, &csep2, &se) != 11) {
        return std::nullopt;
    }
    if (sep1 != '-' || sep2 != '-' || tsep != 'T' || csep1 != ':' || csep2 != ':') {
        return std::nullopt;
    }
    // Skip optional ".<digits>" between seconds and Z; require Z at the very end.
    if (s.size() > 20) {
        if (s[19] != '.') return std::nullopt;
        for (std::size_t i = 20; i + 1 < s.size(); ++i) {
            if (s[i] < '0' || s[i] > '9') return std::nullopt;
        }
    } else if (s.size() != 20) {
        return std::nullopt;
    }

    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31 ||
        h < 0 || h > 23 || mi < 0 || mi > 59 || se < 0 || se > 60) {
        return std::nullopt;
    }

    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon  = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min  = mi;
    tm.tm_sec  = se;
    // timegm/_mkgmtime: convert UTC broken-down → time_t without touching TZ.
#if defined(_WIN32)
    const std::time_t tt = _mkgmtime(&tm);
#else
    const std::time_t tt = timegm(&tm);
#endif
    if (tt == static_cast<std::time_t>(-1)) return std::nullopt;
    return std::chrono::system_clock::from_time_t(tt);
}

LicenseDecision fail(LicenseReason r, std::string detail = {}) {
    LicenseDecision d;
    d.ok = false;
    d.reason = r;
    d.detail = std::move(detail);
    return d;
}

}  // namespace

const char* license_reason_label(LicenseReason r) noexcept {
    switch (r) {
        case LicenseReason::Ok:                    return "Ok";
        case LicenseReason::BadJson:               return "BadJson";
        case LicenseReason::MissingField:          return "MissingField";
        case LicenseReason::BadFieldType:          return "BadFieldType";
        case LicenseReason::BadSignature:          return "BadSignature";
        case LicenseReason::BadSignatureFormat:    return "BadSignatureFormat";
        case LicenseReason::UnknownKid:            return "UnknownKid";
        case LicenseReason::BadIssuedAt:           return "BadIssuedAt";
        case LicenseReason::BadExpiresAt:          return "BadExpiresAt";
        case LicenseReason::NotYetValid:           return "NotYetValid";
        case LicenseReason::Expired:               return "Expired";
        case LicenseReason::CanonicalisationError: return "CanonicalisationError";
    }
    return "Unknown";
}

LicenseDecision verify_license_blob(
    std::string_view raw_json,
    const Keyring& keyring,
    std::chrono::system_clock::time_point now,
    std::chrono::seconds skew) {

    json j;
    try {
        j = json::parse(raw_json);
    } catch (const std::exception& e) {
        return fail(LicenseReason::BadJson, e.what());
    }
    if (!j.is_object()) {
        return fail(LicenseReason::BadJson, "top-level not an object");
    }

    auto require_string = [&](const char* key, std::string& out) -> std::optional<LicenseReason> {
        auto it = j.find(key);
        if (it == j.end()) return LicenseReason::MissingField;
        if (!it->is_string()) return LicenseReason::BadFieldType;
        out = it->get<std::string>();
        return std::nullopt;
    };

    License lic;
    if (auto r = require_string("licenseId",      lic.license_id))      return fail(*r, "licenseId");
    if (auto r = require_string("clientId",       lic.client_id))       return fail(*r, "clientId");
    if (auto r = require_string("product",        lic.product))         return fail(*r, "product");
    if (auto r = require_string("tariff",         lic.tariff))          return fail(*r, "tariff");
    if (auto r = require_string("activationCode", lic.activation_code)) return fail(*r, "activationCode");
    if (auto r = require_string("kid",            lic.kid))             return fail(*r, "kid");
    if (auto r = require_string("signature",      lic.signature_b64url))return fail(*r, "signature");

    // fingerprint — present only for online .lic; absence is meaningful.
    if (auto it = j.find("fingerprint"); it != j.end()) {
        if (!it->is_string()) return fail(LicenseReason::BadFieldType, "fingerprint");
        lic.fingerprint = it->get<std::string>();
    }

    // maxServers — required integer.
    if (auto it = j.find("maxServers"); it == j.end()) {
        return fail(LicenseReason::MissingField, "maxServers");
    } else if (!it->is_number_integer()) {
        return fail(LicenseReason::BadFieldType, "maxServers");
    } else {
        lic.max_servers = it->get<int64_t>();
    }

    // limits — required object with optional requestLimit and bool gpu.
    if (auto it = j.find("limits"); it == j.end() || !it->is_object()) {
        return fail(LicenseReason::MissingField, "limits");
    } else {
        const auto& L = *it;
        if (auto rit = L.find("requestLimit"); rit != L.end()) {
            // null treated as "absent" per server convention (README §3
            // says it's omitted when unset; we accept null as a defensive
            // alias but also accept integer).
            if (rit->is_null()) {
                // leave optional empty
            } else if (rit->is_number_integer()) {
                lic.limits.request_limit = rit->get<int64_t>();
            } else {
                return fail(LicenseReason::BadFieldType, "limits.requestLimit");
            }
        }
        if (auto git = L.find("gpu"); git != L.end()) {
            if (!git->is_boolean()) return fail(LicenseReason::BadFieldType, "limits.gpu");
            lic.limits.gpu = git->get<bool>();
        }
    }

    // issuedAt / expiresAt — required strings, RFC 3339 UTC.
    std::string issued_str, expires_str;
    if (auto r = require_string("issuedAt",  issued_str))  return fail(*r, "issuedAt");
    if (auto r = require_string("expiresAt", expires_str)) return fail(*r, "expiresAt");
    auto issued = parse_rfc3339_utc(issued_str);
    if (!issued)  return fail(LicenseReason::BadIssuedAt,  issued_str);
    auto expires = parse_rfc3339_utc(expires_str);
    if (!expires) return fail(LicenseReason::BadExpiresAt, expires_str);
    lic.issued_at  = *issued;
    lic.expires_at = *expires;

    // ---- Signature verification ----
    // Build the payload to canonicalize: a copy of `j` with `signature` removed.
    // The .lic on disk may or may not be in canonical form — we don't care,
    // we always re-canonicalize the parsed tree.  (For the golden vectors
    // both forms coincide: server already emits canonical bytes.)
    json payload = j;
    payload.erase("signature");
    std::string canonical;
    try {
        canonical = jcs_canonicalize(payload);
    } catch (const std::exception& e) {
        return fail(LicenseReason::CanonicalisationError, e.what());
    }

    auto sig_bytes = base64url_decode(lic.signature_b64url);
    if (!sig_bytes || sig_bytes->size() != 64) {
        return fail(LicenseReason::BadSignatureFormat,
                    "signature must be base64url of 64 bytes");
    }

    auto pubkey = keyring.find(lic.kid);
    if (!pubkey) {
        return fail(LicenseReason::UnknownKid, lic.kid);
    }

    if (!ed25519_verify(pubkey->data(), sig_bytes->data(),
                        reinterpret_cast<const uint8_t*>(canonical.data()),
                        canonical.size())) {
        return fail(LicenseReason::BadSignature,
                    "Ed25519 verify rejected the signature over canonical payload");
    }

    // ---- Time window ----
    if (now + skew < lic.issued_at) {
        return fail(LicenseReason::NotYetValid, issued_str);
    }
    if (now > lic.expires_at + skew) {
        return fail(LicenseReason::Expired, expires_str);
    }

    LicenseDecision d;
    d.ok = true;
    d.reason = LicenseReason::Ok;
    d.lic = std::move(lic);
    return d;
}

}  // namespace visio::license
