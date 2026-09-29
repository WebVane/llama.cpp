// .lic model + parser + validator.
//
// One entry point: License::parse_and_verify(raw_json, keyring, now) →
// LicenseDecision.  All failure modes carry a specific Reason so the
// surrounding HTTP / log layer can report something actionable.
//
// This is the INTEROP CORE — no network, no filesystem, no fingerprint
// computation.  Those layers live above (LicenseLib facade) and consume
// this module's verdict.  Keeping the verifier pure simplifies the
// conformance test (round-trip parse + canonicalize + Ed25519-verify
// against the golden vectors).
#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace visio::license {

class Keyring;

enum class LicenseReason {
    Ok,
    BadJson,             // parse failure
    MissingField,        // required field absent
    BadFieldType,        // wrong JSON type
    BadSignature,        // Ed25519 verify rejected
    BadSignatureFormat,  // signature field not valid base64url / wrong length
    UnknownKid,          // kid not in the trusted keyring
    BadIssuedAt,         // issuedAt unparseable
    BadExpiresAt,        // expiresAt unparseable
    NotYetValid,         // now < issuedAt - skew
    Expired,             // now > expiresAt + skew
    CanonicalisationError,
};

// Symbolic label for logs / surfaced details.  Stable across builds
// (used by Activation.cpp and LicenseLib::init_offline_or_online to
// produce actionable messages instead of opaque integer codes like
// "reason=6").
const char* license_reason_label(LicenseReason r) noexcept;

struct LicenseLimits {
    std::optional<int64_t> request_limit;  // absent ↔ unlimited
    bool gpu = false;
};

struct License {
    std::string license_id;
    std::string client_id;
    std::string product;
    std::string tariff;
    std::string activation_code;
    std::optional<std::string> fingerprint;  // present ↔ online
    int64_t     max_servers = 0;
    LicenseLimits limits;
    std::chrono::system_clock::time_point issued_at{};
    std::chrono::system_clock::time_point expires_at{};
    std::string kid;
    std::string signature_b64url;
};

struct LicenseDecision {
    bool          ok = false;
    LicenseReason reason = LicenseReason::Ok;
    std::optional<License> lic;        // populated iff parse succeeded; cleared on failure for clarity
    std::string   detail;              // human-readable extra context (logged)
};

// Verify a .lic blob.
//   raw_json — the on-disk bytes (UTF-8 JSON).
//   keyring  — trusted Ed25519 pubkeys (kid → pubkey).
//   now      — clock reference for expiry/not-yet-valid checks.  Caller
//              uses system_clock::now() in production, fixed instants in
//              tests.
//   skew     — tolerated clock drift on both bounds; default 60 s.
//
// Returns:
//   * ok=true  and reason=Ok when signature, kid lookup, and time window
//     all pass.  lic is populated with parsed fields.
//   * ok=false and reason=<specific> otherwise.
LicenseDecision verify_license_blob(
    std::string_view raw_json,
    const Keyring& keyring,
    std::chrono::system_clock::time_point now,
    std::chrono::seconds skew = std::chrono::seconds{60});

}  // namespace visio::license
