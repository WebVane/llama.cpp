// Public façade for the license subsystem.
//
// Two call sites in the OCR server (per DESIGN.md §2):
//   * Startup (after AppConfig::load): LicenseLib::instance().init(cfg).
//     Fatal on !ok unless trial fallback is acceptable.
//   * Per-request advice in Routes.cpp: auto d = ...authorize_request();
//     if (!d.allow) return 403 with d.reason.
//
// Construction is via singleton — there is exactly one license state per
// process, and it carries non-trivial in-memory caches (parsed license,
// counter file handle) plus the synchronous activation client.
//
// Tests reset/seed the singleton via the test-only helpers at the bottom
// of the public API.  Production callers never touch those.
#pragma once

#include "license/Activation.h"
#include "license/Fingerprint.h"
#include "license/Http.h"
#include "license/Keyring.h"
#include "license/License.h"
#include "license/RequestCounter.h"

#include <optional>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <optional>
#include <string>
#include <string_view>

namespace visio::license {

enum class Mode {
    None,        // init() never ran or failed without a fallback
    Trial,       // legacy Trial.cpp counter — PROVISION_TRIAL=ON build
    Offline,     // .lic without fingerprint
    Online,      // .lic with fingerprint
    Token,       // optional integrator-signed token bundle
};

const char* mode_label(Mode m) noexcept;

enum class DenyReason {
    None,
    NotInitialised,
    NoLicense,
    BadSignature,
    UnknownKid,
    FingerprintMismatch,
    Expired,
    NotYetValid,
    RequestLimitReached,
    TamperedCounter,
    ClockRollback,
    LicHashMismatch,
    UnstableFingerprint,
    ProductMismatch,
    ClientMismatch,
    TokenInvalid,
};

const char* deny_reason_label(DenyReason r) noexcept;

struct Decision {
    bool       allow = false;
    DenyReason reason = DenyReason::None;
    Mode       mode = Mode::None;
    int64_t    used = 0;
    int64_t    limit = 0;
    std::chrono::system_clock::time_point expires_at{};
    std::string detail;  // logged, surfaced to operators on 4xx
};

struct Config {
    std::string product;             // baked at CI; e.g. "OCR"
    std::string client_id;           // baked at CI; UUID
    std::string activation_url;      // e.g. "https://license.provlabs.tech"
    std::filesystem::path state_dir; // /var/lib/provision-llm
    std::filesystem::path lic_path;  // state_dir / "license.lic" by default
    std::filesystem::path counter_path; // state_dir / "counter.dat" by default
    std::filesystem::path token_bundle_path; // state_dir / "tokens.json" (optional)
    int64_t trial_limit = 50;
    bool require_stable_cpu_id = true;   // fail init on MAC-fallback CPU_ID
    std::chrono::seconds clock_skew{60}; // applied to time window checks

    // Production-default factory.  Pulls every field from the
    // CI-supplied compile-time defines (PROVISION_LICENSE_CLIENT_ID,
    // PROVISION_LICENSE_PRODUCT, PROVISION_LICENSE_ACTIVATION_URL,
    // PROVISION_TRIAL_LIMIT) with env overrides:
    //   PROVISION_LICENSE_STATE_DIR      → state_dir (default /var/lib/provision-llm
    //                                  on Linux, %PROGRAMDATA%\ProvisionLLM on Win)
    //   PROVISION_LICENSE_PRODUCT        → product (overrides compile default)
    //   PROVISION_LICENSE_CLIENT_ID      → client_id
    //   PROVISION_LICENSE_ACTIVATION_URL → activation_url
    //   PROVISION_LICENSE_TRIAL_LIMIT    → trial_limit (decimal)
    //   PROVISION_LICENSE_REQUIRE_STABLE_CPU_ID=0 disables the stability gate.
    //
    // main.cpp uses this once at startup; tests pass a hand-built Config
    // and never invoke this factory.
    static Config from_defaults();
};

struct InitResult {
    bool ok = false;
    Mode mode = Mode::None;
    std::string detail;
};

class LicenseLib {
public:
    static LicenseLib& instance();

    // Resolve mode, load + verify .lic, prepare the request counter.
    // Idempotent: subsequent calls re-run resolution (useful after
    // activate() updates the .lic file).
    InitResult init(Config cfg);

    // Online activation.  Wraps ActivationClient::activate(), checks the
    // returned .lic belongs to this build (product / clientId) BEFORE it
    // replaces the installed one, binds the request counter — keeping the
    // licence's existing tally when it has one, see RequestCounter.h —
    // then re-runs init() to publish the new state.  Never throws: any
    // internal failure comes back as ok=false with the reason in detail.
    // Idempotent on the wire
    // (same Idempotency-Key for the same machine + code).  Blocking, but
    // the round-trip itself runs without the facade lock, so the request
    // gate keeps answering while the License Server is slow.
    InitResult activate(std::string_view activation_code,
                        std::chrono::milliseconds timeout = std::chrono::seconds{15});

    // Hot path — per inference request.  Consumes `units` counter slots
    // (one per result the request produces) when limits.requestLimit > 0;
    // all-or-nothing, so a batch that does not fit is refused whole.
    // O(1) on the unlimited path, ~one rename(2) on the limited path.
    // Call it BEFORE running the request (reserve) and refund_request()
    // when the result is not delivered — that is what makes the last slot
    // safe against concurrent requests.
    Decision authorize_request(int64_t units = 1);

    // Give reserved slots back (handler error, client gone before the
    // result).  No-op in modes without a counter.  Returns false when the
    // counter refused (fail-closed) or could not persist.
    bool refund_request(int64_t units = 1);

    // Snapshot without consuming anything.  Used by /license/status and
    // the request pre-check.  The only mutation it performs is rolling the
    // counter's 30-day window forward when it has expired, so an
    // exhausted budget shows as reset without waiting for the next
    // consume() or a restart.
    Decision check_license();

    Mode mode() const;
    std::optional<License> current() const;
    // Read-only snapshot of the request counter (used / limit /
    // window_start_unix / lic_hash).  std::nullopt when no counter is
    // bound (e.g. mode=none).  Used by /license/status to surface
    // `windowResetsAt`.
    std::optional<CounterSnapshot> counter_snapshot() const;

    // -------- Test seams (do not call from production code) --------
    // Clear singleton state — equivalent to "process restart" for tests.
    void reset_for_test();
    // Inject keyring.  In production init() pulls from Keyring::baked().
    void set_keyring_for_test(Keyring kr);
    // Inject HTTP transport (replaces the cpp-httplib backend).
    void set_activation_transport_for_test(HttpTransport t);
    // Freeze the wall clock.  The golden .lic vectors carry fixed
    // expiresAt dates (2026-07-01) — tests pin "now" inside that window
    // so they don't start failing the day the vectors age out.
    // Cleared by reset_for_test().
    void set_now_for_test(std::function<std::chrono::system_clock::time_point()> fn);

private:
    LicenseLib() = default;
    LicenseLib(const LicenseLib&) = delete;
    LicenseLib& operator=(const LicenseLib&) = delete;

    Mode resolve_mode_impl(const Config&,
                           std::string& detail_out) const;
    InitResult init_impl(Config cfg);
    InitResult activate_impl(std::string_view activation_code,
                             std::chrono::milliseconds timeout);
    InitResult init_offline_or_online(Config cfg, std::string raw_lic);
    InitResult init_trial(Config cfg);
    InitResult init_token(Config cfg);

    // Reader/writer lock: check_license() runs in Drogon's preHandling
    // advice on EVERY request — as a plain mutex it serialized all IO
    // event-loop threads behind the license gate.  Read-only paths
    // (check_license / mode / current / counter_snapshot) take a shared
    // lock; mutating paths (init / activate's publish step /
    // authorize_request which consumes the counter / test seams) take an
    // exclusive one.  activate() deliberately drops mu_ for the License
    // Server round-trip — see the comment in LicenseLib.cpp.
    mutable std::shared_mutex mu_;
    // Serialises concurrent activate() calls without touching mu_.
    std::mutex activate_mu_;
    Config cfg_;
    Keyring keyring_;
    Mode    mode_ = Mode::None;
    std::optional<License> lic_;
    std::optional<RequestCounter> counter_;
    std::optional<Fingerprint> fingerprint_;
    FingerprintSources fp_sources_;
    std::optional<HttpTransport> activation_transport_override_;
    // Test-only clock override; production always uses system_clock.
    std::function<std::chrono::system_clock::time_point()> now_override_;
    std::chrono::system_clock::time_point now() const {
        return now_override_ ? now_override_()
                             : std::chrono::system_clock::now();
    }
    bool initialised_ = false;
};

}  // namespace visio::license
