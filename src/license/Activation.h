// Online-activation client.  Wraps the single public endpoint
// POST /api/v1/activate from src/license/docs/openapi.json.
//
// Lifecycle:
//   1. construct: pass a Config (URL, timeout, retries) and a trusted
//      Keyring (used to verify the .lic the server returns).
//   2. activate(activation_code, fingerprint): blocking call.  Builds
//      the JSON body, derives the deterministic Idempotency-Key, POSTs,
//      handles the response.
//
// Status mapping (1:1 with openapi.json):
//   * 200 → verify the returned .lic via the interop core (signature,
//     kid, time window), assert the .lic's `fingerprint` field matches
//     the local one; on any mismatch → BadResponse / FingerprintMismatch.
//   * 401 → InvalidCode (bad / revoked / expired activation code).
//   * 403 → NoFreeSlot (all maxServers slots occupied by other fps).
//   * 409 → IdempotencyConflict (programming error — same key, different
//     body; the activation client never causes this on its own because
//     the key is a pure function of the body).
//   * other 4xx → HttpError.
//   * 5xx / transport_error → retried with exponential backoff up to
//     Config::max_retries, then surfaces as HttpError or NetworkError.
//
// Recovery (no local .lic / corrupted .lic): the caller re-invokes
// activate() with the same activation_code; the same machine's
// Idempotency-Key is deterministic, so the server returns the cached
// response without consuming another slot.  No client-side state is
// needed beyond persisting the resulting .lic.
#pragma once

#include "license/Fingerprint.h"
#include "license/Http.h"
#include "license/License.h"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace visio::license {

class Keyring;

enum class ActivationStatus {
    Ok,                   // 200, .lic verified and saved
    InvalidCode,          // 401
    NoFreeSlot,           // 403
    IdempotencyConflict,  // 409
    NetworkError,         // transport failure (DNS, connect, TLS, timeout)
    HttpError,            // other 4xx / 5xx (after retries)
    BadResponse,          // 200 but body malformed / signature invalid /
                          //         kid unknown
    FingerprintMismatch,  // 200, .lic structurally valid, but its
                          //         `fingerprint` ≠ local
};

struct ActivationResult {
    ActivationStatus status = ActivationStatus::NetworkError;
    int http_code = 0;                   // 0 on transport failure
    std::string detail;                  // human-readable extra context
    std::optional<License> lic;          // populated iff Ok
    std::string raw_lic_json;            // canonical bytes saved to disk, iff Ok
    std::string idempotency_key;         // the UUID actually sent (for logs)
};

class ActivationClient {
public:
    struct Config {
        std::string activation_url;                       // e.g. "https://license.provlabs.tech"
        std::chrono::milliseconds timeout{15000};
        int max_retries = 2;                              // applied to 5xx / network only
        std::chrono::milliseconds retry_backoff_base{500};
    };

    ActivationClient(Config cfg, const Keyring& keyring);

    // Inject a transport.  When omitted, default_http_transport() is used
    // (cpp-httplib).  Tests pass an in-process lambda that returns canned
    // HttpResponses.
    void set_transport(HttpTransport t) { transport_ = std::move(t); }

    // Read-only view of the Idempotency-Key this client will produce for
    // a given (fingerprint, activation_code) pair.  Exposed for tests and
    // for diagnostic logging.  Pure function of the inputs + the
    // compile-time clientId/product baked into the binary.
    std::string idempotency_key_for(
        std::string_view activation_code,
        const Fingerprint& fingerprint) const;

    // Blocking activation call.
    ActivationResult activate(
        std::string_view activation_code,
        const Fingerprint& fingerprint,
        std::chrono::system_clock::time_point now = std::chrono::system_clock::now()) const;

    // Persist the .lic atomically (temp + rename) at `path`.  Used by
    // the caller after activate() succeeds; exposed separately so the
    // facade layer can choose its own state directory.  Returns false
    // on I/O failure.
    static bool save_lic_atomic(const std::filesystem::path& path,
                                std::string_view raw_json);

private:
    Config       cfg_;
    const Keyring& keyring_;
    HttpTransport transport_;
};

}  // namespace visio::license
