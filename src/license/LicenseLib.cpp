#include "license/LicenseLib.h"

#include "license/Jcs.h"
#include "license/Sha256.h"

#include "log.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace visio::license {
namespace {

namespace fs = std::filesystem;

std::optional<std::string> read_file_bytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
}

std::string hex_lower(const uint8_t* d, std::size_t n) {
    std::ostringstream o;
    o << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < n; ++i) o << std::setw(2) << static_cast<unsigned>(d[i]);
    return o.str();
}

// SHA-256 hex of canonical .lic bytes — used to bind RequestCounter to a
// specific license file.  Idempotent on canonical input, defensive
// against pretty-printer variation otherwise.
std::optional<std::string> compute_lic_hash(std::string_view raw) {
    try {
        auto j = nlohmann::json::parse(raw);
        auto canon = jcs_canonicalize(j);
        auto h = sha256(canon);
        return hex_lower(h.data(), h.size());
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// Does this licence belong to this build?  One definition for both places
// that ask: init() — a .lic already on disk — and activate(), which must ask
// BEFORE it overwrites the installed licence.  It used to write first and let
// init() reject afterwards, so a correctly signed .lic for another product
// (or another clientId, when PROVISION_LICENSE_CLIENT_ID is set) replaced a
// working licence and left the server locked, with the old file gone.
std::optional<std::string> identity_mismatch(const License& L,
                                             const std::string& product,
                                             const std::string& client_id) {
    if (L.product != product) {
        return "lic product='" + L.product + "' does not match build product='" +
               product + "'";
    }
    if (!client_id.empty() && L.client_id != client_id) {
        return "lic clientId='" + L.client_id + "' does not match build clientId='" +
               client_id + "'";
    }
    return std::nullopt;
}

// Sentinel lic_hash for trial mode — stable per (product), so trial
// counters survive process restarts even though no actual .lic exists.
std::string trial_lic_hash(std::string_view product) {
    std::string seed = "TRIAL/v2/";
    seed.append(product.data(), product.size());
    auto h = sha256(seed);
    return hex_lower(h.data(), h.size());
}

}  // namespace

const char* mode_label(Mode m) noexcept {
    switch (m) {
        case Mode::None:    return "none";
        case Mode::Trial:   return "trial";
        case Mode::Offline: return "offline";
        case Mode::Online:  return "online";
        case Mode::Token:   return "token";
    }
    return "unknown";
}

const char* deny_reason_label(DenyReason r) noexcept {
    switch (r) {
        case DenyReason::None:                return "none";
        case DenyReason::NotInitialised:      return "not_initialised";
        case DenyReason::NoLicense:           return "no_license";
        case DenyReason::BadSignature:        return "bad_signature";
        case DenyReason::UnknownKid:          return "unknown_kid";
        case DenyReason::FingerprintMismatch: return "fingerprint_mismatch";
        case DenyReason::Expired:             return "expired";
        case DenyReason::NotYetValid:         return "not_yet_valid";
        case DenyReason::RequestLimitReached: return "request_limit_reached";
        case DenyReason::TamperedCounter:     return "tampered_counter";
        case DenyReason::ClockRollback:       return "clock_rollback";
        case DenyReason::LicHashMismatch:     return "lic_hash_mismatch";
        case DenyReason::UnstableFingerprint: return "unstable_fingerprint";
        case DenyReason::ProductMismatch:     return "product_mismatch";
        case DenyReason::ClientMismatch:      return "client_mismatch";
        case DenyReason::TokenInvalid:        return "token_invalid";
    }
    return "unknown";
}

namespace {

const char* env_or(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return (v && *v) ? v : fallback;
}

std::filesystem::path platform_default_state_dir() {
#if defined(_WIN32)
    if (const char* pd = std::getenv("ProgramData"); pd && *pd) {
        return std::filesystem::path{pd} / "ProvisionLLM";
    }
    return std::filesystem::path{"C:\\ProgramData\\ProvisionLLM"};
#else
    return std::filesystem::path{"/var/lib/provision-llm"};
#endif
}

}  // namespace

Config Config::from_defaults() {
    Config c;
    c.product = env_or("PROVISION_LICENSE_PRODUCT",
#ifdef PROVISION_LICENSE_PRODUCT
        PROVISION_LICENSE_PRODUCT
#else
        "LLM"
#endif
    );
    // clientId is intentionally NOT a compile-time bake — per-tenant
    // image builds would be a nightmare and the .lic signature is the
    // real cryptographic anchor.  Runtime env override is still honoured
    // for operators who want the defense-in-depth clientId match
    // (init_offline_or_online compares L.client_id against cfg.client_id
    // only when the latter is non-empty).
    c.client_id = env_or("PROVISION_LICENSE_CLIENT_ID", "");
    c.activation_url = env_or("PROVISION_LICENSE_ACTIVATION_URL",
#ifdef PROVISION_LICENSE_ACTIVATION_URL
        PROVISION_LICENSE_ACTIVATION_URL
#else
        ""
#endif
    );
    if (const char* sd = std::getenv("PROVISION_LICENSE_STATE_DIR"); sd && *sd) {
        c.state_dir = sd;
    } else {
        c.state_dir = platform_default_state_dir();
    }
    c.lic_path = c.state_dir / "license.lic";
    c.counter_path = c.state_dir / "counter.dat";
    c.token_bundle_path = c.state_dir / "tokens.json";
    if (const char* tl = std::getenv("PROVISION_LICENSE_TRIAL_LIMIT"); tl && *tl) {
        try { c.trial_limit = std::stoll(tl); } catch (...) {}
    } else {
#ifdef PROVISION_TRIAL_LIMIT
        c.trial_limit = PROVISION_TRIAL_LIMIT;
#endif
    }
    if (const char* g = std::getenv("PROVISION_LICENSE_REQUIRE_STABLE_CPU_ID");
        g && (g[0] == '0' || g[0] == 'n' || g[0] == 'N')) {
        c.require_stable_cpu_id = false;
    }
    return c;
}

LicenseLib& LicenseLib::instance() {
    static LicenseLib lib;
    return lib;
}

void LicenseLib::reset_for_test() {
    std::lock_guard<std::shared_mutex> lk(mu_);
    cfg_ = Config{};
    mode_ = Mode::None;
    lic_.reset();
    counter_.reset();
    fingerprint_.reset();
    fp_sources_ = FingerprintSources{};
    activation_transport_override_.reset();
    now_override_ = nullptr;
    initialised_ = false;
}

void LicenseLib::set_keyring_for_test(Keyring kr) {
    std::lock_guard<std::shared_mutex> lk(mu_);
    keyring_ = std::move(kr);
}

void LicenseLib::set_activation_transport_for_test(HttpTransport t) {
    std::lock_guard<std::shared_mutex> lk(mu_);
    activation_transport_override_ = std::move(t);
}

void LicenseLib::set_now_for_test(
        std::function<std::chrono::system_clock::time_point()> fn) {
    std::lock_guard<std::shared_mutex> lk(mu_);
    now_override_ = std::move(fn);
}

Mode LicenseLib::mode() const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    return mode_;
}

std::optional<License> LicenseLib::current() const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    return lic_;
}

std::optional<CounterSnapshot> LicenseLib::counter_snapshot() const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    if (!counter_) return std::nullopt;
    return counter_->snapshot();
}

InitResult LicenseLib::init(Config cfg) {
    std::lock_guard<std::shared_mutex> lk(mu_);
    return init_impl(std::move(cfg));
}

Mode LicenseLib::resolve_mode_impl(const Config& cfg,
                                   std::string& detail_out) const {
    // 1. env override
    if (const char* env = std::getenv("PROVISION_LICENSE_MODE"); env && *env) {
        std::string v{env};
        if (v == "trial")   return Mode::Trial;
        if (v == "online")  return Mode::Online;
        if (v == "offline") return Mode::Offline;
        if (v == "token")   return Mode::Token;
        detail_out = "unknown PROVISION_LICENSE_MODE='" + v + "'";
        return Mode::None;
    }

    // 2. .lic — distinguish online (has `fingerprint`) from offline
    std::error_code ec;
    if (fs::exists(cfg.lic_path, ec)) {
        auto raw = read_file_bytes(cfg.lic_path);
        if (raw) {
            try {
                auto j = nlohmann::json::parse(*raw);
                if (j.is_object()) {
                    return j.contains("fingerprint") ? Mode::Online : Mode::Offline;
                }
            } catch (...) {
                // fall through to next source — malformed file is not a
                // resolver vote; init_offline_or_online would reject anyway.
            }
        }
    }

    // 3. token bundle
    if (!cfg.token_bundle_path.empty() && fs::exists(cfg.token_bundle_path, ec)) {
        return Mode::Token;
    }

    // 4. build-time trial fallback
#ifdef PROVISION_TRIAL
    return Mode::Trial;
#else
    detail_out = "no license sources found (no .lic, no tokens, no trial build)";
    return Mode::None;
#endif
}

InitResult LicenseLib::init_impl(Config cfg) {
    // Reset prior state.
    mode_ = Mode::None;
    lic_.reset();
    counter_.reset();
    initialised_ = false;

    // Compute fingerprint up front; it informs both stability checks
    // and downstream online verification.  Pass cfg.state_dir so the
    // chain's terminal link (persisted-state-id at
    // <state_dir>/machine-id) can fire on hosts without a stable
    // hardware machine-id (Docker Desktop / WSL2 / some Windows VDIs).
    FingerprintSources src;
    auto fp = Fingerprint::from_system(cfg.client_id, cfg.product,
                                       cfg.state_dir, &src);
    fp_sources_ = src;
    fingerprint_ = fp;

    LOG_INF("[license] fingerprint: source=%s cpu_id=%s... host=%s client_id=%s product=%s fp=%s\n",
            cpu_id_source_label(src.cpu_id_source),
            src.cpu_id.substr(0, std::min<std::size_t>(8, src.cpu_id.size())).c_str(),
            src.hostname.c_str(), src.client_id.c_str(), src.product.c_str(), fp.hex().c_str());

    // Default the trusted keyring to the CI-baked one (Keyring::baked())
    // in production.  Tests override via set_keyring_for_test() — that
    // populates keyring_ before init() runs, so the size>0 check skips
    // the default here.  Without this, verify_license_blob() ran against
    // an EMPTY keyring_ and rejected every .lic with UnknownKid — the
    // server-issued kid was never even looked up.
    if (keyring_.size() == 0) {
        keyring_ = Keyring::baked();
    }

    // Surface which kids made it into the baked keyring.  Helps diagnose
    // "lic rejected (UnknownKid)" — if the operator sees an empty list
    // or a kid set that doesn't include the one the License Server
    // signs with, the build's keyring secret was wrong.
    {
        const auto kids = keyring_.kids();
        if (kids.empty()) {
            LOG_WRN("%s", "[license] baked keyring is EMPTY - every signed .lic will "
                          "fail verify with UnknownKid (build was missing "
                          "LLAMA_SERVER_LICENSE_KEYRING_JSON at configure time)\n");
        } else {
            std::string list;
            for (std::size_t i = 0; i < kids.size(); ++i) {
                if (i) list += ", ";
                list += kids[i];
            }
            LOG_INF("[license] baked keyring kids: [%s] (%zu trusted key%s)\n",
                    list.c_str(), kids.size(), kids.size() == 1 ? "" : "s");
        }
    }

    if (!cpu_id_source_stable(src.cpu_id_source)) {
        const std::string msg =
            std::string{"CPU_ID source '"} + cpu_id_source_label(src.cpu_id_source) +
            "' is not stable across reboots — set PROVISION_LICENSE_CPU_ID_OVERRIDE "
            "or bind-mount a stable /etc/machine-id";
        if (cfg.require_stable_cpu_id) {
            LOG_ERR("[license] %s\n", msg.c_str());
            cfg_ = std::move(cfg);
            return {false, Mode::None, msg};
        }
        LOG_WRN("[license] %s (proceeding because require_stable_cpu_id=false)\n", msg.c_str());
    }

    cfg_ = cfg;

    std::string detail;
    const Mode m = resolve_mode_impl(cfg_, detail);
    switch (m) {
        case Mode::Trial:   return init_trial(cfg_);
        case Mode::Online:
        case Mode::Offline: {
            auto raw = read_file_bytes(cfg_.lic_path);
            if (!raw) {
                return {false, Mode::None,
                        "lic file declared but unreadable: " + cfg_.lic_path.string()};
            }
            return init_offline_or_online(cfg_, *raw);
        }
        case Mode::Token:   return init_token(cfg_);
        case Mode::None:
        default:
            return {false, Mode::None, detail.empty() ? "mode resolution failed" : detail};
    }
}

InitResult LicenseLib::init_offline_or_online(Config cfg, std::string raw_lic) {
    auto verdict = verify_license_blob(
        raw_lic, keyring_, now(), cfg.clock_skew);
    if (!verdict.ok) {
        DenyReason r;
        switch (verdict.reason) {
            case LicenseReason::BadSignature:
            case LicenseReason::BadSignatureFormat: r = DenyReason::BadSignature; break;
            case LicenseReason::UnknownKid:         r = DenyReason::UnknownKid; break;
            case LicenseReason::Expired:            r = DenyReason::Expired; break;
            case LicenseReason::NotYetValid:        r = DenyReason::NotYetValid; break;
            default: r = DenyReason::BadSignature; break;
        }
        // Surface the SPECIFIC LicenseReason from verify (UnknownKid /
        // BadSignature / MissingField / ProductMismatch / ...) instead
        // of just the umbrella DenyReason.  Lets the operator distinguish
        // "the kid isn't in our keyring" (build / kid-rotation issue)
        // from "the signature itself doesn't verify" (key mismatch /
        // canonicalisation drift).
        return {false, Mode::None,
                "lic rejected (reason=" + std::string{deny_reason_label(r)} +
                "; verify=" + std::string{license_reason_label(verdict.reason)} +
                "; detail=" + verdict.detail + ")"};
    }

    const License& L = *verdict.lic;

    // Cross-check baked product / client_id.
    if (auto why = identity_mismatch(L, cfg.product, cfg.client_id)) {
        return {false, Mode::None, *why};
    }

    // Online: fingerprint must match local.
    const bool online = L.fingerprint.has_value();
    if (online) {
        if (!fingerprint_.has_value() ||
            *L.fingerprint != fingerprint_->hex()) {
            return {false, Mode::None,
                    "online .lic fingerprint does not match local machine"};
        }
    }

    // Bind the request counter.
    auto lic_hash = compute_lic_hash(raw_lic);
    if (!lic_hash) {
        return {false, Mode::None, "failed to canonicalize lic for hash binding"};
    }
    const int64_t limit = L.limits.request_limit.value_or(0);
    counter_.emplace(cfg.counter_path, *lic_hash, limit);
    auto status = counter_->load(now());
    // counter.dat missing or unreadable for the licence that IS installed:
    // take its tally from the ledger instead of starting it over.  Before
    // this, deleting counter.dat and restarting handed out a fresh budget.
    // LicHashMismatch and Rollback deliberately stay fail-closed — the first
    // is a swap back to another .lic (possibly one the vendor revoked), the
    // second a clock moved back; neither is something to paper over.
    if ((status == CounterLoad::FreshState || status == CounterLoad::Tampered ||
         status == CounterLoad::IoError) &&
        counter_->restore_from_ledger(now())) {
        LOG_INF("[license] counter.dat %s - request tally restored from the ledger\n",
                status == CounterLoad::FreshState ? "missing" : "unreadable");
        status = CounterLoad::Ok;
    }
    if (status != CounterLoad::Ok && status != CounterLoad::FreshState) {
        // load() leaves the counter fail-closed; that's surfaced through
        // authorize_request().  We still treat init as "ok" so the
        // facade can report the specific reason per-request.
        LOG_WRN("[license] counter file load status=%d\n", static_cast<int>(status));
    }

    lic_ = L;
    mode_ = online ? Mode::Online : Mode::Offline;
    initialised_ = true;
    return {true, mode_,
            "loaded " + std::string{mode_label(mode_)} +
            " license, limit=" + std::to_string(limit)};
}

InitResult LicenseLib::init_trial(Config cfg) {
    if (cfg.trial_limit <= 0) {
        return {false, Mode::None, "trial mode requires trial_limit > 0"};
    }
    const fs::path trial_path =
        cfg.counter_path.empty()
            ? cfg.state_dir / "trial.dat"
            : cfg.counter_path;
    counter_.emplace(trial_path, trial_lic_hash(cfg.product), cfg.trial_limit,
                     /*hard_limit*/ true);
    const auto status = counter_->load(now());
    if (status != CounterLoad::Ok && status != CounterLoad::FreshState) {
        LOG_WRN("[license] trial counter load status=%d\n", static_cast<int>(status));
    }
    mode_ = Mode::Trial;
    initialised_ = true;
    return {true, Mode::Trial, "trial mode (limit=" + std::to_string(cfg.trial_limit) + ")"};
}

InitResult LicenseLib::init_token(Config cfg) {
    // Stub: detection works (mode resolution returns Token when the
    // bundle file is present), but per-request authorization will deny
    // until the full token verification path is implemented.  Documented
    // in DESIGN.md §3 as the "optional Token mode" — out of scope for
    // the C4 milestone but kept in the resolver chain so the file format
    // can be designed against a known dispatch.
    (void)cfg;
    mode_ = Mode::Token;
    initialised_ = true;
    return {true, Mode::Token,
            "token bundle detected — verification not implemented (stub)"};
}

InitResult LicenseLib::activate(std::string_view activation_code,
                                std::chrono::milliseconds timeout) {
    // Every caller — the /license/activate thread and the startup
    // PROVISION_LICENSE_ACTIVATION_CODE path — treats this as a function
    // that reports failure, not one that throws.  It did throw: a code that
    // is not valid UTF-8 made the request serialiser raise, the exception
    // left the detached HTTP thread, and std::terminate took the server down
    // on ONE request.  Whatever goes wrong inside is now an ordinary failed
    // activation with the reason in detail.
    try {
        return activate_impl(activation_code, timeout);
    } catch (const std::exception& e) {
        LOG_ERR("[license] activation aborted by an internal error: %s\n", e.what());
        return {false, mode(), std::string{"activation failed: internal error: "} + e.what()};
    } catch (...) {
        LOG_ERR("%s", "[license] activation aborted by an unknown internal error\n");
        return {false, mode(), "activation failed: unknown internal error"};
    }
}

InitResult LicenseLib::activate_impl(std::string_view activation_code,
                                     std::chrono::milliseconds timeout) {
    // Activations are serialised among themselves (two at once would
    // race the .lic write and the counter reset) but must NOT hold mu_
    // across the License Server round-trip: with retries that is up to
    // ~45 s during which every check_license() in the request gate —
    // i.e. every /processing call — would block on the shared lock.
    std::lock_guard<std::mutex> serial(activate_mu_);

    // Phase 1: snapshot the inputs of the network call under a shared
    // lock.  The client keeps a reference to the keyring, so it gets its
    // own copy that lives for the duration of the call.
    ActivationClient::Config ac;
    Keyring keyring;
    std::optional<Fingerprint> fingerprint;
    std::optional<HttpTransport> transport;
    std::chrono::system_clock::time_point at;
    {
        std::shared_lock<std::shared_mutex> lk(mu_);
        if (!fingerprint_.has_value()) {
            // init() was never called.  Caller is supposed to init first
            // and then call activate when the user provides a code.
            return {false, Mode::None,
                    "activate() before init() — fingerprint unknown"};
        }
        ac.activation_url = cfg_.activation_url;
        ac.timeout = timeout;
        ac.max_retries = 2;
        ac.retry_backoff_base = std::chrono::milliseconds{200};
        keyring     = keyring_;
        fingerprint = fingerprint_;
        transport   = activation_transport_override_;
        at          = now();
    }

    // Phase 2: the blocking round-trip, no facade lock held.
    // Log both sides of it.  Until now activate() was silent end to end,
    // so a failed activation left the operator with a single `detail`
    // string and no way to tell which server was called or which
    // Idempotency-Key to quote when asking the License Server side to
    // look up the request.  The activation CODE is never logged — it is
    // the secret here; the key derived from it is safe and is exactly
    // what correlates the two sides.
    ActivationClient client{ac, keyring};
    if (transport) client.set_transport(*transport);
    LOG_INF("[license] activating against %s (idempotency-key %s)\n",
            ac.activation_url.empty() ? "<activation URL not configured>" : ac.activation_url.c_str(),
            client.idempotency_key_for(activation_code, *fingerprint).c_str());
    const auto result = client.activate(activation_code, *fingerprint, at);
    if (result.status == ActivationStatus::Ok) {
        LOG_INF("[license] activation accepted (HTTP %d)\n", result.http_code);
    } else {
        LOG_ERR("[license] activation failed: status=%d http=%d detail=%s\n",
                static_cast<int>(result.status), result.http_code, result.detail.c_str());
    }

    // Phase 3: publish — exclusive lock for the state mutation only.
    std::lock_guard<std::shared_mutex> lk(mu_);
    if (result.status != ActivationStatus::Ok) {
        return {false, mode_,
                "activation failed (status=" +
                std::to_string(static_cast<int>(result.status)) +
                "; detail=" + result.detail + ")"};
    }

    // The licence has to belong to this build before it may replace the one
    // installed — see identity_mismatch().  Nothing on disk has been touched
    // yet, so a refusal here leaves the working licence and its tally intact.
    if (auto why = identity_mismatch(*result.lic, cfg_.product, cfg_.client_id)) {
        LOG_ERR("[license] activation refused, licence is not for this build: %s\n", why->c_str());
        return {false, mode_, "activation refused: " + *why};
    }

    // Persist new .lic atomically.
    if (!ActivationClient::save_lic_atomic(cfg_.lic_path, result.raw_lic_json)) {
        return {false, mode_,
                "failed to persist .lic to " + cfg_.lic_path.string()};
    }

    // Bind the counter to the licence just installed.  Without a rebind the
    // next load() would report LicHashMismatch and fail closed; HOW it is
    // rebound — kept, restored from the ledger, or fresh — is decided below.
    auto lic_hash = compute_lic_hash(result.raw_lic_json);
    if (!lic_hash) {
        return {false, mode_, "failed to canonicalize fresh .lic for hash binding"};
    }
    const int64_t limit = result.lic->limits.request_limit.value_or(0);
    counter_.emplace(cfg_.counter_path, *lic_hash, limit);
    // Re-entering the SAME activation code must not hand out a fresh budget.
    // The License Server answers a repeat of one code from one machine with the
    // IDENTICAL .lic — "тот же fingerprint → идемпотентно тот же .lic"
    // (docs/openapi.json) — so an unconditional reset here meant an operator
    // could zero `used` and re-anchor the 30-day window on demand, simply by
    // activating again.  Nothing else stopped it: the allowlist governs WHO may
    // call activate, not how often, and the counter HMAC only detects hand-edits.
    // So: keep the tally when the counter on disk is bound to this very licence
    // and loads cleanly, and reset only when the licence genuinely changed (a
    // different lic_hash loads as LicHashMismatch) or when there is nothing
    // usable to keep (FreshState / Tampered / Rollback / IoError).  The reset
    // in those cases is what binds the counter to the new .lic, which is what
    // this call was always for — see RequestCounter.h.
    const CounterLoad existing = counter_->load(now());
    if (existing == CounterLoad::Ok) {
        const auto snap = counter_->snapshot();
        LOG_INF("[license] activation returned the licence already installed"
                " - request tally kept (%lld of %lld used)\n",
                (long long) snap.used, (long long) snap.limit);
    } else if (counter_->restore_from_ledger(now())) {
        // A licence this machine has counted before — reached back through
        // another licence (A -> B -> A) or with counter.dat gone or damaged.
        // It gets its real tally back, not a fresh budget.
        const auto snap = counter_->snapshot();
        LOG_INF("[license] activation of a previously used licence"
                " - request tally restored from the ledger (%lld of %lld used)\n",
                (long long) snap.used, (long long) snap.limit);
    } else {
        // Genuinely new to this machine: fresh budget, window anchored now.
        counter_->reset(now());
    }

    // Re-resolve mode + cache.
    // init_impl() would re-run mode resolution AND recompute the
    // fingerprint (no-op since cfg unchanged).
    Config cfg_copy = cfg_;
    auto r = init_impl(std::move(cfg_copy));
    return r;
}

Decision LicenseLib::authorize_request(int64_t units) {
    std::lock_guard<std::shared_mutex> lk(mu_);
    Decision d;
    d.mode = mode_;
    // When init() ran but found nothing usable (no .lic, no token bundle,
    // no Trial fallback) the facade stays in a consistent `mode=None`
    // state — server keeps running, business endpoints get a clean
    // NoLicense deny here, diagnostic endpoints stay reachable.  We
    // treat that as equivalent to "uninitialised from the caller's POV":
    // both surface as NoLicense so the operator gets one consistent
    // failure mode regardless of whether init() was skipped entirely
    // or just produced no source.
    if (mode_ == Mode::None) {
        d.reason = DenyReason::NoLicense;
        d.detail = initialised_
            ? "no usable license loaded"
            : "license subsystem not initialised";
        return d;
    }
    if (!initialised_) {
        d.reason = DenyReason::NotInitialised;
        d.detail = "init() not called";
        return d;
    }

    const auto now = this->now();

    auto fill_from_counter = [&] {
        if (counter_) {
            auto s = counter_->snapshot();
            d.used = s.used;
            d.limit = s.limit;
        }
    };

    auto reason_from_counter = [&](CounterLoad cl) {
        switch (cl) {
            case CounterLoad::Tampered:        return DenyReason::TamperedCounter;
            case CounterLoad::Rollback:        return DenyReason::ClockRollback;
            case CounterLoad::LicHashMismatch: return DenyReason::LicHashMismatch;
            default:                            return DenyReason::RequestLimitReached;
        }
    };

    switch (mode_) {
        case Mode::None:
            d.reason = DenyReason::NoLicense;
            return d;
        case Mode::Token:
            d.reason = DenyReason::TokenInvalid;
            d.detail = "token mode verification not implemented";
            return d;
        case Mode::Trial: {
            if (!counter_) { d.reason = DenyReason::NotInitialised; return d; }
            const bool ok = counter_->consume(now, units);
            fill_from_counter();
            if (!ok) {
                d.reason = reason_from_counter(counter_->last_load_status());
            } else {
                d.allow = true;
            }
            return d;
        }
        case Mode::Offline:
        case Mode::Online: {
            if (!lic_) { d.reason = DenyReason::NoLicense; return d; }
            d.expires_at = lic_->expires_at;
            // Re-check time window — cheap, catches clock changes mid-process.
            if (now > lic_->expires_at + cfg_.clock_skew) {
                d.reason = DenyReason::Expired;
                return d;
            }
            if (now + cfg_.clock_skew < lic_->issued_at) {
                d.reason = DenyReason::NotYetValid;
                return d;
            }
            // Counter (only when a positive limit exists).
            if (counter_ && counter_->snapshot().limit > 0) {
                const bool ok = counter_->consume(now, units);
                fill_from_counter();
                if (!ok) {
                    d.reason = reason_from_counter(counter_->last_load_status());
                    return d;
                }
            } else {
                fill_from_counter();
            }
            d.allow = true;
            return d;
        }
    }
    d.reason = DenyReason::NoLicense;
    return d;
}

bool LicenseLib::refund_request(int64_t units) {
    std::lock_guard<std::shared_mutex> lk(mu_);
    if (!initialised_ || !counter_) return true;
    switch (mode_) {
        case Mode::Trial:
            return counter_->refund(now(), units);
        case Mode::Offline:
        case Mode::Online:
            if (counter_->snapshot().limit > 0) {
                return counter_->refund(now(), units);
            }
            return true;
        default:
            return true;
    }
}

Decision LicenseLib::check_license() {
    // Exclusive rather than shared: rolling the counter's 30-day window
    // forward mutates it.  The section is a few comparisons, so the lock
    // is not on any measurable path.
    std::lock_guard<std::shared_mutex> lk(mu_);
    Decision d;
    d.mode = mode_;
    // Same mode=None treatment as authorize_request — /license/status
    // must always produce a structured snapshot, never a "wait, what
    // happened?" answer.
    if (mode_ == Mode::None) {
        d.reason = DenyReason::NoLicense;
        return d;
    }
    if (!initialised_) {
        d.reason = DenyReason::NotInitialised;
        return d;
    }
    const auto now = this->now();
    if (counter_) {
        // Without this an exhausted licence stayed "request_limit_reached"
        // after the window had reset, because only consume() advanced the
        // window and the pre-check never let a request reach consume().
        (void)counter_->refresh(now);
        auto s = counter_->snapshot();
        d.used = s.used;
        d.limit = s.limit;
    }
    if (lic_) {
        d.expires_at = lic_->expires_at;
    }
    // Report whether the cached state would currently allow.
    if (lic_) {
        if (now > lic_->expires_at + cfg_.clock_skew) {
            d.reason = DenyReason::Expired;
            return d;
        }
        if (now + cfg_.clock_skew < lic_->issued_at) {
            d.reason = DenyReason::NotYetValid;
            return d;
        }
    }
    if (counter_) {
        const auto cl = counter_->last_load_status();
        if (cl == CounterLoad::Tampered)        { d.reason = DenyReason::TamperedCounter;  return d; }
        if (cl == CounterLoad::Rollback)        { d.reason = DenyReason::ClockRollback;    return d; }
        if (cl == CounterLoad::LicHashMismatch) { d.reason = DenyReason::LicHashMismatch;  return d; }
        if (d.limit > 0 && d.used >= d.limit)   { d.reason = DenyReason::RequestLimitReached; return d; }
    }
    if (mode_ == Mode::Token) {
        d.reason = DenyReason::TokenInvalid;
        return d;
    }
    d.allow = true;
    return d;
}

}  // namespace visio::license
