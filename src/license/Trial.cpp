#include "license/Trial.h"

#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/crypto.h>        // CRYPTO_memcmp (OpenSSL)
#if defined(OPENSSL_IS_BORINGSSL)
#  include <openssl/mem.h>         // CRYPTO_memcmp (BoringSSL)
#endif

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <shlobj.h>
#endif

namespace visio::license {
namespace {

namespace fs = std::filesystem;
using clock_t_sys = std::chrono::system_clock;

// Minimal stderr logger so this module stays dependency-free. The original
// drop-in used spdlog; llama.cpp has its own logging, so we avoid pulling in
// an extra dependency and just write to stderr.
inline void trial_log_error(const std::string & msg) {
    std::fprintf(stderr, "[TRIAL] %s\n", msg.c_str());
}

// Compile-time HMAC key. Override per-deployment with -DVISIO_TRIAL_KEY=...;
// keep the default in source so a leaked CMake invocation alone is not
// enough to forge state. Anti-casual-tampering, not cryptographic.
constexpr std::string_view kHmacKey =
#ifdef VISIO_TRIAL_KEY
    VISIO_TRIAL_KEY
#else
    "provision-ocr-trial/v1/4f7a2c9b8e1d6053"
#endif
    ;

fs::path resolve_state_dir() {
#ifdef _WIN32
    // %PROGRAMDATA% — world-writable system-wide dir, persists across
    // user sessions. Standard location for service-owned state.
    PWSTR path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &path))) {
        fs::path p{path};
        CoTaskMemFree(path);
        return p / L"ProvisionOCR";
    }
    if (const char* pd = std::getenv("ProgramData"); pd && *pd) {
        return fs::path{pd} / "ProvisionOCR";
    }
    return fs::path{"C:\\ProgramData\\ProvisionOCR"};
#else
    // FHS — /var/lib/<service>/ is the canonical place for mutable
    // service state. Installer / Docker entrypoint must ensure it is
    // writable by the service user.
    return fs::path{"/var/lib/provision-ocr"};
#endif
}

fs::path resolve_state_path() {
    return resolve_state_dir() / "trial.dat";
}

std::string hex(const unsigned char* data, std::size_t len) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < len; ++i) {
        out << std::setw(2) << static_cast<unsigned>(data[i]);
    }
    return out.str();
}

std::string hmac_hex(std::string_view msg) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> buf{};
    unsigned int out_len = 0;
    HMAC(EVP_sha256(),
         kHmacKey.data(), static_cast<int>(kHmacKey.size()),
         reinterpret_cast<const unsigned char*>(msg.data()), msg.size(),
         buf.data(), &out_len);
    return hex(buf.data(), out_len);
}

std::string iso8601(clock_t_sys::time_point tp) {
    const auto t = clock_t_sys::to_time_t(tp);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

std::int64_t epoch_seconds(std::string_view iso) {
    std::tm tm{};
    std::istringstream in{std::string{iso}};
    in >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    if (in.fail()) return -1;
#ifdef _WIN32
    return static_cast<std::int64_t>(_mkgmtime(&tm));
#else
    return static_cast<std::int64_t>(timegm(&tm));
#endif
}

// Plain key=value text — one parser, one writer, no external deps. The
// canonical message that gets HMACed is the concatenation of fields in
// fixed order, separated by '|', so the hash is independent of file
// formatting whitespace.
struct State {
    int     version = 1;
    int     limit   = Trial::kLimit;
    int     used    = 0;
    std::string first;   // ISO8601 UTC
    std::string last;    // ISO8601 UTC

    std::string canonical_message() const {
        std::ostringstream o;
        o << "v=" << version
          << "|limit=" << limit
          << "|used=" << used
          << "|first=" << first
          << "|last=" << last;
        return o.str();
    }
};

bool write_state(const fs::path& path, const State& s) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    // Write to .tmp + rename for atomicity. A power-cut mid-write
    // otherwise produces an unsigned/short file that fails verification
    // and locks the trial out for legitimate users.
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        const auto sig = hmac_hex(s.canonical_message());
        f << "v="     << s.version << "\n"
          << "limit=" << s.limit   << "\n"
          << "used="  << s.used    << "\n"
          << "first=" << s.first   << "\n"
          << "last="  << s.last    << "\n"
          << "hmac="  << sig       << "\n";
        if (!f) return false;
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        // rename across filesystems / locked file — try copy+remove.
        fs::copy_file(tmp, path, fs::copy_options::overwrite_existing, ec);
        fs::remove(tmp);
        if (ec) return false;
    }
    return true;
}

// Returns true on success; out_tampered set if the file existed but was
// modified/truncated/rolled-back — caller must treat this as "exhausted".
bool read_state(const fs::path& path, State& out, bool& out_tampered) {
    out_tampered = false;
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;

    std::string line, sig;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto key = line.substr(0, eq);
        auto val = line.substr(eq + 1);
        if      (key == "v")     out.version = std::atoi(val.c_str());
        else if (key == "limit") out.limit   = std::atoi(val.c_str());
        else if (key == "used")  out.used    = std::atoi(val.c_str());
        else if (key == "first") out.first   = val;
        else if (key == "last")  out.last    = val;
        else if (key == "hmac")  sig         = val;
    }
    if (sig.empty()) { out_tampered = true; return true; }
    const auto expected = hmac_hex(out.canonical_message());
    if (expected.size() != sig.size() ||
        CRYPTO_memcmp(expected.data(), sig.data(), expected.size()) != 0) {
        out_tampered = true;
    }
    return true;
}

bool clock_rolled_back(const State& s) {
    if (s.last.empty()) return false;
    const auto last_epoch = epoch_seconds(s.last);
    if (last_epoch < 0) return true;
    const auto now_epoch = static_cast<std::int64_t>(
        clock_t_sys::to_time_t(clock_t_sys::now()));
    // 1-day grace — allow minor clock skew (NTP corrections, VM resume).
    return now_epoch + 86400 < last_epoch;
}

// Global state, guarded by mutex. Loaded lazily on first init()/consume().
std::mutex            g_mu;
bool                  g_loaded     = false;
bool                  g_exhausted  = false;
bool                  g_tampered   = false;
State                 g_state;
fs::path              g_path;

void load_locked() {
    g_path = resolve_state_path();
    State on_disk;
    bool tampered = false;
    if (read_state(g_path, on_disk, tampered)) {
        if (tampered) {
            trial_log_error("state file at " + g_path.string() +
                            " fails integrity check — locking out");
            g_tampered = true;
            g_exhausted = true;
            g_state = on_disk;
            g_state.limit = Trial::kLimit;
            return;
        }
        if (clock_rolled_back(on_disk)) {
            trial_log_error("clock rolled back relative to last-seen (" +
                            on_disk.last + ") — locking out");
            g_tampered  = true;
            g_exhausted = true;
            g_state = on_disk;
            return;
        }
        g_state = on_disk;
        // If a build with a different VISIO_TRIAL_LIMIT now runs against
        // an older state file, honour the COMPILED limit (lower wins —
        // can't extend a trial by recompiling with a bigger limit).
        if (g_state.limit > Trial::kLimit) g_state.limit = Trial::kLimit;
        g_exhausted = g_state.used >= g_state.limit;
    } else {
        // First run on this machine — seed.
        g_state = State{};
        g_state.limit = Trial::kLimit;
        g_state.first = iso8601(clock_t_sys::now());
        g_state.last  = g_state.first;
        if (!write_state(g_path, g_state)) {
            trial_log_error("cannot create state file at " + g_path.string() +
                            " — check that the directory is writable");
            // Fail closed: if we can't persist, every restart would
            // reset the counter. Better to block than to give an
            // unbounded effective trial.
            g_exhausted = true;
            g_tampered  = true;
        }
    }
    g_loaded = true;
}

}  // namespace

Trial& Trial::instance() {
    static Trial t;
    return t;
}

bool Trial::init() {
    std::lock_guard lock{g_mu};
    if (!g_loaded) load_locked();
    return g_loaded && !g_tampered;
}

bool Trial::consume() {
    std::lock_guard lock{g_mu};
    if (!g_loaded) load_locked();
    if (g_exhausted) return false;
    g_state.used += 1;
    g_state.last = iso8601(clock_t_sys::now());
    if (g_state.used >= g_state.limit) g_exhausted = true;
    if (!write_state(g_path, g_state)) {
        // Persisting failed — refuse the request rather than silently
        // letting it through (which would reset on restart).
        trial_log_error("failed to persist state to " + g_path.string() +
                        " — denying request");
        g_state.used -= 1;   // roll back in-memory so status() is honest
        g_exhausted = true;
        return false;
    }
    return true;
}

TrialStatus Trial::status() {
    std::lock_guard lock{g_mu};
    if (!g_loaded) load_locked();
    TrialStatus s;
    s.used       = g_state.used;
    s.limit      = g_state.limit;
    s.exhausted  = g_exhausted;
    s.tampered   = g_tampered;
    s.state_path = g_path.string();
    return s;
}

}  // namespace visio::license
