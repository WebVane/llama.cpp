#include "license/Activation.h"

#include "license/DurableWrite.h"
#include "license/Jcs.h"
#include "license/Keyring.h"
#include "license/Uuid.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <random>
#include <thread>

namespace visio::license {
namespace {

using json = nlohmann::json;

// Project-scoped namespace UUID for Idempotency-Key derivation.  Any
// fixed UUID works as long as we keep it constant across builds — the
// server treats Idempotency-Key as opaque.  Value chosen with the
// `uuidgen` tool, recorded here verbatim for reproducibility.
constexpr std::array<uint8_t, 16> kActivationNs = {
    0x2f, 0xa3, 0xc1, 0xd7, 0x4b, 0x86, 0x4e, 0x91,
    0xa6, 0x18, 0x90, 0x2c, 0x33, 0x7b, 0xe1, 0x05,
};

// Idempotency-Key namespace constants.  The server treats the resulting
// UUIDv5 as opaque correlation token, so the literal value of these
// inputs doesn't matter — only that they're STABLE across retries from
// the same machine.  We use a fixed placeholder for clientId (no CI
// bake; runtime tenant binding is handled by the facade's cfg.client_id
// check separately) and the compile-time product, fall back to "LLM".
constexpr const char* kBakedClientId = "00000000-0000-0000-0000-000000000000";

constexpr const char* kBakedProduct =
#ifdef PROVISION_LICENSE_PRODUCT
    PROVISION_LICENSE_PRODUCT
#else
    "LLM"
#endif
    ;

// The server explains itself in the response body; without it the operator
// sees only a bare status number and has to guess.  Two precautions: the
// body is only ever surfaced on FAILURE (a 2xx body carries the signed .lic
// and has no business in a log), and it is clipped — a stray HTML error page
// from a proxy must not flood the journal.
std::string body_excerpt(const std::string& body) {
    constexpr std::size_t kMax = 400;
    if (body.empty()) return "<empty body>";
    std::size_t cut = std::min(body.size(), kMax);
    // Never split a UTF-8 sequence: back off over continuation bytes to the
    // lead byte, and drop that lead too if its sequence would not fit.  A
    // half character used to end up in the journal and in the /license/
    // activate JSON as U+FFFD, and a server error page in Russian is exactly
    // the body likely to be longer than the cap.
    if (cut < body.size()) {
        std::size_t lead = cut;
        while (lead > 0 && (static_cast<unsigned char>(body[lead]) & 0xC0) == 0x80) --lead;
        cut = lead;
    }
    std::string out;
    out.reserve(cut + 32);
    for (std::size_t i = 0; i < cut; ++i) {
        const auto c = static_cast<unsigned char>(body[i]);
        // The body is untrusted: control bytes (ESC sequences, NUL) must not
        // reach a terminal tailing the log or make grep treat it as binary.
        if (c < 0x20 || c == 0x7F) out += ' ';
        else out += static_cast<char>(c);
    }
    if (body.size() > cut) out += "… (" + std::to_string(body.size()) + " bytes)";
    return out;
}

// A 404 from the License Server itself means "unknown code" — it answers
// {"error":"activation code invalid, revoked or expired"}.  A 404 from
// anything else — a mistyped PROVISION_LICENSE_ACTIVATION_URL, a proxy
// without the route — is a routing problem, and calling it a bad code sends
// the operator off to request a new one instead of fixing the address.  Only
// the server's own JSON error shape counts as the former.
bool is_license_server_error_body(const std::string& body) {
    try {
        const auto j = json::parse(body);
        // The server is Spring Boot, and its OWN route-not-found 404 carries an
        // "error" string too: {"timestamp":…,"status":404,"error":"Not Found",
        // "path":"/…"}.  Only the application's answer lacks "path" — that is
        // the one that means "unknown code" (measured against
        // license.scan-dev.com, 23.09.2026).
        return j.is_object() && j.contains("error") && j["error"].is_string() &&
               !j.contains("path");
    } catch (...) {
        return false;
    }
}

bool is_retryable(int code, const std::string& transport_err) {
    if (!transport_err.empty()) return true;          // DNS / connect / timeout
    if (code >= 500 && code < 600) return true;       // server-side
    return false;
}

}  // namespace

ActivationClient::ActivationClient(Config cfg, const Keyring& keyring)
    : cfg_(std::move(cfg)), keyring_(keyring), transport_(default_http_transport()) {}

std::string ActivationClient::idempotency_key_for(
    std::string_view activation_code,
    const Fingerprint& fp) const {
    // Name = clientId|product|fingerprint|activationCode, pipe-separated
    // so independent components never collide via concatenation.  The
    // server treats the resulting UUID as opaque, so the exact recipe
    // doesn't need to match anything server-side; it only needs to be
    // STABLE across retries from the same machine for the same code.
    std::string name;
    name.reserve(160);
    name += kBakedClientId;
    name.push_back('|');
    name += kBakedProduct;
    name.push_back('|');
    name += fp.hex();
    name.push_back('|');
    name.append(activation_code.data(), activation_code.size());
    const auto u = uuid_v5(kActivationNs, name);
    return uuid_format(u);
}

ActivationResult ActivationClient::activate(
    std::string_view activation_code,
    const Fingerprint& fp,
    std::chrono::system_clock::time_point now) const {

    ActivationResult result;
    result.idempotency_key = idempotency_key_for(activation_code, fp);

    HttpRequest req;
    req.method = "POST";
    req.url = cfg_.activation_url + "/api/v1/activate";
    req.headers["Content-Type"] = "application/json";
    req.headers["Idempotency-Key"] = result.idempotency_key;
    req.timeout = cfg_.timeout;

    json body;
    body["activationCode"] = std::string{activation_code};
    body["fingerprint"] = fp.hex();
    // `replace`, not the default `strict`: a code that is not valid UTF-8
    // made dump() throw type_error.316, and nothing above caught it — the
    // exception left the detached /license/activate thread and took the whole
    // process down with std::terminate.  The route now rejects such codes
    // up front; this line keeps every other caller (the startup
    // PROVISION_LICENSE_ACTIVATION_CODE path) from crashing on them too.
    req.body = body.dump(-1, ' ', false, json::error_handler_t::replace);

    HttpResponse resp;
    int attempt = 0;
    while (true) {
        resp = transport_(req);
        if (!is_retryable(resp.code, resp.transport_error) || attempt >= cfg_.max_retries) {
            break;
        }
        // Exponential backoff with deterministic ladder — 500ms, 1s, 2s, ...
        const auto delay = cfg_.retry_backoff_base * (1 << attempt);
        std::this_thread::sleep_for(delay);
        ++attempt;
    }

    result.http_code = resp.code;

    if (resp.code == 0) {
        result.status = ActivationStatus::NetworkError;
        result.detail = resp.transport_error.empty()
                            ? "transport failure (no response)"
                            : resp.transport_error;
        return result;
    }

    // 401 is what openapi.json declares for a bad code; the deployed
    // License Server answers 404 with {"error":"activation code invalid,
    // revoked or expired"} instead.  Both mean the same thing to the
    // operator, and leaving 404 to fall through to the generic HttpError
    // branch below is what turned "wrong code" into an opaque
    // "status=5; detail=HTTP 404" (measured against license.scan-dev.com,
    // 20.09.2026).  The server-side contract mismatch is worth fixing on
    // its own; until then the client reports the meaning, not the number.
    if (resp.code == 401 ||
        (resp.code == 404 && is_license_server_error_body(resp.body))) {
        result.status = ActivationStatus::InvalidCode;
        result.detail = "HTTP " + std::to_string(resp.code) +
                        " — activation code unknown / revoked / expired: " +
                        body_excerpt(resp.body);
        return result;
    }
    if (resp.code == 404) {
        result.status = ActivationStatus::HttpError;
        result.detail = "HTTP 404 from " + req.url +
                        " — no activation endpoint there; check "
                        "PROVISION_LICENSE_ACTIVATION_URL: " + body_excerpt(resp.body);
        return result;
    }
    if (resp.code == 403) {
        result.status = ActivationStatus::NoFreeSlot;
        result.detail = "403 — no free slot; revoke an existing binding to free one: " +
                        body_excerpt(resp.body);
        return result;
    }
    if (resp.code == 409) {
        result.status = ActivationStatus::IdempotencyConflict;
        result.detail = "409 — same Idempotency-Key with different body (programming error): " +
                        body_excerpt(resp.body);
        return result;
    }
    if (resp.code < 200 || resp.code >= 300) {
        result.status = ActivationStatus::HttpError;
        result.detail = "HTTP " + std::to_string(resp.code) + ": " + body_excerpt(resp.body);
        return result;
    }

    // ---- 2xx: parse + verify ----
    json env;
    try {
        env = json::parse(resp.body);
    } catch (const std::exception& e) {
        result.status = ActivationStatus::BadResponse;
        result.detail = std::string{"malformed activation response: "} + e.what();
        return result;
    }
    if (!env.is_object() || !env.contains("lic") || !env["lic"].is_object()) {
        result.status = ActivationStatus::BadResponse;
        result.detail = "activation response missing 'lic' object";
        return result;
    }

    // Canonicalize the embedded lic so its on-disk bytes are exactly the
    // form the interop verifier expects.  We could trust the wire form
    // is already canonical (the server promises this), but recanonicalizing
    // is idempotent on canonical input and defensive against intermediary
    // pretty-printing.
    std::string raw_lic;
    try {
        raw_lic = jcs_canonicalize(env["lic"]);
    } catch (const std::exception& e) {
        result.status = ActivationStatus::BadResponse;
        result.detail = std::string{"failed to canonicalize lic: "} + e.what();
        return result;
    }

    auto verdict = verify_license_blob(raw_lic, keyring_, now);
    if (!verdict.ok) {
        result.status = ActivationStatus::BadResponse;
        // Surface the SYMBOLIC reason (UnknownKid / BadSignature /
        // MissingField / Expired / ...) instead of the raw integer.
        // "reason=6" forced operators to look up the enum every time;
        // "reason=UnknownKid" is actionable on its own.
        result.detail = "lic signature/structure rejected by verifier (reason=" +
                        std::string{license_reason_label(verdict.reason)} +
                        "; detail=" + verdict.detail + ")";
        return result;
    }
    if (!verdict.lic->fingerprint.has_value()) {
        result.status = ActivationStatus::BadResponse;
        result.detail = "activation response returned offline-shaped lic (no fingerprint)";
        return result;
    }
    if (*verdict.lic->fingerprint != fp.hex()) {
        result.status = ActivationStatus::FingerprintMismatch;
        result.detail = "server-issued fingerprint does not match local";
        return result;
    }

    result.status = ActivationStatus::Ok;
    result.detail = "activation succeeded";
    result.lic = std::move(verdict.lic);
    result.raw_lic_json = std::move(raw_lic);
    return result;
}

bool ActivationClient::save_lic_atomic(const std::filesystem::path& path,
                                       std::string_view raw_json) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    // ec on create_directories is non-fatal if the dir already exists; the
    // subsequent file open will fail loud if the dir is genuinely absent.

    // Temp filename uses PID to avoid collisions across concurrent
    // activations of the same machine (rare, but possible if a sysadmin
    // re-runs activate during init).  Deterministic random not needed.
    std::random_device rd;
    const auto suffix = std::to_string(rd()) + "." + std::to_string(rd());
    const auto tmp = path.parent_path() / (path.filename().string() + ".tmp." + suffix);

    // every byte on disk (write, fsync, size, close checked) before the rename: a full disk must not leave
    // a short .lic in place of the activated one
    if (!durable_write_file(tmp, raw_json)) {
        return false;
    }
    // A failed rename leaves the previous .lic in place (durable_replace never removes it first).
    return durable_replace(tmp, path);
}

}  // namespace visio::license
