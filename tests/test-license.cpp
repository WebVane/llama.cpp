// Conformance + behaviour tests for the license subsystem (src/license) as
// built into provision-server: the interop core against the golden vectors
// (byte-identical JCS round-trip, Ed25519 verify), the request counter, the
// activation client with a mocked transport, and the LicenseLib facade in
// trial mode — the "N requests, then activation" flow the trial image ships.
//
// The vectors directory is passed by CMake (LICENSE_VECTORS_DIR); the golden
// .lic files are valid between 2026-06-01 and 2026-07-01, so every check pins
// "now" inside that window instead of trusting the wall clock.

#include "license/Activation.h"
#include "license/Base64Url.h"
#include "license/DurableWrite.h"
#include "license/Fingerprint.h"
#include "license/Jcs.h"
#include "license/Keyring.h"
#include "license/License.h"
#include "license/LicenseLib.h"
#include "license/RequestCounter.h"
#include "license/Sha256.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace visio::license;
using namespace std::chrono_literals;

static int n_failed = 0;

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            n_failed++; \
        } \
    } while (0)

static fs::path vectors_dir() {
#ifdef LICENSE_VECTORS_DIR
    return fs::path{LICENSE_VECTORS_DIR};
#else
    return fs::path{"src/license/docs/vectors"};
#endif
}

static std::string slurp(const fs::path & p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
}

static std::chrono::system_clock::time_point utc(int year, int month, int day) {
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon  = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = 12;
#if defined(_WIN32)
    return std::chrono::system_clock::from_time_t(_mkgmtime(&tm));
#else
    return std::chrono::system_clock::from_time_t(timegm(&tm));
#endif
}

// inside both golden .lic windows (issuedAt 2026-06-01, expiresAt 2026-07-01)
static std::chrono::system_clock::time_point t_inside() {
    return utc(2026, 6, 15);
}

static void set_env(const char * key, const char * value) {
#if defined(_WIN32)
    _putenv_s(key, value);
#else
    setenv(key, value, 1);
#endif
}

static Keyring test_keyring() {
    Keyring kr;
    const auto doc = slurp(vectors_dir() / "keyring.json");
    CHECK(!doc.empty());
    CHECK(kr.load_json(doc));
    CHECK(kr.size() == 1);
    return kr;
}

static std::string trim_right(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) {
        s.pop_back();
    }
    return s;
}

static fs::path make_sandbox(const char * name) {
    const auto root = fs::temp_directory_path() / (std::string("provision-license-test-") + name);
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
    return root;
}

// ---------------------------------------------------------------------------
// interop core
// ---------------------------------------------------------------------------

static void test_base64url() {
    struct C { std::string in; std::vector<uint8_t> out; };
    const std::vector<C> cases = {
        {"",       {}},
        {"Zg",     {0x66}},
        {"Zm8",    {0x66, 0x6F}},
        {"Zm9v",   {0x66, 0x6F, 0x6F}},
        {"Zm9vYg", {0x66, 0x6F, 0x6F, 0x62}},
        {"_w",     {0xFF}},
        {"-w",     {0xFB}},
    };
    for (const auto & c : cases) {
        const auto r = base64url_decode(c.in);
        CHECK(r.has_value());
        CHECK(r.has_value() && *r == c.out);
    }
    CHECK(!base64url_decode("A").has_value());
    CHECK(!base64url_decode("AB!D").has_value());
    CHECK(!base64url_decode("A+B/").has_value());
}

static void test_sha256() {
    // FIPS 180-4 B.1: SHA-256("abc")
    const auto h = sha256(std::string_view{"abc"});
    const std::array<uint8_t, 32> expected = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
    CHECK(h == expected);
}

static void test_jcs_golden_roundtrip() {
    for (const char * name : {"online.payload.json", "offline.payload.json"}) {
        const auto raw = slurp(vectors_dir() / "golden" / name);
        CHECK(!raw.empty());
        const auto canon = jcs_canonicalize(nlohmann::json::parse(raw));
        CHECK(canon == trim_right(raw));
    }
}

static void test_verify_golden() {
    const auto kr = test_keyring();

    const auto online = slurp(vectors_dir() / "golden" / "online.lic");
    CHECK(!online.empty());
    auto d = verify_license_blob(online, kr, t_inside());
    CHECK(d.ok);
    CHECK(d.lic.has_value());
    if (d.lic) {
        CHECK(d.lic->product == "OCR");
        CHECK(d.lic->kid == "k3");
        CHECK(d.lic->fingerprint.has_value());
        CHECK(d.lic->limits.request_limit.has_value() && *d.lic->limits.request_limit == 100000);
    }

    const auto offline = slurp(vectors_dir() / "golden" / "offline.lic");
    d = verify_license_blob(offline, kr, t_inside());
    CHECK(d.ok);
    CHECK(d.lic.has_value() && !d.lic->fingerprint.has_value());

    // tampered signature
    {
        auto j = nlohmann::json::parse(online);
        std::string sig = j["signature"].get<std::string>();
        sig[0] = (sig[0] == 'A') ? 'B' : 'A';
        j["signature"] = sig;
        d = verify_license_blob(j.dump(), kr, t_inside());
        CHECK(!d.ok);
        CHECK(d.reason == LicenseReason::BadSignature);
    }
    // tampered payload: "more requests than paid for"
    {
        auto j = nlohmann::json::parse(online);
        j["limits"]["requestLimit"] = 999999;
        d = verify_license_blob(j.dump(), kr, t_inside());
        CHECK(!d.ok);
        CHECK(d.reason == LicenseReason::BadSignature);
    }
    // expired / not yet valid
    d = verify_license_blob(online, kr, utc(2027, 1, 1));
    CHECK(!d.ok && d.reason == LicenseReason::Expired);
    d = verify_license_blob(online, kr, utc(2025, 1, 1));
    CHECK(!d.ok && d.reason == LicenseReason::NotYetValid);
    // unknown kid (empty keyring)
    d = verify_license_blob(online, Keyring{}, t_inside());
    CHECK(!d.ok && d.reason == LicenseReason::UnknownKid);
    // garbage
    d = verify_license_blob("{not json", kr, t_inside());
    CHECK(!d.ok && d.reason == LicenseReason::BadJson);
}

static void test_fingerprint() {
    FingerprintSources s;
    s.cpu_id    = "cpu-1";
    s.hostname  = "host-1";
    s.client_id = "22222222-2222-2222-2222-222222222222";
    s.product   = "LLM";
    const auto a = Fingerprint::from_sources(s).hex();
    const auto b = Fingerprint::from_sources(s).hex();
    CHECK(a == b);
    CHECK(a.size() == 64);
    s.hostname = "host-2";
    CHECK(Fingerprint::from_sources(s).hex() != a);
}

// ---------------------------------------------------------------------------
// request counter
// ---------------------------------------------------------------------------

static void test_request_counter() {
    const auto root = make_sandbox("counter");
    const auto path = root / "counter.dat";
    const std::string lic_hash(64, 'a');
    const auto now = t_inside();

    {
        RequestCounter c(path, lic_hash, 3, /*hard_limit*/ true);
        CHECK(c.load(now) == CounterLoad::FreshState);
        CHECK(c.consume(now));
        CHECK(c.consume(now + 1s));
        CHECK(c.consume(now + 2s));
        CHECK(!c.consume(now + 3s));
        CHECK(c.snapshot().used == 3);
    }
    // persisted across instances
    {
        RequestCounter c(path, lic_hash, 3, true);
        CHECK(c.load(now + 4s) == CounterLoad::Ok);
        CHECK(c.snapshot().used == 3);
        CHECK(!c.consume(now + 5s));
    }
    // bound to another licence -> fail closed
    {
        RequestCounter c(path, std::string(64, 'b'), 3, true);
        CHECK(c.load(now + 6s) == CounterLoad::LicHashMismatch);
        CHECK(!c.consume(now + 7s));
    }
    // clock rollback -> fail closed
    {
        RequestCounter c(path, lic_hash, 3, true);
        CHECK(c.load(now - 1h) == CounterLoad::Rollback);
    }
    // hand-edited file -> tampered
    {
        auto bytes = slurp(path);
        CHECK(!bytes.empty());
        const auto pos = bytes.find("\"used\"");
        if (pos != std::string::npos) {
            bytes[pos + 7] = (bytes[pos + 7] == '0') ? '1' : '0';
        } else {
            bytes[0] ^= 0x01;
        }
        std::ofstream(path, std::ios::binary) << bytes;
        RequestCounter c(path, lic_hash, 3, true);
        CHECK(c.load(now + 8s) == CounterLoad::Tampered);
        CHECK(!c.consume(now + 9s));
    }
    // unlimited never blocks
    {
        RequestCounter c(root / "unlimited.dat", lic_hash, 0);
        CHECK(c.load(now) == CounterLoad::FreshState);
        for (int i = 0; i < 1000; ++i) {
            CHECK(c.consume(now + std::chrono::seconds(i)));
        }
    }
    // batches: consume(n) is all-or-nothing, refund gives slots back
    {
        RequestCounter c(root / "batch.dat", lic_hash, 5, true);
        CHECK(c.load(now) == CounterLoad::FreshState);
        CHECK(c.consume(now, 3));
        CHECK(c.snapshot().used == 3);
        CHECK(!c.consume(now + 1s, 3));           // 3 + 3 > 5: refused whole
        CHECK(c.snapshot().used == 3);
        CHECK(c.refund(now + 2s, 2));
        CHECK(c.snapshot().used == 1);
        CHECK(c.consume(now + 3s, 4));            // exactly fits
        CHECK(c.snapshot().used == 5);
        CHECK(!c.consume(now + 4s));
        CHECK(c.refund(now + 5s, 100));           // never below 0
        CHECK(c.snapshot().used == 0);
        RequestCounter again(root / "batch.dat", lic_hash, 5, true);
        CHECK(again.load(now + 6s) == CounterLoad::Ok);
        CHECK(again.snapshot().used == 0);
    }
    // licensed (rolling) window: refresh() resets the budget after 30 days
    // without a consume, and a refund after the rollover is not re-credited
    {
        RequestCounter c(root / "window.dat", lic_hash, 2, /*hard_limit*/ false);
        CHECK(c.load(now) == CounterLoad::FreshState);
        CHECK(c.reset(now));
        CHECK(c.consume(now + 1s, 2));
        CHECK(!c.consume(now + 2s));
        CHECK(!c.refresh(now + 3s));               // still inside the window
        CHECK(c.snapshot().used == 2);
        const auto later = now + std::chrono::seconds(kCounterWindowSeconds + 60);
        CHECK(c.refresh(later));
        CHECK(c.snapshot().used == 0);
        CHECK(c.snapshot().window_start_unix ==
              std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count() + kCounterWindowSeconds);
        CHECK(c.consume(later + 1s));
        CHECK(c.snapshot().used == 1);
        CHECK(c.refund(later + std::chrono::seconds(kCounterWindowSeconds), 1)); // rolled over: nothing to give back
        CHECK(c.snapshot().used == 0);
    }
    std::error_code ec;
    fs::remove_all(root, ec);
}

// ---------------------------------------------------------------------------
// durable writes: a full disk never leaves a short counter / ledger / .lic in place
// ---------------------------------------------------------------------------

static size_t count_temp_files(const fs::path & dir) {
    size_t n = 0;
    std::error_code ec;
    for (const auto & e : fs::directory_iterator(dir, ec)) {
        if (e.path().filename().string().find(".tmp") != std::string::npos) {
            n++;
        }
    }
    return n;
}

static void test_counter_full_disk() {
    const auto root = make_sandbox("counter-full-disk");
    const auto path = root / "counter.dat";
    const auto ledger = fs::path(path.string() + ".ledger");
    const std::string lic_hash(64, 'c');
    const auto now = t_inside();

    {
        RequestCounter c(path, lic_hash, 10, /*hard_limit*/ true);
        CHECK(c.load(now) == CounterLoad::FreshState);
        CHECK(c.consume(now));
        const std::string before = slurp(path);
        CHECK(!before.empty());
        // the write stops after `cut` bytes, as on a full disk: refused, nothing charged, the file untouched
        for (size_t cut : { (size_t) 0, (size_t) 1, (size_t) 7, before.size() - 1 }) {
            durable_write_test_fail(0, 1, cut);
            CHECK(!c.consume(now + 1s));
            CHECK(c.snapshot().used == 1);
            CHECK(slurp(path) == before);
        }
        durable_write_test_fail(0, 0, 0);
        CHECK(count_temp_files(root) == 0); // no temp file is left behind
    }
    // the next start reads a valid counter: never "tampered" because the disk was full
    {
        RequestCounter c(path, lic_hash, 10, true);
        CHECK(c.load(now + 2s) == CounterLoad::Ok);
        CHECK(c.snapshot().used == 1);
        CHECK(c.consume(now + 3s));
        CHECK(c.snapshot().used == 2);
    }
    // the ledger write fails after a good counter write: the counter counts, the previous ledger stays valid
    {
        RequestCounter c(path, lic_hash, 10, true);
        CHECK(c.load(now + 4s) == CounterLoad::Ok);
        const std::string ledger_before = slurp(ledger);
        CHECK(!ledger_before.empty());
        durable_write_test_fail(1, 1, 3);
        CHECK(c.consume(now + 5s));
        CHECK(c.snapshot().used == 3);
        CHECK(slurp(ledger) == ledger_before);
        durable_write_test_fail(0, 0, 0);
        CHECK(count_temp_files(root) == 0);
    }
    {
        // the ledger that stayed is still verified and used: counter.dat lost, the licence keeps its tally of 2
        std::error_code ec;
        fs::remove(path, ec);
        RequestCounter c(path, lic_hash, 10, true);
        CHECK(c.load(now + 6s) == CounterLoad::FreshState);
        CHECK(c.restore_from_ledger(now + 6s));
        CHECK(c.snapshot().used == 2);
    }
    // reset() (activation) on a full disk: refused, the previous counter stays
    {
        RequestCounter c(path, lic_hash, 10, true);
        CHECK(c.load(now + 7s) == CounterLoad::Ok);
        const std::string before = slurp(path);
        durable_write_test_fail(0, 1, 4);
        CHECK(!c.reset(now + 8s));
        CHECK(slurp(path) == before);
        durable_write_test_fail(0, 0, 0);
    }
    // the installed .lic: a full disk keeps the previous one
    {
        const auto lic = root / "license.lic";
        CHECK(ActivationClient::save_lic_atomic(lic, "{\"a\":1}"));
        durable_write_test_fail(0, 1, 2);
        CHECK(!ActivationClient::save_lic_atomic(lic, "{\"a\":2,\"b\":3}"));
        CHECK(slurp(lic) == "{\"a\":1}");
        durable_write_test_fail(0, 0, 0);
        CHECK(ActivationClient::save_lic_atomic(lic, "{\"a\":2}"));
        CHECK(slurp(lic) == "{\"a\":2}");
        CHECK(count_temp_files(root) == 0);
    }
    // the primitives themselves: a short write is reported and nothing is renamed
    {
        const auto target = root / "plain.txt";
        const auto tmp    = root / "plain.txt.tmp";
        CHECK(durable_write_file(tmp, "hello") && durable_replace(tmp, target));
        CHECK(slurp(target) == "hello");
        durable_write_test_fail(0, 1, 3);
        CHECK(!durable_write_file(tmp, "goodbye"));
        CHECK(!fs::exists(tmp));
        CHECK(slurp(target) == "hello");
        durable_write_test_fail(0, 0, 0);
        // a rename that fails (ENOSPC on a full btrfs, a quota, EIO) keeps the previous file: it is never
        // removed before the new one is in place
        CHECK(durable_write_file(tmp, "goodbye"));
        durable_replace_test_fail(1);
        CHECK(!durable_replace(tmp, target));
        CHECK(!fs::exists(tmp));
        CHECK(slurp(target) == "hello");
        durable_replace_test_fail(0);
    }
    // the same through the counter and the .lic: a failed rename refuses the write, the previous file stays
    {
        RequestCounter c(path, lic_hash, 10, true);
        CHECK(c.load(now + 9s) == CounterLoad::Ok);
        const std::string before = slurp(path);
        const int64_t used = c.snapshot().used;
        durable_replace_test_fail(1);
        CHECK(!c.consume(now + 10s));
        CHECK(c.snapshot().used == used);
        CHECK(slurp(path) == before);
        durable_replace_test_fail(0);
        CHECK(count_temp_files(root) == 0);

        const auto lic = root / "license.lic";
        const std::string lic_before = slurp(lic);
        durable_replace_test_fail(1);
        CHECK(!ActivationClient::save_lic_atomic(lic, "{\"a\":3}"));
        CHECK(slurp(lic) == lic_before);
        durable_replace_test_fail(0);
        CHECK(count_temp_files(root) == 0);
    }
    // the next start still reads the counter: a failed rename never reset it
    {
        RequestCounter c(path, lic_hash, 10, true);
        CHECK(c.load(now + 11s) == CounterLoad::Ok);
    }
    std::error_code ec;
    fs::remove_all(root, ec);
}

// ---------------------------------------------------------------------------
// activation client (mocked transport)
// ---------------------------------------------------------------------------

static std::string activation_success_body() {
    const auto raw = slurp(vectors_dir() / "golden" / "online.lic");
    CHECK(!raw.empty());
    auto lic = nlohmann::json::parse(raw);
    nlohmann::json env;
    env["lic"] = lic;
    env["kid"] = lic["kid"];
    return env.dump();
}

static ActivationClient::Config activation_config() {
    ActivationClient::Config c;
    c.activation_url = "https://license.test/dummy";
    c.timeout = 5000ms;
    c.max_retries = 2;
    c.retry_backoff_base = 1ms;
    return c;
}

static void test_activation_client() {
    const auto kr = test_keyring();
    const auto root = make_sandbox("activation");

    // success: 200 with a verifiable .lic whose fingerprint matches ours
    {
        ActivationClient client{activation_config(), kr};
        int calls = 0;
        std::string seen_key;
        client.set_transport([&](const HttpRequest & req) {
            ++calls;
            CHECK(req.method == "POST");
            CHECK(req.url == "https://license.test/dummy/api/v1/activate");
            seen_key = req.headers.count("Idempotency-Key") ? req.headers.at("Idempotency-Key") : "";
            const auto j = nlohmann::json::parse(req.body);
            CHECK(j["activationCode"].get<std::string>() == "XXXX-XXXX-XXXX");
            HttpResponse r;
            r.code = 200;
            r.body = activation_success_body();
            return r;
        });
        const auto r = client.activate("XXXX-XXXX-XXXX", Fingerprint{"abc123def456"}, t_inside());
        CHECK(r.status == ActivationStatus::Ok);
        CHECK(r.lic.has_value() && r.lic->product == "OCR");
        CHECK(calls == 1);
        CHECK(!seen_key.empty() && seen_key == r.idempotency_key);
        // same machine + same code -> same Idempotency-Key (recovery re-runs are free)
        CHECK(client.idempotency_key_for("XXXX-XXXX-XXXX", Fingerprint{"abc123def456"}) == r.idempotency_key);
        CHECK(client.idempotency_key_for("YYYY-YYYY-YYYY", Fingerprint{"abc123def456"}) != r.idempotency_key);

        const auto lic_path = root / "license.lic";
        CHECK(ActivationClient::save_lic_atomic(lic_path, r.raw_lic_json));
        CHECK(slurp(lic_path) == r.raw_lic_json);
    }
    // the returned .lic is bound to another machine
    {
        ActivationClient client{activation_config(), kr};
        client.set_transport([&](const HttpRequest &) {
            HttpResponse r;
            r.code = 200;
            r.body = activation_success_body();
            return r;
        });
        const auto r = client.activate("XXXX-XXXX-XXXX", Fingerprint{"someone-else"}, t_inside());
        CHECK(r.status == ActivationStatus::FingerprintMismatch);
    }
    // status mapping
    const auto status_for = [&](int code) {
        ActivationClient client{activation_config(), kr};
        client.set_transport([&](const HttpRequest &) {
            HttpResponse r;
            r.code = code;
            r.body = "{}";
            return r;
        });
        return client.activate("XXXX-XXXX-XXXX", Fingerprint{"abc123def456"}, t_inside()).status;
    };
    CHECK(status_for(401) == ActivationStatus::InvalidCode);
    CHECK(status_for(403) == ActivationStatus::NoFreeSlot);
    CHECK(status_for(409) == ActivationStatus::IdempotencyConflict);
    CHECK(status_for(500) == ActivationStatus::HttpError);
    // transport failure after retries
    {
        ActivationClient client{activation_config(), kr};
        int calls = 0;
        client.set_transport([&](const HttpRequest &) {
            ++calls;
            HttpResponse r;
            r.transport_error = "connection refused";
            return r;
        });
        const auto r = client.activate("XXXX-XXXX-XXXX", Fingerprint{"abc123def456"}, t_inside());
        CHECK(r.status == ActivationStatus::NetworkError);
        CHECK(calls == 1 + activation_config().max_retries);
    }
    std::error_code ec;
    fs::remove_all(root, ec);
}

// ---------------------------------------------------------------------------
// facade: trial mode, then activation
// ---------------------------------------------------------------------------

static Config facade_config(const fs::path & state_dir) {
    Config c;
    c.product = "LLM";
    c.client_id = "";
    c.activation_url = "https://license.test/dummy";
    c.state_dir = state_dir;
    c.lic_path = state_dir / "license.lic";
    c.counter_path = state_dir / "counter.dat";
    c.token_bundle_path = state_dir / "tokens.json";
    c.trial_limit = 3;
    c.require_stable_cpu_id = true;
    return c;
}

static void test_facade_trial_then_activation() {
    const auto root = make_sandbox("facade");
    // deterministic machine identity + forced trial mode, independent of the
    // host and of whether the library was compiled with PROVISION_TRIAL
    set_env("PROVISION_LICENSE_CPU_ID_OVERRIDE", "stable-machine-id-aaaa");
    set_env("PROVISION_LICENSE_HOSTNAME_OVERRIDE", "host-1");
    set_env("PROVISION_LICENSE_MODE", "trial");

    auto & lib = LicenseLib::instance();
    lib.reset_for_test();
    lib.set_keyring_for_test(test_keyring());
    lib.set_now_for_test([] { return t_inside(); });

    auto r = lib.init(facade_config(root));
    CHECK(r.ok);
    CHECK(r.mode == Mode::Trial);
    CHECK(lib.mode() == Mode::Trial);

    // the read-only check never consumes
    for (int i = 0; i < 5; ++i) {
        CHECK(lib.check_license().allow);
    }
    CHECK(lib.check_license().used == 0);

    // reserve-then-commit with batches: 2 units, refund 1, then 2 again
    {
        auto d = lib.authorize_request(2);
        CHECK(d.allow && d.used == 2 && d.limit == 3);
        d = lib.authorize_request(2);
        CHECK(!d.allow && d.reason == DenyReason::RequestLimitReached && d.used == 2);
        CHECK(lib.refund_request(1));
        CHECK(lib.check_license().used == 1);
        d = lib.authorize_request(2);
        CHECK(d.allow && d.used == 3);
        CHECK(lib.refund_request(3));
        CHECK(lib.check_license().used == 0);
    }

    for (int i = 0; i < 3; ++i) {
        const auto d = lib.authorize_request();
        CHECK(d.allow);
        CHECK(d.used == i + 1);
        CHECK(d.limit == 3);
    }
    auto d = lib.authorize_request();
    CHECK(!d.allow);
    CHECK(d.reason == DenyReason::RequestLimitReached);
    d = lib.check_license();
    CHECK(!d.allow && d.reason == DenyReason::RequestLimitReached);

    // the tally survives a "process restart"
    lib.reset_for_test();
    lib.set_keyring_for_test(test_keyring());
    lib.set_now_for_test([] { return t_inside(); });
    r = lib.init(facade_config(root));
    CHECK(r.ok && r.mode == Mode::Trial);
    d = lib.check_license();
    CHECK(!d.allow);
    CHECK(d.used == 3);

    // activation with a licence for another product / machine is refused and
    // leaves the trial state untouched (the golden .lic is OCR, fp abc123def456)
    int calls = 0;
    lib.set_activation_transport_for_test([&](const HttpRequest &) {
        ++calls;
        HttpResponse resp;
        resp.code = 200;
        resp.body = activation_success_body();
        return resp;
    });
    const auto a = lib.activate("XXXX-XXXX-XXXX", 2000ms);
    CHECK(!a.ok);
    CHECK(calls == 1);
    CHECK(a.detail.find("fingerprint") != std::string::npos);
    CHECK(!fs::exists(root / "license.lic"));
    CHECK(lib.mode() == Mode::Trial);

    // activation failure with the License Server unreachable
    lib.set_activation_transport_for_test([&](const HttpRequest &) {
        HttpResponse resp;
        resp.transport_error = "connection refused";
        return resp;
    });
    const auto b = lib.activate("XXXX-XXXX-XXXX", 2000ms);
    CHECK(!b.ok);
    CHECK(lib.mode() == Mode::Trial);

    lib.reset_for_test();
    set_env("PROVISION_LICENSE_MODE", "");
    std::error_code ec;
    fs::remove_all(root, ec);
}

int main() {
    test_base64url();
    test_sha256();
    test_jcs_golden_roundtrip();
    test_verify_golden();
    test_fingerprint();
    test_request_counter();
    test_counter_full_disk();
    test_activation_client();
    test_facade_trial_then_activation();

    if (n_failed) {
        std::fprintf(stderr, "%d check(s) failed\n", n_failed);
        return 1;
    }
    std::printf("all license checks passed\n");
    return 0;
}
