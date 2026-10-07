// rand_s() (the CSPRNG behind the generated admin key on Windows) is only
// declared when this is defined before the first CRT header
#if defined(_WIN32) && !defined(_CRT_RAND_S)
#define _CRT_RAND_S
#endif

#include "server-admin.h"
#include "server-admin-internal.h"
#include "server-admin-upload.h"

#include "server-common.h"
#include "server-http.h"
#include "server-models.h"

#include "arg.h"
#include "build-info.h"
#include "common.h"
#include "download.h"
#include "fit.h"
#include "hf-cache.h"
#include "http.h"
#include "log.h"
#include "preset.h"
#include "speculative.h"

#include "ggml-backend.h"
#include "ggml-cpp.h"
#include "gguf.h"
#include "llama.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <clocale>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <aclapi.h>
#include <io.h>
#include <process.h>
#include <sddl.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__) && defined(__MACH__)
#include <mach-o/dyld.h>
#endif
extern char ** environ;
#endif

namespace fs = std::filesystem;

static constexpr int ADMIN_STATE_VERSION  = 1;
static constexpr int ADMIN_MODELS_MAX_MIN = 1;
static constexpr int ADMIN_MODELS_MAX_MAX = 16;

// total network budget of one download pre-flight check (the contract says <= 10 s)
static constexpr int ADMIN_HF_BUDGET_MS = 9000;
// a repository listing larger than this is not read
static constexpr size_t ADMIN_HF_MAX_BODY = 16 * 1024 * 1024;

static const char * const ADMIN_KEY_HEADER     = "x-admin-key";
static const char * const ADMIN_STATE_FILE     = "admin-state.json";
static const char * const ADMIN_KEY_FILE       = "admin.key";
static const char * const ADMIN_PARTIAL_SUFFIX = ".downloadInProgress"; // see common/download.cpp

// per-model settings (stage 2), see the "per-model settings" sections below
static constexpr int    ADMIN_MODELS_FILE_VERSION = 1;
static const char * const ADMIN_MODELS_FILE       = "admin-models.json";
static constexpr size_t ADMIN_MODELS_MAX_ENTRIES  = 256;
static constexpr size_t ADMIN_MODEL_ID_MAX        = 256;
static constexpr size_t ADMIN_VALUE_MAX           = 128;
static constexpr int    ADMIN_MAX_FAILURES        = 2;     // consecutive failed starts of new settings before they are reverted
static constexpr int    ADMIN_STOP_EXTRA_S        = 10;    // a restart waits stop-timeout + this for the old instance to exit

// helper processes and probes (the router itself never creates a GPU context)
static const char * const ADMIN_HELPER_ENV        = "PROVISION_ADMIN_HELPER";      // inventory | fit | cat
static const char * const ADMIN_HELPER_FILE_ENV   = "PROVISION_ADMIN_HELPER_FILE"; // cat: the file to print
static const char * const ADMIN_RESULT_PREFIX     = "PROVISION_ADMIN_RESULT ";     // the one line a helper answers with
static const char * const ADMIN_NEXT_COMMAND      = "PROVISION_ADMIN_NEXT_COMMAND"; // separates the outputs of a multi-command job
static constexpr int    ADMIN_INVENTORY_TIMEOUT_MS = 20000;
static constexpr int    ADMIN_FIT_TIMEOUT_MS       = 30000;
static constexpr int    ADMIN_PROBE_TIMEOUT_MS     = 15000; // one vendor tool job (all its commands): nvidia-smi without persistence
                                                            // mode re-initialises every GPU on each run, amd-smi runs twice
static constexpr int    ADMIN_PROBE_WAIT_MS        = 5000;  // what a caller waits for a probe, separate from the probe itself
static constexpr int    ADMIN_PROBE_CACHE_MS       = 3000;
static constexpr int    ADMIN_PROBE_REFRESH_MIN_MS = 1000;  // ?refresh=1 skips the cache at most this often
static constexpr size_t ADMIN_PROC_MAX_OUTPUT      = 1024 * 1024;
static constexpr int    ADMIN_SETTLE_MS            = 1500;  // memory readings are not trusted this long after a status change
static constexpr uint64_t ADMIN_FIT_MARGIN         = 1024ull * 1024 * 1024; // same as the default target of upstream fit (common.h)

// log tail of the model children
static constexpr size_t ADMIN_LOG_MAX_LINES   = 200;
static constexpr size_t ADMIN_LOG_MAX_BYTES   = 64 * 1024;
static constexpr size_t ADMIN_LOG_MAX_LINE    = 2048;
static constexpr size_t ADMIN_LOG_MAX_NAMES   = 64;

// GGUF facts cache
static constexpr size_t ADMIN_FACTS_MAX = 64;

//
// state
//

// the part of the settings that is persisted in <state>/admin-state.json
// every field is optional: an unset field falls back to the startup default
struct admin_settings {
    std::optional<int>                      models_max;
    std::optional<std::string>              default_device;
    std::optional<std::vector<std::string>> pinned;
};

//
// per-model settings (stage 2): data kept by the admin layer
//

// admin_kv (INI key -> value, canonical spelling for stored settings) is in server-admin-internal.h

// one entry of <state>/admin-models.json
struct admin_model_entry {
    admin_kv                settings;           // what the operator saved (canonical values)
    std::map<std::string, json> kept;           // stored values that are not applied (unknown key / invalid value), written back as read
    std::optional<admin_kv> last_good;          // the settings of the last start that reached LOADED
    std::string             phase = "pending";  // pending | trial | failed | committed
    int                     failures = 0;       // consecutive failed starts with `settings`
    std::string             updated;            // ISO time of the last change by the operator
    std::map<std::string, std::string> device_pci; // device name -> PCI bus id it had when it was saved
    json                    reverted;           // null, or {reason, time, settings, failures}: the last automatic revert
    json                    raw = json::object(); // the entry as read, unknown fields are kept on rewrite
    bool                    reverted_at_startup = false; // in memory only
};

struct admin_models_file {
    std::map<std::string, admin_model_entry> entries;
    json        raw = json::object(); // the whole document as read, unknown top-level fields are kept on rewrite
    std::string error;                // why the file could not be used as is
    bool        read_only     = false; // corrupt file or unknown version: never overwritten
    bool        backup_loaded = false; // the entries come from the .bak file
    std::vector<std::string> problems; // values skipped while reading
    std::map<std::string, json> kept_entries; // entries that could not be read, written back as read
};

// MODEL_DEVICES="<section>=<dev>,...,*=<dev>" (docker-dual/entrypoint.sh grammar)
struct admin_model_devices {
    bool        present  = false;
    std::string raw;
    bool        has_star = false;
    std::string star;
    std::map<std::string, std::string> sections; // section name (tag canonicalised like the preset loader) -> device
};

// what server_admin_overlay_presets() saw for one model in the latest reload
struct admin_seen_model {
    server_model_source source = SERVER_MODEL_SOURCE_CACHE;
    admin_kv    inherited;      // whitelisted keys (+ kv-unified) before the admin pass: models.ini section / [*]
    admin_kv    values;         // the same keys after the admin pass (before the router command line is merged)
    std::map<std::string, std::string> inactive; // stored key -> why it is not applied
    std::string default_device; // set by "GPU for new models", "" = not filled
    std::string model_path;     // LLAMA_ARG_MODEL
    std::string hf_repo;        // LLAMA_ARG_HF_REPO
    std::string hf_file;        // LLAMA_ARG_HF_FILE
    std::string mmproj;         // LLAMA_ARG_MMPROJ
    uint64_t    generation = 0;
};

// a GPU (or other non-CPU device) as the inventory helper reported it
struct admin_inv_device {
    std::string name;        // registry name: CUDA0, ROCm0, Vulkan0, SYCL0, ...
    std::string backend;     // backend registry name
    std::string description;
    std::string type;        // gpu | igpu | accel
    std::string device_id;   // as the backend reports it
    std::string pci;         // normalised dddd:bb:dd.f, "" = none
    std::string vendor;      // nvidia | amd | intel | ...
    uint64_t    memory_free  = 0;
    uint64_t    memory_total = 0;
};

struct admin_inventory {
    bool        ran = false;
    bool        ok  = false;
    std::string source;      // helper | file | off
    std::string error;       // disabled | timeout | failed | parse_error | unsupported
    std::string message;
    int64_t     time_ms     = 0; // wall clock of the measurement
    int64_t     duration_ms = 0;
    std::vector<admin_inv_device> devices;
    std::vector<std::string>      backends;
};

// one helper / vendor tool process, run by a detached thread: struct admin_job is in server-admin-internal.h

// a row of a vendor tool (nvidia-smi, amd-smi, rocm-smi)
struct admin_vendor_gpu {
    std::string tool;
    int         index = -1;
    std::string pci;     // normalised
    std::string pci_raw;
    std::string uuid;
    std::string name;
    bool        has_memory  = false;
    bool        unsupported = false; // MIG / vGPU / [N/A]
    uint64_t    total = 0;
    uint64_t    used  = 0;
    uint64_t    free  = 0;
};

struct admin_vendor_proc {
    long long   pid  = 0;
    std::string pci;
    uint64_t    used = 0;
};

struct admin_tool_state {
    std::string tool;             // nvidia-smi | amd-smi | rocm-smi
    std::string path;             // executable, or the canned file
    bool        canned = false;   // file:
    std::shared_ptr<admin_job> job;
    bool        job_consumed = false; // the result of `job` went into the fields below
    bool        has_sample = false; // gpus below come from a successful run
    std::string error;            // code of the latest attempt, "" = ok
    std::string message;
    int64_t     t_sample = 0;     // steady ms of the latest successful run
    int64_t     t_try    = 0;     // steady ms of the latest attempt
    std::vector<admin_vendor_gpu>  gpus;
    std::vector<admin_vendor_proc> procs;
    bool        procs_ok = false;
};

struct admin_breakdown {
    bool     host    = false;
    uint64_t model   = 0;
    uint64_t context = 0;
    uint64_t compute = 0;
    uint64_t total   = 0;
    uint64_t free    = 0;
};

// the output tail of one child process
struct admin_log_instance {
    const void * key  = nullptr;
    int          port = 0;
    uint64_t     seq  = 0;
    int64_t      wall_start = 0;
    std::deque<std::string> lines;
    size_t       bytes   = 0;
    uint64_t     n_lines = 0;
    bool         truncated = false;
    std::map<std::string, admin_breakdown> breakdown; // device name -> memory the child reported (memory breakdown lines)
    int64_t      wall_breakdown = 0;
};

struct admin_log_ring {
    admin_log_instance cur;
    bool               has_cur = false;
    int64_t            t_used  = 0;
};

// what the event thread knows about a model's status
struct admin_track {
    std::string status;
    int64_t     t_change = 0;       // steady ms
    bool        reached_loaded = false;
    uint64_t    n_unloaded = 0;
    uint64_t    n_loaded   = 0;
};

struct admin_facts_entry {
    std::string path;
    uint64_t    size  = 0;
    int64_t     mtime = 0;
    json        facts;
    int64_t     t_used = 0;
};

struct admin_facts_request {
    std::string path;
    std::string mmproj;
};

// an IPv4 or IPv6 address, 16 bytes in network order. IPv4 is held as IPv4-mapped IPv6
// (::ffff:a.b.c.d), so that 127.0.0.1 and the ::ffff:127.0.0.1 a dual-stack listener reports
// are the same address and match the same rules
struct admin_ip {
    uint8_t b[16] = {};
};

// one entry of PROVISION_ADMIN_ALLOW_FROM / PROVISION_ADMIN_TRUSTED_PROXIES
struct admin_ip_range {
    admin_ip    net;        // first address of the range (host bits cleared)
    int         prefix = 0; // network bits of the 128 (an IPv4 /n is 96 + n)
    std::string text;       // canonical spelling, for the log and GET /admin/diagnostics
};

// the address allow-list of the /admin routes; off unless PROVISION_ADMIN_ALLOW_FROM has entries
struct admin_access {
    bool                        enabled = false; // the allow-list applies to every /admin route
    bool                        locked  = false; // an invalid entry: every /admin request is refused (fail closed)
    std::vector<admin_ip_range> allow;           // PROVISION_ADMIN_ALLOW_FROM
    std::vector<admin_ip_range> trusted;         // PROVISION_ADMIN_TRUSTED_PROXIES
    std::vector<std::string>    warnings;        // configuration notes, for GET /admin/diagnostics
};

// where an admin request comes from, as far as the allow-list is concerned
struct admin_client {
    std::string peer;                  // the TCP peer as the HTTP layer reports it ("" for a Unix socket)
    bool        known = false;         // `ip` holds the address the allow-list is checked against
    admin_ip    ip;
    std::string address;               // `ip` as text, "" when unknown
    bool        via_forwarded = false; // taken from X-Forwarded-For (the peer is a trusted proxy)
    std::string note;                  // why X-Forwarded-For of a trusted proxy was not used, "" = n/a
};

struct admin_context {
    std::atomic<bool> enabled{false};

    // written once by server_admin_init() / server_admin_register_routes(), read-only afterwards
    fs::path                 state_dir;
    std::string              key;                   // empty = no key could be obtained
    std::string              key_source;            // "env" | "file" | "generated"
    std::string              env_default_device;    // PROVISION_ADMIN_DEFAULT_DEVICE
    std::vector<std::string> env_pinned;            // PROVISION_ADMIN_PINNED
    bool                     device_locked = false; // MODEL_DEVICES has a "*=" entry
    std::string              locked_device;
    server_models_routes *   routes = nullptr;
    admin_model_devices      model_devices;         // MODEL_DEVICES, parsed
    std::string              bin_path;              // this binary, for the helper processes
    std::vector<std::string> env_snapshot;          // the environment children get (PROVISION_ADMIN* removed)
    std::string              gpu_probe;             // PROVISION_ADMIN_GPU_PROBE
    std::string              inventory_mode;        // PROVISION_ADMIN_INVENTORY
    std::vector<std::string> gpu_libraries;         // GPU backend libraries next to the binary
    std::vector<std::pair<std::string, size_t>> registry_backends; // backend registries of the router (name, devices)
    std::vector<std::string> registry_devices;      // device names of the router's non-CPU registries
    admin_access             access;                // PROVISION_ADMIN_ALLOW_FROM / PROVISION_ADMIN_TRUSTED_PROXIES

    // LEAF mutex: guards the snapshot below. It is never held while calling into
    // server_models, because server_models::load_models() reads the snapshot
    // through server_admin_overlay_presets()
    std::mutex     mtx_state;
    admin_settings saved;
    std::string    state_error;
    admin_models_file models_file;              // per-model settings as saved
    std::map<std::string, admin_kv> applied;    // id -> settings the hook applies

    // bumped (under mtx_state) whenever what the overlay applies changes;
    // read without a lock by server_admin_overlay_changed()
    std::atomic<uint64_t> overlay_gen{1};

    // serialises settings changes (validate -> persist -> apply). It may be held
    // while calling into server_models; server_models never takes it
    std::mutex            mtx_apply;
    std::set<std::string> pinned_applied; // last set pushed into server_models (admin_pinned_view::protect)
    bool                  pinned_pushed = false;

    // leaf: serialises writes of admin-models.json (taken under mtx_apply or alone, never with another leaf)
    std::mutex mtx_file;

    // leaf: what the hook saw in the latest reload
    std::mutex mtx_seen;
    std::map<std::string, admin_seen_model> seen;
    admin_kv   cli_locks; // whitelisted keys on the router command line -> value
    uint64_t   seen_gen = 0;

    // leaf: device inventory (helper process) and the devices a model may name
    std::mutex       mtx_inv;
    admin_inventory  inventory;
    std::shared_ptr<admin_job> inv_job;
    bool                       inv_rescan_running = false; // POST /admin/gpus/rescan reserved the helper
    std::vector<std::string>   build_devices; // GPU device names; empty = none found

    // leaf: vendor tools (GPU memory)
    std::mutex mtx_gpu;
    std::vector<admin_tool_state> tools;
    bool        tools_ready = false;
    std::string probe_mode;     // auto | off | tool | file
    std::string probe_error;    // configuration problem (not_found, disabled, unsupported)
    std::string probe_message;
    int64_t     t_refresh = 0;  // steady ms of the latest ?refresh=1
    struct tool_version {
        std::string text;
        bool        ok    = false;
        int64_t     t_ms  = 0; // steady ms of the answer
    };
    std::map<std::string, tool_version> tool_versions;                    // tool path -> version line (diagnostics)
    std::map<std::string, std::shared_ptr<admin_job>> tool_version_jobs; // tool path -> its latest version run

    // leaf: output tails, last failures
    std::mutex mtx_logs;
    std::map<std::string, admin_log_ring> logs;
    std::map<std::string, json>           failures; // id -> last failure snapshot
    uint64_t   log_seq = 0;

    // leaf: model status as seen through the /models/sse feed
    std::mutex mtx_track;
    std::map<std::string, admin_track> track;

    // leaf: GGUF facts
    std::mutex mtx_facts;
    std::condition_variable cv_facts;
    std::map<std::string, admin_facts_entry> facts; // path -> facts
    std::deque<admin_facts_request> facts_queue;
    std::set<std::string> facts_queued;

    // leaf: dry-run fit checks
    std::mutex mtx_check;
    std::shared_ptr<admin_job> check_job;
    std::map<std::string, json> check_cache; // model + args + inventory time -> helper result
    std::deque<std::string>     check_order;

    // background threads (event feed, facts); stopped through admin_lifetime_sentinel
    std::thread       th_events;
    std::thread       th_facts;
    std::atomic<bool> stop_threads{false};
    bool              threads_started = false;
    int               sse_id = INT_MIN + 7919; // never produced by the counter of /models/sse clients (counts up from 0)
    bool              sse_subscribed = false;

    std::atomic<bool> proxy_seen{false}; // an admin request carried X-Forwarded-For
};

// never destroyed: detached helper threads may still use it while the process exits
static admin_context & admin_ctx() {
    static admin_context * ctx = new admin_context();
    return *ctx;
}

// struct admin_http_error (an error that maps to a specific HTTP status and error.type) is in server-admin-internal.h

static admin_http_error admin_bad_request(const std::string & message) {
    return admin_http_error(400, "invalid_request_error", message);
}

//
// small helpers
//

static bool admin_is_alpha(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static bool admin_is_digit(char c) {
    return c >= '0' && c <= '9';
}

// string_strip() for text that may hold any byte: it hands bytes >= 0x80 to std::isspace(char), which is
// undefined for negative values (the debug CRT asserts); only ASCII blanks are removed here
static std::string admin_strip(const std::string & s) {
    static const char * const blanks = " \t\n\v\f\r";
    const size_t start = s.find_first_not_of(blanks);
    if (start == std::string::npos) {
        return "";
    }
    return s.substr(start, s.find_last_not_of(blanks) - start + 1);
}

static bool admin_is_printable_ascii(const std::string & s) {
    for (char c : s) {
        if ((unsigned char) c < 0x20 || (unsigned char) c > 0x7E) {
            return false;
        }
    }
    return true;
}

static std::string admin_to_lower(const std::string & s) {
    std::string out = s;
    for (char & c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = (char) (c - 'A' + 'a');
        }
    }
    return out;
}

static std::string admin_to_upper(const std::string & s) {
    std::string out = s;
    for (char & c : out) {
        if (c >= 'a' && c <= 'z') {
            c = (char) (c - 'a' + 'A');
        }
    }
    return out;
}

static std::string admin_join(const std::vector<std::string> & items, const char * separator = ", ") {
    std::string out;
    for (const auto & item : items) {
        if (!out.empty()) {
            out += separator;
        }
        out += item;
    }
    return out;
}

#if defined(_WIN32)
static std::string admin_wide_to_utf8(const wchar_t * ws) {
    if (ws == nullptr || *ws == L'\0') {
        return {};
    }
    const int len   = (int) wcslen(ws);
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, ws, len, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) {
        return {};
    }
    std::string out((size_t) bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, len, &out[0], bytes, nullptr, nullptr);
    return out;
}

static std::wstring admin_utf8_to_wide(const std::string & s) {
    if (s.empty()) {
        return {};
    }
    const int chars = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int) s.size(), nullptr, 0);
    if (chars <= 0) {
        return {};
    }
    std::wstring out((size_t) chars, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int) s.size(), &out[0], chars);
    return out;
}
#endif

// a std::string is always UTF-8 here, a narrow fs::path would use the active code page on Windows
static fs::path admin_path_from_utf8(const std::string & s) {
    return fs::u8path(s);
}

static std::string admin_path_to_utf8(const fs::path & p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

// environment variable as UTF-8, "" when unset
static std::string admin_getenv(const char * name) {
#if defined(_WIN32)
    const wchar_t * value = _wgetenv(admin_utf8_to_wide(name).c_str());
    return value ? admin_wide_to_utf8(value) : std::string();
#else
    const char * value = std::getenv(name);
    return value ? std::string(value) : std::string();
#endif
}

// Remove every PROVISION_ADMIN* variable from the process environment.
// server_models snapshots the environment for its child processes right after
// this, and nothing spawned later (MCP servers, tools) may see the admin key.
static void admin_scrub_env() {
    static const std::string prefix = "PROVISION_ADMIN";
    std::vector<std::string> names;
#if defined(_WIN32)
    LPWCH block = GetEnvironmentStringsW();
    if (block != nullptr) {
        for (LPWCH e = block; *e; e += wcslen(e) + 1) {
            const std::string entry = admin_wide_to_utf8(e);
            const size_t eq = entry.find('=', 1); // hidden entries such as "=C:=C:\dir" start with '='
            if (eq == std::string::npos) {
                continue;
            }
            const std::string name = entry.substr(0, eq);
            // variable names are case-insensitive on Windows
            if (admin_to_upper(name).compare(0, prefix.size(), prefix) == 0) {
                names.push_back(name);
            }
        }
        FreeEnvironmentStringsW(block);
    }
    for (const auto & name : names) {
        const std::wstring wname = admin_utf8_to_wide(name);
        _wputenv_s(wname.c_str(), L"");                  // CRT copy, what getenv() reads
        SetEnvironmentVariableW(wname.c_str(), nullptr); // process block, what GetEnvironmentStringsW() returns
    }
#else
    for (char ** e = environ; e != nullptr && *e != nullptr; e++) {
        const std::string entry = *e;
        const size_t eq = entry.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        if (entry.compare(0, prefix.size(), prefix) == 0) {
            names.push_back(entry.substr(0, eq));
        }
    }
    for (const auto & name : names) {
        unsetenv(name.c_str());
    }
#endif
}

static bool admin_is_enabled_value(const std::string & value) {
    const std::string v = admin_to_lower(admin_strip(value));
    return v == "1" || v == "true" || v == "on";
}

static int admin_getpid() {
#if defined(_WIN32)
    return (int) _getpid();
#else
    return (int) getpid();
#endif
}

static FILE * admin_fopen(const fs::path & path, const char * mode) {
#if defined(_WIN32)
    return _wfopen(path.c_str(), admin_utf8_to_wide(mode).c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

// whole file as a string, false when it cannot be read
static bool admin_read_file(const fs::path & path, std::string & out, size_t max_size = 1024 * 1024) {
    FILE * f = admin_fopen(path, "rb");
    if (f == nullptr) {
        return false;
    }
    out.clear();
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
        if (out.size() > max_size) {
            std::fclose(f);
            return false;
        }
    }
    const bool ok = std::ferror(f) == 0;
    std::fclose(f);
    return ok;
}

// write + flush to disk, the caller renames the file into place
// private_mode: only the owner may read it (POSIX; on Windows the ACL of the directory applies)
static bool admin_write_file(const fs::path & path, const std::string & content, bool private_mode) {
    FILE * f = nullptr;
#if defined(_WIN32)
    (void) private_mode;
    f = admin_fopen(path, "wb");
#else
    std::error_code ec;
    fs::remove(path, ec); // a stale temporary file must not keep its old permissions
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, private_mode ? 0600 : 0644);
    if (fd < 0) {
        return false;
    }
    f = fdopen(fd, "wb");
    if (f == nullptr) {
        ::close(fd);
        return false;
    }
#endif
    if (f == nullptr) {
        return false;
    }
    bool ok = std::fwrite(content.data(), 1, content.size(), f) == content.size();
    ok = std::fflush(f) == 0 && ok;
#if defined(_WIN32)
    ok = _commit(_fileno(f)) == 0 && ok;
#else
    ok = fsync(fileno(f)) == 0 && ok;
#endif
    ok = std::fclose(f) == 0 && ok;
    return ok;
}

// makes the renames in a directory durable (POSIX: fsync of the directory; NTFS journals them itself)
static void admin_fsync_dir(const fs::path & dir) {
#if defined(_WIN32)
    (void) dir;
#else
    const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        (void) ::fsync(fd);
        ::close(fd);
    }
#endif
}

// can a file be created in this directory right now? (probe file, removed at once)
static bool admin_dir_writable(const fs::path & dir) {
    static std::atomic<unsigned> counter{0};
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) {
        return false;
    }
    const fs::path probe = dir / (".provision-admin-probe-" + std::to_string(admin_getpid()) + "-" + std::to_string(counter.fetch_add(1)));
    FILE * f = admin_fopen(probe, "wb");
    if (f == nullptr) {
        return false;
    }
    std::fclose(f);
    fs::remove(probe, ec);
    return true;
}

// the directory itself, or its nearest ancestor that exists
static fs::path admin_nearest_existing(const fs::path & dir) {
    std::error_code ec;
    fs::path p = fs::absolute(dir, ec);
    if (ec) {
        p = dir;
    }
    while (!p.empty() && !fs::exists(p, ec)) {
        const fs::path parent = p.parent_path();
        if (parent == p) {
            break;
        }
        p = parent;
    }
    return p;
}

// free / total bytes of the filesystem that holds (or would hold) dir
static void admin_dir_space(const fs::path & dir, uint64_t & total_bytes, uint64_t & free_bytes) {
    total_bytes = 0;
    free_bytes  = 0;
    if (dir.empty()) {
        return;
    }
    std::error_code ec;
    const fs::space_info info = fs::space(admin_nearest_existing(dir), ec);
    if (!ec) {
        total_bytes = (uint64_t) info.capacity;
        free_bytes  = (uint64_t) info.available;
    }
}

static bool admin_random_bytes(unsigned char * out, size_t n) {
#if defined(_WIN32)
    size_t i = 0;
    while (i < n) {
        unsigned int value = 0;
        if (rand_s(&value) != 0) {
            return false;
        }
        for (size_t k = 0; k < sizeof(value) && i < n; k++, i++) {
            out[i] = (unsigned char) (value >> (8 * k));
        }
    }
    return true;
#else
    FILE * f = std::fopen("/dev/urandom", "rb");
    if (f == nullptr) {
        return false;
    }
    const size_t got = std::fread(out, 1, n, f);
    std::fclose(f);
    return got == n;
#endif
}

// compares all of `presented`, so the time does not depend on where the first difference is
static bool admin_secure_equals(const std::string & presented, const std::string & expected) {
    if (expected.empty()) {
        return false;
    }
    unsigned char diff = presented.size() == expected.size() ? 0 : 1;
    for (size_t i = 0; i < presented.size(); i++) {
        diff = (unsigned char) (diff | ((unsigned char) presented[i] ^ (unsigned char) expected[i % expected.size()]));
    }
    return diff == 0;
}

//
// small helpers (stage 2)
//

static int64_t admin_now_ms() {
    return (int64_t) std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static int64_t admin_wall_ms() {
    return (int64_t) std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// 2026-10-06T10:00:00Z
static std::string admin_iso_time(int64_t wall_ms) {
    const std::time_t t = (std::time_t) (wall_ms / 1000);
    std::tm tm = {};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

static bool admin_iequals(const std::string & a, const std::string & b) {
    return admin_to_lower(a) == admin_to_lower(b);
}

static bool admin_icontains(const std::string & haystack, const std::string & needle) {
    return admin_to_lower(haystack).find(admin_to_lower(needle)) != std::string::npos;
}

// a whole decimal integer: optional '-', at most 18 digits, nothing else
static bool admin_parse_int(const std::string & text, long long & out) {
    const std::string s = admin_strip(text);
    size_t i = 0;
    bool negative = false;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
        negative = s[i] == '-';
        i++;
    }
    if (i >= s.size() || s.size() - i > 18) {
        return false;
    }
    long long value = 0;
    for (; i < s.size(); i++) {
        if (!admin_is_digit(s[i])) {
            return false;
        }
        value = value * 10 + (s[i] - '0');
    }
    out = negative ? -value : value;
    return true;
}

// a non-negative decimal number with an optional fraction ("11264", "11264.5")
static bool admin_parse_number(const std::string & text, double & out) {
    const std::string s = admin_strip(text);
    if (s.empty()) {
        return false;
    }
    size_t i = 0;
    bool digits = false;
    bool dot    = false;
    for (; i < s.size(); i++) {
        if (admin_is_digit(s[i])) {
            digits = true;
        } else if (s[i] == '.' && !dot) {
            dot = true;
        } else {
            return false;
        }
    }
    if (!digits) {
        return false;
    }
    out = std::strtod(s.c_str(), nullptr);
    return true;
}

// removes terminal escape sequences (colours) and other control bytes but TAB
static std::string admin_strip_ansi(const std::string & s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        const unsigned char c = (unsigned char) s[i];
        if (c == 0x1B) {
            if (i + 1 < s.size() && s[i + 1] == '[') {
                // CSI: parameters and intermediates, then one final byte in 0x40..0x7E
                size_t j = i + 2;
                while (j < s.size() && ((unsigned char) s[j] < 0x40 || (unsigned char) s[j] > 0x7E)) {
                    j++;
                }
                i = j;
            } else {
                i++; // two-byte sequence
            }
            continue;
        }
        if (c < 0x20 && c != '\t') {
            continue;
        }
        out += (char) c;
    }
    return out;
}

static std::string admin_cut(const std::string & s, size_t max_len) {
    if (s.size() <= max_len) {
        return s;
    }
    // do not cut a UTF-8 sequence in half
    size_t n = max_len;
    while (n > 0 && ((unsigned char) s[n] & 0xC0) == 0x80) {
        n--;
    }
    return s.substr(0, n) + "...";
}

static std::vector<std::string> admin_split_lines(const std::string & text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t nl = text.find('\n', start);
        std::string line = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
        if (nl == std::string::npos) {
            break;
        }
        start = nl + 1;
    }
    while (!lines.empty() && lines.back().empty()) {
        lines.pop_back();
    }
    return lines;
}

// "0000:01:00.0", "00000000:01:00.0" (nvidia-smi), "01:00.0", any case -> "0000:01:00.0"; "" when it is no PCI address
static std::string admin_normalize_pci(const std::string & in) {
    const std::string s = admin_to_lower(admin_strip(in));
    std::vector<std::string> parts = string_split<std::string>(s, ':');
    if (parts.size() == 2) {
        parts.insert(parts.begin(), "0");
    }
    if (parts.size() != 3) {
        return "";
    }
    const size_t dot = parts[2].find('.');
    if (dot == std::string::npos) {
        return "";
    }
    const std::string fields[4] = { parts[0], parts[1], parts[2].substr(0, dot), parts[2].substr(dot + 1) };
    const size_t max_len[4] = { 8, 2, 2, 1 };
    unsigned long values[4] = {};
    for (int i = 0; i < 4; i++) {
        if (fields[i].empty() || fields[i].size() > max_len[i]) {
            return "";
        }
        for (char c : fields[i]) {
            if (!admin_is_digit(c) && !(c >= 'a' && c <= 'f')) {
                return "";
            }
        }
        values[i] = std::strtoul(fields[i].c_str(), nullptr, 16);
    }
    if (values[0] > 0xFFFF || values[2] > 0x1F || values[3] > 0x7) {
        return "";
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%04lx:%02lx:%02lx.%lx", values[0], values[1], values[2], values[3]);
    return buf;
}

// the whole environment of this process as NAME=value strings (UTF-8)
static std::vector<std::string> admin_environment() {
    std::vector<std::string> env;
#if defined(_WIN32)
    LPWCH block = GetEnvironmentStringsW();
    if (block != nullptr) {
        for (LPWCH e = block; *e; e += wcslen(e) + 1) {
            env.push_back(admin_wide_to_utf8(e));
        }
        FreeEnvironmentStringsW(block);
    }
#else
    for (char ** e = environ; e != nullptr && *e != nullptr; e++) {
        env.emplace_back(*e);
    }
#endif
    return env;
}

// the path of this executable, "" when unknown
static std::string admin_exe_path() {
#if defined(_WIN32)
    std::vector<wchar_t> buf(32768);
    const DWORD len = GetModuleFileNameW(nullptr, buf.data(), (DWORD) buf.size());
    if (len == 0 || len >= buf.size()) {
        return "";
    }
    buf[len] = L'\0';
    return admin_wide_to_utf8(buf.data());
#elif defined(__APPLE__) && defined(__MACH__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size + 1, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) {
        return "";
    }
    std::error_code ec;
    const fs::path p = fs::canonical(fs::path(buf.data()), ec);
    return ec ? std::string(buf.data()) : p.string();
#else
    std::vector<char> buf(4096, '\0');
    const ssize_t n = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (n <= 0) {
        return "";
    }
    return std::string(buf.data(), (size_t) n);
#endif
}

static bool admin_is_regular_file(const fs::path & p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

// the first PATH entry that holds an executable of this name, "" when none does
static std::string admin_find_in_path(const std::string & name) {
    const std::string path_env = admin_getenv("PATH");
#if defined(_WIN32)
    const char separator = ';';
    const std::vector<std::string> suffixes = { ".exe", "" };
#else
    const char separator = ':';
    const std::vector<std::string> suffixes = { "" };
#endif
    for (const auto & dir : string_split<std::string>(path_env, separator)) {
        const std::string d = admin_strip(dir);
        if (d.empty()) {
            continue;
        }
        for (const auto & suffix : suffixes) {
            const fs::path candidate = admin_path_from_utf8(d) / admin_path_from_utf8(name + suffix);
            if (!admin_is_regular_file(candidate)) {
                continue;
            }
#if !defined(_WIN32)
            if (access(candidate.c_str(), X_OK) != 0) {
                continue;
            }
#endif
            return admin_path_to_utf8(candidate);
        }
    }
    return "";
}

static std::string admin_json_str(const json & obj, const char * key) {
    if (obj.is_object() && obj.contains(key) && obj.at(key).is_string()) {
        return obj.at(key).get<std::string>();
    }
    return "";
}

static long long admin_json_int(const json & obj, const char * key, long long def) {
    if (obj.is_object() && obj.contains(key)) {
        const json & v = obj.at(key);
        if (v.is_number_integer()) {
            return v.get<long long>();
        }
        if (v.is_number_float()) {
            return (long long) v.get<double>();
        }
        if (v.is_string()) {
            long long n = 0;
            if (admin_parse_int(v.get<std::string>(), n)) {
                return n;
            }
        }
    }
    return def;
}

//
// settings: validation
//

// NAME = ^[A-Za-z][A-Za-z0-9_]*[0-9]+$
static bool admin_is_device_name(const std::string & s) {
    if (s.size() < 2 || s.size() > 64 || !admin_is_alpha(s.front()) || !admin_is_digit(s.back())) {
        return false;
    }
    for (char c : s) {
        if (!admin_is_alpha(c) && !admin_is_digit(c) && c != '_') {
            return false;
        }
    }
    return true;
}

// splits on ',' and ';', drops empty items
static std::vector<std::string> admin_split_devices(const std::string & value) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= value.size()) {
        const size_t end = value.find_first_of(",;", start);
        const std::string name = admin_strip(value.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (!name.empty()) {
            out.push_back(name);
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return out;
}

// accepts "" (clear), "none", or device names separated by ',' or ';'
// out is the canonical form handed to --device (comma-separated)
static bool admin_normalize_device(const std::string & in, std::string & out) {
    const std::string s = admin_strip(in);
    out.clear();
    if (s.empty() || s == "none") {
        out = s;
        return true;
    }
    if (s.front() == ',' || s.front() == ';' || s.back() == ',' || s.back() == ';' ||
        s.find(",,") != std::string::npos || s.find(";;") != std::string::npos ||
        s.find(",;") != std::string::npos || s.find(";,") != std::string::npos) {
        return false; // an empty name
    }
    const std::vector<std::string> names = admin_split_devices(s);
    for (const auto & name : names) {
        if (!admin_is_device_name(name)) {
            return false;
        }
    }
    out = admin_join(names, ",");
    return !out.empty();
}

// reads the "settings" object of the state file / a POST body into `out`
// fields the object does not mention are left alone (null counts as not mentioned)
// strict: throw on an invalid field (POST); otherwise note it in `problems` and skip the field (state file)
static void admin_parse_settings(const json & obj, admin_settings & out, bool strict, std::vector<std::string> & problems) {
    auto fail = [&](const std::string & message) {
        if (strict) {
            throw admin_bad_request(message);
        }
        problems.push_back(message);
    };

    if (obj.contains("models_max") && !obj.at("models_max").is_null()) {
        const json & v = obj.at("models_max");
        long long n = 0;
        if (v.is_number_integer()) {
            n = v.get<long long>();
        }
        if (!v.is_number_integer() || n < ADMIN_MODELS_MAX_MIN || n > ADMIN_MODELS_MAX_MAX) {
            fail(string_format("models_max must be an integer between %d and %d", ADMIN_MODELS_MAX_MIN, ADMIN_MODELS_MAX_MAX));
        } else {
            out.models_max = (int) n;
        }
    }

    if (obj.contains("default_device") && !obj.at("default_device").is_null()) {
        const json & v = obj.at("default_device");
        std::string device;
        if (!v.is_string() || !admin_normalize_device(v.get<std::string>(), device)) {
            fail("default_device must be \"\" (not set), \"none\" or device names such as CUDA0, separated by ',' or ';'");
        } else {
            out.default_device = device;
        }
    }

    if (obj.contains("pinned") && !obj.at("pinned").is_null()) {
        const json & v = obj.at("pinned");
        bool ok = v.is_array();
        std::vector<std::string> pinned;
        if (ok) {
            for (const auto & item : v) {
                if (!item.is_string() || item.get<std::string>().empty()) {
                    ok = false;
                    break;
                }
                const std::string id = item.get<std::string>();
                if (std::find(pinned.begin(), pinned.end(), id) == pinned.end()) {
                    pinned.push_back(id); // duplicates removed, order kept
                }
            }
        }
        if (!ok) {
            fail("pinned must be an array of model ids");
        } else {
            out.pinned = pinned;
        }
    }
}

//
// per-model settings: whitelist and value spelling
//
// API keys = INI names = keys in admin-models.json. Internally every key is
// addressed by its env name: common_preset::get_option / set_option /
// unset_option match env names only (common/preset.cpp).
//

enum admin_kind {
    ADMIN_KIND_DEVICE,   // "none" or registry device names joined by ','
    ADMIN_KIND_INT,      // decimal within [min, max]
    ADMIN_KIND_PARALLEL, // -1 (auto) or [min, max]
    ADMIN_KIND_NGL,      // "auto" | "all" | [min, max]
    ADMIN_KIND_ENUM,     // one of `values` ('|' separated)
    ADMIN_KIND_BOOL,     // "true" | "false"
};

struct admin_key_spec {
    const char * key;
    const char * env;
    admin_kind   kind;
    long long    min;
    long long    max;
    const char * values; // ADMIN_KIND_ENUM
    const char * def;    // built-in default in canonical spelling, nullptr = none (the option is simply absent)
    const char * group;  // placement | advanced | role
};

static const admin_key_spec ADMIN_KEYS[] = {
    { "device",              "LLAMA_ARG_DEVICE",              ADMIN_KIND_DEVICE,   0,   0,       nullptr,                   nullptr, "placement" },
    { "ctx-size",            "LLAMA_ARG_CTX_SIZE",            ADMIN_KIND_INT,      256, 1048576, nullptr,                   nullptr, "placement" },
    { "kv-unified-per-slot", "LLAMA_ARG_KV_UNIFIED_PER_SLOT", ADMIN_KIND_INT,      256, 1048576, nullptr,                   nullptr, "placement" },
    { "parallel",            "LLAMA_ARG_N_PARALLEL",          ADMIN_KIND_PARALLEL, 1,   16,      nullptr,                   "-1",    "placement" },
    { "n-gpu-layers",        "LLAMA_ARG_N_GPU_LAYERS",        ADMIN_KIND_NGL,      0,   999,     nullptr,                   "auto",  "advanced"  },
    { "flash-attn",          "LLAMA_ARG_FLASH_ATTN",          ADMIN_KIND_ENUM,     0,   0,       "on|off|auto",             "auto",  "advanced"  },
    { "cache-type-k",        "LLAMA_ARG_CACHE_TYPE_K",        ADMIN_KIND_ENUM,     0,   0,       "f16|bf16|q8_0|q4_0",      "f16",   "advanced"  },
    { "cache-type-v",        "LLAMA_ARG_CACHE_TYPE_V",        ADMIN_KIND_ENUM,     0,   0,       "f16|bf16|q8_0|q4_0",      "f16",   "advanced"  },
    { "ubatch-size",         "LLAMA_ARG_UBATCH",              ADMIN_KIND_INT,      32,  8192,    nullptr,                   "512",   "advanced"  },
    { "embeddings",          "LLAMA_ARG_EMBEDDINGS",          ADMIN_KIND_BOOL,     0,   0,       nullptr,                   "false", "role"      },
    { "pooling",             "LLAMA_ARG_POOLING",             ADMIN_KIND_ENUM,     0,   0,       "none|mean|cls|last|rank", nullptr, "role"      },
    { "reranking",           "LLAMA_ARG_RERANKING",           ADMIN_KIND_BOOL,     0,   0,       nullptr,                   "false", "role"      },
};

// coupled to an explicit `parallel` (never stored): keeps "context size" meaning one shared pool
static const char * const ADMIN_KV_UNIFIED_KEY = "kv-unified";
static const char * const ADMIN_KV_UNIFIED_ENV = "LLAMA_ARG_KV_UNIFIED";

static const admin_key_spec * admin_key_find(const std::string & key) {
    for (const auto & spec : ADMIN_KEYS) {
        if (key == spec.key) {
            return &spec;
        }
    }
    return nullptr;
}

static const char * admin_key_env(const std::string & key) {
    if (key == ADMIN_KV_UNIFIED_KEY) {
        return ADMIN_KV_UNIFIED_ENV;
    }
    const admin_key_spec * spec = admin_key_find(key);
    return spec != nullptr ? spec->env : nullptr;
}

static bool admin_is_truthy_text(const std::string & s) {
    const std::string v = admin_to_lower(admin_strip(s));
    return v == "true" || v == "1" || v == "on" || v == "enabled" || v == "yes";
}

static bool admin_is_falsey_text(const std::string & s) {
    const std::string v = admin_to_lower(admin_strip(s));
    return v == "false" || v == "0" || v == "off" || v == "disabled" || v == "no";
}

// a device token as --device would see it: printable ASCII, no blanks and none of ",;="
static bool admin_is_device_token(const std::string & s) {
    if (s.empty() || s.size() > 64) {
        return false;
    }
    for (char c : s) {
        if ((unsigned char) c <= 0x20 || (unsigned char) c > 0x7E || c == ',' || c == ';' || c == '=') {
            return false;
        }
    }
    return true;
}

// The canonical spelling of a value: the reload of server_models compares the option
// maps of running models as text, so "1024" vs "01024" or "on" vs "true" would restart
// a model. `v` comes from the API (any JSON type) or from a file / preset (a string).
// devices: names are resolved case-insensitively against this list and stored in its
// spelling; nullptr = only the syntax is checked (and the given spelling is kept).
static bool admin_canon_value(const admin_key_spec & spec, const json & v, std::string & out, std::string & err,
                              const std::vector<std::string> * devices) {
    out.clear();
    if (v.is_string() && v.get<std::string>().size() > ADMIN_VALUE_MAX) {
        err = string_format("%s: the value is longer than %zu bytes", spec.key, ADMIN_VALUE_MAX);
        return false;
    }
    auto as_int = [&](long long & n) -> bool {
        if (v.is_number_integer()) {
            n = v.get<long long>();
            return true;
        }
        return v.is_string() && admin_parse_int(v.get<std::string>(), n);
    };
    switch (spec.kind) {
        case ADMIN_KIND_INT: {
            long long n = 0;
            if (!as_int(n) || n < spec.min || n > spec.max) {
                err = string_format("%s must be an integer between %lld and %lld", spec.key, spec.min, spec.max);
                return false;
            }
            out = std::to_string(n);
            return true;
        }
        case ADMIN_KIND_PARALLEL: {
            long long n = 0;
            if (v.is_string() && admin_iequals(admin_strip(v.get<std::string>()), "auto")) {
                n = -1;
            } else if (!as_int(n)) {
                n = 0;
            }
            if (n != -1 && (n < spec.min || n > spec.max)) {
                err = string_format("%s must be -1 (auto) or an integer between %lld and %lld", spec.key, spec.min, spec.max);
                return false;
            }
            out = std::to_string(n);
            return true;
        }
        case ADMIN_KIND_NGL: {
            if (v.is_string()) {
                const std::string t = admin_to_lower(admin_strip(v.get<std::string>()));
                if (t == "auto" || t == "all") {
                    out = t;
                    return true;
                }
            }
            long long n = 0;
            if (as_int(n)) {
                if (n == -1) {
                    out = "auto";
                    return true;
                }
                if (n < -1) {
                    out = "all";
                    return true;
                }
                if (n >= spec.min && n <= spec.max) {
                    out = std::to_string(n);
                    return true;
                }
            }
            err = string_format("%s must be \"auto\", \"all\" or an integer between %lld and %lld", spec.key, spec.min, spec.max);
            return false;
        }
        case ADMIN_KIND_ENUM: {
            std::string t;
            if (v.is_boolean() && std::string(spec.key) == "flash-attn") {
                t = v.get<bool>() ? "on" : "off";
            } else if (v.is_string()) {
                t = admin_to_lower(admin_strip(v.get<std::string>()));
                if (std::string(spec.key) == "flash-attn") {
                    if (admin_is_truthy_text(t)) {
                        t = "on";
                    } else if (admin_is_falsey_text(t)) {
                        t = "off";
                    } else if (t == "-1") {
                        t = "auto";
                    }
                }
            }
            for (const auto & allowed : string_split<std::string>(spec.values, '|')) {
                if (!t.empty() && t == allowed) {
                    out = t;
                    return true;
                }
            }
            std::string list = spec.values;
            std::replace(list.begin(), list.end(), '|', ',');
            err = string_format("%s must be one of %s", spec.key, list.c_str());
            return false;
        }
        case ADMIN_KIND_BOOL: {
            if (v.is_boolean()) {
                out = v.get<bool>() ? "true" : "false";
                return true;
            }
            if (v.is_number_integer() && (v.get<long long>() == 0 || v.get<long long>() == 1)) {
                out = v.get<long long>() == 1 ? "true" : "false";
                return true;
            }
            if (v.is_string() && (admin_is_truthy_text(v.get<std::string>()) || admin_is_falsey_text(v.get<std::string>()))) {
                out = admin_is_truthy_text(v.get<std::string>()) ? "true" : "false";
                return true;
            }
            err = string_format("%s must be true or false", spec.key);
            return false;
        }
        case ADMIN_KIND_DEVICE: {
            if (!v.is_string()) {
                err = "device must be a string: \"none\" or device names separated by ','";
                return false;
            }
            const std::string t = admin_strip(v.get<std::string>());
            if (admin_iequals(t, "none")) {
                out = "none";
                return true;
            }
            std::vector<std::string> names;
            for (const auto & part : admin_split_devices(t)) {
                if (!admin_is_device_token(part)) {
                    err = "device: '" + part + "' is not a device name";
                    return false;
                }
                std::string name = part;
                if (devices != nullptr) {
                    bool found = false;
                    for (const auto & d : *devices) {
                        if (admin_iequals(d, part)) {
                            name  = d;
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        err = devices->empty()
                            ? "device: this server found no GPU (CPU-only build, or the GPU backend did not initialise); only \"none\" is accepted"
                            : "device: unknown device '" + part + "', this server has " + admin_join(*devices);
                        return false;
                    }
                }
                for (const auto & other : names) {
                    if (admin_iequals(other, name)) {
                        err = "device: '" + part + "' is listed twice";
                        return false;
                    }
                }
                names.push_back(name);
            }
            if (names.empty() || names.size() > 4) {
                err = "device must be \"none\" or 1 to 4 device names separated by ','";
                return false;
            }
            out = admin_join(names, ",");
            return true;
        }
    }
    err = string_format("%s: unsupported value", spec.key);
    return false;
}

// same meaning: compares the canonical spellings (device names case-insensitively)
static bool admin_values_equal(const std::string & key, const std::string & a, const std::string & b) {
    if (key == ADMIN_KV_UNIFIED_KEY) {
        return (admin_is_truthy_text(a) && admin_is_truthy_text(b)) || (admin_is_falsey_text(a) && admin_is_falsey_text(b));
    }
    const admin_key_spec * spec = admin_key_find(key);
    if (spec == nullptr) {
        return a == b;
    }
    std::string ca;
    std::string cb;
    std::string err;
    if (!admin_canon_value(*spec, json(a), ca, err, nullptr) || !admin_canon_value(*spec, json(b), cb, err, nullptr)) {
        return a == b;
    }
    return spec->kind == ADMIN_KIND_DEVICE ? admin_iequals(ca, cb) : ca == cb;
}

// the value in effect: the option, else the built-in default ("" = none)
static std::string admin_value_or_default(const admin_kv & values, const std::string & key) {
    const auto it = values.find(key);
    if (it != values.end()) {
        return it->second;
    }
    const admin_key_spec * spec = admin_key_find(key);
    return spec != nullptr && spec->def != nullptr ? spec->def : "";
}

// same effect for the child: an absent key counts as its built-in default
static bool admin_effect_equal(const admin_kv & a, const admin_kv & b) {
    std::set<std::string> keys;
    for (const auto & kv : a) {
        keys.insert(kv.first);
    }
    for (const auto & kv : b) {
        keys.insert(kv.first);
    }
    for (const auto & key : keys) {
        const std::string va = admin_value_or_default(a, key);
        const std::string vb = admin_value_or_default(b, key);
        if (va.empty() != vb.empty()) {
            return false;
        }
        if (!va.empty() && !admin_values_equal(key, va, vb)) {
            return false;
        }
    }
    return true;
}

// devices of a --device value: [] = all GPUs (unset), ["none"] = CPU only
static std::vector<std::string> admin_device_list(const std::string & value) {
    if (admin_iequals(admin_strip(value), "none")) {
        return { "none" };
    }
    return admin_split_devices(value);
}

//
// MODEL_DEVICES (docker-dual/entrypoint.sh): read only, it stays in the environment
//

// the section name the preset loader makes of "repo:tag" (copy of canonical_tag(), static in common/preset.cpp)
static std::string admin_canonical_section(const std::string & section) {
    if (section == "*" || section == COMMON_PRESET_DEFAULT_NAME) {
        return section;
    }
    const size_t colon = section.rfind(':');
    if (colon == std::string::npos) {
        return section;
    }
    const std::string tag = section.substr(colon + 1);
    static const std::regex re_tag("[-.]([A-Z0-9_]+)$", std::regex::icase);
    std::smatch m;
    std::string canon;
    if (std::regex_search(tag, m, re_tag)) {
        canon = admin_to_upper(m[1].str());
    } else {
        canon = admin_to_upper(tag);
    }
    return section.substr(0, colon + 1) + canon;
}

// ',' separates entries, ';' inside a value becomes ',', the last entry for a name wins
static admin_model_devices admin_parse_model_devices(const std::string & value) {
    admin_model_devices out;
    out.raw     = value;
    out.present = !admin_strip(value).empty();
    for (const auto & part : string_split<std::string>(value, ',')) {
        const std::string entry = admin_strip(part);
        const size_t eq = entry.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string name = admin_strip(entry.substr(0, eq));
        std::string dev = admin_strip(entry.substr(eq + 1));
        std::replace(dev.begin(), dev.end(), ';', ',');
        if (name.empty() || dev.empty()) {
            continue;
        }
        if (name == "*") {
            out.has_star = true;
            out.star     = dev;
        } else {
            out.sections[admin_canonical_section(name)] = dev;
        }
    }
    return out;
}

// what MODEL_DEVICES asks for this model: its section entry, else "*"; "" = nothing
static std::string admin_model_devices_for(const admin_model_devices & md, const std::string & name, bool is_section) {
    if (is_section) {
        const auto it = md.sections.find(name);
        if (it != md.sections.end()) {
            return it->second;
        }
    }
    return md.has_star ? md.star : "";
}

static bool admin_device_lists_equal(const std::string & a, const std::string & b) {
    const std::vector<std::string> la = admin_device_list(a);
    const std::vector<std::string> lb = admin_device_list(b);
    if (la.size() != lb.size()) {
        return false;
    }
    for (size_t i = 0; i < la.size(); i++) {
        if (!admin_iequals(la[i], lb[i])) {
            return false;
        }
    }
    return true;
}

//
// state file
//

static fs::path admin_state_path(const admin_context & ctx) {
    return ctx.state_dir / ADMIN_STATE_FILE;
}

// a corrupt or unreadable file never prevents startup: the error is kept for
// GET /admin/status and the file is left alone until a successful POST /admin/settings
static void admin_load_state(admin_context & ctx) {
    const fs::path path = admin_state_path(ctx);
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        return; // nothing saved yet
    }
    std::string text;
    if (!admin_read_file(path, text)) {
        ctx.state_error = "cannot read " + admin_path_to_utf8(path);
        return;
    }
    json doc;
    try {
        doc = json::parse(text);
    } catch (const std::exception & e) {
        ctx.state_error = "invalid JSON in " + admin_path_to_utf8(path) + ": " + e.what();
        return;
    }
    if (!doc.is_object()) {
        ctx.state_error = admin_path_to_utf8(path) + " is not a JSON object";
        return;
    }
    if (doc.contains("version")) {
        const json & version = doc.at("version");
        if (!version.is_number_integer() || version.get<long long>() != ADMIN_STATE_VERSION) {
            ctx.state_error = admin_path_to_utf8(path) + " has an unsupported version";
            return;
        }
    }
    if (!doc.contains("settings")) {
        return;
    }
    if (!doc.at("settings").is_object()) {
        ctx.state_error = "\"settings\" in " + admin_path_to_utf8(path) + " is not an object";
        return;
    }
    std::vector<std::string> problems;
    admin_parse_settings(doc.at("settings"), ctx.saved, false, problems);
    if (!problems.empty()) {
        ctx.state_error = "ignored invalid settings in " + admin_path_to_utf8(path) + ": " + admin_join(problems, "; ");
    }
}

// atomic: write <file>.tmp, keep the previous file as <file>.bak, rename into place
static bool admin_save_state(const admin_context & ctx, const admin_settings & settings, std::string & error) {
    json j_settings = json::object();
    if (settings.models_max.has_value()) {
        j_settings["models_max"] = *settings.models_max;
    }
    if (settings.default_device.has_value()) {
        j_settings["default_device"] = *settings.default_device;
    }
    if (settings.pinned.has_value()) {
        j_settings["pinned"] = *settings.pinned;
    }
    const json doc = {
        {"version",  ADMIN_STATE_VERSION},
        {"settings", j_settings},
    };

    const fs::path path     = admin_state_path(ctx);
    const fs::path path_tmp = ctx.state_dir / (std::string(ADMIN_STATE_FILE) + ".tmp");
    const fs::path path_bak = ctx.state_dir / (std::string(ADMIN_STATE_FILE) + ".bak");

    std::error_code ec;
    fs::create_directories(ctx.state_dir, ec);
    if (!admin_write_file(path_tmp, doc.dump(2) + "\n", false)) {
        fs::remove(path_tmp, ec);
        error = "cannot write " + admin_path_to_utf8(path_tmp);
        return false;
    }
    if (fs::exists(path, ec)) {
        fs::copy_file(path, path_bak, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            SRV_WRN("admin: cannot keep the previous state as %s: %s\n", admin_path_to_utf8(path_bak).c_str(), ec.message().c_str());
        }
    }
    fs::rename(path_tmp, path, ec);
    if (ec) {
        error = "cannot replace " + admin_path_to_utf8(path) + ": " + ec.message();
        fs::remove(path_tmp, ec);
        return false;
    }
    return true;
}

//
// per-model settings file: <state>/admin-models.json
//
// Separate from admin-state.json, whose format stays as in stage 1 (the
// entrypoint reads its "models_max" line, and a stage-1 binary must keep
// loading it). Own "version": 1. A file that cannot be read, or that has
// another version, is never overwritten: the settings become read-only.
//
//   { "version": 1,
//     "models": { "<id>": { "settings": {"ctx-size": "8192"}, "last_good": {...} | null,
//                           "phase": "pending|trial|failed|committed", "failures": 0,
//                           "updated": "<iso>", "device_pci": {"CUDA1": "0000:02:00.0"},
//                           "reverted": {"reason": ..., "time": ..., "settings": {...}} } } }
//

static fs::path admin_models_path(const admin_context & ctx) {
    return ctx.state_dir / ADMIN_MODELS_FILE;
}

static bool admin_is_model_id(const std::string & id) {
    return !id.empty() && id.size() <= ADMIN_MODEL_ID_MAX && admin_is_printable_ascii(id);
}

static bool admin_is_phase(const std::string & phase) {
    return phase == "pending" || phase == "trial" || phase == "failed" || phase == "committed";
}

// a "settings" object as stored; values that cannot be used are kept aside when `kept` is given
static admin_kv admin_kv_from_json(const json & obj, std::map<std::string, json> * kept,
                                   std::vector<std::string> & problems, const std::string & where) {
    admin_kv out;
    if (!obj.is_object()) {
        return out;
    }
    for (const auto & [key, value] : obj.items()) {
        const admin_key_spec * spec = admin_key_find(key);
        std::string canon;
        std::string err;
        if (spec == nullptr) {
            problems.push_back(where + ": unknown key '" + key + "' is not applied");
        } else if (value.is_null()) {
            continue;
        } else if (!admin_canon_value(*spec, value, canon, err, nullptr)) {
            problems.push_back(where + ": " + err);
        } else {
            out[key] = canon;
            continue;
        }
        if (kept != nullptr) {
            (*kept)[key] = value;
        }
    }
    return out;
}

static json admin_kv_to_json(const admin_kv & kv) {
    json out = json::object();
    for (const auto & [key, value] : kv) {
        out[key] = value;
    }
    return out;
}

static admin_model_entry admin_entry_from_json(const std::string & id, const json & j, std::vector<std::string> & problems) {
    admin_model_entry e;
    e.raw = j;
    const std::string where = "model '" + id + "'";
    if (j.contains("settings")) {
        e.settings = admin_kv_from_json(j.at("settings"), &e.kept, problems, where);
    }
    if (j.contains("last_good") && j.at("last_good").is_object()) {
        std::vector<std::string> ignored;
        e.last_good = admin_kv_from_json(j.at("last_good"), nullptr, ignored, where);
    }
    const std::string phase = admin_json_str(j, "phase");
    e.phase    = admin_is_phase(phase) ? phase : "pending";
    e.failures = (int) std::max<long long>(0, std::min<long long>(1000, admin_json_int(j, "failures", 0)));
    e.updated  = admin_json_str(j, "updated");
    if (j.contains("device_pci") && j.at("device_pci").is_object()) {
        for (const auto & [name, pci] : j.at("device_pci").items()) {
            if (pci.is_string()) {
                e.device_pci[name] = pci.get<std::string>();
            }
        }
    }
    if (j.contains("reverted") && j.at("reverted").is_object()) {
        e.reverted = j.at("reverted");
    }
    return e;
}

static json admin_entry_to_json(const admin_model_entry & e) {
    json j = e.raw.is_object() ? e.raw : json::object();
    json settings = admin_kv_to_json(e.settings);
    for (const auto & [key, value] : e.kept) {
        if (!settings.contains(key)) {
            settings[key] = value;
        }
    }
    j["settings"]  = settings;
    j["last_good"] = e.last_good.has_value() ? admin_kv_to_json(*e.last_good) : json(nullptr);
    j["phase"]     = e.phase;
    j["failures"]  = e.failures;
    j["updated"]   = e.updated;
    json pci = json::object();
    for (const auto & [name, id] : e.device_pci) {
        pci[name] = id;
    }
    j["device_pci"] = pci;
    if (e.reverted.is_object()) {
        j["reverted"] = e.reverted;
    } else {
        j.erase("reverted");
    }
    return j;
}

// false: the text is not a usable version-1 file (error says why; unknown_version tells the two cases apart)
static bool admin_models_parse(const std::string & text, admin_models_file & out, std::string & error, bool & unknown_version) {
    unknown_version = false;
    json doc;
    try {
        doc = json::parse(text);
    } catch (const std::exception & e) {
        error = std::string("invalid JSON: ") + e.what();
        return false;
    }
    if (!doc.is_object()) {
        error = "not a JSON object";
        return false;
    }
    if (doc.contains("version")) {
        const json & version = doc.at("version");
        if (!version.is_number_integer() || version.get<long long>() != ADMIN_MODELS_FILE_VERSION) {
            error = "unsupported version " + version.dump() + " (this server reads version 1)";
            unknown_version = true;
            return false;
        }
    }
    if (doc.contains("models") && !doc.at("models").is_object()) {
        error = "\"models\" is not an object";
        return false;
    }
    out = admin_models_file();
    out.raw = doc;
    if (doc.contains("models")) {
        for (const auto & [id, entry] : doc.at("models").items()) {
            if (!admin_is_model_id(id) || !entry.is_object()) {
                out.problems.push_back("entry '" + admin_cut(id, 64) + "' is not usable and is kept as it is");
                out.kept_entries[id] = entry;
                continue;
            }
            out.entries[id] = admin_entry_from_json(id, entry, out.problems);
        }
    }
    return true;
}

static std::string admin_models_dump(const admin_models_file & f) {
    json doc = f.raw.is_object() ? f.raw : json::object();
    doc["version"] = ADMIN_MODELS_FILE_VERSION;
    json models = json::object();
    for (const auto & [id, entry] : f.kept_entries) {
        models[id] = entry;
    }
    for (const auto & [id, e] : f.entries) {
        models[id] = admin_entry_to_json(e);
    }
    doc["models"] = models;
    return doc.dump(2) + "\n";
}

// atomic: <file>.tmp, fsync, previous file kept as <file>.bak, rename into place
// the caller holds mtx_file (or is server_admin_init, before any other thread exists)
static bool admin_models_save(admin_context & ctx, const admin_models_file & f, std::string & error) {
    if (f.read_only) {
        error = admin_path_to_utf8(admin_models_path(ctx)) + " is read-only for this server: " + f.error;
        return false;
    }
    const fs::path path     = admin_models_path(ctx);
    const fs::path path_tmp = ctx.state_dir / (std::string(ADMIN_MODELS_FILE) + ".tmp");
    const fs::path path_bak = ctx.state_dir / (std::string(ADMIN_MODELS_FILE) + ".bak");
    std::error_code ec;
    fs::create_directories(ctx.state_dir, ec);
    if (!admin_write_file(path_tmp, admin_models_dump(f), false)) {
        fs::remove(path_tmp, ec);
        error = "cannot write " + admin_path_to_utf8(path_tmp);
        return false;
    }
    if (fs::exists(path, ec)) {
        fs::copy_file(path, path_bak, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            SRV_WRN("admin: cannot keep the previous model settings as %s: %s\n", admin_path_to_utf8(path_bak).c_str(), ec.message().c_str());
        }
    }
    fs::rename(path_tmp, path, ec);
    if (ec) {
        error = "cannot replace " + admin_path_to_utf8(path) + ": " + ec.message();
        fs::remove(path_tmp, ec);
        return false;
    }
    // durable before the caller goes on: an upload publish writes the entry BEFORE it moves the model files into
    // place (and syncs that move), so the entry must not be the one of the two that a power loss undoes
    admin_fsync_dir(ctx.state_dir);
    return true;
}

// at startup: a corrupt file is left alone (read-only) and its .bak is used if it can be read;
// a file of another version is left alone and nothing is applied
static void admin_models_load(admin_context & ctx) {
    admin_models_file & f = ctx.models_file;
    f = admin_models_file();
    const fs::path path = admin_models_path(ctx);
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        return;
    }
    std::string text;
    std::string error;
    bool unknown_version = false;
    if (!admin_read_file(path, text, 16 * 1024 * 1024)) {
        error = "cannot read " + admin_path_to_utf8(path);
    } else if (admin_models_parse(text, f, error, unknown_version)) {
        if (!f.problems.empty()) {
            SRV_WRN("admin: %s: %s\n", admin_path_to_utf8(path).c_str(), admin_join(f.problems, "; ").c_str());
        }
        return;
    }
    f = admin_models_file();
    f.read_only = true;
    f.error     = admin_path_to_utf8(path) + ": " + error;
    if (unknown_version) {
        SRV_ERR("admin: %s - per-model settings are not applied and cannot be changed; the file is left untouched\n", f.error.c_str());
        return;
    }
    const fs::path path_bak = ctx.state_dir / (std::string(ADMIN_MODELS_FILE) + ".bak");
    std::string text_bak;
    std::string error_bak;
    admin_models_file bak;
    if (admin_read_file(path_bak, text_bak, 16 * 1024 * 1024) && admin_models_parse(text_bak, bak, error_bak, unknown_version)) {
        bak.read_only     = true;
        bak.backup_loaded = true;
        bak.error         = f.error + "; using the previous version " + admin_path_to_utf8(path_bak);
        f = bak;
        SRV_ERR("admin: %s - the per-model settings cannot be changed until the file is fixed or removed\n", f.error.c_str());
        return;
    }
    SRV_ERR("admin: %s - no per-model settings are applied and they cannot be changed until the file is fixed or removed; "
            "the file is left untouched\n", f.error.c_str());
}

// the settings the hook applies (caller holds mtx_state)
static void admin_rebuild_applied(admin_context & ctx) {
    ctx.applied.clear();
    for (const auto & [id, e] : ctx.models_file.entries) {
        if (!e.settings.empty()) {
            ctx.applied[id] = e.settings;
        }
    }
}

// Read-modify-write of the per-model settings. Every writer holds mtx_file for the whole update, so
// none is lost; readers (the hook, GETs) only take mtx_state. Lock order: mtx_apply -> mtx_file -> mtx_state.
// fn changes the copy it gets and returns false when nothing changed. persist: the change must reach the
// file (false + error when it cannot be written; nothing changes then); otherwise a write failure only
// logs and the change stays in memory (e.g. the phase of a start, or a read-only file).
// Bumps the overlay generation when the settings the hook applies change.
template <typename F>
static bool admin_models_update(admin_context & ctx, F fn, bool persist, std::string & error) {
    std::lock_guard<std::mutex> lk_file(ctx.mtx_file);
    admin_models_file next;
    {
        std::lock_guard<std::mutex> lk(ctx.mtx_state);
        next = ctx.models_file;
    }
    if (!fn(next)) {
        return true;
    }
    if (next.read_only) {
        if (persist) {
            error = admin_path_to_utf8(admin_models_path(ctx)) + " is read-only for this server: " + next.error;
            return false;
        }
    } else if (!admin_models_save(ctx, next, error)) {
        if (persist) {
            return false;
        }
        SRV_ERR("admin: the model settings are changed in memory only: %s\n", error.c_str());
        error.clear();
    }
    std::lock_guard<std::mutex> lk(ctx.mtx_state);
    const std::map<std::string, admin_kv> before = ctx.applied;
    ctx.models_file = next;
    admin_rebuild_applied(ctx);
    if (ctx.applied != before) {
        ctx.overlay_gen++; // a reload that read the old settings is redone, see server_admin_overlay_changed()
    }
    return true;
}

// replaces the settings of an entry that failed with its last good ones (or none); caller holds mtx_state
static void admin_entry_revert(admin_model_entry & e, const std::string & reason, const std::string & message) {
    json failed = admin_kv_to_json(e.settings);
    e.reverted = {
        {"reason",   reason},
        {"message",  message},
        {"time",     admin_iso_time(admin_wall_ms())},
        {"settings", failed},
        {"failures", e.failures},
    };
    e.settings = e.last_good.has_value() ? *e.last_good : admin_kv();
    e.phase    = "committed";
    e.failures = 0;
}

// Startup: settings that were being tried (or had failed) when the server stopped are not
// applied again; the model starts with its last good settings and reports reverted_at_startup.
static void admin_models_startup_revert(admin_context & ctx) {
    bool changed = false;
    for (auto & [id, e] : ctx.models_file.entries) {
        if (e.phase != "trial" && e.phase != "failed") {
            continue;
        }
        const std::string was = e.phase;
        admin_entry_revert(e, "startup", "the server stopped while these settings were " +
            std::string(was == "trial" ? "being tried" : "failing") + ", so the model starts with its last good settings");
        e.reverted_at_startup = true;
        changed = true;
        SRV_WRN("admin: model %s: settings in phase '%s' at startup are not applied again, using its last good settings\n",
                id.c_str(), was.c_str());
    }
    if (changed && !ctx.models_file.read_only) {
        std::string error;
        if (!admin_models_save(ctx, ctx.models_file, error)) {
            SRV_ERR("admin: %s\n", error.c_str());
        }
    }
}

//
// admin key
//

// The admin key in the text of a key file: its first line without surrounding blanks; a UTF-8 BOM is skipped.
// UTF-16 (a BOM or NUL bytes, e.g. written by Windows PowerShell 5.1 with '>') and any other text that is not
// printable ASCII is refused, with the reason in `problem`: such a key could never be sent in a request.
static bool admin_key_from_text(const std::string & text, std::string & key, std::string & problem) {
    key.clear();
    const bool utf16_bom = text.size() >= 2 &&
        (((unsigned char) text[0] == 0xFF && (unsigned char) text[1] == 0xFE) ||
         ((unsigned char) text[0] == 0xFE && (unsigned char) text[1] == 0xFF));
    if (utf16_bom || text.find('\0') != std::string::npos) {
        problem = "it is UTF-16 text, save it as ASCII or UTF-8";
        return false;
    }
    const size_t start = string_starts_with(text, "\xEF\xBB\xBF") ? 3 : 0; // UTF-8 BOM
    const size_t end   = text.find_first_of("\r\n", start);
    const std::string line = admin_strip(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
    if (line.empty()) {
        problem = "its first line is empty";
        return false;
    }
    if (!admin_is_printable_ascii(line)) {
        problem = "the key is not plain ASCII text (another encoding?)";
        return false;
    }
    key = line;
    return true;
}

// PROVISION_ADMIN_KEY > PROVISION_ADMIN_KEY_FILE > <state>/admin.key > generated into <state>/admin.key
// the value is never logged
static void admin_resolve_key(admin_context & ctx, const std::string & env_key, const std::string & env_key_file) {
    ctx.key.clear();
    ctx.key_source.clear();

    const std::string key_from_env = admin_strip(env_key);
    if (!key_from_env.empty()) {
        ctx.key        = key_from_env;
        ctx.key_source = "env";
        SRV_INF("%s", "admin: using the admin key from PROVISION_ADMIN_KEY\n");
        if (!admin_is_printable_ascii(key_from_env)) {
            SRV_WRN("%s", "admin: PROVISION_ADMIN_KEY is not plain printable ASCII, a browser cannot send such a key\n");
        }
        return;
    }

    if (!env_key_file.empty()) {
        std::string text;
        std::string key;
        std::string problem = "it cannot be read";
        if (admin_read_file(admin_path_from_utf8(env_key_file), text) && admin_key_from_text(text, key, problem)) {
            ctx.key        = key;
            ctx.key_source = "file";
            SRV_INF("admin: using the admin key from PROVISION_ADMIN_KEY_FILE (%s)\n", env_key_file.c_str());
            return;
        }
        SRV_ERR("admin: PROVISION_ADMIN_KEY_FILE (%s) is not usable: %s; falling back to the state directory\n",
                env_key_file.c_str(), problem.c_str());
    }

    const fs::path path = ctx.state_dir / ADMIN_KEY_FILE;
    {
        std::string text;
        if (admin_read_file(path, text)) {
            std::string key;
            std::string problem;
            if (admin_key_from_text(text, key, problem)) {
                ctx.key        = key;
                ctx.key_source = "generated";
                SRV_INF("admin: using the admin key stored in %s\n", admin_path_to_utf8(path).c_str());
                return;
            }
            SRV_ERR("admin: %s is not usable: %s; generating a new admin key into it\n", admin_path_to_utf8(path).c_str(), problem.c_str());
        }
    }

    // generate: 32 random bytes, hex
    unsigned char raw[32];
    if (!admin_random_bytes(raw, sizeof(raw))) {
        SRV_ERR("%s", "admin: no secure random source available, cannot generate an admin key\n");
        return;
    }
    static const char hex[] = "0123456789abcdef";
    std::string generated;
    for (unsigned char b : raw) {
        generated += hex[b >> 4];
        generated += hex[b & 0x0F];
    }
    const fs::path path_tmp = ctx.state_dir / (std::string(ADMIN_KEY_FILE) + ".tmp");
    std::error_code ec;
    if (!admin_write_file(path_tmp, generated + "\n", true)) {
        fs::remove(path_tmp, ec);
        SRV_ERR("admin: cannot write the generated admin key to %s (state directory not writable?)\n", admin_path_to_utf8(path).c_str());
        return;
    }
    fs::rename(path_tmp, path, ec);
    if (ec) {
        SRV_ERR("admin: cannot write the generated admin key to %s: %s\n", admin_path_to_utf8(path).c_str(), ec.message().c_str());
        fs::remove(path_tmp, ec);
        return;
    }
    ctx.key        = generated;
    ctx.key_source = "generated";
    SRV_WRN("admin: no admin key configured, generated one into %s\n", admin_path_to_utf8(path).c_str());
}

//
// helper processes
//
// Everything that may create a GPU context (device inventory, dry-run fit) or that can
// hang in a driver (vendor tools such as nvidia-smi) runs in a separate process, started
// by a detached thread. A caller waits for it with its own bound (admin_job_wait). At the
// deadline the thread terminates the process (SIGKILL / TerminateProcess) and publishes the
// result at once; reaping a process that is stuck in a driver can take forever, so `alive`
// stays true until it is reaped and no second process of that kind is started meanwhile.
//

std::shared_ptr<admin_job> admin_job_start(
        const std::string & label,
        const std::vector<std::vector<std::string>> & commands,
        const std::vector<std::string> & env,
        int timeout_ms,
        bool ignore_terminate) {
    auto job = std::make_shared<admin_job>();
    job->label            = label;
    job->t_start          = admin_now_ms();
    job->ignore_terminate = ignore_terminate;

    std::thread([job, commands, env, timeout_ms]() {
        const int64_t deadline = job->t_start + timeout_ms;
        std::string output;
        bool spawned   = true;
        bool timed_out = false;
        int  exit_code = -1;

        auto publish = [&]() {
            std::lock_guard<std::mutex> lk(job->mtx);
            job->spawned   = spawned;
            job->timed_out = timed_out;
            job->exit_code = exit_code;
            job->output    = output;
            job->done      = true;
            job->t_done    = admin_now_ms();
            job->cv.notify_all();
        };

        for (size_t ci = 0; ci < commands.size(); ci++) {
            if (ci > 0) {
                output += std::string("\n") + ADMIN_NEXT_COMMAND + "\n";
            }
            server_subproc proc;
            const int options = subprocess_option_no_window | subprocess_option_combined_stdout_stderr;
            if (!proc.sproc.create(commands[ci], options, env)) {
                spawned   = false;
                exit_code = -1;
                break;
            }
            server_subproc::waiter waiter;
            bool eof = !proc.has_output();
            char buf[4096];
            while (true) {
                if (!eof) {
                    int n = 0;
                    while ((n = proc.read_output(buf, sizeof(buf))) > 0) {
                        if (output.size() < ADMIN_PROC_MAX_OUTPUT) {
                            output.append(buf, (size_t) n);
                        }
                    }
                    if (n < 0) {
                        eof = true;
                    }
                }
                if (eof && !proc.is_alive()) {
                    break;
                }
                const int64_t now = admin_now_ms();
                if (now >= deadline) {
                    timed_out = true;
                    break;
                }
                if (eof) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                } else {
                    std::vector<server_subproc *> procs = { &proc };
                    std::vector<bool> ready;
                    waiter.wait(procs, ready, std::min<int64_t>(deadline - now, 200));
                }
            }
            if (timed_out) {
                if (!job->ignore_terminate) {
                    proc.terminate();
                }
                publish(); // the caller must not wait for the reaping
                proc.join();
                job->alive.store(false);
                return;
            }
            exit_code = proc.join();
        }
        publish();
        job->alive.store(false);
    }).detach();

    return job;
}

// true when the job finished within wait_ms
bool admin_job_wait(const std::shared_ptr<admin_job> & job, int64_t wait_ms) {
    std::unique_lock<std::mutex> lk(job->mtx);
    return job->cv.wait_for(lk, std::chrono::milliseconds(std::max<int64_t>(0, wait_ms)), [&]() { return job->done; });
}

bool admin_job_is_done(const std::shared_ptr<admin_job> & job) {
    std::lock_guard<std::mutex> lk(job->mtx);
    return job->done;
}

// a helper that was stopped at its deadline but has not exited a while later (stuck in a GPU driver:
// uninterruptible sleep survives SIGKILL); a normal kill is reaped within milliseconds
static constexpr int64_t ADMIN_STUCK_GRACE_MS = 3000;

static bool admin_job_is_stuck(const std::shared_ptr<admin_job> & job) {
    std::lock_guard<std::mutex> lk(job->mtx);
    return job->done && job->timed_out && job->alive.load() && admin_now_ms() - job->t_done >= ADMIN_STUCK_GRACE_MS;
}

static std::string admin_stuck_message(const char * what, const std::shared_ptr<admin_job> & job) {
    std::lock_guard<std::mutex> lk(job->mtx);
    return string_format("the previous %s helper was stopped %lld s ago but has not exited (its process is probably stuck in "
                         "the GPU driver); no new one is started while it lives. If this does not clear, check the GPU "
                         "driver (e.g. dmesg) and restart the server", what, (long long) ((admin_now_ms() - job->t_done) / 1000));
}

// the {...} of the (last) PROVISION_ADMIN_RESULT line, null when there is none
json admin_result_line(const std::string & output) {
    json found = nullptr;
    for (const auto & line : admin_split_lines(output)) {
        const size_t pos = line.find(ADMIN_RESULT_PREFIX);
        if (pos == std::string::npos) {
            continue;
        }
        json j = json::parse_no_throw(line.substr(pos + strlen(ADMIN_RESULT_PREFIX)));
        if (!j.is_discarded() && j.is_object()) {
            found = j;
        }
    }
    return found;
}

// the last lines of a helper's output that are not the result line, for error messages
std::vector<std::string> admin_output_tail(const std::string & output, size_t n) {
    std::vector<std::string> lines;
    for (const auto & line : admin_split_lines(output)) {
        const std::string clean = admin_strip(admin_strip_ansi(line));
        if (clean.empty() || clean.find(ADMIN_RESULT_PREFIX) != std::string::npos || clean == ADMIN_NEXT_COMMAND) {
            continue;
        }
        lines.push_back(admin_cut(clean, ADMIN_LOG_MAX_LINE));
    }
    if (lines.size() > n) {
        lines.erase(lines.begin(), lines.end() - (std::ptrdiff_t) n);
    }
    return lines;
}

// command line that starts this binary again (as the unified "llama" binary does for its children)
static std::vector<std::string> admin_self_command(const admin_context & ctx) {
    std::vector<std::string> cmd = { ctx.bin_path };
    const std::string app_cmd = admin_getenv("LLAMA_APP_CMD");
    if (!app_cmd.empty()) {
        cmd.push_back(app_cmd);
    }
    return cmd;
}

static std::vector<std::string> admin_helper_env(const admin_context & ctx, const std::string & mode, const std::string & file = "") {
    std::vector<std::string> env = ctx.env_snapshot;
    env.push_back(std::string(ADMIN_HELPER_ENV) + "=" + mode);
    if (!file.empty()) {
        env.push_back(std::string(ADMIN_HELPER_FILE_ENV) + "=" + file);
    }
    return env;
}

// canned output (tests): "# ignore-terminate" makes the job leave the process alone at its deadline
static bool admin_canned_ignores_terminate(const std::string & path) {
    std::string text;
    if (!admin_read_file(admin_path_from_utf8(path), text, 4 * 1024 * 1024)) {
        return false;
    }
    for (const auto & line : admin_split_lines(text)) {
        if (admin_strip(line) == "# ignore-terminate") {
            return true;
        }
    }
    return false;
}

//
// GPU backends of this build (router side: names only, no device query)
//

static bool admin_is_gpu_backend_name(const std::string & name) {
    const std::string n = admin_to_lower(name);
    return n != "cpu" && n != "blas" && n != "rpc" && n != "meta";
}

// ggml-cuda.dll, libggml-vulkan.so, libggml-hip.so, ... next to the binary
static std::vector<std::string> admin_find_gpu_libraries(const std::string & bin_path) {
    static const char * const kinds[] = { "cuda", "hip", "rocm", "vulkan", "sycl", "musa", "opencl", "cann", "metal", "webgpu", "openvino", "zdnn", "virtgpu", "hexagon" };
    std::vector<std::string> out;
    if (bin_path.empty()) {
        return out;
    }
    std::error_code ec;
    const fs::path dir = admin_path_from_utf8(bin_path).parent_path();
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    const fs::directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        const std::string name = admin_path_to_utf8(it->path().filename());
        const std::string lower = admin_to_lower(name);
        const bool lib = string_ends_with(lower, ".dll") || string_ends_with(lower, ".dylib") || lower.find(".so") != std::string::npos;
        if (!lib || (!string_starts_with(lower, "ggml-") && !string_starts_with(lower, "libggml-"))) {
            continue;
        }
        for (const char * kind : kinds) {
            if (lower.find(std::string("ggml-") + kind) != std::string::npos) {
                out.push_back(name);
                break;
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// the backend registries of this process: ggml_backend_reg_* / ggml_backend_dev_name only, which read what
// the registration stored; no device is queried (memory / props / type would create a GPU context)
static void admin_scan_registry(admin_context & ctx) {
    ctx.registry_backends.clear();
    ctx.registry_devices.clear();
    for (size_t i = 0; i < ggml_backend_reg_count(); i++) {
        ggml_backend_reg_t reg = ggml_backend_reg_get(i);
        const std::string name = ggml_backend_reg_name(reg);
        const size_t n = ggml_backend_reg_dev_count(reg);
        ctx.registry_backends.push_back({ name, n });
        if (!admin_is_gpu_backend_name(name)) {
            continue;
        }
        for (size_t j = 0; j < n; j++) {
            const char * dev_name = ggml_backend_dev_name(ggml_backend_reg_dev_get(reg, j));
            if (dev_name != nullptr && admin_is_device_token(dev_name)) {
                ctx.registry_devices.push_back(dev_name);
            }
        }
    }
}

static std::string admin_vendor_of(const std::string & backend, const std::string & description) {
    const std::string b = admin_to_lower(backend);
    if (b == "cuda") {
        return "nvidia";
    }
    if (b == "rocm" || b == "hip") {
        return "amd";
    }
    if (b == "musa") {
        return "moorethreads";
    }
    if (b == "cann") {
        return "huawei";
    }
    if (b == "metal" || b == "mtl") {
        return "apple";
    }
    const std::string d = admin_to_lower(description);
    if (d.find("nvidia") != std::string::npos || d.find("geforce") != std::string::npos || d.find("quadro") != std::string::npos ||
        d.find("tesla") != std::string::npos) {
        return "nvidia";
    }
    if (d.find("amd") != std::string::npos || d.find("radeon") != std::string::npos || d.find("instinct") != std::string::npos) {
        return "amd";
    }
    if (d.find("intel") != std::string::npos) {
        return "intel";
    }
    if (d.find("apple") != std::string::npos) {
        return "apple";
    }
    if (b == "sycl") {
        return "intel";
    }
    return "unknown";
}

// the JSON a helper (or a canned file) gave for the inventory
static bool admin_inventory_from_json(const json & j, admin_inventory & inv, std::string & error) {
    if (!j.is_object()) {
        error = "the inventory is not a JSON object";
        return false;
    }
    if (j.contains("ok") && j.at("ok").is_boolean() && !j.at("ok").get<bool>()) {
        const json err = j.contains("error") ? j.at("error") : json();
        error = err.is_object() ? admin_json_str(err, "message") : (err.is_string() ? err.get<std::string>() : "the inventory helper failed");
        return false;
    }
    if (!j.contains("devices") || !j.at("devices").is_array()) {
        error = "the inventory has no \"devices\" array";
        return false;
    }
    inv.devices.clear();
    for (const auto & d : j.at("devices")) {
        if (!d.is_object()) {
            continue;
        }
        admin_inv_device dev;
        dev.name        = admin_json_str(d, "name");
        dev.backend     = admin_json_str(d, "backend");
        dev.description = admin_json_str(d, "description");
        dev.type        = admin_json_str(d, "type");
        dev.device_id   = admin_json_str(d, "device_id");
        dev.memory_free  = (uint64_t) std::max<long long>(0, admin_json_int(d, "memory_free", 0));
        dev.memory_total = (uint64_t) std::max<long long>(0, admin_json_int(d, "memory_total", 0));
        if (!admin_is_device_token(dev.name)) {
            continue;
        }
        if (dev.type.empty()) {
            dev.type = "gpu";
        }
        dev.pci    = admin_normalize_pci(dev.device_id);
        dev.vendor = admin_json_str(d, "vendor");
        if (dev.vendor.empty()) {
            dev.vendor = admin_vendor_of(dev.backend, dev.description);
        }
        inv.devices.push_back(dev);
    }
    inv.backends.clear();
    if (j.contains("backends") && j.at("backends").is_array()) {
        for (const auto & b : j.at("backends")) {
            if (b.is_string()) {
                inv.backends.push_back(b.get<std::string>());
            } else if (b.is_object()) {
                inv.backends.push_back(admin_json_str(b, "name"));
            }
        }
    }
    return true;
}

// Runs the inventory helper (this binary, PROVISION_ADMIN_HELPER=inventory): it loads the backends,
// creates a context on every device to read its memory and PCI id, prints one result line and exits.
// PROVISION_ADMIN_INVENTORY=off skips it, file:<path> prints a canned result instead (tests).
static admin_inventory admin_run_inventory(admin_context & ctx) {
    admin_inventory inv;
    inv.ran     = true;
    inv.time_ms = admin_wall_ms();
    const std::string mode = admin_strip(ctx.inventory_mode);
    if (admin_iequals(mode, "off")) {
        inv.source  = "off";
        inv.error   = "disabled";
        inv.message = "the device inventory is disabled (PROVISION_ADMIN_INVENTORY=off)";
        return inv;
    }
    if (!common_subproc::is_supported() || ctx.bin_path.empty()) {
        inv.source  = "helper";
        inv.error   = "unsupported";
        inv.message = "this build cannot start helper processes";
        return inv;
    }
    std::shared_ptr<admin_job> job;
    if (string_starts_with(mode, "file:")) {
        const std::string file = mode.substr(5);
        inv.source = "file";
        job = admin_job_start("inventory", { admin_self_command(ctx) }, admin_helper_env(ctx, "cat", file),
                              ADMIN_INVENTORY_TIMEOUT_MS, admin_canned_ignores_terminate(file));
    } else {
        inv.source = "helper";
        job = admin_job_start("inventory", { admin_self_command(ctx) }, admin_helper_env(ctx, "inventory"), ADMIN_INVENTORY_TIMEOUT_MS);
    }
    {
        std::lock_guard<std::mutex> lk(ctx.mtx_inv);
        ctx.inv_job = job;
    }
    admin_job_wait(job, ADMIN_INVENTORY_TIMEOUT_MS + 2000);
    std::lock_guard<std::mutex> lk(job->mtx);
    inv.duration_ms = admin_now_ms() - job->t_start;
    if (!job->done) {
        inv.error   = "timeout";
        inv.message = "the device inventory helper did not answer";
        return inv;
    }
    if (!job->spawned) {
        inv.error   = "failed";
        inv.message = "cannot start the device inventory helper (" + ctx.bin_path + ")";
        return inv;
    }
    if (job->timed_out) {
        inv.error   = "timeout";
        inv.message = string_format("the device inventory helper did not finish within %d s (a GPU driver may be hanging)",
                                    ADMIN_INVENTORY_TIMEOUT_MS / 1000);
        return inv;
    }
    json result = admin_result_line(job->output);
    if (!result.is_object() && inv.source == "file") {
        result = json::parse_no_throw(job->output);
    }
    if (result.is_discarded() || !result.is_object()) {
        const std::vector<std::string> tail = admin_output_tail(job->output, 3);
        inv.error   = job->exit_code != 0 ? "failed" : "parse_error";
        inv.message = string_format("the device inventory helper exited with status %d without a result", job->exit_code) +
                      (tail.empty() ? std::string() : ": " + admin_join(tail, " | "));
        return inv;
    }
    std::string error;
    if (!admin_inventory_from_json(result, inv, error)) {
        inv.error   = "failed";
        inv.message = error;
        return inv;
    }
    inv.ok = true;
    return inv;
}

// devices a model may name: the inventory's GPUs, else (inventory failed) the router's registry names
static std::vector<std::string> admin_devices_from(const admin_inventory & inv, const std::vector<std::string> & registry) {
    if (!inv.ok) {
        return registry;
    }
    std::vector<std::string> out;
    for (const auto & d : inv.devices) {
        if (d.type == "gpu" || d.type == "igpu") {
            out.push_back(d.name);
        }
    }
    return out;
}

static void admin_set_inventory(admin_context & ctx, const admin_inventory & inv) {
    std::lock_guard<std::mutex> lk(ctx.mtx_inv);
    ctx.inventory     = inv;
    ctx.build_devices = admin_devices_from(inv, ctx.registry_devices);
}

static std::vector<std::string> admin_build_devices(admin_context & ctx) {
    std::lock_guard<std::mutex> lk(ctx.mtx_inv);
    return ctx.build_devices;
}

static admin_inventory admin_get_inventory(admin_context & ctx) {
    std::lock_guard<std::mutex> lk(ctx.mtx_inv);
    return ctx.inventory;
}

// the PCI id the inventory reports for a device name, "" = none
static std::string admin_device_pci(const admin_inventory & inv, const std::string & name) {
    for (const auto & d : inv.devices) {
        if (admin_iequals(d.name, name)) {
            return d.pci;
        }
    }
    return "";
}

// GPU backend state of this build: ok | cpu_only | no_devices
static json admin_gpu_backend_json(admin_context & ctx) {
    const std::vector<std::string> devices = admin_build_devices(ctx);
    bool gpu_registry = false;
    json backends = json::array();
    for (const auto & [name, n] : ctx.registry_backends) {
        backends.push_back({ {"name", name}, {"devices", n} });
        if (admin_is_gpu_backend_name(name)) {
            gpu_registry = true;
        }
    }
    std::string state;
    std::string message;
    if (!devices.empty()) {
        state   = "ok";
        message = string_format("%zu GPU device(s) found", devices.size());
    } else if (!gpu_registry && ctx.gpu_libraries.empty()) {
        state   = "cpu_only";
        message = "this server build has no GPU backend: models run on the CPU";
    } else {
        state   = "no_devices";
        message = std::string("GPU backend did not initialise") +
                  (gpu_registry ? "" : " (its library could not be loaded)") +
                  ": check the GPU driver, permissions such as /dev/kfd or /dev/dri, or whether the container was started without GPUs";
    }
    return {
        {"state",     state},
        {"message",   message},
        {"libraries", ctx.gpu_libraries},
        {"backends",  backends},
    };
}

//
// address allow-list of the /admin routes (PROVISION_ADMIN_ALLOW_FROM, PROVISION_ADMIN_TRUSTED_PROXIES)
//
// Off by default: any address may use /admin (with the admin key), most deployments lock /admin in
// their reverse proxy. Addresses are parsed here rather than with inet_pton(), so that the rules are
// the same on every platform: strict dotted IPv4 (no leading zeros, which some parsers read as
// octal), RFC 4291 IPv6, no host names. Matching is numeric, never on the text.
//

static bool admin_parse_ipv4(const std::string & s, uint8_t out[4]) {
    size_t i = 0;
    for (int part = 0; part < 4; part++) {
        if (part > 0) {
            if (i >= s.size() || s[i] != '.') {
                return false;
            }
            i++;
        }
        const size_t start = i;
        unsigned value = 0;
        while (i < s.size() && admin_is_digit(s[i]) && i - start < 3) {
            value = value * 10 + (unsigned) (s[i] - '0');
            i++;
        }
        const size_t n = i - start;
        if (n == 0 || value > 255 || (n > 1 && s[start] == '0')) {
            return false;
        }
        out[part] = (uint8_t) value;
    }
    return i == s.size();
}

static int admin_hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// RFC 4291 text form: eight groups of 1-4 hex digits, at most one "::" for a run of zero groups,
// optionally an IPv4 address as the last 32 bits
static bool admin_parse_ipv6(const std::string & s, uint8_t out[16]) {
    uint16_t groups[8] = {};
    int      n   = 0;
    int      gap = -1; // number of groups before the "::", -1 = none
    size_t   i   = 0;
    if (s.compare(0, 2, "::") == 0) {
        gap = 0;
        i   = 2;
    } else if (!s.empty() && s[0] == ':') {
        return false;
    }
    while (i < s.size()) {
        if (n == 8) {
            return false;
        }
        const size_t end = s.find(':', i);
        const std::string group = s.substr(i, end == std::string::npos ? std::string::npos : end - i);
        if (end == std::string::npos && group.find('.') != std::string::npos) {
            uint8_t v4[4];
            if (n > 6 || !admin_parse_ipv4(group, v4)) {
                return false;
            }
            groups[n++] = (uint16_t) ((v4[0] << 8) | v4[1]);
            groups[n++] = (uint16_t) ((v4[2] << 8) | v4[3]);
            break;
        }
        if (group.empty() || group.size() > 4) {
            return false;
        }
        unsigned value = 0;
        for (char c : group) {
            const int digit = admin_hex_digit(c);
            if (digit < 0) {
                return false;
            }
            value = value * 16 + (unsigned) digit;
        }
        groups[n++] = (uint16_t) value;
        if (end == std::string::npos) {
            break;
        }
        i = end + 1;
        if (i < s.size() && s[i] == ':') {
            if (gap >= 0) {
                return false; // a second "::"
            }
            gap = n;
            i++;
        } else if (i == s.size()) {
            return false; // a single ':' at the end
        }
    }
    if (gap < 0 ? n != 8 : n > 7) {
        return false;
    }
    const int n_tail = gap < 0 ? 0 : n - gap;
    const int n_head = n - n_tail;
    std::memset(out, 0, 16);
    for (int g = 0; g < n; g++) {
        const int pos = g < n_head ? g : 8 - n_tail + (g - n_head);
        out[2 * pos]     = (uint8_t) (groups[g] >> 8);
        out[2 * pos + 1] = (uint8_t) (groups[g] & 0xff);
    }
    return true;
}

static bool admin_ip_is_v4(const admin_ip & ip) {
    for (int i = 0; i < 10; i++) {
        if (ip.b[i] != 0) {
            return false;
        }
    }
    return ip.b[10] == 0xff && ip.b[11] == 0xff;
}

// a literal IPv4 or IPv6 address (IPv4 is stored IPv4-mapped)
static bool admin_parse_ip(const std::string & s, admin_ip & out) {
    out = admin_ip();
    if (s.find(':') != std::string::npos) {
        return admin_parse_ipv6(s, out.b);
    }
    uint8_t v4[4];
    if (!admin_parse_ipv4(s, v4)) {
        return false;
    }
    out.b[10] = 0xff;
    out.b[11] = 0xff;
    std::memcpy(out.b + 12, v4, 4);
    return true;
}

// dotted IPv4 (also for ::ffff:a.b.c.d), else RFC 5952 IPv6 (lower case, the longest run of
// two or more zero groups, the first of equal runs, written as "::")
static std::string admin_ip_to_string(const admin_ip & ip) {
    if (admin_ip_is_v4(ip)) {
        return string_format("%u.%u.%u.%u", ip.b[12], ip.b[13], ip.b[14], ip.b[15]);
    }
    unsigned groups[8];
    for (int g = 0; g < 8; g++) {
        groups[g] = ((unsigned) ip.b[2 * g] << 8) | ip.b[2 * g + 1];
    }
    int best     = -1;
    int best_len = 1;
    for (int g = 0; g < 8; ) {
        if (groups[g] != 0) {
            g++;
            continue;
        }
        int len = 0;
        while (g + len < 8 && groups[g + len] == 0) {
            len++;
        }
        if (len > best_len) {
            best     = g;
            best_len = len;
        }
        g += len;
    }
    std::string out;
    for (int g = 0; g < 8; g++) {
        if (g == best) {
            out += "::";
            g += best_len - 1;
            continue;
        }
        if (!out.empty() && out.back() != ':') {
            out += ':';
        }
        out += string_format("%x", groups[g]);
    }
    return out;
}

static void admin_ip_mask(admin_ip & ip, int prefix) {
    for (int i = 0; i < 16; i++) {
        const int bits = prefix - 8 * i;
        if (bits >= 8) {
            continue;
        }
        ip.b[i] = bits <= 0 ? 0 : (uint8_t) (ip.b[i] & (0xff << (8 - bits)));
    }
}

static bool admin_ip_in_range(const admin_ip & ip, const admin_ip_range & range) {
    admin_ip masked = ip;
    admin_ip_mask(masked, range.prefix);
    return std::memcmp(masked.b, range.net.b, 16) == 0;
}

static bool admin_ip_in(const std::vector<admin_ip_range> & ranges, const admin_ip & ip) {
    for (const auto & range : ranges) {
        if (admin_ip_in_range(ip, range)) {
            return true;
        }
    }
    return false;
}

// two ranges share an address when the wider one holds the first address of the other
static bool admin_ranges_overlap(const admin_ip_range & a, const admin_ip_range & b) {
    return a.prefix <= b.prefix ? admin_ip_in_range(b.net, a) : admin_ip_in_range(a.net, b);
}

static std::string admin_range_text(const admin_ip_range & range) {
    const bool v4   = admin_ip_is_v4(range.net) && range.prefix >= 96;
    const int  bits = v4 ? range.prefix - 96 : range.prefix;
    return admin_ip_to_string(range.net) + (range.prefix == 128 ? std::string() : "/" + std::to_string(bits));
}

// "<address>" or "<address>/<prefix length>"; on failure `error` says why
static bool admin_parse_range(const std::string & entry, admin_ip_range & out, std::string & error, bool & host_bits) {
    const size_t slash = entry.find('/');
    const std::string addr = entry.substr(0, slash);
    if (addr.find('%') != std::string::npos) {
        error = "zone ids (%...) are not supported";
        return false;
    }
    if (addr.find('[') != std::string::npos || addr.find(']') != std::string::npos) {
        error = "write IPv6 addresses without brackets";
        return false;
    }
    admin_ip ip;
    if (!admin_parse_ip(addr, ip)) {
        error = "not an IPv4 or IPv6 address or range (host names are not supported)";
        return false;
    }
    const bool v4 = addr.find(':') == std::string::npos;
    int prefix = 128;
    if (slash != std::string::npos) {
        const std::string text = entry.substr(slash + 1);
        bool digits = !text.empty() && text.size() <= 3;
        for (char c : text) {
            digits = digits && admin_is_digit(c);
        }
        const int max_bits = v4 ? 32 : 128;
        const int bits     = digits ? std::atoi(text.c_str()) : -1;
        if (bits < 0 || bits > max_bits) {
            error = string_format("the prefix length after '/' must be a number from 0 to %d", max_bits);
            return false;
        }
        prefix = v4 ? 96 + bits : bits;
        if (!v4 && admin_ip_is_v4(ip) && prefix < 96) {
            // "::ffff:10.0.0.0/8" is ::/8 (every IPv4 address and more), not the IPv4 range 10.0.0.0/8
            error = bits <= 32
                ? string_format("an IPv4-mapped address takes a prefix length from 96 to 128; for the IPv4 range "
                                "write %s/%d", admin_ip_to_string(ip).c_str(), bits)
                : std::string("an IPv4-mapped address takes a prefix length from 96 to 128");
            return false;
        }
    }
    out.net    = ip;
    out.prefix = prefix;
    admin_ip_mask(out.net, prefix);
    host_bits = std::memcmp(out.net.b, ip.b, 16) != 0;
    out.text  = admin_range_text(out);
    return true;
}

// A list separated by commas and / or blanks. Every invalid entry is logged by name; false when
// there was one. An entry with host bits set is read as its network, with a note in `warnings`.
static bool admin_parse_ranges(const char * var, const std::string & value, std::vector<admin_ip_range> & out,
                               std::vector<std::string> & warnings) {
    bool ok = true;
    std::string entry;
    for (size_t i = 0; i <= value.size(); i++) {
        const char c = i < value.size() ? value[i] : ',';
        if (c != ',' && c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            entry += c;
            continue;
        }
        if (entry.empty()) {
            continue;
        }
        admin_ip_range range;
        std::string error;
        bool host_bits = false;
        if (admin_parse_range(entry, range, error, host_bits)) {
            if (host_bits) {
                warnings.push_back(string_format("%s: '%s' has host bits set, it is read as %s", var, entry.c_str(),
                                                 range.text.c_str()));
                SRV_WRN("admin: %s\n", warnings.back().c_str());
            }
            out.push_back(range);
        } else {
            std::string shown = admin_cut(entry, 80);
            for (char & ch : shown) {
                if ((unsigned char) ch < 0x20 || (unsigned char) ch > 0x7E) {
                    ch = '?';
                }
            }
            SRV_ERR("admin: %s: invalid entry '%s': %s\n", var, shown.c_str(), error.c_str());
            ok = false;
        }
        entry.clear();
    }
    return ok;
}

static std::vector<std::string> admin_range_texts(const std::vector<admin_ip_range> & ranges) {
    std::vector<std::string> out;
    for (const auto & range : ranges) {
        out.push_back(range.text);
    }
    return out;
}

// The allow-list from the two variables. Unset or empty PROVISION_ADMIN_ALLOW_FROM = off. An entry
// that cannot be read in either variable locks /admin for everybody (fail closed) instead of
// silently dropping a restriction the operator asked for.
static admin_access admin_access_from_env(const std::string & allow_from, const std::string & trusted_proxies,
                                          const common_params & params) {
    admin_access acc;
    const bool allow_ok   = admin_parse_ranges("PROVISION_ADMIN_ALLOW_FROM", allow_from, acc.allow, acc.warnings);
    const bool trusted_ok = admin_parse_ranges("PROVISION_ADMIN_TRUSTED_PROXIES", trusted_proxies, acc.trusted,
                                               acc.warnings);

    if (allow_ok && acc.allow.empty()) {
        // off: any address may use /admin
        if (!trusted_ok || !acc.trusted.empty()) {
            acc.warnings.push_back("PROVISION_ADMIN_TRUSTED_PROXIES has no effect without PROVISION_ADMIN_ALLOW_FROM");
            SRV_WRN("admin: %s\n", acc.warnings.back().c_str());
        }
        return acc;
    }

    acc.enabled = true;
    if (!allow_ok || !trusted_ok) {
        acc.locked = true;
        SRV_ERR("%s", "admin: the /admin address allow-list has an invalid entry (see above): every /admin request is "
                      "refused with 403 until PROVISION_ADMIN_ALLOW_FROM / PROVISION_ADMIN_TRUSTED_PROXIES are fixed\n");
        return acc;
    }

    for (const auto & t : acc.trusted) {
        for (const auto & a : acc.allow) {
            if (admin_ranges_overlap(t, a)) {
                acc.warnings.push_back(string_format(
                    "the trusted proxy %s is also on the allow-list: a request from there without a usable "
                    "X-Forwarded-For counts as coming from the proxy and is allowed", t.text.c_str()));
                SRV_WRN("admin: %s\n", acc.warnings.back().c_str());
                break;
            }
        }
        // a trusted address can claim any client address with an X-Forwarded-For of its own: a range
        // wider than a few proxies (one that holds more IPv4 addresses than a /24, or an IPv6 range
        // shorter than /64) most likely holds clients too
        admin_ip_range v4_space;
        v4_space.net.b[10] = 0xff;
        v4_space.net.b[11] = 0xff;
        v4_space.prefix    = 96;
        if (admin_ranges_overlap(t, v4_space) ? t.prefix < 96 + 24 : t.prefix < 64) {
            acc.warnings.push_back(string_format(
                "the trusted proxy range %s is wide: every address in it can claim any client address with an "
                "X-Forwarded-For of its own (list only the reverse proxies' own addresses)", t.text.c_str()));
            SRV_WRN("admin: %s\n", acc.warnings.back().c_str());
        }
    }
    if (params.ui_mcp_proxy) {
        acc.warnings.push_back("the CORS proxy (--ui-mcp-proxy / --agent) is on: through it any client can make this "
                               "server request its own /admin from an address of this host");
        SRV_WRN("admin: %s\n", acc.warnings.back().c_str());
    }
    const std::string forwarded = acc.trusted.empty()
        ? std::string(", X-Forwarded-For is ignored")
        : ", X-Forwarded-For is used from the trusted proxies " + admin_join(admin_range_texts(acc.trusted));
    SRV_INF("admin: /admin is limited to %s (PROVISION_ADMIN_ALLOW_FROM)%s\n",
            admin_join(admin_range_texts(acc.allow)).c_str(), forwarded.c_str());
    return acc;
}

// one X-Forwarded-For entry: an address, "[IPv6]" or "[IPv6]:port", "IPv4:port"; a zone id is ignored
static bool admin_parse_forwarded_hop(const std::string & hop, admin_ip & out) {
    std::string addr = hop;
    if (!addr.empty() && addr[0] == '[') {
        const size_t close = addr.find(']');
        if (close == std::string::npos) {
            return false;
        }
        const std::string rest = addr.substr(close + 1);
        bool port_ok = rest.empty() || (rest.size() > 1 && rest[0] == ':');
        for (size_t i = 1; i < rest.size(); i++) {
            port_ok = port_ok && admin_is_digit(rest[i]);
        }
        if (!port_ok) {
            return false;
        }
        addr = addr.substr(1, close - 1);
    } else if (std::count(addr.begin(), addr.end(), ':') == 1) {
        const size_t colon = addr.find(':');
        bool port_ok = colon + 1 < addr.size();
        for (size_t i = colon + 1; i < addr.size(); i++) {
            port_ok = port_ok && admin_is_digit(addr[i]);
        }
        if (!port_ok) {
            return false;
        }
        addr = addr.substr(0, colon);
    }
    return admin_parse_ip(addr.substr(0, addr.find('%')), out);
}

// The address the allow-list is checked against: the TCP peer, or, only when the peer is a trusted
// proxy, the first address of X-Forwarded-For that is not a trusted proxy, counted from the right
// (every proxy appends the address it got the request from; what the client wrote itself is left
// of that and is never looked at). An X-Forwarded-For that cannot be used (missing, sent twice, an
// entry that is not an address) leaves the peer as the client. Without trusted proxies
// X-Forwarded-For is ignored: any client can send one.
static admin_client admin_client_of(const admin_access & acc, const server_http_req & req) {
    admin_client c;
    c.peer = req.remote_addr;
    admin_ip peer_ip;
    if (!admin_parse_ip(req.remote_addr.substr(0, req.remote_addr.find('%')), peer_ip)) {
        return c; // no network address (Unix socket) or one that cannot be read: unknown
    }
    c.known   = true;
    c.ip      = peer_ip;
    c.address = admin_ip_to_string(peer_ip);
    if (!admin_ip_in(acc.trusted, peer_ip)) {
        return c;
    }

    const std::string * forwarded = nullptr;
    int n_forwarded = 0;
    for (const auto & [name, value] : req.headers) {
        if (admin_to_lower(name) == "x-forwarded-for") {
            forwarded = &value;
            n_forwarded++;
        }
    }
    if (n_forwarded == 0 || admin_strip(*forwarded).empty()) {
        c.note = "the trusted proxy sent no X-Forwarded-For";
        return c;
    }
    if (n_forwarded > 1) {
        c.note = "X-Forwarded-For was sent more than once";
        return c;
    }
    const std::vector<std::string> hops = string_split<std::string>(*forwarded, ',');
    for (size_t i = hops.size(); i-- > 0; ) {
        admin_ip hop;
        if (!admin_parse_forwarded_hop(admin_strip(hops[i]), hop)) {
            c.note          = "X-Forwarded-For has an entry that is not an address";
            c.ip            = peer_ip;
            c.address       = admin_ip_to_string(peer_ip);
            c.via_forwarded = false;
            return c;
        }
        c.ip            = hop;
        c.address       = admin_ip_to_string(hop);
        c.via_forwarded = true;
        if (!admin_ip_in(acc.trusted, hop)) {
            break; // the client
        }
        // another trusted proxy: keep walking (all of them trusted: the leftmost one is the client)
    }
    return c;
}

//
// init
//

// For a state directory the server has just created: it holds the admin key, so only the account the server
// runs as may use it. POSIX: mode 0700. Windows: a DACL of its own instead of the one inherited from the parent
// (below %ProgramData% every local user could read the key otherwise): full control for SYSTEM, the
// Administrators and the account of this process, inherited by everything created inside.
static void admin_restrict_dir(const fs::path & dir) {
#if defined(_WIN32)
    std::string error;
    LPWSTR sid = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        error = "OpenProcessToken failed with error " + std::to_string(GetLastError());
    } else {
        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        std::vector<unsigned char> user(size > 0 ? size : 1);
        if (size == 0 || !GetTokenInformation(token, TokenUser, user.data(), size, &size) ||
            !ConvertSidToStringSidW(((TOKEN_USER *) user.data())->User.Sid, &sid)) {
            error = "cannot read the account of this process, error " + std::to_string(GetLastError());
        }
        CloseHandle(token);
    }
    if (error.empty()) {
        // D:P = protected DACL (not inherited), OICI = inherited by files and directories inside, FA = full access
        const std::wstring sddl = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;" + std::wstring(sid) + L")";
        PACL dacl = nullptr;
        BOOL present = FALSE;
        BOOL defaulted = FALSE;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr) ||
            !GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) || !present) {
            error = "cannot build the access control list, error " + std::to_string(GetLastError());
        } else {
            std::wstring wpath = dir.wstring();
            const DWORD rc = SetNamedSecurityInfoW(&wpath[0], SE_FILE_OBJECT,
                DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr);
            if (rc != ERROR_SUCCESS) {
                error = "SetNamedSecurityInfo failed with error " + std::to_string(rc);
            }
        }
    }
    if (sid != nullptr) {
        LocalFree(sid);
    }
    if (sd != nullptr) {
        LocalFree(sd);
    }
    if (!error.empty()) {
        SRV_WRN("admin: cannot restrict the access to %s to this account: %s\n", admin_path_to_utf8(dir).c_str(), error.c_str());
    }
#else
    std::error_code ec;
    fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace, ec);
#endif
}

static fs::path admin_default_state_dir() {
#if defined(_WIN32)
    const std::string program_data = admin_getenv("ProgramData");
    const fs::path base = program_data.empty() ? fs::path("C:\\ProgramData") : admin_path_from_utf8(program_data);
    return base / "ProvisionLLM" / "admin";
#else
    return fs::path("/var/lib/provision-llm-admin");
#endif
}

// Number of models the preset file marks load-on-startup (best effort, 0 when unknown).
// server_models refuses to start when it exceeds models_max, so a saved models_max below it
// would keep the server from starting at all - with the admin UI out of reach to fix it.
static int admin_count_startup_models(const common_params & params) {
    if (params.models_preset.empty()) {
        return 0;
    }
    try {
        const common_preset_context ctx_preset(LLAMA_EXAMPLE_SERVER);
        common_preset global;
        const common_presets sections = ctx_preset.load_from_ini(params.models_preset, global); // fills global from [*]
        const common_presets presets  = ctx_preset.cascade(global, sections);
        int n = 0;
        for (const auto & item : presets) {
            std::string value;
            if (item.second.get_option(COMMON_ARG_PRESET_LOAD_ON_STARTUP, value) && common_arg_utils::is_truthy(value)) {
                n++;
            }
        }
        return n;
    } catch (const std::exception &) {
        return 0; // the router reports a broken preset file itself
    }
}

void server_admin_init(common_params & params) {
    admin_context & ctx = admin_ctx();
    ctx.enabled.store(false);

    // a router child never gets here (it is not a router), this is a second line of defence
    if (server_child().is_child()) {
        return;
    }

    const std::string env_enabled   = admin_getenv("PROVISION_ADMIN");
    const std::string env_key       = admin_getenv("PROVISION_ADMIN_KEY");
    const std::string env_key_file  = admin_getenv("PROVISION_ADMIN_KEY_FILE");
    const std::string env_state_dir = admin_getenv("PROVISION_ADMIN_STATE_DIR");
    const std::string env_device    = admin_getenv("PROVISION_ADMIN_DEFAULT_DEVICE");
    const std::string env_pinned    = admin_getenv("PROVISION_ADMIN_PINNED");
    const std::string env_gpu_probe = admin_getenv("PROVISION_ADMIN_GPU_PROBE");
    const std::string env_inventory = admin_getenv("PROVISION_ADMIN_INVENTORY");
    const std::string env_allow     = admin_getenv("PROVISION_ADMIN_ALLOW_FROM");
    const std::string env_trusted   = admin_getenv("PROVISION_ADMIN_TRUSTED_PROXIES");
    const std::string model_devices = admin_getenv("MODEL_DEVICES"); // only read, stays in the environment
    admin_upload_read_env(); // stage 3: the test-only PROVISION_ADMIN_TEST_* hooks of the upload code

    // always, also when the layer is disabled: the key must not reach a child process
    admin_scrub_env();

    if (!admin_is_enabled_value(env_enabled)) {
        return;
    }

    // start from a clean slate (the CLI can start the server more than once per process)
    ctx.state_dir = admin_strip(env_state_dir).empty() ? admin_default_state_dir() : admin_path_from_utf8(admin_strip(env_state_dir));
    ctx.env_default_device.clear();
    ctx.env_pinned.clear();
    ctx.device_locked = false;
    ctx.locked_device.clear();
    ctx.routes = nullptr;
    ctx.saved  = admin_settings();
    ctx.state_error.clear();
    ctx.pinned_applied.clear();
    ctx.pinned_pushed = false;
    ctx.models_file   = admin_models_file();
    ctx.applied.clear();
    ctx.gpu_probe      = admin_strip(env_gpu_probe).empty() ? "auto" : admin_strip(env_gpu_probe);
    ctx.inventory_mode = admin_strip(env_inventory).empty() ? "auto" : admin_strip(env_inventory);
    ctx.bin_path       = admin_exe_path();
    ctx.env_snapshot   = admin_environment(); // after the scrub: what the children get
    {
        std::lock_guard<std::mutex> lk(ctx.mtx_seen);
        ctx.seen.clear();
        ctx.cli_locks.clear();
        ctx.seen_gen = 0;
    }

    {
        std::error_code ec;
        const bool existed = fs::exists(ctx.state_dir, ec);
        fs::create_directories(ctx.state_dir, ec);
        if (ec) {
            SRV_ERR("admin: cannot create the state directory %s: %s - set PROVISION_ADMIN_STATE_DIR to a writable directory\n",
                    admin_path_to_utf8(ctx.state_dir).c_str(), ec.message().c_str());
        } else if (!existed) {
            admin_restrict_dir(ctx.state_dir); // it holds the admin key
        }
    }

    admin_load_state(ctx);
    if (!ctx.state_error.empty()) {
        SRV_ERR("admin: %s - using the startup defaults, the file is left untouched until settings are saved\n", ctx.state_error.c_str());
    }

    if (!admin_strip(env_device).empty()) {
        std::string device;
        if (admin_normalize_device(env_device, device)) {
            ctx.env_default_device = device;
        } else {
            SRV_WRN("admin: ignoring invalid PROVISION_ADMIN_DEFAULT_DEVICE='%s'\n", env_device.c_str());
        }
    }
    for (const auto & part : string_split<std::string>(env_pinned, ',')) {
        const std::string id = admin_strip(part);
        if (!id.empty() && std::find(ctx.env_pinned.begin(), ctx.env_pinned.end(), id) == ctx.env_pinned.end()) {
            ctx.env_pinned.push_back(id);
        }
    }
    ctx.model_devices = admin_parse_model_devices(model_devices);
    ctx.device_locked = ctx.model_devices.has_star;
    ctx.locked_device = ctx.model_devices.star;

    admin_resolve_key(ctx, env_key, env_key_file);

    // per-model settings: applied by the first load_models() in the server_models constructor
    admin_models_load(ctx);
    admin_models_startup_revert(ctx);
    admin_rebuild_applied(ctx);

    // the devices the children will see: the inventory helper, run before any model loads
    admin_scan_registry(ctx);
    ctx.gpu_libraries = admin_find_gpu_libraries(ctx.bin_path);
    {
        const admin_inventory inv = admin_run_inventory(ctx);
        admin_set_inventory(ctx, inv);
        if (inv.ok) {
            std::vector<std::string> names;
            for (const auto & d : inv.devices) {
                names.push_back(d.name + (d.pci.empty() ? "" : " (" + d.pci + ")"));
            }
            SRV_INF("admin: device inventory: %s (%.1f s)\n", names.empty() ? "no GPU devices" : admin_join(names).c_str(),
                    inv.duration_ms / 1000.0);
        } else {
            SRV_WRN("admin: device inventory unavailable (%s): %s - using the device names of the router's own backends\n",
                    inv.error.c_str(), inv.message.c_str());
        }
    }

    if (ctx.saved.models_max.has_value()) {
        const int n_startup = admin_count_startup_models(params);
        if (*ctx.saved.models_max < n_startup) {
            const std::string message = string_format(
                "models_max = %d from %s is ignored: %d models are marked load-on-startup in the preset file",
                *ctx.saved.models_max, admin_path_to_utf8(admin_state_path(ctx)).c_str(), n_startup);
            SRV_ERR("admin: %s\n", message.c_str());
            ctx.state_error = ctx.state_error.empty() ? message : ctx.state_error + "; " + message;
            ctx.saved.models_max.reset();
        }
    }
    if (ctx.saved.models_max.has_value() && *ctx.saved.models_max != params.models_max) {
        SRV_INF("admin: models_max = %d from %s (overrides --models-max %d)\n",
                *ctx.saved.models_max, admin_path_to_utf8(admin_state_path(ctx)).c_str(), params.models_max);
        params.models_max = *ctx.saved.models_max;
    }

    // the address allow-list of /admin (off unless PROVISION_ADMIN_ALLOW_FROM is set)
    ctx.access = admin_access_from_env(env_allow, env_trusted, params);

    ctx.enabled.store(true);

    const std::string key_info = ctx.key.empty() ? "UNAVAILABLE (protected endpoints answer 503)" : "source: " + ctx.key_source;
    SRV_INF("admin: enabled, state directory %s, admin key %s\n", admin_path_to_utf8(ctx.state_dir).c_str(), key_info.c_str());
    if (ctx.device_locked) {
        SRV_INF("admin: MODEL_DEVICES sets '*=%s', the default device for new models is locked\n", ctx.locked_device.c_str());
    }
}

bool server_admin_enabled() {
    return admin_ctx().enabled.load();
}

//
// hook: preset overlay
//

// the device the overlay applies right now, "" = none (caller holds mtx_state)
static std::string admin_overlay_device(const admin_context & ctx) {
    if (ctx.device_locked) {
        return ""; // MODEL_DEVICES already put the device into the [*] section of the preset file
    }
    return ctx.saved.default_device.has_value() ? *ctx.saved.default_device : ctx.env_default_device;
}

// what GET /admin/settings reports as default_device (caller holds mtx_state)
static std::string admin_reported_device(const admin_context & ctx) {
    if (ctx.device_locked) {
        return ctx.locked_device;
    }
    return ctx.saved.default_device.has_value() ? *ctx.saved.default_device : ctx.env_default_device;
}

// "GPU for new models" names a device this server does not have; only judged when GPU devices were
// found (a CPU-only build keeps the stage-1 behaviour: the child reports the unknown device itself)
static bool admin_default_device_missing(const std::string & device, const std::vector<std::string> & devices) {
    if (device.empty() || admin_iequals(admin_strip(device), "none") || devices.empty()) {
        return false;
    }
    for (const auto & name : admin_split_devices(device)) {
        bool found = false;
        for (const auto & d : devices) {
            found = found || admin_iequals(d, name);
        }
        if (!found) {
            return true;
        }
    }
    return false;
}

// what the admin pass works with, the same for every model of one reload
struct admin_eval_env {
    admin_kv                    cli;          // whitelisted keys on the router command line: merged last, they win
    std::vector<std::string>    devices;      // GPU devices found; empty = none (CPU-only build / not initialised)
    const admin_model_devices * md = nullptr; // MODEL_DEVICES
    std::string                 fill_device;  // "GPU for new models" in effect, "" = off
};

struct admin_eval {
    admin_kv    values;                       // whitelisted keys (+ kv-unified) after the admin pass
    std::map<std::string, std::string> inactive; // stored key -> why it is not applied
    std::string default_device;               // set by "GPU for new models"
    std::string model_devices_value;          // what MODEL_DEVICES asks for this model, "" = nothing
    bool        model_devices_lock = false;   // ... and the model really has it
};

// The admin pass for one model, as a pure function of what the model had before it
// (`inherited`) and of the stored settings. Used by the hook and, to tell whether a
// change has any effect, by POST /admin/models/settings and the fit check.
static admin_eval admin_evaluate(const std::string & name, server_model_source source, const admin_kv & inherited,
                                 const admin_kv * overrides, const admin_eval_env & env) {
    admin_eval ev;
    ev.values = inherited;

    // MODEL_DEVICES is applied by the docker-dual entrypoint to the preset file: a lock only when the
    // device the model has before this pass is the one MODEL_DEVICES asks for (else the entrypoint
    // did not apply it, e.g. a CRLF models.ini, and nothing enforces it)
    if (env.md != nullptr) {
        ev.model_devices_value = admin_model_devices_for(*env.md, name, source == SERVER_MODEL_SOURCE_PRESET);
        if (!ev.model_devices_value.empty()) {
            const auto it = inherited.find("device");
            ev.model_devices_lock = it != inherited.end() && admin_device_lists_equal(it->second, ev.model_devices_value);
        }
    }

    bool parallel_set = false;
    if (overrides != nullptr) {
        for (const auto & [key, value] : *overrides) {
            const admin_key_spec * spec = admin_key_find(key);
            if (spec == nullptr) {
                ev.inactive[key] = "unknown_key";
                continue;
            }
            if (env.cli.count(key)) {
                ev.inactive[key] = "router_cli";
                continue;
            }
            if (key == "device" && ev.model_devices_lock) {
                ev.inactive[key] = "model_devices";
                continue;
            }
            // re-validated: a hand-edited file must never reach a child's argument parser
            std::string canon;
            std::string err;
            if (!admin_canon_value(*spec, json(value), canon, err, nullptr)) {
                ev.inactive[key] = "invalid";
                continue;
            }
            if (key == "device" && canon != "none") {
                std::string resolved;
                if (!admin_canon_value(*spec, json(canon), resolved, err, &env.devices)) {
                    ev.inactive[key] = "device_missing"; // the model keeps its inherited device
                    continue;
                }
                canon = resolved;
            }
            const auto cur = ev.values.find(key);
            if (cur != ev.values.end() && admin_values_equal(key, cur->second, canon)) {
                continue; // same meaning: the inherited spelling stays, so nothing restarts
            }
            if (cur == ev.values.end() && spec->def != nullptr && canon == spec->def) {
                continue; // the built-in default anyway
            }
            ev.values[key] = canon;
            parallel_set = parallel_set || (key == "parallel" && canon != "-1");
        }
    }
    // an explicit slot count keeps the default kv_unified = false, which would split the pool per slot;
    // the coupled key keeps "context size" meaning one shared pool (unless the preset decides it)
    if (parallel_set && !inherited.count(ADMIN_KV_UNIFIED_KEY)) {
        ev.values[ADMIN_KV_UNIFIED_KEY] = "true";
    }
    // "GPU for new models" (stage 1): cache / models_dir models that still have no device of their own
    if (source != SERVER_MODEL_SOURCE_PRESET && !env.fill_device.empty() && !ev.values.count("device") &&
        !admin_default_device_missing(env.fill_device, env.devices)) {
        ev.values["device"] = env.fill_device;
        ev.default_device   = env.fill_device;
    }
    return ev;
}

// the whitelisted values as the child gets them: the router command line is merged last
static admin_kv admin_with_cli(const admin_kv & values, const admin_kv & cli) {
    admin_kv out = values;
    for (const auto & [key, value] : cli) {
        out[key] = value;
    }
    return out;
}

static admin_kv admin_cli_locks_of(const common_preset & base_preset) {
    admin_kv cli;
    for (const auto & spec : ADMIN_KEYS) {
        std::string value;
        if (base_preset.get_option(spec.env, value)) {
            cli[spec.key] = value;
        }
    }
    std::string value;
    if (base_preset.get_option(ADMIN_KV_UNIFIED_ENV, value)) {
        cli[ADMIN_KV_UNIFIED_KEY] = value;
    }
    return cli;
}

uint64_t server_admin_overlay_presets(
        const common_preset_context & ctx_preset,
        common_presets & presets,
        const std::unordered_map<std::string, server_model_source> & source_map,
        const common_preset & base_preset) {
    admin_context & ctx = admin_ctx();
    if (!ctx.enabled.load()) {
        return 0;
    }
    // work on a copy: the lock is a leaf and set_option may throw
    std::map<std::string, admin_kv> applied;
    admin_eval_env env;
    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lk(ctx.mtx_state);
        applied          = ctx.applied;
        env.fill_device  = admin_overlay_device(ctx);
        generation       = ctx.overlay_gen.load();
    }
    env.devices = admin_build_devices(ctx);
    env.md      = &ctx.model_devices;
    env.cli     = admin_cli_locks_of(base_preset);

    std::map<std::string, admin_seen_model> seen;
    try {
        for (auto & [name, preset] : presets) {
            admin_seen_model rec;
            rec.generation = generation;
            const auto src = source_map.find(name);
            rec.source = src != source_map.end() ? src->second : SERVER_MODEL_SOURCE_PRESET;
            for (const auto & spec : ADMIN_KEYS) {
                std::string value;
                if (preset.get_option(spec.env, value)) {
                    rec.inherited[spec.key] = value;
                }
            }
            std::string kv_unified;
            if (preset.get_option(ADMIN_KV_UNIFIED_ENV, kv_unified)) {
                rec.inherited[ADMIN_KV_UNIFIED_KEY] = kv_unified;
            }
            preset.get_option("LLAMA_ARG_MODEL",   rec.model_path);
            preset.get_option("LLAMA_ARG_HF_REPO", rec.hf_repo);
            preset.get_option("LLAMA_ARG_HF_FILE", rec.hf_file);
            preset.get_option("LLAMA_ARG_MMPROJ",  rec.mmproj);

            const auto it = applied.find(name);
            const admin_eval ev = admin_evaluate(name, rec.source, rec.inherited, it != applied.end() ? &it->second : nullptr, env);
            for (const auto & [key, value] : ev.values) {
                const auto before = rec.inherited.find(key);
                if (before == rec.inherited.end() || before->second != value) {
                    preset.set_option(ctx_preset, admin_key_env(key), value);
                }
            }
            rec.values         = ev.values;
            rec.inactive       = ev.inactive;
            rec.default_device = ev.default_device;
            seen[name] = std::move(rec);
        }
    } catch (const std::exception & e) {
        // a throw in the first load would make the router exit (server.cpp): log it once per generation
        static std::atomic<uint64_t> logged{0};
        if (logged.exchange(generation) != generation) {
            SRV_ERR("admin: applying the per-model settings failed, some models use their models.ini values: %s\n", e.what());
        }
    }
    {
        std::lock_guard<std::mutex> lk(ctx.mtx_seen);
        if (generation >= ctx.seen_gen) {
            ctx.seen      = std::move(seen);
            ctx.cli_locks = env.cli;
            ctx.seen_gen  = generation;
        }
    }
    return generation;
}

bool server_admin_overlay_changed(uint64_t generation) {
    const admin_context & ctx = admin_ctx();
    return ctx.enabled.load() && ctx.overlay_gen.load() != generation;
}

//
// output tail of the model children (call-out in server_monitor::on_line, plain log lines only)
//

// the level letter of a child log line: "0.00.030.735 E srv ..." or "E srv ..." -> 'E'; 0 = no log prefix
static char admin_line_level(const std::string & line) {
    size_t i = 0;
    // optional timestamp: digits and dots
    size_t j = i;
    while (j < line.size() && (admin_is_digit(line[j]) || line[j] == '.')) {
        j++;
    }
    if (j > i && j < line.size() && line[j] == ' ') {
        i = j + 1;
    }
    if (i + 1 < line.size() && line[i + 1] == ' ' && std::strchr("IWEDT", line[i]) != nullptr && line[i] != '\0') {
        return line[i];
    }
    return 0;
}

static bool admin_line_is_error(const std::string & line) {
    const char level = admin_line_level(line);
    if (level == 'E') {
        return true;
    }
    if (level != 0) {
        return false;
    }
    // argument errors, aborts and crashes are printed without the log prefix
    const std::string l = admin_to_lower(line);
    static const char * const words[] = { "error", "failed", "abort", "ggml_assert", "terminate called", "segmentation fault",
                                          "out of memory", "exception", "invalid device" };
    for (const char * w : words) {
        if (l.find(w) != std::string::npos) {
            return true;
        }
    }
    return false;
}

struct admin_reason {
    std::string code; // out_of_memory | invalid_device | invalid_settings | model_load_failed | unknown | none
    std::string line; // the line the code comes from
    std::vector<std::string> hints;
};

static bool admin_line_matches(const std::string & lower, std::initializer_list<const char *> needles) {
    for (const char * n : needles) {
        if (lower.find(n) != std::string::npos) {
            return true;
        }
    }
    return false;
}

static std::string admin_line_cause(const std::string & line) {
    const std::string l = admin_to_lower(line);
    if (admin_line_matches(l, { "out of memory", "malloc failed", "failed to allocate", "unable to allocate", "outofdevicememory",
                                "outofhostmemory", "out_of_memory", "insufficient memory" })) {
        return "out_of_memory";
    }
    if (admin_line_matches(l, { "invalid device" })) {
        return "invalid_device";
    }
    // "K cache type q8_0 with block size 32 does not divide n_embd_head_k=80": a quantized KV cache needs a head size
    // that is a multiple of its block size; "model does not support different K (q8_0) and V (f16) cache types" (MLA)
    if (admin_line_matches(l, { "quantized v cache", "requires flash attention", "requires flash_attn", "error while handling argument",
                                "invalid value", "unknown value for", "invalid argument", "not supported by this model",
                                "does not divide n_embd_head", "does not support different k" })) {
        return "invalid_settings";
    }
    if (admin_line_matches(l, { "error loading model", "failed to load model", "unable to load model", "failed to open", "gguf_init",
                                "invalid magic", "failed to read", "no such file", "is not a gguf" })) {
        return "model_load_failed";
    }
    return "";
}

// The reason of a failed start, from the last error-level lines of its output: the last line that names
// a specific cause (memory, device, setting) wins; the generic "failed to load model" lines that always
// follow such a cause count only when no specific cause is found. The "failed to fit params" warning of
// the child's own fit is only a hint (memory_tight): it is printed for loads that then work, too.
template <typename Lines>
static admin_reason admin_classify(const Lines & lines) {
    admin_reason r;
    std::string generic_line;
    std::string last_error;
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        const std::string & line = *it;
        if (r.hints.empty() && admin_icontains(line, "failed to fit params to free device memory")) {
            r.hints.push_back("memory_tight");
        }
        if (!admin_line_is_error(line)) {
            continue;
        }
        if (last_error.empty()) {
            last_error = line;
        }
        const std::string cause = admin_line_cause(line);
        if (cause.empty()) {
            continue;
        }
        if (cause == "model_load_failed") {
            if (generic_line.empty()) {
                generic_line = line;
            }
            continue;
        }
        if (r.code.empty()) {
            r.code = cause;
            r.line = line;
        }
    }
    if (r.code.empty() && !generic_line.empty()) {
        r.code = "model_load_failed";
        r.line = generic_line;
    }
    if (r.code.empty() && !last_error.empty()) {
        r.code = "unknown";
        r.line = last_error;
    }
    if (r.code.empty()) {
        r.code = "none";
    }
    return r;
}

static json admin_reason_json(const admin_reason & r, int exit_code) {
    std::string message;
    std::string hint;
    if (r.code == "out_of_memory") {
        message = "Out of GPU memory: the model, its context (KV cache) or its compute buffers do not fit";
        hint    = "lower the context size, use KV cache precision q8_0, offload fewer GPU layers or choose another GPU";
    } else if (r.code == "invalid_device") {
        message = "The configured device does not exist on this server";
        hint    = "choose one of the devices listed on the GPU cards, or Inherit";
    } else if (r.code == "invalid_settings") {
        message = "A setting is not valid for this model or this server";
        hint    = "see the line below; for example a quantized V cache needs flash attention";
    } else if (r.code == "model_load_failed") {
        message = "The model file could not be loaded";
        hint    = "check that the file exists and is a complete GGUF file";
    } else if (r.code == "unknown") {
        message = string_format("The model process stopped with an error (exit status %d)", exit_code);
        hint    = "see the output below";
    } else {
        message = string_format("The model process exited with status %d without reporting an error (stopped or killed)", exit_code);
    }
    return {
        {"code",    r.code},
        {"message", message},
        {"hint",    hint},
        {"line",    r.line},
        {"hints",   r.hints},
    };
}

// "common_memory_breakdown_print: |   - CUDA1 (GTX 1080 Ti) | 11165 = 10667 + (8835 =  4829 +    3584 +     422) +       -8337 |"
// "common_memory_breakdown_print: |   - Host               |                    2 =     1 +       0 +       0                |"
// (printed by the child at verbosity >= 4: by its own fit dry run at load, and at exit)
static void admin_parse_breakdown(const std::string & line, admin_log_instance & inst) {
    const size_t pos = line.find("memory_breakdown_print:");
    if (pos == std::string::npos) {
        return;
    }
    const size_t bar1 = line.find('|', pos);
    if (bar1 == std::string::npos) {
        return;
    }
    const size_t dash = line.find("- ", bar1);
    const size_t bar2 = line.find('|', bar1 + 1);
    if (dash == std::string::npos || bar2 == std::string::npos || dash > bar2) {
        return; // the header row
    }
    std::string label = admin_strip(line.substr(dash + 2, bar2 - dash - 2));
    const size_t paren = label.find(" (");
    const std::string name = admin_strip(paren == std::string::npos ? label : label.substr(0, paren));
    std::vector<long long> numbers;
    const std::string rest = line.substr(bar2 + 1);
    for (size_t i = 0; i < rest.size();) {
        if (admin_is_digit(rest[i]) || (rest[i] == '-' && i + 1 < rest.size() && admin_is_digit(rest[i + 1]))) {
            size_t j = i + 1;
            while (j < rest.size() && admin_is_digit(rest[j])) {
                j++;
            }
            long long n = 0;
            if (admin_parse_int(rest.substr(i, j - i), n)) {
                numbers.push_back(n);
            }
            i = j;
        } else {
            i++;
        }
    }
    auto mib = [](long long v) { return v > 0 ? (uint64_t) v * 1024 * 1024 : (uint64_t) 0; };
    admin_breakdown b;
    if (numbers.size() == 7) {
        b.total   = mib(numbers[0]);
        b.free    = mib(numbers[1]);
        b.model   = mib(numbers[3]);
        b.context = mib(numbers[4]);
        b.compute = mib(numbers[5]);
    } else if (numbers.size() == 4 && admin_iequals(name, "Host")) {
        b.host    = true;
        b.model   = mib(numbers[1]);
        b.context = mib(numbers[2]);
        b.compute = mib(numbers[3]);
    } else {
        return;
    }
    inst.breakdown[b.host ? std::string("Host") : name] = b;
    inst.wall_breakdown = admin_wall_ms();
}

void server_admin_child_line(const std::string & name, const void * instance, int port, const std::string & line) {
    admin_context & ctx = admin_ctx();
    if (!ctx.enabled.load()) {
        return;
    }
    const std::string clean = admin_cut(admin_strip_ansi(line), ADMIN_LOG_MAX_LINE);
    std::lock_guard<std::mutex> lk(ctx.mtx_logs);
    auto it = ctx.logs.find(name);
    if (it == ctx.logs.end()) {
        if (ctx.logs.size() >= ADMIN_LOG_MAX_NAMES) {
            auto oldest = ctx.logs.begin();
            for (auto o = ctx.logs.begin(); o != ctx.logs.end(); ++o) {
                if (o->second.t_used < oldest->second.t_used) {
                    oldest = o;
                }
            }
            ctx.logs.erase(oldest);
        }
        it = ctx.logs.emplace(name, admin_log_ring()).first;
    }
    admin_log_ring & ring = it->second;
    if (!ring.has_cur || ring.cur.key != instance || ring.cur.port != port) {
        // a new child for this name: its tail starts empty (the last failure is kept separately)
        ring.cur            = admin_log_instance();
        ring.cur.key        = instance;
        ring.cur.port       = port;
        ring.cur.seq        = ++ctx.log_seq;
        ring.cur.wall_start = admin_wall_ms();
        ring.has_cur        = true;
    }
    ring.t_used = admin_now_ms();
    if (admin_strip(clean).empty()) {
        return; // e.g. the newline the child writes before a state message
    }
    admin_log_instance & inst = ring.cur;
    inst.lines.push_back(clean);
    inst.bytes += clean.size() + 1;
    inst.n_lines++;
    while (!inst.lines.empty() && (inst.lines.size() > ADMIN_LOG_MAX_LINES || inst.bytes > ADMIN_LOG_MAX_BYTES)) {
        inst.bytes -= inst.lines.front().size() + 1;
        inst.lines.pop_front();
        inst.truncated = true;
    }
    admin_parse_breakdown(clean, inst);
}

// a copy of the current tail of a model (false: none)
static bool admin_log_copy(admin_context & ctx, const std::string & name, admin_log_instance & out) {
    std::lock_guard<std::mutex> lk(ctx.mtx_logs);
    const auto it = ctx.logs.find(name);
    if (it == ctx.logs.end() || !it->second.has_cur) {
        return false;
    }
    out = it->second.cur;
    return true;
}

static json admin_get_failure(admin_context & ctx, const std::string & name) {
    std::lock_guard<std::mutex> lk(ctx.mtx_logs);
    const auto it = ctx.failures.find(name);
    return it != ctx.failures.end() ? it->second : json(nullptr);
}

//
// live view of the router
//

struct admin_pinned_view {
    std::vector<std::string> protect; // what server_models never evicts: in effect, listed or not (yet)
    std::vector<std::string> applied; // what is pinned right now: in effect and in the model list
    std::vector<std::string> ignored; // configured, but dropped to keep one slot evictable
    std::vector<std::string> unknown; // configured, but not in the model list
};

struct admin_live {
    std::vector<server_model_meta> models; // without hidden entries and download placeholders
    int               models_max = 0;
    admin_pinned_view pinned;
};

static bool admin_has_model(const std::vector<server_model_meta> & models, const std::string & id) {
    for (const auto & m : models) {
        if (m.name == id) {
            return true;
        }
    }
    return false;
}

// Invariant: fewer pinned models than models_max, at least one slot must stay
// evictable (otherwise a request for another model would wait forever).
// POST /admin/settings rejects a violation; a list that comes from the
// environment or from an older state file is cut here instead: its first
// models_max - 1 ids are in effect. They are protected whether they are in the
// model list or not, so a model that enters the list later (rescan, download,
// preset change) is protected the moment it does, with no admin request.
// models_max <= 0: no cut (used to count, and "no limit" never evicts anyway)
static admin_pinned_view admin_compute_pinned(
        const std::vector<std::string> & configured,
        const std::vector<server_model_meta> & models,
        int models_max) {
    admin_pinned_view view;
    for (const auto & id : configured) {
        const bool in_effect = models_max <= 0 || (int) view.protect.size() < models_max - 1;
        if (in_effect) {
            view.protect.push_back(id);
        }
        if (!admin_has_model(models, id)) {
            view.unknown.push_back(id);
        } else if (in_effect) {
            view.applied.push_back(id);
        } else {
            view.ignored.push_back(id);
        }
    }
    return view;
}

static std::vector<server_model_meta> admin_list_models(server_models & models) {
    std::vector<server_model_meta> out;
    for (auto & meta : models.get_all_meta()) {
        if (meta.hidden) {
            continue; // not listed by GET /models either
        }
        if (meta.status == SERVER_MODEL_STATUS_DOWNLOADING || meta.status == SERVER_MODEL_STATUS_DOWNLOADED) {
            continue; // placeholder of a download, not a model
        }
        out.push_back(std::move(meta));
    }
    return out;
}

// Reads the live model list and keeps the pinned set inside server_models in
// sync with the settings (it depends on models_max, not on the model list).
// apply_held: the caller holds mtx_apply. Otherwise the sync is skipped while a
// settings change is in progress, so a GET never waits for it.
static admin_live admin_refresh(admin_context & ctx, bool apply_held) {
    server_models & models = ctx.routes->models;

    std::unique_lock<std::mutex> lk_apply(ctx.mtx_apply, std::defer_lock);
    const bool may_apply = apply_held || lk_apply.try_lock();

    std::vector<std::string> configured;
    {
        std::lock_guard<std::mutex> lk(ctx.mtx_state);
        configured = ctx.saved.pinned.has_value() ? *ctx.saved.pinned : ctx.env_pinned;
    }

    admin_live live;
    live.models     = admin_list_models(models);
    live.models_max = models.get_models_max();
    live.pinned     = admin_compute_pinned(configured, live.models, live.models_max);

    if (may_apply) {
        const std::set<std::string> protect(live.pinned.protect.begin(), live.pinned.protect.end());
        if (!ctx.pinned_pushed || protect != ctx.pinned_applied) {
            models.set_pinned_models(protect);
            ctx.pinned_applied = protect;
            ctx.pinned_pushed  = true;
            SRV_INF("admin: pinned models: %s\n", protect.empty() ? "(none)" : admin_join(live.pinned.protect).c_str());
            if (!live.pinned.ignored.empty()) {
                SRV_WRN("admin: not pinned, at least one of the %d slots must stay evictable: %s\n",
                        live.models_max, admin_join(live.pinned.ignored).c_str());
            }
            if (!live.pinned.unknown.empty()) {
                SRV_WRN("admin: pinned ids that are not in the model list (yet): %s\n", admin_join(live.pinned.unknown).c_str());
            }
        }
    }
    return live;
}

//
// HTTP helpers
//

static server_http_res_ptr admin_res_json(int status, const json & data) {
    auto res = std::make_unique<server_http_res>();
    res->status = status;
    res->data   = safe_json_to_str(data);
    res->headers["Cache-Control"] = "no-store";
    return res;
}

// the server's error format: {"error":{"code":..,"message":..,"type":..}} (+ "param" for a request field)
static server_http_res_ptr admin_res_error(int status, const std::string & type, const std::string & message,
                                           const std::string & param = "", const json & extra = json()) {
    json err = {
        {"code",    status},
        {"message", message},
        {"type",    type},
    };
    if (!param.empty()) {
        err["param"] = param;
    }
    if (extra.is_object()) {
        for (const auto & [k, v] : extra.items()) {
            err[k] = v;
        }
    }
    return admin_res_json(status, { {"error", err} });
}

enum admin_auth {
    ADMIN_AUTH_OK,
    ADMIN_AUTH_DENIED,
    ADMIN_AUTH_NO_KEY,
};

// header "X-Admin-Key", name matched case-insensitively
static admin_auth admin_check_key(const admin_context & ctx, const server_http_req & req) {
    if (ctx.key.empty()) {
        return ADMIN_AUTH_NO_KEY;
    }
    bool ok = false;
    for (const auto & [name, value] : req.headers) {
        if (admin_to_lower(name) == ADMIN_KEY_HEADER) {
            ok = admin_secure_equals(value, ctx.key) || ok;
        }
    }
    return ok ? ADMIN_AUTH_OK : ADMIN_AUTH_DENIED;
}

// The address allow-list, before anything else (the key check included): nullptr = allowed.
// A refused request is logged once, without its headers (the admin key stays out of the log).
static server_http_res_ptr admin_check_access(const admin_context & ctx, const server_http_req & req) {
    const admin_access & acc = ctx.access;
    if (!acc.enabled) {
        return nullptr;
    }
    const admin_client client = admin_client_of(acc, req);
    const std::string seen = client.known ? client.address : std::string("an unknown address");
    std::string via;
    if (client.via_forwarded) {
        via = " (X-Forwarded-For of the trusted proxy " + admin_cut(client.peer, 64) + ")";
    } else if (!client.note.empty()) {
        via = " (" + client.note + ")";
    }
    if (acc.locked) {
        SRV_WRN("admin: refused %s from %s%s: the address allow-list has an invalid entry, see the error logged at startup\n",
                req.path.c_str(), seen.c_str(), via.c_str());
        return admin_res_error(403, "admin_forbidden",
            "The admin API is locked: the address allow-list of the server has an invalid entry (see the server log); "
            "the request comes from " + seen);
    }
    if (!client.known || !admin_ip_in(acc.allow, client.ip)) {
        SRV_WRN("admin: refused %s from %s%s: not on PROVISION_ADMIN_ALLOW_FROM\n", req.path.c_str(), seen.c_str(), via.c_str());
        return admin_res_error(403, "admin_forbidden",
            "The admin API is not available from this address; the server sees the request coming from " + seen);
    }
    return nullptr;
}

using admin_handler_t = std::function<server_http_res_ptr(admin_context &, const server_http_req &)>;

// never throws: every failure becomes an error response
static server_http_context::handler_t admin_wrap(admin_handler_t handler, bool need_key) {
    return [handler = std::move(handler), need_key](const server_http_req & req) -> server_http_res_ptr {
        admin_context & ctx = admin_ctx();
        try {
            server_http_res_ptr refused = admin_check_access(ctx, req);
            if (refused) {
                return refused;
            }
            for (const auto & [name, value] : req.headers) {
                if (admin_to_lower(name) == "x-forwarded-for" && !value.empty()) {
                    ctx.proxy_seen.store(true); // for GET /admin/diagnostics
                }
            }
            if (need_key) {
                const admin_auth auth = admin_check_key(ctx, req);
                if (auth == ADMIN_AUTH_NO_KEY) {
                    return admin_res_error(503, "admin_key_unavailable",
                        "no admin key is available: set PROVISION_ADMIN_KEY or make the admin state directory writable");
                }
                if (auth != ADMIN_AUTH_OK) {
                    SRV_WRN("admin: rejected %s (missing or wrong X-Admin-Key)\n", req.path.c_str());
                    return admin_res_error(401, "admin_key_required", "a valid X-Admin-Key header is required");
                }
            }
            return handler(ctx, req);
        } catch (const admin_http_error & e) {
            return admin_res_error(e.status, e.type, e.what(), e.param, e.extra);
        } catch (const std::invalid_argument & e) {
            return admin_res_error(400, "invalid_request_error", e.what());
        } catch (const std::exception & e) {
            SRV_ERR("admin: %s failed: %s\n", req.path.c_str(), e.what());
            return admin_res_error(500, "server_error", e.what());
        } catch (...) {
            SRV_ERR("admin: %s failed: unknown error\n", req.path.c_str());
            return admin_res_error(500, "server_error", "unknown error");
        }
    };
}

static json admin_parse_body_object(const server_http_req & req) {
    json body;
    try {
        body = json::parse(req.body);
    } catch (const std::exception &) {
        throw admin_bad_request("the request body must be a JSON object");
    }
    if (!body.is_object()) {
        throw admin_bad_request("the request body must be a JSON object");
    }
    return body;
}

static std::string admin_cache_dir() {
    try {
        return hf_cache::get_cache_path();
    } catch (const std::exception &) {
        return "";
    }
}

//
// GET /admin/status
//

static server_http_res_ptr admin_handle_status(admin_context & ctx, const server_http_req & req) {
    const bool authorized = admin_check_key(ctx, req) == ADMIN_AUTH_OK;
    json out = {
        {"admin",          true},
        {"authorized",     authorized},
        {"key_configured", !ctx.key.empty()},
    };
    if (!authorized) {
        return admin_res_json(200, out); // the probe: nothing else without the key
    }

    const common_params & params = ctx.routes->params;
    const admin_live live = admin_refresh(ctx, false);

    int n_running = 0;
    for (const auto & m : live.models) {
        if (m.is_running()) {
            n_running++;
        }
    }
    std::string state_error;
    {
        std::lock_guard<std::mutex> lk(ctx.mtx_state);
        state_error = ctx.state_error;
    }
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    const bool has_ssl = true;
#else
    const bool has_ssl = false;
#endif
#ifdef PROVISION_LICENSE
    const bool license_build = true;
#else
    const bool license_build = false;
#endif

    out["version"]    = ADMIN_STATE_VERSION;
    out["key_source"] = ctx.key_source;
    out["models"]     = {
        {"max",     live.models_max},
        {"running", n_running},
        {"total",   live.models.size()},
    };
    out["state"] = {
        {"dir",      admin_path_to_utf8(ctx.state_dir)},
        {"writable", admin_dir_writable(ctx.state_dir)},
        {"error",    state_error},
    };
    out["download"] = {
        {"available", has_ssl && !params.offline},
        {"offline",   params.offline},
    };
    out["dirs"] = {
        {"cache",      admin_cache_dir()},
        {"models_dir", params.models_dir},
        {"preset",     params.models_preset},
    };
    out["license_build"] = license_build;
    out["ip_allowlist"]  = ctx.access.enabled; // PROVISION_ADMIN_ALLOW_FROM applies

    // stage 2 (additive): what this server supports, so that a newer UI bundle can tell
    out["features"] = json::array({ "model_settings", "gpus", "model_log", "diagnostics", "fit_check", "upload", "delete" });
    out["upload"]   = admin_upload_status_json(); // stage 3: {available, reason, message}
    std::string default_device;
    {
        std::lock_guard<std::mutex> lk(ctx.mtx_state);
        const admin_models_file & f = ctx.models_file;
        out["state"]["model_settings"] = {
            {"file",          admin_path_to_utf8(admin_models_path(ctx))},
            {"error",         f.error},
            {"read_only",     f.read_only},
            {"backup_loaded", f.backup_loaded},
            {"entries",       f.entries.size()},
        };
        default_device = admin_overlay_device(ctx);
    }
    json warnings = json::array();
    if (admin_default_device_missing(default_device, admin_build_devices(ctx))) {
        warnings.push_back("default_device_not_present");
    }
    out["warnings"] = warnings;
    return admin_res_json(200, out);
}

//
// GET / POST /admin/settings
//

static json admin_settings_json(admin_context & ctx, const admin_live & live) {
    std::string device;
    std::string overlay_device;
    {
        std::lock_guard<std::mutex> lk(ctx.mtx_state);
        device         = admin_reported_device(ctx);
        overlay_device = admin_overlay_device(ctx);
    }
    const std::vector<std::string> build_devices = admin_build_devices(ctx);
    json warnings = json::array();
    if (admin_default_device_missing(overlay_device, build_devices)) {
        warnings.push_back("default_device_not_present"); // the hook does not apply it
    }

    // a hint list: the router never touches the GPU, it only knows the devices the presets mention
    std::set<std::string> known_devices;
    for (const auto & name : admin_split_devices(device)) {
        known_devices.insert(name);
    }
    json j_models = json::array();
    for (const auto & m : live.models) {
        std::string value;
        if (m.preset.get_option("LLAMA_ARG_DEVICE", value)) {
            for (const auto & name : admin_split_devices(value)) {
                known_devices.insert(name);
            }
        }
        const bool pinned = std::find(live.pinned.applied.begin(), live.pinned.applied.end(), m.name) != live.pinned.applied.end();
        j_models.push_back({
            {"id",     m.name},
            {"source", server_model_source_to_string(m.source)},
            {"status", server_model_status_to_string(m.status)},
            {"pinned", pinned},
        });
    }

    return json {
        {"models_max",            live.models_max},
        {"default_device",        device},
        {"default_device_locked", ctx.device_locked},
        {"pinned",                live.pinned.applied},
        {"limits", {
            {"models_max_min", ADMIN_MODELS_MAX_MIN},
            {"models_max_max", ADMIN_MODELS_MAX_MAX},
        }},
        {"known_devices",         known_devices},
        {"models",                j_models},
        // extra: configured pins that are not in effect (see admin_compute_pinned)
        {"pinned_ignored",        live.pinned.ignored},
        {"pinned_unknown",        live.pinned.unknown},
        // stage 2: the GPU devices of this server (device inventory), and what does not fit them
        {"build_devices",         build_devices},
        {"warnings",              warnings},
    };
}

static server_http_res_ptr admin_handle_get_settings(admin_context & ctx, const server_http_req &) {
    return admin_res_json(200, admin_settings_json(ctx, admin_refresh(ctx, false)));
}

static server_http_res_ptr admin_handle_post_settings(admin_context & ctx, const server_http_req & req) {
    server_models & models = ctx.routes->models;
    const json body = admin_parse_body_object(req);

    // one settings change at a time: validate -> persist -> apply
    std::lock_guard<std::mutex> lk_apply(ctx.mtx_apply);

    const std::vector<server_model_meta> known_models = admin_list_models(models);
    const int models_max_before = models.get_models_max();

    admin_settings saved;
    std::string    device_before;
    {
        std::lock_guard<std::mutex> lk(ctx.mtx_state);
        saved         = ctx.saved;
        device_before = admin_overlay_device(ctx);
    }

    // 1. validate; nothing is saved or applied when anything is wrong
    admin_settings requested;
    std::vector<std::string> problems; // not used in strict mode
    admin_parse_settings(body, requested, true, problems);

    admin_settings next = saved;
    if (requested.models_max.has_value()) {
        // the server refuses to start when more models are marked load-on-startup than models_max allows
        int n_startup = 0;
        for (const auto & m : known_models) {
            std::string value;
            if (m.preset.get_option(COMMON_ARG_PRESET_LOAD_ON_STARTUP, value) && common_arg_utils::is_truthy(value)) {
                n_startup++;
            }
        }
        if (*requested.models_max < n_startup) {
            throw admin_bad_request(string_format(
                "models_max must be at least %d: that many models are marked load-on-startup", n_startup));
        }
        next.models_max = requested.models_max;
    }
    if (requested.default_device.has_value()) {
        if (!ctx.device_locked) {
            next.default_device = requested.default_device;
        } else if (*requested.default_device != ctx.locked_device) {
            throw admin_http_error(409, "default_device_locked",
                "default_device is set by MODEL_DEVICES ('*=" + ctx.locked_device + "') and cannot be changed here");
        }
        // locked and unchanged: nothing to do
    }
    if (requested.pinned.has_value()) {
        for (const auto & id : *requested.pinned) {
            if (!admin_has_model(known_models, id)) {
                throw admin_bad_request("pinned: unknown model id '" + id + "'");
            }
        }
        next.pinned = requested.pinned;
    }
    if (requested.models_max.has_value() || requested.pinned.has_value()) {
        const int models_max_next = next.models_max.has_value() ? *next.models_max : models_max_before;
        // the requested list as a whole (every id is in the model list, see above); without one, the pinned
        // models in effect now: a startup list that was cut to models_max - 1 counts as cut
        const size_t n_pinned = requested.pinned.has_value()
            ? admin_compute_pinned(*requested.pinned, known_models, 0).applied.size()
            : admin_compute_pinned(saved.pinned.has_value() ? *saved.pinned : ctx.env_pinned, known_models, models_max_before).applied.size();
        if (models_max_next > 0 && n_pinned >= (size_t) models_max_next) {
            throw admin_bad_request(string_format(
                "at least one slot must stay evictable: %zu pinned model(s) need models_max >= %zu (models_max is %d)",
                n_pinned, n_pinned + 1, models_max_next));
        }
    }

    // 2. persist
    std::string error;
    if (!admin_save_state(ctx, next, error)) {
        SRV_ERR("admin: settings not saved: %s\n", error.c_str());
        throw admin_http_error(500, "server_error", "the settings could not be saved: " + error);
    }

    // 3. apply
    std::string device_after;
    {
        std::lock_guard<std::mutex> lk(ctx.mtx_state);
        ctx.saved = next;
        ctx.state_error.clear(); // the file was just rewritten
        device_after = admin_overlay_device(ctx);
        if (device_after != device_before) {
            ctx.overlay_gen++; // a reload that read the old device is redone, see server_admin_overlay_changed()
        }
    }
    if (next.models_max.has_value() && *next.models_max != models_max_before) {
        SRV_INF("admin: models_max %d -> %d\n", models_max_before, *next.models_max);
        models.set_models_max(*next.models_max);
    }
    admin_live live = admin_refresh(ctx, true); // pushes the pinned set

    std::string reload_error;
    if (device_after != device_before) {
        // the presets are rebuilt through server_admin_overlay_presets()
        // note: an upstream reload unloads a running model whose preset changed
        SRV_INF("admin: default device for new models '%s' -> '%s', reloading the model list\n", device_before.c_str(), device_after.c_str());
        try {
            models.load_models();
        } catch (const std::exception & e) {
            reload_error = e.what();
            SRV_ERR("admin: the settings are saved, but reloading the model list failed: %s\n", e.what());
        }
        live = admin_refresh(ctx, true);
    }

    json out = admin_settings_json(ctx, live);
    if (!reload_error.empty()) {
        out["reload_error"] = reload_error;
    }
    return admin_res_json(200, out);
}

//
// GET /admin/storage
//

static json admin_dir_json(const std::string & kind, const fs::path & dir) {
    std::error_code ec;
    const bool exists = fs::is_directory(dir, ec);
    uint64_t total_bytes = 0;
    uint64_t free_bytes  = 0;
    admin_dir_space(dir, total_bytes, free_bytes);
    return json {
        {"kind",        kind},
        {"path",        admin_path_to_utf8(dir)},
        {"exists",      exists},
        // a directory that does not exist yet counts as writable when it can be created
        {"writable",    admin_dir_writable(exists ? dir : admin_nearest_existing(dir))},
        {"total_bytes", total_bytes},
        {"free_bytes",  free_bytes},
    };
}

static server_http_res_ptr admin_handle_storage(admin_context & ctx, const server_http_req &) {
    const common_params & params = ctx.routes->params;
    const std::string cache_dir = admin_cache_dir();

    json dirs = json::array();
    if (!cache_dir.empty()) {
        dirs.push_back(admin_dir_json("cache", admin_path_from_utf8(cache_dir)));
    }
    if (!params.models_dir.empty()) {
        dirs.push_back(admin_dir_json("models_dir", admin_path_from_utf8(params.models_dir)));
    }
    dirs.push_back(admin_dir_json("state", ctx.state_dir));

    // unfinished downloads left in the cache (a download that is running has one, too)
    json partial = json::array();
    if (!cache_dir.empty()) {
        static const size_t max_entries = 500;
        std::error_code ec;
        fs::recursive_directory_iterator it(admin_path_from_utf8(cache_dir), fs::directory_options::skip_permission_denied, ec);
        const fs::recursive_directory_iterator end;
        while (!ec && it != end && partial.size() < max_entries) {
            if (it.depth() > 8) {
                it.disable_recursion_pending();
            }
            const std::string name = admin_path_to_utf8(it->path().filename());
            std::error_code ec_file;
            if (string_ends_with(name, ADMIN_PARTIAL_SUFFIX) && it->is_regular_file(ec_file) && !ec_file) {
                const uintmax_t size = it->file_size(ec_file);
                partial.push_back({
                    {"path",  admin_path_to_utf8(it->path())},
                    {"bytes", ec_file ? (uint64_t) 0 : (uint64_t) size},
                });
            }
            it.increment(ec);
        }
    }

    json out = {
        {"dirs",              dirs},
        {"partial_downloads", partial},
    };
    // stage 3 (additive): unfinished uploads in <models_dir>/.provision-upload, with their bytes
    const json uploads = admin_upload_storage_json();
    for (const auto & [key, value] : uploads.items()) {
        out[key] = value;
    }
    return admin_res_json(200, out);
}

//
// POST /admin/models/download/check
//
// Advisory only: it never blocks a download. The file selection mirrors what
// the download child would fetch (common_download_get_hf_plan() in
// common/download.cpp, whose helpers are local to that file).
//

struct admin_hf_file {
    std::string path;
    long long   size = -1; // -1 = unknown
};

struct admin_gguf_split {
    std::string prefix; // tag included
    std::string tag;
    int index = 0;
    int count = 0;
};

static admin_gguf_split admin_gguf_split_info(const std::string & path) {
    static const std::regex re_split("^(.+)-([0-9]{5})-of-([0-9]{5})$", std::regex::icase);
    static const std::regex re_tag("[-.]([A-Z0-9_]+)$", std::regex::icase);
    std::smatch m;

    std::string prefix = path;
    if (!string_remove_suffix(prefix, ".gguf")) {
        return {};
    }

    int index = 1;
    int count = 1;
    if (std::regex_match(prefix, m, re_split)) {
        index = std::stoi(m[2].str());
        count = std::stoi(m[3].str());
        const std::string base = m[1].str();
        prefix = base;
    }

    std::string tag;
    if (std::regex_search(prefix, m, re_tag)) {
        tag = admin_to_upper(m[1].str());
    }
    return {prefix, tag, index, count};
}

// Q4_0 -> 4, F16 -> 16, Q8_K_M -> 8
static int admin_quant_bits(const std::string & path) {
    const std::string tag = admin_gguf_split_info(path).tag;
    const size_t pos = tag.find_first_of("0123456789");
    if (pos == std::string::npos) {
        return 0;
    }
    try {
        return std::stoi(tag.substr(pos));
    } catch (const std::exception &) {
        return 0;
    }
}

static bool admin_gguf_is_model(const std::string & path) {
    if (!string_ends_with(path, ".gguf")) {
        return false;
    }
    std::string filename = path;
    if (const size_t pos = filename.rfind('/'); pos != std::string::npos) {
        filename = filename.substr(pos + 1);
    }
    return filename.find("mmproj")  == std::string::npos &&
           filename.find("imatrix") == std::string::npos &&
           filename.find("mtp-")    == std::string::npos &&
           filename.find("eagle3-") == std::string::npos &&
           filename.find("dflash-") == std::string::npos &&
           filename.find("dspark-") == std::string::npos;
}

// the quant given by the user, else Q4_K_M, else Q8_0, else the first GGUF
static const admin_hf_file * admin_find_model(const std::vector<admin_hf_file> & files, const std::string & tag) {
    std::vector<std::string> tags;
    if (!tag.empty()) {
        tags.push_back(tag);
    } else {
        tags = {"Q4_K_M", "Q8_0"};
    }
    for (const auto & t : tags) {
        const std::regex pattern(t + "[.-]", std::regex::icase);
        for (const auto & f : files) {
            if (admin_gguf_is_model(f.path) && std::regex_search(f.path, pattern)) {
                const admin_gguf_split split = admin_gguf_split_info(f.path);
                if (split.count > 1 && split.index != 1) {
                    continue;
                }
                return &f;
            }
        }
    }
    if (tag.empty()) {
        for (const auto & f : files) {
            if (admin_gguf_is_model(f.path)) {
                const admin_gguf_split split = admin_gguf_split_info(f.path);
                if (split.count > 1 && split.index != 1) {
                    continue;
                }
                return &f;
            }
        }
    }
    return nullptr;
}

// the mmproj the server would fetch along with the model: deepest shared directory, then closest quantization
static const admin_hf_file * admin_find_mmproj(const std::vector<admin_hf_file> & files, const std::string & model) {
    const admin_hf_file * best = nullptr;
    size_t best_depth = 0;
    int    best_diff  = 0;

    const int model_bits = admin_quant_bits(model);
    const std::vector<std::string> model_parts = string_split<std::string>(model, '/');
    if (model_parts.empty()) {
        return nullptr;
    }
    const auto model_dir = model_parts.end() - 1;

    for (const auto & f : files) {
        if (!string_ends_with(f.path, ".gguf") || f.path.find("mmproj") == std::string::npos) {
            continue;
        }
        const std::vector<std::string> sib_parts = string_split<std::string>(f.path, '/');
        if (sib_parts.empty()) {
            continue;
        }
        const auto sib_dir = sib_parts.end() - 1;
        const auto mm = std::mismatch(model_parts.begin(), model_dir, sib_parts.begin(), sib_dir);
        if (mm.second != sib_dir) {
            continue; // not in the directory of the model or above it
        }
        const size_t depth = (size_t) (mm.second - sib_parts.begin());
        const int    diff  = std::abs(admin_quant_bits(f.path) - model_bits);
        if (best == nullptr || depth > best_depth || (depth == best_depth && diff < best_diff)) {
            best       = &f;
            best_depth = depth;
            best_diff  = diff;
        }
    }
    return best;
}

// ^[A-Za-z0-9][A-Za-z0-9._-]*/[A-Za-z0-9][A-Za-z0-9._-]*(:[A-Za-z0-9._-]+)?$
static bool admin_parse_hf_model(const std::string & model, std::string & repo, std::string & quant) {
    auto is_word = [](char c) {
        return admin_is_alpha(c) || admin_is_digit(c);
    };
    auto is_tail = [&](const std::string & s) {
        for (char c : s) {
            if (!is_word(c) && c != '.' && c != '_' && c != '-') {
                return false;
            }
        }
        return true;
    };
    auto is_name = [&](const std::string & s) {
        return !s.empty() && is_word(s.front()) && is_tail(s);
    };

    if (model.empty() || model.size() > 256) {
        return false;
    }
    const size_t colon = model.find(':');
    repo  = model.substr(0, colon);
    quant = colon == std::string::npos ? "" : model.substr(colon + 1);
    if (colon != std::string::npos && (quant.empty() || !is_tail(quant))) {
        return false;
    }
    const size_t slash = repo.find('/');
    if (slash == std::string::npos || repo.find('/', slash + 1) != std::string::npos) {
        return false;
    }
    return is_name(repo.substr(0, slash)) && is_name(repo.substr(slash + 1));
}

// same rule as the downloader (common/hf-cache.cpp): anything else is not sent
static bool admin_is_valid_hf_token(const std::string & token) {
    if (token.size() < 37 || token.size() > 256 || !string_starts_with(token, "hf_")) {
        return false;
    }
    for (size_t i = 3; i < token.size(); i++) {
        if (!admin_is_alpha(token[i]) && !admin_is_digit(token[i])) {
            return false;
        }
    }
    return true;
}

static bool admin_is_hex_string(const std::string & s, size_t len) {
    if (s.size() != len) {
        return false;
    }
    for (char c : s) {
        if (!admin_is_digit(c) && !(c >= 'a' && c <= 'f') && !(c >= 'A' && c <= 'F')) {
            return false;
        }
    }
    return true;
}

struct admin_hf_response {
    bool        reached = false; // an HTTP response was received
    int         status  = 0;
    std::string body;
};

// a path inside a repository as part of a URL: every byte but unreserved characters and '/' is percent-encoded,
// so nothing in a file name ('@', '?', '#', '%', ':') can change where the request goes
static std::string admin_url_encode_path(const std::string & path) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (char c : path) {
        if (admin_is_alpha(c) || admin_is_digit(c) || c == '-' || c == '.' || c == '_' || c == '~' || c == '/') {
            out += c;
        } else {
            out += '%';
            out += hex[(unsigned char) c >> 4];
            out += hex[(unsigned char) c & 0x0F];
        }
    }
    return out;
}

// scheme://[user info@]host[:port] of a parsed URL
static std::string admin_url_origin(const common_http_url & parts) {
    const bool default_port = (parts.scheme == "https" && parts.port == 443) || (parts.scheme == "http" && parts.port == 80);
    std::string user_info;
    if (!parts.user.empty()) {
        user_info = parts.user + (parts.password.empty() ? "" : ":" + parts.password) + "@";
    }
    return parts.scheme + "://" + user_info + common_http_format_host(parts.host) + (default_port ? "" : ":" + std::to_string(parts.port));
}

// true when a request for url goes to the configured endpoint: same scheme, host, port and user info.
// common_http_parse_url() ends the user info at the first '@' anywhere after "://", so an '@' in a path or in a
// redirect target would otherwise send the request - and the token - to whatever host follows it
static bool admin_is_endpoint_url(const std::string & url, const common_http_url & endpoint) {
    try {
        const common_http_url parts = common_http_parse_url(url);
        return parts.scheme == endpoint.scheme && admin_to_lower(parts.host) == admin_to_lower(endpoint.host) &&
               parts.port == endpoint.port && parts.user == endpoint.user && parts.password == endpoint.password;
    } catch (const std::exception &) {
        return false;
    }
}

// One request to the configured Hugging Face endpoint, bounded by `deadline`.
// Every URL, the first one and each redirect target, is checked against the
// endpoint: neither the request nor the token ever goes to another host.
static admin_hf_response admin_hf_request(
        const std::string & method,
        const std::string & url,
        const std::string & token,
        const std::chrono::steady_clock::time_point deadline) {
    admin_hf_response out;
    common_http_url endpoint = {};
    try {
        endpoint = common_http_parse_url(common_get_model_endpoint());
    } catch (const std::exception & e) {
        SRV_DBG("admin: the model endpoint is not a valid URL: %s\n", e.what());
        return out;
    }
    std::string current = url;
    for (int hop = 0; hop < 4; hop++) {
        if (!admin_is_endpoint_url(current, endpoint)) {
            SRV_WRN("%s", "admin: a request of the download check would not go to the configured model endpoint, not sent\n");
            return out;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (remaining.count() < 100) {
            return out; // out of time
        }
        try {
            auto [cli, parts] = common_http_client(current);
            cli.set_follow_location(false);
            cli.set_connection_timeout(std::min(remaining, std::chrono::milliseconds(5000)));
            cli.set_read_timeout(remaining);
            cli.set_write_timeout(remaining);
            cli.set_max_timeout(remaining);

            httplib::Headers headers = {
                {"User-Agent", "llama-cpp/" + std::string(llama_build_info())},
                {"Accept",     "application/json"},
            };
            if (admin_is_valid_hf_token(token)) {
                headers.emplace("Authorization", "Bearer " + token);
            }

            std::string body;
            httplib::Result res = method == "HEAD"
                ? cli.Head(parts.path, headers)
                : cli.Get(parts.path, headers, [&body](const char * data, size_t len) {
                      body.append(data, len);
                      return body.size() <= ADMIN_HF_MAX_BODY;
                  });
            if (!res) {
                return out;
            }
            const int status = res->status;
            if (method != "HEAD" && (status == 301 || status == 302 || status == 303 || status == 307 || status == 308)) {
                const std::string location = res->get_header_value("Location");
                // relative: e.g. a renamed repository
                const bool relative = string_starts_with(location, "/") && !string_starts_with(location, "//");
                const std::string next = relative ? admin_url_origin(parts) + location : location;
                if (admin_is_endpoint_url(next, endpoint)) {
                    current = next;
                    continue;
                }
                // anything else is not followed: the redirect itself is the answer
            }
            out.reached = true;
            out.status  = status;
            out.body    = std::move(body);
            return out;
        } catch (const std::exception & e) {
            SRV_DBG("admin: request to the model endpoint failed: %s\n", e.what());
            return out;
        }
    }
    return out;
}

static server_http_res_ptr admin_handle_download_check(admin_context & ctx, const server_http_req & req) {
    static const char * const model_error = "model must look like user/repo or user/repo:QUANT";

    const common_params & params = ctx.routes->params;
    server_models & models = ctx.routes->models;

    const json body = admin_parse_body_object(req);
    if (!body.contains("model") || !body.at("model").is_string()) {
        throw admin_bad_request(model_error);
    }
    const std::string model = body.at("model").get<std::string>();
    std::string repo;
    std::string quant;
    if (!admin_parse_hf_model(model, repo, quant)) {
        throw admin_bad_request(model_error);
    }

    std::vector<std::string> warnings;
    auto warn = [&warnings](const std::string & code) {
        if (std::find(warnings.begin(), warnings.end(), code) == warnings.end()) {
            warnings.push_back(code);
        }
    };

    bool hf_reachable = false;
    bool exists       = false;
    bool listed       = false; // the file list of the repository is known
    std::vector<admin_hf_file> repo_files;
    // a repository with a preset.ini in its root: a download fetches that file and nothing else, whatever
    // the quant (common_download_get_hf_plan() in common/download.cpp); no model appears in the list from it
    bool          is_preset_repo = false;
    admin_hf_file preset_file;

    if (quant.empty()) {
        warn("quant_not_specified");
    }

    if (params.offline) {
        warn("offline_mode"); // the router must not use the network
    } else {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ADMIN_HF_BUDGET_MS);
        const std::string endpoint = common_get_model_endpoint();

        // 1. branch -> commit, like hf_cache::get_repo_files() ("main", else the first branch)
        std::string commit;
        const admin_hf_response refs = admin_hf_request("GET", endpoint + "api/models/" + repo + "/refs", params.hf_token, deadline);
        if (!refs.reached || refs.status >= 500 || refs.status == 429) {
            warn("hf_unreachable");
        } else if (refs.status != 200) {
            hf_reachable = true;
            warn("repo_not_found"); // also: gated / private without access
        } else {
            hf_reachable = true;
            exists       = true;
            const json j_refs = json::parse_no_throw(refs.body);
            if (!j_refs.is_discarded() && j_refs.is_object() && j_refs.contains("branches") && j_refs.at("branches").is_array()) {
                for (const auto & branch : j_refs.at("branches")) {
                    if (!branch.is_object() ||
                        !branch.contains("name") || !branch.at("name").is_string() ||
                        !branch.contains("targetCommit") || !branch.at("targetCommit").is_string()) {
                        continue;
                    }
                    const std::string name   = branch.at("name").get<std::string>();
                    const std::string target = branch.at("targetCommit").get<std::string>();
                    if (!admin_is_hex_string(target, 40)) {
                        continue;
                    }
                    if (name == "main") {
                        commit = target;
                        break;
                    }
                    if (commit.empty()) {
                        commit = target;
                    }
                }
            }
        }

        // 2. file list with sizes
        if (!commit.empty()) {
            const admin_hf_response tree = admin_hf_request(
                "GET", endpoint + "api/models/" + repo + "/tree/" + commit + "?recursive=true", params.hf_token, deadline);
            if (!tree.reached || tree.status >= 500 || tree.status == 429) {
                warn("hf_unreachable");
                hf_reachable = false;
            } else if (tree.status != 200) {
                warn("repo_not_found");
                exists = false;
            } else {
                const json j_tree = json::parse_no_throw(tree.body);
                if (!j_tree.is_discarded() && j_tree.is_array()) {
                    listed = true;
                    for (const auto & item : j_tree) {
                        if (!item.is_object() ||
                            !item.contains("type") || !item.at("type").is_string() || item.at("type").get<std::string>() != "file" ||
                            !item.contains("path") || !item.at("path").is_string()) {
                            continue;
                        }
                        admin_hf_file file;
                        file.path = item.at("path").get<std::string>();
                        if (file.path.empty() || file.path.size() > 1024) {
                            continue;
                        }
                        if (item.contains("lfs") && item.at("lfs").is_object() &&
                            item.at("lfs").contains("size") && item.at("lfs").at("size").is_number_integer()) {
                            file.size = item.at("lfs").at("size").get<long long>();
                        } else if (item.contains("size") && item.at("size").is_number_integer()) {
                            file.size = item.at("size").get<long long>();
                        }
                        repo_files.push_back(std::move(file));
                    }
                }
            }
        }

        if (listed) {
            for (const auto & f : repo_files) {
                if (f.path == "preset.ini") {
                    preset_file    = f;
                    is_preset_repo = true;
                    break;
                }
            }
        }

        // 3. the listing of a gated repository is public, its files are not: ask for the first file
        //    (no redirect is followed, a 3xx to the CDN already means "allowed")
        if (listed) {
            const admin_hf_file * primary = is_preset_repo ? &preset_file : admin_find_model(repo_files, quant);
            if (primary != nullptr) {
                const admin_hf_response head = admin_hf_request(
                    "HEAD", endpoint + repo + "/resolve/" + commit + "/" + admin_url_encode_path(primary->path), params.hf_token, deadline);
                if (head.reached && (head.status == 401 || head.status == 403)) {
                    warn("repo_not_found");
                    exists = false;
                    listed = false;
                    repo_files.clear();
                    is_preset_repo = false;
                }
            }
        }
    }

    // what would be downloaded
    std::vector<admin_hf_file> selected;
    std::string resolved_quant;
    if (listed && is_preset_repo) {
        selected.push_back(preset_file);
        warn("preset_repository");
        // the quant plays no part in such a download
        warnings.erase(std::remove(warnings.begin(), warnings.end(), std::string("quant_not_specified")), warnings.end());
    } else if (listed) {
        const admin_hf_file * primary = admin_find_model(repo_files, quant);
        if (primary != nullptr) {
            const admin_gguf_split split = admin_gguf_split_info(primary->path);
            resolved_quant = split.tag;
            if (split.count <= 1) {
                selected.push_back(*primary);
            } else {
                for (const auto & f : repo_files) {
                    const admin_gguf_split other = admin_gguf_split_info(f.path);
                    if (other.count == split.count && other.prefix == split.prefix) {
                        selected.push_back(f);
                    }
                }
            }
            const admin_hf_file * mmproj = admin_find_mmproj(repo_files, primary->path);
            if (mmproj != nullptr) {
                selected.push_back(*mmproj);
            }
        } else {
            warn("no_gguf_for_quant");
        }
    } else if (exists) {
        warn("size_unknown"); // the repository answered, but its file list could not be read
    }

    // already in the cache?
    bool already_present = false;
    try {
        if (!selected.empty() && !is_preset_repo) {
            std::set<std::string> cached;
            for (const auto & f : hf_cache::get_cached_files(repo)) {
                cached.insert(f.path);
            }
            already_present = true;
            for (const auto & f : selected) {
                if (f.path.find("mmproj") == std::string::npos && cached.count(f.path) == 0) {
                    already_present = false;
                    break;
                }
            }
        } else if (selected.empty()) {
            already_present = !common_download_resolve_path(model).empty();
        }
    } catch (const std::exception & e) {
        SRV_DBG("admin: cannot inspect the model cache: %s\n", e.what());
    }
    if (models.has_model(model)) {
        already_present = true; // POST /models refuses a name that is already in the list
    }
    if (already_present) {
        warn("already_present");
    }

    json j_files = json::array();
    uint64_t total_bytes  = 0;
    bool     size_unknown = false;
    for (const auto & f : selected) {
        if (f.size < 0) {
            size_unknown = true;
        } else {
            total_bytes += (uint64_t) f.size;
        }
        j_files.push_back({
            {"name",  f.path},
            {"bytes", f.size < 0 ? (uint64_t) 0 : (uint64_t) f.size},
        });
    }
    if (size_unknown) {
        warn("size_unknown");
    }

    // no quota and no reserve by design: only a warning, the download is never blocked here
    uint64_t dir_total_bytes = 0;
    uint64_t free_bytes      = 0;
    const std::string cache_dir = admin_cache_dir();
    if (!cache_dir.empty()) {
        admin_dir_space(admin_path_from_utf8(cache_dir), dir_total_bytes, free_bytes);
    }
    const bool enough_space = total_bytes <= free_bytes;
    if (!enough_space) {
        warn("not_enough_space");
    }

    return admin_res_json(200, {
        {"model",           model},
        {"repo",            repo},
        {"quant",           quant},
        {"hf_reachable",    hf_reachable},
        {"exists",          exists},
        {"already_present", already_present},
        {"files",           j_files},
        {"total_bytes",     total_bytes},
        {"free_bytes",      free_bytes},
        {"enough_space",    enough_space},
        {"warnings",        warnings},
        // extra: the quant the downloader picks when none is given
        {"resolved_quant",  resolved_quant},
    });
}

//
// model facts from the GGUF metadata (router side: gguf only, no backend, no GPU)
//

static bool admin_gguf_int(const gguf_context * g, const std::string & key, long long & out) {
    const int64_t id = gguf_find_key(g, key.c_str());
    if (id < 0) {
        return false;
    }
    switch (gguf_get_kv_type(g, id)) {
        case GGUF_TYPE_UINT8:   out = gguf_get_val_u8 (g, id); return true;
        case GGUF_TYPE_INT8:    out = gguf_get_val_i8 (g, id); return true;
        case GGUF_TYPE_UINT16:  out = gguf_get_val_u16(g, id); return true;
        case GGUF_TYPE_INT16:   out = gguf_get_val_i16(g, id); return true;
        case GGUF_TYPE_UINT32:  out = gguf_get_val_u32(g, id); return true;
        case GGUF_TYPE_INT32:   out = gguf_get_val_i32(g, id); return true;
        case GGUF_TYPE_UINT64:  out = (long long) gguf_get_val_u64(g, id); return true;
        case GGUF_TYPE_INT64:   out = gguf_get_val_i64(g, id); return true;
        case GGUF_TYPE_BOOL:    out = gguf_get_val_bool(g, id) ? 1 : 0; return true;
        default:                return false;
    }
}

// Limits for numbers read from a model file. The file may be damaged or hostile: the router reads it
// for every model it lists, so a value outside these makes the facts an error instead of a crash, a
// huge allocation or a loop that never ends (llama.cpp itself allows at most 512 layers).
static constexpr long long ADMIN_FACTS_MAX_LAYERS = 4096;
static constexpr long long ADMIN_FACTS_MAX_DIM    = 1 << 20;    // heads, head size, embedding length
static constexpr long long ADMIN_FACTS_MAX_CTX    = UINT32_MAX; // llama.cpp reads context_length as a uint32
static constexpr long long ADMIN_FACTS_MAX_SHARDS = 65535;      // split.count is a uint16 in llama.cpp

// a per-layer integer array (e.g. head_count_kv), empty when the key is a scalar, absent, not an
// integer array, or longer than any model has layers
static std::vector<long long> admin_gguf_int_array(const gguf_context * g, const std::string & key) {
    std::vector<long long> out;
    const int64_t id = gguf_find_key(g, key.c_str());
    if (id < 0 || gguf_get_kv_type(g, id) != GGUF_TYPE_ARRAY) {
        return out;
    }
    // the element type first: gguf_get_arr_data() aborts the process for an array of strings
    const enum gguf_type type = gguf_get_arr_type(g, id);
    if (type != GGUF_TYPE_INT32 && type != GGUF_TYPE_UINT32 && type != GGUF_TYPE_INT16 && type != GGUF_TYPE_UINT16 &&
        type != GGUF_TYPE_INT8 && type != GGUF_TYPE_UINT8) {
        return out;
    }
    const size_t n = gguf_get_arr_n(g, id);
    if (n > (size_t) ADMIN_FACTS_MAX_LAYERS) {
        return out;
    }
    const void * data = gguf_get_arr_data(g, id);
    switch (type) {
        case GGUF_TYPE_INT32:  for (size_t i = 0; i < n; i++) { out.push_back(((const int32_t  *) data)[i]); } break;
        case GGUF_TYPE_UINT32: for (size_t i = 0; i < n; i++) { out.push_back(((const uint32_t *) data)[i]); } break;
        case GGUF_TYPE_INT16:  for (size_t i = 0; i < n; i++) { out.push_back(((const int16_t  *) data)[i]); } break;
        case GGUF_TYPE_UINT16: for (size_t i = 0; i < n; i++) { out.push_back(((const uint16_t *) data)[i]); } break;
        case GGUF_TYPE_INT8:   for (size_t i = 0; i < n; i++) { out.push_back(((const int8_t   *) data)[i]); } break;
        case GGUF_TYPE_UINT8:  for (size_t i = 0; i < n; i++) { out.push_back(((const uint8_t  *) data)[i]); } break;
        default: break;
    }
    return out;
}

static std::string admin_gguf_str(const gguf_context * g, const std::string & key) {
    const int64_t id = gguf_find_key(g, key.c_str());
    if (id < 0 || gguf_get_kv_type(g, id) != GGUF_TYPE_STRING) {
        return "";
    }
    return gguf_get_val_str(g, id);
}

// bytes of n elements of a KV cache type
static uint64_t admin_kv_row_bytes(const std::string & type, uint64_t n) {
    if (type == "q8_0") {
        return n * 34 / 32;
    }
    if (type == "q4_0") {
        return n * 18 / 32;
    }
    return n * 2; // f16, bf16
}

// shards of a split model: <prefix>-0000N-of-0000M.gguf next to the first one. The count must be the
// same in the file name and in the metadata (split.count), so a damaged value cannot make a long list.
static std::vector<std::string> admin_gguf_shards(const std::string & first, long long count, std::string & error) {
    std::vector<std::string> out = { first };
    static const std::regex re_split("^(.*)-([0-9]{5})-of-([0-9]{5})\\.gguf$", std::regex::icase);
    std::smatch m;
    const std::string file = admin_path_to_utf8(admin_path_from_utf8(first).filename());
    if (count <= 1 || !std::regex_match(file, m, re_split)) {
        return out;
    }
    long long named = 0;
    if (!admin_parse_int(m[3].str(), named) || named != count || count > ADMIN_FACTS_MAX_SHARDS) {
        error = string_format("split.count %lld does not match the file name %s", count, file.c_str());
        return out;
    }
    const fs::path dir = admin_path_from_utf8(first).parent_path();
    for (long long i = 2; i <= count; i++) {
        out.push_back(admin_path_to_utf8(dir / admin_path_from_utf8(string_format("%s-%05lld-of-%05lld.gguf", m[1].str().c_str(), i, count))));
    }
    return out;
}

static json admin_read_facts(const std::string & path, const std::string & mmproj_path) {
    json f = {
        {"error", ""},
    };
    // stage 3: a hand-dropped file with a huge metadata section must not make the router allocate gigabytes
    std::string too_large;
    if (!admin_gguf_header_within_limit(path, too_large)) {
        f["error"] = "cannot read the GGUF metadata of " + path + ": " + too_large;
        return f;
    }
    gguf_init_params params = { /*no_alloc =*/ true, /*ctx =*/ nullptr };
    gguf_context_ptr g_ptr(gguf_init_from_file(path.c_str(), params));
    const gguf_context * g = g_ptr.get();
    if (g == nullptr) {
        f["error"] = "cannot read the GGUF metadata of " + path;
        return f;
    }
    const std::string arch = admin_gguf_str(g, "general.architecture");
    auto key = [&](const char * suffix) { return arch + "." + suffix; };
    long long n_layer = 0, n_ctx_train = 0, n_embd = 0, n_head = 0, n_head_kv = 0, d_k = 0, d_v = 0;
    long long sliding_window = 0, pooling = -1, causal = 1, split_count = 1, n_vocab = 0;
    admin_gguf_int(g, key("block_count"), n_layer);
    admin_gguf_int(g, key("context_length"), n_ctx_train);
    admin_gguf_int(g, key("embedding_length"), n_embd);
    std::vector<long long> head_arr = admin_gguf_int_array(g, key("attention.head_count"));
    if (!admin_gguf_int(g, key("attention.head_count"), n_head) && !head_arr.empty()) {
        n_head = *std::max_element(head_arr.begin(), head_arr.end());
    }
    std::vector<long long> head_kv_arr = admin_gguf_int_array(g, key("attention.head_count_kv"));
    if (!admin_gguf_int(g, key("attention.head_count_kv"), n_head_kv)) {
        n_head_kv = head_kv_arr.empty() ? n_head : *std::max_element(head_kv_arr.begin(), head_kv_arr.end());
    }
    if (!admin_gguf_int(g, key("attention.key_length"), d_k)) {
        d_k = n_head > 0 ? n_embd / n_head : 0;
    }
    if (!admin_gguf_int(g, key("attention.value_length"), d_v)) {
        d_v = n_head > 0 ? n_embd / n_head : 0;
    }
    admin_gguf_int(g, key("attention.sliding_window"), sliding_window);
    admin_gguf_int(g, key("pooling_type"), pooling);
    admin_gguf_int(g, key("attention.causal"), causal);
    admin_gguf_int(g, "split.count", split_count);
    {
        const int64_t id = gguf_find_key(g, "tokenizer.ggml.tokens");
        if (id >= 0 && gguf_get_kv_type(g, id) == GGUF_TYPE_ARRAY) {
            n_vocab = (long long) gguf_get_arr_n(g, id);
        }
    }
    long long kv_lora_rank = 0;
    const bool mla       = admin_gguf_int(g, key("attention.kv_lora_rank"), kv_lora_rank) && kv_lora_rank > 0;
    long long ssm = 0;
    const bool recurrent = admin_gguf_int(g, key("ssm.conv_kernel"), ssm) || admin_gguf_int(g, key("ssm.state_size"), ssm) ||
                           admin_gguf_int(g, key("wkv.head_size"), ssm) || admin_gguf_int(g, key("rwkv.head_size"), ssm);
    g_ptr.reset();

    // every loop and size below depends on these numbers
    {
        std::string bad;
        auto check = [&](const char * name, long long v, long long max) {
            if (bad.empty() && (v < 0 || v > max)) {
                bad = string_format("%s = %lld", name, v);
            }
        };
        check("block_count", n_layer, ADMIN_FACTS_MAX_LAYERS);
        check("context_length", n_ctx_train, ADMIN_FACTS_MAX_CTX);
        check("embedding_length", n_embd, ADMIN_FACTS_MAX_DIM);
        check("attention.head_count", n_head, ADMIN_FACTS_MAX_DIM);
        check("attention.head_count_kv", n_head_kv, ADMIN_FACTS_MAX_DIM);
        check("attention.key_length", d_k, ADMIN_FACTS_MAX_DIM);
        check("attention.value_length", d_v, ADMIN_FACTS_MAX_DIM);
        for (long long h : head_arr) {
            check("attention.head_count[]", h, ADMIN_FACTS_MAX_DIM);
        }
        for (long long h : head_kv_arr) {
            check("attention.head_count_kv[]", h, ADMIN_FACTS_MAX_DIM);
        }
        if (!bad.empty()) {
            f["error"] = "the GGUF metadata of " + path + " is not plausible (" + bad + "): the file may be damaged";
            return f;
        }
    }

    // tensor bytes per repeating layer, the output layer and the input embeddings, over all shards
    std::vector<uint64_t> layer_bytes((size_t) n_layer, 0);
    uint64_t output_bytes = 0, token_embd_bytes = 0, other_bytes = 0;
    bool has_output = false;
    std::map<std::string, uint64_t> type_bytes;
    json files = json::array();
    uint64_t file_total = 0;
    std::string shard_error;
    for (const auto & shard : admin_gguf_shards(path, split_count, shard_error)) {
        std::error_code ec;
        const uintmax_t size = fs::file_size(admin_path_from_utf8(shard), ec);
        files.push_back({ {"name", admin_path_to_utf8(admin_path_from_utf8(shard).filename())}, {"bytes", ec ? (uint64_t) 0 : (uint64_t) size} });
        if (ec) {
            f["error"] = "missing part of a split model: " + shard;
            break;
        }
        file_total += (uint64_t) size;
        if (shard != path && !admin_gguf_header_within_limit(shard, too_large)) {
            f["error"] = "cannot read the GGUF metadata of " + shard + ": " + too_large;
            break;
        }
        gguf_context_ptr gs_ptr(gguf_init_from_file(shard.c_str(), params));
        const gguf_context * gs = gs_ptr.get();
        if (gs == nullptr) {
            f["error"] = "cannot read the GGUF metadata of " + shard;
            break;
        }
        for (int64_t t = 0; t < gguf_get_n_tensors(gs); t++) {
            const std::string name = gguf_get_tensor_name(gs, t);
            const uint64_t bytes = gguf_get_tensor_size(gs, t);
            type_bytes[ggml_type_name(gguf_get_tensor_type(gs, t))] += bytes;
            if (string_starts_with(name, "blk.")) {
                const size_t dot = name.find('.', 4);
                long long il = -1;
                if (dot != std::string::npos && admin_parse_int(name.substr(4, dot - 4), il) && il >= 0 && il < (long long) layer_bytes.size()) {
                    layer_bytes[(size_t) il] += bytes;
                    continue;
                }
                other_bytes += bytes;
            } else if (name == "token_embd.weight") {
                token_embd_bytes += bytes;
            } else if (string_starts_with(name, "output.") || string_starts_with(name, "output_norm.")) {
                output_bytes += bytes;
                has_output = has_output || name == "output.weight";
            } else {
                other_bytes += bytes;
            }
        }
    }
    if (!shard_error.empty() && admin_json_str(f, "error").empty()) {
        f["error"] = shard_error;
    }
    // tied embeddings: no output.weight, the graph uses a copy of token_embd as the output on the output device
    const bool tied = !has_output && token_embd_bytes > 0;
    if (tied) {
        output_bytes += token_embd_bytes;
    }

    std::string quant = admin_gguf_split_info(admin_path_to_utf8(admin_path_from_utf8(path).filename())).tag;
    if (quant.empty()) {
        uint64_t best = 0;
        for (const auto & [type, bytes] : type_bytes) {
            if (bytes > best) {
                best  = bytes;
                quant = admin_to_upper(type);
            }
        }
    }

    // KV cache bytes per token: sum over attention layers of K row + V row
    std::string kv_estimate = "exact";
    std::string kv_note;
    if (recurrent) {
        kv_estimate = "unsupported";
        kv_note     = "recurrent or hybrid architecture: its state does not grow with the context";
    } else if (mla) {
        kv_estimate = "unsupported";
        kv_note     = "MLA attention: the KV cache is compressed";
    } else if (sliding_window > 0) {
        kv_estimate = "approximate";
        kv_note     = "sliding-window attention: some layers keep only a window";
    }
    // a quantized KV cache needs a head size that is a multiple of its block size (llama_init_from_model:
    // "K cache type q8_0 with block size 32 does not divide n_embd_head_k"): such a type has no size here
    const long long qblock = (long long) ggml_blck_size(GGML_TYPE_Q8_0); // q4_0: the same
    const bool quant_kv_ok = qblock > 0 && d_k % qblock == 0 && d_v % qblock == 0;
    if (!quant_kv_ok && kv_estimate != "unsupported") {
        kv_note += string_format("%sq8_0 / q4_0 KV cache: not possible for this model (head size %lld / %lld is not a multiple of %lld)",
                                 kv_note.empty() ? "" : "; ", d_k, d_v, qblock);
    }
    json kv_per_token = json::object();
    for (const char * type : { "f16", "bf16", "q8_0", "q4_0" }) {
        const bool quantized = type[0] == 'q';
        uint64_t total = 0;
        for (long long il = 0; il < n_layer; il++) {
            const long long hkv = il < (long long) head_kv_arr.size() ? head_kv_arr[(size_t) il] : n_head_kv;
            if (hkv <= 0) {
                continue; // no attention in this layer
            }
            total += admin_kv_row_bytes(type, (uint64_t) (hkv * d_k)) + admin_kv_row_bytes(type, (uint64_t) (hkv * d_v));
        }
        kv_per_token[type] = kv_estimate == "unsupported" || (quantized && !quant_kv_ok) ? json(nullptr) : json(total);
    }

    uint64_t mmproj_bytes = 0;
    if (!mmproj_path.empty()) {
        std::error_code ec;
        const uintmax_t size = fs::file_size(admin_path_from_utf8(mmproj_path), ec);
        mmproj_bytes = ec ? 0 : (uint64_t) size;
    }
    static const char * const pooling_names[] = { "none", "mean", "cls", "last", "rank" };
    uint64_t tensor_bytes = output_bytes - (tied ? token_embd_bytes : 0) + token_embd_bytes + other_bytes;
    for (uint64_t b : layer_bytes) {
        tensor_bytes += b;
    }

    f["arch"]              = arch;
    f["n_layer"]           = n_layer;
    f["n_ctx_train"]       = n_ctx_train;
    f["n_embd"]            = n_embd;
    f["n_head"]            = n_head;
    f["n_head_kv"]         = n_head_kv;
    f["n_head_kv_layers"]  = head_kv_arr.empty() ? json(nullptr) : json(head_kv_arr);
    f["d_k"]               = d_k;
    f["d_v"]               = d_v;
    f["n_vocab"]           = n_vocab;
    f["files"]             = files;
    f["file_bytes"]        = file_total;
    f["tensor_bytes"]      = tensor_bytes;
    f["layer_bytes"]       = layer_bytes;
    f["output_bytes"]      = output_bytes;
    f["token_embd_bytes"]  = token_embd_bytes;
    f["other_bytes"]       = other_bytes;
    f["tied_embeddings"]   = tied;
    f["mmproj_bytes"]      = mmproj_bytes;
    f["quant"]             = quant;
    f["sliding_window"]    = sliding_window;
    f["pooling"]           = pooling >= 0 && pooling <= 4 ? json(pooling_names[pooling]) : json(nullptr);
    f["causal"]            = causal != 0;
    f["kv_bytes_per_token"] = kv_per_token;
    f["kv_estimate"]       = kv_estimate;
    f["kv_note"]           = kv_note;
    return f;
}

// the local model file of a model, "" when it has none (an HF model that is not in the cache)
static std::string admin_model_file(const admin_seen_model & rec) {
    if (!rec.model_path.empty()) {
        return rec.model_path;
    }
    if (!rec.hf_repo.empty()) {
        try {
            return common_download_resolve_path(rec.hf_repo, rec.hf_file);
        } catch (const std::exception &) {
            return "";
        }
    }
    return "";
}

static bool admin_file_stamp(const std::string & path, uint64_t & size, int64_t & mtime) {
    std::error_code ec;
    const fs::path p = admin_path_from_utf8(path);
    const uintmax_t s = fs::file_size(p, ec);
    if (ec) {
        return false;
    }
    const auto t = fs::last_write_time(p, ec);
    if (ec) {
        return false;
    }
    size  = (uint64_t) s;
    mtime = (int64_t) t.time_since_epoch().count();
    return true;
}

// cached facts of a file: status ready | pending | error | unsupported (no local file)
static std::string admin_facts_lookup(admin_context & ctx, const std::string & path, json & facts) {
    if (path.empty()) {
        return "unsupported";
    }
    uint64_t size = 0;
    int64_t  mtime = 0;
    if (!admin_file_stamp(path, size, mtime)) {
        return "unsupported";
    }
    std::lock_guard<std::mutex> lk(ctx.mtx_facts);
    auto it = ctx.facts.find(path);
    if (it == ctx.facts.end() || it->second.size != size || it->second.mtime != mtime) {
        return "pending";
    }
    it->second.t_used = admin_now_ms();
    facts = it->second.facts;
    return admin_json_str(facts, "error").empty() ? "ready" : "error";
}

static void admin_facts_store(admin_context & ctx, const std::string & path, const json & facts) {
    uint64_t size = 0;
    int64_t  mtime = 0;
    if (!admin_file_stamp(path, size, mtime)) {
        return;
    }
    std::lock_guard<std::mutex> lk(ctx.mtx_facts);
    if (ctx.facts.size() >= ADMIN_FACTS_MAX && !ctx.facts.count(path)) {
        auto oldest = ctx.facts.begin();
        for (auto it = ctx.facts.begin(); it != ctx.facts.end(); ++it) {
            if (it->second.t_used < oldest->second.t_used) {
                oldest = it;
            }
        }
        ctx.facts.erase(oldest);
    }
    admin_facts_entry & e = ctx.facts[path];
    e.path   = path;
    e.size   = size;
    e.mtime  = mtime;
    e.facts  = facts;
    e.t_used = admin_now_ms();
}

// reads the facts now (outside every lock) unless they are cached
static std::string admin_facts_get(admin_context & ctx, const std::string & path, const std::string & mmproj, json & facts) {
    const std::string status = admin_facts_lookup(ctx, path, facts);
    if (status != "pending") {
        return status;
    }
    try {
        facts = admin_read_facts(path, mmproj);
    } catch (const std::exception & e) {
        // e.g. bad_alloc for a damaged file: an error for this model, never the end of the router
        facts = { {"error", "cannot read the GGUF metadata of " + path + ": " + e.what()} };
    }
    admin_facts_store(ctx, path, facts);
    return admin_json_str(facts, "error").empty() ? "ready" : "error";
}

// for the background worker (one at a time, outside every lock)
static void admin_facts_enqueue(admin_context & ctx, const std::string & path, const std::string & mmproj) {
    if (path.empty()) {
        return;
    }
    json cached;
    if (admin_facts_lookup(ctx, path, cached) != "pending") {
        return;
    }
    std::lock_guard<std::mutex> lk(ctx.mtx_facts);
    if (ctx.facts_queued.count(path) || ctx.facts_queue.size() >= ADMIN_FACTS_MAX) {
        return;
    }
    ctx.facts_queued.insert(path);
    ctx.facts_queue.push_back({ path, mmproj });
    ctx.cv_facts.notify_all();
}

static void admin_facts_thread() {
    admin_context & ctx = admin_ctx();
    while (!ctx.stop_threads.load()) {
        admin_facts_request req;
        {
            std::unique_lock<std::mutex> lk(ctx.mtx_facts);
            ctx.cv_facts.wait_for(lk, std::chrono::milliseconds(500), [&]() { return ctx.stop_threads.load() || !ctx.facts_queue.empty(); });
            if (ctx.stop_threads.load() || ctx.facts_queue.empty()) {
                continue;
            }
            req = ctx.facts_queue.front();
            ctx.facts_queue.pop_front();
        }
        try {
            json facts;
            const std::string status = admin_facts_get(ctx, req.path, req.mmproj, facts);
            SRV_INF("admin: facts of %s: %s (background read)\n", req.path.c_str(), status.c_str());
        } catch (const std::exception & e) {
            // an exception that leaves this thread would end the whole router (std::terminate)
            SRV_WRN("admin: reading the facts of %s failed: %s\n", req.path.c_str(), e.what());
        }
        std::lock_guard<std::mutex> lk(ctx.mtx_facts);
        ctx.facts_queued.erase(req.path);
    }
}

// queue the facts of the running, pinned and load-on-startup models (their GPU segments need them)
static void admin_facts_warm(admin_context & ctx, const std::vector<server_model_meta> & models, const std::vector<std::string> & pinned) {
    std::map<std::string, admin_seen_model> seen;
    {
        std::lock_guard<std::mutex> lk(ctx.mtx_seen);
        seen = ctx.seen;
    }
    for (const auto & m : models) {
        std::string startup;
        const bool wanted = m.is_running() || std::find(pinned.begin(), pinned.end(), m.name) != pinned.end() ||
            (m.preset.get_option(COMMON_ARG_PRESET_LOAD_ON_STARTUP, startup) && common_arg_utils::is_truthy(startup));
        const auto it = seen.find(m.name);
        if (wanted && it != seen.end()) {
            admin_facts_enqueue(ctx, admin_model_file(it->second), it->second.mmproj);
        }
    }
}

//
// live GPU memory from vendor tools (separate processes, no GPU context in the router)
//
// PROVISION_ADMIN_GPU_PROBE = auto (the tools of the vendors the inventory found, from PATH)
//   | off | nvidia-smi:<path> | amd-smi:<path> | rocm-smi:<path> | <absolute path> | file:<path>
// file: canned output for tests and unusual installs; each section starts with a line that names
// its format (nvidia-smi, nvidia-smi-compute-apps, amd-smi, rocm-smi). The AMD parsers follow the
// documented JSON formats and are verified only with recorded output.
//

static const char * const ADMIN_NVIDIA_QUERY = "--query-gpu=index,pci.bus_id,uuid,memory.total,memory.used,memory.free,name";
static const char * const ADMIN_NVIDIA_APPS  = "--query-compute-apps=pid,used_memory,gpu_bus_id";

static bool admin_is_tool_name(const std::string & s) {
    return s == "nvidia-smi" || s == "amd-smi" || s == "rocm-smi";
}

// "[N/A]", "[Not Supported]", "N/A"
static bool admin_is_na(const std::string & s) {
    const std::string t = admin_to_lower(admin_strip(s));
    return t.empty() || t.find("n/a") != std::string::npos || t.find("not supported") != std::string::npos || t.find("unknown") != std::string::npos;
}

// nvidia-smi --query-gpu=index,pci.bus_id,uuid,memory.total,memory.used,memory.free,name --format=csv,noheader,nounits
// "0, 00000000:01:00.0, GPU-..., 11264, 2001, 9002, NVIDIA GeForce GTX 1080 Ti" (MiB; the name may contain commas)
static bool admin_parse_nvidia(const std::string & text, std::vector<admin_vendor_gpu> & out, std::string & error) {
    size_t bad = 0;
    size_t lines = 0;
    for (const auto & raw : admin_split_lines(text)) {
        const std::string line = admin_strip(raw);
        if (line.empty()) {
            continue;
        }
        if (admin_icontains(line, "no devices were found")) {
            continue;
        }
        lines++;
        std::vector<std::string> f;
        size_t start = 0;
        for (int i = 0; i < 6; i++) {
            const size_t comma = line.find(',', start);
            if (comma == std::string::npos) {
                break;
            }
            f.push_back(admin_strip(line.substr(start, comma - start)));
            start = comma + 1;
        }
        long long index = -1;
        if (f.size() != 6 || !admin_parse_int(f[0], index)) {
            bad++;
            continue;
        }
        f.push_back(admin_strip(line.substr(start)));
        admin_vendor_gpu g;
        g.tool    = "nvidia-smi";
        g.index   = (int) index;
        g.pci_raw = f[1];
        g.pci     = admin_normalize_pci(f[1]);
        g.uuid    = f[2];
        g.name    = f[6];
        double total = 0, used = 0, free = 0;
        if (admin_parse_number(f[3], total) && admin_parse_number(f[4], used) && admin_parse_number(f[5], free)) {
            constexpr double MiB = 1024.0 * 1024.0;
            g.has_memory = true;
            g.total = (uint64_t) (total * MiB);
            g.used  = (uint64_t) (used * MiB);
            g.free  = (uint64_t) (free * MiB);
        } else if (admin_is_na(f[3]) || admin_is_na(f[4]) || admin_is_na(f[5])) {
            g.unsupported = true; // MIG / vGPU: no memory per device
        } else {
            bad++;
            continue;
        }
        out.push_back(g);
    }
    if (out.empty() && lines > 0) {
        error = string_format("cannot parse the nvidia-smi output (%zu unreadable lines)", bad);
        return false;
    }
    return true;
}

// nvidia-smi --query-compute-apps=pid,used_memory,gpu_bus_id --format=csv,noheader,nounits
static void admin_parse_nvidia_apps(const std::string & text, std::vector<admin_vendor_proc> & out) {
    for (const auto & raw : admin_split_lines(text)) {
        const std::vector<std::string> f = string_split<std::string>(admin_strip(raw), ',');
        long long pid = 0;
        double used = 0;
        if (f.size() < 3 || !admin_parse_int(f[0], pid) || !admin_parse_number(f[1], used)) {
            continue;
        }
        admin_vendor_proc p;
        p.pid  = pid;
        p.used = (uint64_t) (used * 1024.0 * 1024.0);
        p.pci  = admin_normalize_pci(f[2]);
        out.push_back(p);
    }
}

// an amd-smi memory value: 123 | "123 MB" | {"value": 123, "unit": "MB"} -> bytes
static bool admin_amd_bytes(const json & v, uint64_t & out) {
    double value = 0;
    std::string unit = "MB";
    if (v.is_number()) {
        value = v.get<double>();
    } else if (v.is_string()) {
        const std::string s = admin_strip(v.get<std::string>());
        const size_t sp = s.find(' ');
        if (!admin_parse_number(sp == std::string::npos ? s : s.substr(0, sp), value)) {
            return false;
        }
        if (sp != std::string::npos) {
            unit = admin_strip(s.substr(sp + 1));
        }
    } else if (v.is_object() && v.contains("value")) {
        const json & val = v.at("value");
        if (val.is_number()) {
            value = val.get<double>();
        } else if (!val.is_string() || !admin_parse_number(val.get<std::string>(), value)) {
            return false;
        }
        unit = admin_json_str(v, "unit");
    } else {
        return false;
    }
    const std::string u = admin_to_upper(admin_strip(unit));
    double mul = 1024.0 * 1024.0; // amd-smi reports MB meaning MiB
    if (u == "B") {
        mul = 1;
    } else if (u == "KB" || u == "KIB") {
        mul = 1024.0;
    } else if (u == "GB" || u == "GIB") {
        mul = 1024.0 * 1024.0 * 1024.0;
    }
    out = (uint64_t) (value * mul);
    return true;
}

// the per-GPU objects of an amd-smi JSON document: a top-level array, or {"gpu_data": [...]} (newer releases)
static std::vector<json> admin_amd_items(const json & doc) {
    std::vector<json> out;
    const json * arr = &doc;
    if (doc.is_object()) {
        for (const char * k : { "gpu_data", "gpus", "devices" }) {
            if (doc.contains(k) && doc.at(k).is_array()) {
                arr = &doc.at(k);
                break;
            }
        }
        if (arr == &doc) {
            out.push_back(doc);
            return out;
        }
    }
    if (arr->is_array()) {
        for (const auto & item : *arr) {
            if (item.is_object()) {
                out.push_back(item);
            }
        }
    }
    return out;
}

// The JSON document in a tool's output. stdout and stderr are read together, and the Python tools
// (amd-smi, rocm-smi) may print warnings around the document ("User is missing the following required
// groups: ...", "WARNING: AMD GPU device(s) is/are in a low-power state", a DeprecationWarning): those
// lines are skipped. Tries the whole text, then the spans from a line that starts with '[' or '{' to a
// later line that ends with ']' or '}' (a bounded number of them).
static json admin_parse_json_lenient(const std::string & text) {
    const json whole = json::parse_no_throw(text);
    if (!whole.is_discarded()) {
        return whole;
    }
    const std::vector<std::string> lines = admin_split_lines(text);
    std::vector<size_t> starts;
    std::vector<size_t> ends;
    for (size_t i = 0; i < lines.size(); i++) {
        const std::string s = admin_strip(lines[i]);
        if (!s.empty() && (s.front() == '[' || s.front() == '{')) {
            starts.push_back(i);
        }
        if (!s.empty() && (s.back() == ']' || s.back() == '}')) {
            ends.push_back(i);
        }
    }
    size_t n_starts = 0;
    for (size_t a : starts) {
        if (++n_starts > 16) {
            break;
        }
        size_t n_ends = 0;
        for (auto it = ends.rbegin(); it != ends.rend() && *it >= a; ++it) {
            if (++n_ends > 4) {
                break;
            }
            std::string span;
            for (size_t i = a; i <= *it; i++) {
                span += lines[i];
                span += '\n';
            }
            json doc = json::parse_no_throw(span);
            if (!doc.is_discarded()) {
                return doc;
            }
        }
    }
    return whole; // discarded
}

// amd-smi list --json and amd-smi metric --mem-usage --json, joined on "gpu" (one document may hold both)
static bool admin_parse_amd_smi(const std::vector<std::string> & docs, std::vector<admin_vendor_gpu> & out, std::string & error) {
    std::map<int, admin_vendor_gpu> by_index;
    bool any_doc = false;
    for (const auto & text : docs) {
        if (admin_strip(text).empty()) {
            continue;
        }
        const json doc = admin_parse_json_lenient(text);
        if (doc.is_discarded()) {
            error = "cannot parse the amd-smi JSON output";
            return false;
        }
        any_doc = true;
        for (const auto & item : admin_amd_items(doc)) {
            const int index = (int) admin_json_int(item, "gpu", -1);
            if (index < 0) {
                continue;
            }
            admin_vendor_gpu & g = by_index[index];
            g.tool  = "amd-smi";
            g.index = index;
            std::string bdf = admin_json_str(item, "bdf");
            for (const char * k : { "bus", "pcie" }) {
                if (bdf.empty() && item.contains(k) && item.at(k).is_object()) {
                    bdf = admin_json_str(item.at(k), "bdf");
                }
            }
            if (!bdf.empty()) {
                g.pci_raw = bdf;
                g.pci     = admin_normalize_pci(bdf);
            }
            if (g.uuid.empty()) {
                g.uuid = admin_json_str(item, "uuid");
            }
            if (item.contains("asic") && item.at("asic").is_object() && g.name.empty()) {
                g.name = admin_json_str(item.at("asic"), "market_name");
            }
            const json * mem = nullptr;
            for (const char * k : { "mem_usage", "memory_usage", "vram" }) {
                if (item.contains(k) && item.at(k).is_object()) {
                    mem = &item.at(k);
                    break;
                }
            }
            if (mem != nullptr) {
                uint64_t total = 0, used = 0, free = 0;
                const bool has_total = admin_amd_bytes(mem->contains("total_vram") ? mem->at("total_vram") : (mem->contains("total") ? mem->at("total") : json()), total);
                const bool has_used  = admin_amd_bytes(mem->contains("used_vram")  ? mem->at("used_vram")  : (mem->contains("used")  ? mem->at("used")  : json()), used);
                const bool has_free  = admin_amd_bytes(mem->contains("free_vram")  ? mem->at("free_vram")  : (mem->contains("free")  ? mem->at("free")  : json()), free);
                if (has_total && (has_used || has_free)) {
                    g.has_memory = true;
                    g.total = total;
                    g.used  = has_used ? used : (total > free ? total - free : 0);
                    g.free  = has_free ? free : (total > used ? total - used : 0);
                }
            }
        }
    }
    if (!any_doc) {
        error = "amd-smi printed nothing";
        return false;
    }
    for (auto & [index, g] : by_index) {
        if (!g.has_memory) {
            g.unsupported = true;
        }
        out.push_back(g);
    }
    return true;
}

// rocm-smi --showbus --showmeminfo vram --showproductname --json
// {"card0": {"PCI Bus": "0000:03:00.0", "VRAM Total Memory (B)": "17163091968", "VRAM Total Used Memory (B)": "...", "Card series": ...}}
static bool admin_parse_rocm_smi(const std::string & text, std::vector<admin_vendor_gpu> & out, std::string & error) {
    const json doc = admin_parse_json_lenient(text);
    if (doc.is_discarded() || !doc.is_object()) {
        error = "cannot parse the rocm-smi JSON output";
        return false;
    }
    for (const auto & [card, info] : doc.items()) {
        long long index = -1;
        if (!string_starts_with(card, "card") || !info.is_object() || !admin_parse_int(card.substr(4), index)) {
            continue; // e.g. "system"
        }
        admin_vendor_gpu g;
        g.tool  = "rocm-smi";
        g.index = (int) index;
        for (const auto & [k, v] : info.items()) {
            if (!v.is_string() && !v.is_number()) {
                continue;
            }
            const std::string key = admin_to_lower(k);
            const std::string val = v.is_string() ? v.get<std::string>() : v.dump();
            if (key == "pci bus") {
                g.pci_raw = val;
                g.pci     = admin_normalize_pci(val);
            } else if (key.find("vram total memory") != std::string::npos) {
                double n = 0;
                if (admin_parse_number(val, n)) {
                    g.total = (uint64_t) n;
                    g.has_memory = true;
                }
            } else if (key.find("vram total used memory") != std::string::npos) {
                double n = 0;
                if (admin_parse_number(val, n)) {
                    g.used = (uint64_t) n;
                }
            } else if (key == "card series" || (key == "card model" && g.name.empty())) {
                g.name = val;
            } else if (key == "unique id" || key == "uuid") {
                g.uuid = val;
            }
        }
        if (g.has_memory) {
            g.free = g.total > g.used ? g.total - g.used : 0;
        } else {
            g.unsupported = true;
        }
        out.push_back(g);
    }
    if (out.empty()) {
        error = "rocm-smi reported no card";
        return false;
    }
    return true;
}

// a canned file (or its printout): sections that start with a line naming the format
static bool admin_parse_canned(const std::string & text, admin_tool_state & t, std::string & error) {
    std::vector<std::pair<std::string, std::string>> sections;
    for (const auto & line : admin_split_lines(text)) {
        const std::string s = admin_strip(line);
        if (admin_is_tool_name(s) || s == "nvidia-smi-compute-apps") {
            sections.push_back({ s, "" });
            continue;
        }
        if (string_starts_with(s, "# ")) {
            continue; // directive for the cat helper
        }
        if (sections.empty()) {
            if (s.empty()) {
                continue;
            }
            error = "the canned GPU file must start with a line naming its format (nvidia-smi, amd-smi or rocm-smi)";
            return false;
        }
        sections.back().second += line + "\n";
    }
    if (sections.empty()) {
        error = "the canned GPU file is empty";
        return false;
    }
    for (const auto & [format, body] : sections) {
        std::string err;
        bool ok = true;
        if (format == "nvidia-smi") {
            ok = admin_parse_nvidia(body, t.gpus, err);
        } else if (format == "nvidia-smi-compute-apps") {
            admin_parse_nvidia_apps(body, t.procs);
            t.procs_ok = true;
        } else if (format == "amd-smi") {
            std::vector<std::string> docs;
            std::string cur;
            for (const auto & line : admin_split_lines(body)) {
                if (admin_strip(line) == "---") {
                    docs.push_back(cur);
                    cur.clear();
                } else {
                    cur += line + "\n";
                }
            }
            docs.push_back(cur);
            ok = admin_parse_amd_smi(docs, t.gpus, err);
        } else if (format == "rocm-smi") {
            ok = admin_parse_rocm_smi(body, t.gpus, err);
        }
        if (!ok) {
            error = err;
            return false;
        }
    }
    return true;
}

// the processes of this router's model children: pid -> port of the child (Linux, from /proc)
static std::map<long long, int> admin_child_ports() {
    std::map<long long, int> out;
#if defined(__linux__)
    const long long self = (long long) getpid();
    DIR * dir = opendir("/proc");
    if (dir == nullptr) {
        return out;
    }
    while (struct dirent * e = readdir(dir)) {
        long long pid = 0;
        if (!admin_parse_int(e->d_name, pid) || pid <= 0) {
            continue;
        }
        std::string stat;
        if (!admin_read_file(fs::path("/proc") / e->d_name / "stat", stat, 64 * 1024)) {
            continue;
        }
        // pid (comm) state ppid ...: comm may contain blanks and parentheses, read after the last ')'
        const size_t rp = stat.rfind(')');
        if (rp == std::string::npos) {
            continue;
        }
        const std::vector<std::string> f = string_split<std::string>(admin_strip(stat.substr(rp + 1)), ' ');
        long long ppid = 0;
        if (f.size() < 2 || !admin_parse_int(f[1], ppid) || ppid != self) {
            continue;
        }
        std::string cmdline;
        if (!admin_read_file(fs::path("/proc") / e->d_name / "cmdline", cmdline, 256 * 1024)) {
            continue;
        }
        std::vector<std::string> args;
        size_t start = 0;
        while (start < cmdline.size()) {
            const size_t z = cmdline.find('\0', start);
            args.push_back(cmdline.substr(start, z == std::string::npos ? std::string::npos : z - start));
            if (z == std::string::npos) {
                break;
            }
            start = z + 1;
        }
        for (size_t i = 0; i + 1 < args.size(); i++) {
            long long port = 0;
            if (args[i] == "--port" && admin_parse_int(args[i + 1], port)) {
                out[pid] = (int) port;
            }
        }
    }
    closedir(dir);
#endif
    return out;
}

// sets up the tools once (caller holds mtx_gpu)
static void admin_probe_setup(admin_context & ctx, const admin_inventory & inv, const json & backend) {
    ctx.tools.clear();
    ctx.probe_error.clear();
    ctx.probe_message.clear();
    ctx.tools_ready = true;
    const std::string v = ctx.gpu_probe;
    auto add = [&](const std::string & tool, const std::string & path, bool canned) {
        admin_tool_state t;
        t.tool   = tool;
        t.path   = path;
        t.canned = canned;
        ctx.tools.push_back(t);
    };
    if (admin_iequals(v, "off")) {
        ctx.probe_mode    = "off";
        ctx.probe_error   = "disabled";
        ctx.probe_message = "GPU memory probing is disabled (PROVISION_ADMIN_GPU_PROBE=off)";
        return;
    }
    if (string_starts_with(v, "file:")) {
        ctx.probe_mode = "file";
        const std::string path = v.substr(5);
        if (!admin_is_regular_file(admin_path_from_utf8(path))) {
            ctx.probe_error   = "not_found";
            ctx.probe_message = "the canned GPU file " + path + " does not exist";
            return;
        }
        add("file", path, true);
        return;
    }
    const size_t colon = v.find(':');
    if (colon != std::string::npos && admin_is_tool_name(v.substr(0, colon))) {
        ctx.probe_mode = "tool";
        const std::string tool = v.substr(0, colon);
        const std::string path = v.substr(colon + 1);
        if (!admin_is_regular_file(admin_path_from_utf8(path))) {
            ctx.probe_error   = "not_found";
            ctx.probe_message = tool + " was not found at " + path;
            return;
        }
        add(tool, path, false);
        return;
    }
    if (!admin_iequals(v, "auto")) {
        // an absolute path: the tool is told by its file name
        ctx.probe_mode = "tool";
        const std::string file = admin_to_lower(admin_path_to_utf8(admin_path_from_utf8(v).filename()));
        const std::string tool = file.find("rocm-smi") != std::string::npos ? "rocm-smi" : (file.find("amd-smi") != std::string::npos ? "amd-smi" : "nvidia-smi");
        if (!admin_is_regular_file(admin_path_from_utf8(v))) {
            ctx.probe_error   = "not_found";
            ctx.probe_message = tool + " was not found at " + v;
            return;
        }
        add(tool, v, false);
        return;
    }
    ctx.probe_mode = "auto";
    if (admin_json_str(backend, "state") == "cpu_only") {
        ctx.probe_error   = "unsupported";
        ctx.probe_message = "this server build has no GPU backend, GPU memory is not used";
        return;
    }
    std::set<std::string> vendors;
    for (const auto & d : inv.devices) {
        vendors.insert(d.vendor);
    }
    const bool unknown = !inv.ok || inv.devices.empty() || vendors.count("unknown");
    std::vector<std::string> missing;
    if (unknown || vendors.count("nvidia")) {
        const std::string p = admin_find_in_path("nvidia-smi");
        if (!p.empty()) {
            add("nvidia-smi", p, false);
        } else if (vendors.count("nvidia")) {
            missing.push_back("nvidia-smi");
        }
    }
    if (unknown || vendors.count("amd")) {
        // a stock ROCm install keeps its tools in /opt/rocm/bin, which is not on the PATH a service
        // manager (systemd) gives a unit; ROCm's own docs leave adding it to the user
        auto find_amd = [](const char * tool) {
            std::string p = admin_find_in_path(tool);
#if !defined(_WIN32)
            const std::string rocm = admin_getenv("ROCM_PATH");
            for (const std::string & dir : { rocm.empty() ? std::string() : rocm + "/bin", std::string("/opt/rocm/bin") }) {
                if (p.empty() && !dir.empty() && admin_is_regular_file(admin_path_from_utf8(dir + "/" + tool)) &&
                    access((dir + "/" + tool).c_str(), X_OK) == 0) {
                    p = dir + "/" + tool;
                }
            }
#endif
            return p;
        };
        const std::string p1 = find_amd("amd-smi");
        const std::string p2 = p1.empty() ? find_amd("rocm-smi") : "";
        if (!p1.empty()) {
            add("amd-smi", p1, false);
        } else if (!p2.empty()) {
            add("rocm-smi", p2, false);
        } else if (vendors.count("amd")) {
            missing.push_back("amd-smi / rocm-smi");
        }
    }
    const bool amd_missing = std::find(missing.begin(), missing.end(), std::string("amd-smi / rocm-smi")) != missing.end();
    const std::string where = admin_join(missing, ", ") + " not found on PATH" +
        (amd_missing ? " (nor in $ROCM_PATH/bin or /opt/rocm/bin; PROVISION_ADMIN_GPU_PROBE=amd-smi:<path> names another place)" : "");
    if (ctx.tools.empty()) {
        ctx.probe_error   = "not_found";
        ctx.probe_message = (missing.empty() ? std::string("no vendor tool for these GPUs") : where) +
                            ": GPU memory is shown as measured by the device inventory";
    } else if (!missing.empty()) {
        ctx.probe_message = where + ": those GPUs show the memory measured by the device inventory";
    }
}

static std::vector<std::vector<std::string>> admin_tool_commands(admin_context & ctx, const admin_tool_state & t, bool want_procs) {
    if (t.canned) {
        return { admin_self_command(ctx) };
    }
    if (t.tool == "nvidia-smi") {
        std::vector<std::vector<std::string>> cmds = { { t.path, ADMIN_NVIDIA_QUERY, "--format=csv,noheader,nounits" } };
        if (want_procs) {
            cmds.push_back({ t.path, ADMIN_NVIDIA_APPS, "--format=csv,noheader,nounits" });
        }
        return cmds;
    }
    if (t.tool == "amd-smi") {
        return { { t.path, "list", "--json" }, { t.path, "metric", "--mem-usage", "--json" } };
    }
    return { { t.path, "--showbus", "--showmeminfo", "vram", "--showproductname", "--json" } };
}

// a finished job -> the tool's state (caller holds mtx_gpu)
static void admin_tool_consume(admin_tool_state & t) {
    std::shared_ptr<admin_job> job = t.job;
    std::lock_guard<std::mutex> lk(job->mtx);
    t.error.clear();
    t.message.clear();
    if (!job->spawned) {
        t.error   = "not_found";
        t.message = "cannot start " + t.path;
    } else if (job->timed_out) {
        t.error   = "timeout";
        t.message = string_format("%s did not answer within %d s", t.tool.c_str(), ADMIN_PROBE_TIMEOUT_MS / 1000);
    } else if (job->exit_code != 0) {
        const std::vector<std::string> tail = admin_output_tail(job->output, 50);
        t.error   = "failed";
        t.message = string_format("%s exited with status %d", t.tool.c_str(), job->exit_code) + (tail.empty() ? "" : ": " + tail.front());
    } else {
        std::vector<std::string> parts;
        std::string cur;
        for (const auto & line : admin_split_lines(job->output)) {
            if (admin_strip(line) == ADMIN_NEXT_COMMAND) {
                parts.push_back(cur);
                cur.clear();
            } else {
                cur += line + "\n";
            }
        }
        parts.push_back(cur);
        admin_tool_state parsed;
        std::string err;
        bool ok = false;
        if (t.canned) {
            ok = admin_parse_canned(parts[0], parsed, err);
        } else if (t.tool == "nvidia-smi") {
            ok = admin_parse_nvidia(parts[0], parsed.gpus, err);
            if (parts.size() > 1) {
                admin_parse_nvidia_apps(parts[1], parsed.procs);
                parsed.procs_ok = true;
            }
        } else if (t.tool == "amd-smi") {
            ok = admin_parse_amd_smi(parts, parsed.gpus, err);
        } else {
            ok = admin_parse_rocm_smi(parts[0], parsed.gpus, err);
        }
        if (!ok) {
            t.error   = "parse_error";
            t.message = err;
        } else {
            t.gpus       = parsed.gpus;
            t.procs      = parsed.procs;
            t.procs_ok   = parsed.procs_ok;
            t.has_sample = true;
            t.t_sample   = admin_now_ms();
        }
    }
    t.job_consumed = true;
    if (!job->alive.load()) {
        t.job.reset();
    }
}

// Starts the tools whose sample is older than the cache time (or every tool on ?refresh=1, at most once
// a second) and waits for them, at most ADMIN_PROBE_WAIT_MS in total: never longer, whatever the tools do.
static void admin_probe_refresh(admin_context & ctx, bool force, bool want_procs) {
    const admin_inventory inv = admin_get_inventory(ctx);
    const json backend = admin_gpu_backend_json(ctx);
    std::vector<std::shared_ptr<admin_job>> waiting;
    {
        std::lock_guard<std::mutex> lk(ctx.mtx_gpu);
        if (!ctx.tools_ready) {
            admin_probe_setup(ctx, inv, backend);
        }
        const int64_t now = admin_now_ms();
        const bool refresh = force && now - ctx.t_refresh >= ADMIN_PROBE_REFRESH_MIN_MS;
        if (refresh) {
            ctx.t_refresh = now;
        }
        for (auto & t : ctx.tools) {
            if (t.job) {
                if (!admin_job_is_done(t.job)) {
                    waiting.push_back(t.job); // running: wait for it, start nothing
                    continue;
                }
                if (!t.job_consumed) {
                    admin_tool_consume(t); // finished after its caller stopped waiting
                }
                if (t.job && !t.job->alive.load()) {
                    t.job.reset(); // the process that timed out has been reaped meanwhile
                }
                if (t.job) {
                    // timed out and still not reaped (stuck in the driver?): no second process
                    t.error   = "probe_stuck";
                    t.message = string_format("the previous %s run has not exited for %lld s", t.tool.c_str(),
                                              (long long) ((now - t.job->t_start) / 1000));
                    continue;
                }
            }
            // a sample without the per-process rows does not answer a caller that needs them
            const bool lacks_procs = want_procs && !t.canned && t.tool == "nvidia-smi" && !t.procs_ok;
            const bool fresh = t.has_sample && now - t.t_sample < ADMIN_PROBE_CACHE_MS && !lacks_procs;
            if ((fresh || (now - t.t_try < ADMIN_PROBE_CACHE_MS && !lacks_procs)) && !refresh) {
                continue;
            }
            t.t_try        = now;
            t.job_consumed = false;
            if (t.canned) {
                t.job = admin_job_start("gpu probe", admin_tool_commands(ctx, t, want_procs), admin_helper_env(ctx, "cat", t.path),
                                        ADMIN_PROBE_TIMEOUT_MS, admin_canned_ignores_terminate(t.path));
            } else {
                t.job = admin_job_start("gpu probe", admin_tool_commands(ctx, t, want_procs), ctx.env_snapshot, ADMIN_PROBE_TIMEOUT_MS);
            }
            waiting.push_back(t.job);
        }
    }
    const int64_t deadline = admin_now_ms() + ADMIN_PROBE_WAIT_MS;
    for (const auto & job : waiting) {
        admin_job_wait(job, deadline - admin_now_ms());
    }
    std::lock_guard<std::mutex> lk(ctx.mtx_gpu);
    for (auto & t : ctx.tools) {
        if (t.job && admin_job_is_done(t.job) && !t.job_consumed) {
            admin_tool_consume(t);
        } else if (t.job && !admin_job_is_done(t.job) && std::find(waiting.begin(), waiting.end(), t.job) != waiting.end()) {
            t.error   = "timeout";
            t.message = string_format("%s has not answered for %lld s", t.tool.c_str(), (long long) ((admin_now_ms() - t.job->t_start) / 1000));
        }
    }
}

// what a model uses on each device right now: reported by the model (its memory breakdown lines) >
// measured (nvidia-smi per-process, host only) > estimate (a fit check of its current settings, else
// the GGUF formula); bytes per device name
struct admin_model_memory {
    std::map<std::string, uint64_t> bytes;
    std::string source; // reported | measured | estimate | "" (unknown)
    std::string by;     // model | nvidia-smi | llama.cpp | formula
};

//
// effective values of a model, rendering of its child arguments
//

// for set_option() of keys a preset does not have yet (read-only after construction)
static const common_preset_context & admin_preset_ctx() {
    static const common_preset_context ctx(LLAMA_EXAMPLE_SERVER);
    return ctx;
}

// the whitelisted values (+ kv-unified) a child of this model gets now, from its preset
static admin_kv admin_effective_values(const server_model_meta & meta) {
    admin_kv out;
    for (const auto & spec : ADMIN_KEYS) {
        std::string value;
        if (meta.preset.get_option(spec.env, value)) {
            out[spec.key] = value;
        }
    }
    std::string value;
    if (meta.preset.get_option(ADMIN_KV_UNIFIED_ENV, value)) {
        out[ADMIN_KV_UNIFIED_KEY] = value;
    }
    return out;
}

// the command line of a child of this model with these whitelisted values, without the host / port /
// alias the router adds (they do not change what the model needs)
static std::vector<std::string> admin_render_args(const admin_context & ctx, const server_model_meta & meta, const admin_kv & values) {
    common_preset preset = meta.preset;
    for (const auto & spec : ADMIN_KEYS) {
        preset.unset_option(spec.env);
    }
    preset.unset_option(ADMIN_KV_UNIFIED_ENV);
    for (const auto & [key, value] : values) {
        const char * env = admin_key_env(key);
        if (env != nullptr) {
            preset.set_option(admin_preset_ctx(), env, value);
        }
    }
    for (const char * env : { "LLAMA_ARG_HOST", "LLAMA_ARG_PORT", "LLAMA_ARG_ALIAS", "LLAMA_ARG_LOG_FILE", "LLAMA_API_KEY", "LLAMA_ARG_API_KEY_FILE",
                              "LLAMA_ARG_SSL_KEY_FILE", "LLAMA_ARG_SSL_CERT_FILE", "LLAMA_ARG_MODELS_DIR", "LLAMA_ARG_MODELS_MAX",
                              "LLAMA_ARG_MODELS_PRESET", "LLAMA_ARG_MODELS_AUTOLOAD" }) {
        preset.unset_option(env);
    }
    std::vector<std::string> args = preset.to_args(ctx.bin_path);
    const std::string app_cmd = admin_getenv("LLAMA_APP_CMD");
    if (!app_cmd.empty() && !args.empty()) {
        args.insert(args.begin() + 1, app_cmd);
    }
    return args;
}

static std::string admin_check_key(const std::string & model, const std::vector<std::string> & args, int64_t inventory_time) {
    std::string key = model + "\x1f" + std::to_string(inventory_time);
    for (const auto & a : args) {
        key += "\x1f" + a;
    }
    return key;
}

static void admin_check_store(admin_context & ctx, const std::string & key, const json & result) {
    std::lock_guard<std::mutex> lk(ctx.mtx_check);
    if (!ctx.check_cache.count(key)) {
        ctx.check_order.push_back(key);
    }
    ctx.check_cache[key] = result;
    while (ctx.check_order.size() > 32) {
        ctx.check_cache.erase(ctx.check_order.front());
        ctx.check_order.pop_front();
    }
}

static json admin_check_cached(admin_context & ctx, const std::string & key) {
    std::lock_guard<std::mutex> lk(ctx.mtx_check);
    const auto it = ctx.check_cache.find(key);
    return it != ctx.check_cache.end() ? it->second : json(nullptr);
}

// The fit of a dry run (llama.cpp's own, on by default) adapts the values left on auto to the memory that
// was free at that moment, so a cached result answers the dialog again only for a short time, while
// the model is in the same state (running or not: a running model holds memory the check saw as used),
// and while the live free memory of the devices it used moved less than the slack.
static constexpr int64_t  ADMIN_CHECK_TTL_MS     = 60000;
static constexpr uint64_t ADMIN_CHECK_FREE_SLACK = 256ull * 1024 * 1024;

// device name (lower case) -> free bytes, for the GPUs whose memory a vendor tool reports live
static std::map<std::string, uint64_t> admin_live_free(const json & gpus) {
    std::map<std::string, uint64_t> out;
    if (!gpus.contains("gpus")) {
        return out;
    }
    for (const auto & card : gpus.at("gpus")) {
        if (admin_json_str(card, "memory_source") == "vendor_tool" && card.contains("free") && card.at("free").is_number()) {
            out[admin_to_lower(admin_json_str(card, "device"))] = card.at("free").get<uint64_t>();
        }
    }
    return out;
}

static bool admin_check_reusable(const json & cached, bool running_now, const std::map<std::string, uint64_t> & free_now) {
    if (!cached.is_object() || !cached.contains("admin_seen") || !cached.at("admin_seen").is_object()) {
        return false;
    }
    const json & seen = cached.at("admin_seen");
    if (admin_now_ms() - admin_json_int(seen, "t_ms", 0) > ADMIN_CHECK_TTL_MS || seen.value("running", false) != running_now) {
        return false;
    }
    const json seen_free = seen.contains("free") && seen.at("free").is_object() ? seen.at("free") : json::object();
    for (const auto & d : cached.value("devices", json::array())) {
        const std::string dev = admin_to_lower(admin_json_str(d, "device"));
        const auto now_it = free_now.find(dev);
        if (now_it == free_now.end()) {
            continue; // no live reading now: nothing to compare
        }
        if (!seen_free.contains(dev) || !seen_free.at(dev).is_number()) {
            return false; // a live reading appeared since
        }
        const uint64_t then = seen_free.at(dev).get<uint64_t>();
        const uint64_t diff = then > now_it->second ? then - now_it->second : now_it->second - then;
        if (diff > ADMIN_CHECK_FREE_SLACK) {
            return false;
        }
    }
    return true;
}

// what the GGUF formula expects a model to need on each of its GPUs (a rough placeholder, never shown as measured)
static uint64_t admin_formula_bytes(const json & facts, const admin_kv & values, size_t n_devices) {
    if (!facts.is_object() || n_devices == 0) {
        return 0;
    }
    const long long n_layer     = admin_json_int(facts, "n_layer", 0);
    const long long n_ctx_train = admin_json_int(facts, "n_ctx_train", 0);
    const long long n_embd      = admin_json_int(facts, "n_embd", 0);
    const long long n_vocab     = admin_json_int(facts, "n_vocab", 0);
    const long long n_head      = admin_json_int(facts, "n_head", 0);
    if (n_layer <= 0 || n_layer > ADMIN_FACTS_MAX_LAYERS || n_ctx_train < 0 || n_ctx_train > ADMIN_FACTS_MAX_CTX) {
        return 0;
    }
    long long ngl = n_layer + 1;
    long long v = 0;
    const std::string ngl_s = admin_value_or_default(values, "n-gpu-layers");
    if (admin_parse_int(ngl_s, v) && v >= 0) { // a negative count (-1 in a models.ini) is auto: all layers, like "all"
        ngl = std::min(v, n_layer + 1);
    }
    if (ngl <= 0) {
        return 0;
    }
    uint64_t weights = (uint64_t) std::max<long long>(0, admin_json_int(facts, "output_bytes", 0));
    const json & layers = facts.contains("layer_bytes") ? facts.at("layer_bytes") : json::array();
    const long long n_rep = std::min(ngl - 1, n_layer);
    for (long long il = n_layer - n_rep; il < n_layer; il++) {
        if (layers.is_array() && il >= 0 && il < (long long) layers.size() && layers[(size_t) il].is_number()) {
            weights += layers[(size_t) il].get<uint64_t>();
        }
    }
    long long slots = 4;
    if (admin_parse_int(admin_value_or_default(values, "parallel"), v) && v > 0) {
        slots = std::min<long long>(v, 4096);
    }
    // the whole pool, as llama.cpp sizes it: -c N; else (no -c) n_parallel x --kv-unified-per-slot
    // (tools/server/server.cpp); else, and for -c 0, the training context (llama_context, n_ctx = 0)
    long long n_ctx = n_ctx_train;
    if (values.count("ctx-size")) {
        if (admin_parse_int(values.at("ctx-size"), v) && v > 0) {
            n_ctx = v;
        }
    } else if (values.count("kv-unified-per-slot") && admin_parse_int(values.at("kv-unified-per-slot"), v) && v > 0) {
        n_ctx = slots * std::min<long long>(v, ADMIN_FACTS_MAX_CTX);
    }
    n_ctx = std::min<long long>(n_ctx, ADMIN_FACTS_MAX_CTX);
    n_ctx = (n_ctx + 255) / 256 * 256;
    const json kv = facts.contains("kv_bytes_per_token") ? facts.at("kv_bytes_per_token") : json::object();
    const std::string tk = admin_value_or_default(values, "cache-type-k");
    const std::string tv = admin_value_or_default(values, "cache-type-v");
    uint64_t per_token = 0;
    if (kv.contains(tk) && kv.at(tk).is_number() && kv.contains(tv) && kv.at(tv).is_number()) {
        per_token = (kv.at(tk).get<uint64_t>() + kv.at(tv).get<uint64_t>()) / 2;
    }
    const uint64_t kv_bytes = per_token * (uint64_t) n_ctx * (uint64_t) n_rep / (uint64_t) n_layer;
    long long ubatch = 512;
    if (admin_parse_int(admin_value_or_default(values, "ubatch-size"), v) && v > 0) {
        ubatch = v;
    }
    uint64_t compute = 4ull * (uint64_t) ubatch * (uint64_t) std::max<long long>(0, n_vocab + 6 * n_embd);
    if (admin_value_or_default(values, "flash-attn") == "off") {
        compute += 4ull * (uint64_t) ubatch * (uint64_t) n_ctx * (uint64_t) std::max<long long>(1, n_head);
    }
    return (weights + kv_bytes + compute) / n_devices;
}

//
// GPU placement of the models
//

// the GPUs a model uses: its --device list (registry names), or every GPU when it has none
static std::vector<std::string> admin_model_devices_list(const server_model_meta & m, const std::vector<std::string> & gpus, bool & all_gpus) {
    all_gpus = false;
    std::string value;
    if (!m.preset.get_option("LLAMA_ARG_DEVICE", value) || admin_strip(value).empty()) {
        all_gpus = true;
        return gpus;
    }
    std::vector<std::string> out;
    for (const auto & name : admin_device_list(value)) {
        if (name == "none") {
            return {};
        }
        out.push_back(name);
    }
    return out;
}

struct admin_mem_inputs {
    std::map<std::string, admin_seen_model>   seen;
    std::map<std::string, admin_log_instance> tails;     // id -> current output tail
    std::vector<admin_vendor_proc>            procs;     // nvidia-smi per process
    bool                                      procs_ok = false;
    std::map<long long, int>                  child_ports;
    std::map<std::string, std::string>        pci_to_device; // normalised PCI id -> device name
};

static admin_model_memory admin_model_memory_of(admin_context & ctx, const server_model_meta & m, const std::vector<std::string> & devices,
                                                const admin_mem_inputs & in) {
    admin_model_memory mem;
    if (!m.is_running() || devices.empty()) {
        return mem;
    }
    // reported by the model itself (memory breakdown lines, printed at verbosity >= 4)
    const auto tail = in.tails.find(m.name);
    if (tail != in.tails.end() && !tail->second.breakdown.empty()) {
        for (const auto & [name, b] : tail->second.breakdown) {
            if (!b.host) {
                mem.bytes[name] = b.model + b.context + b.compute;
            }
        }
        if (!mem.bytes.empty()) {
            mem.source = "reported";
            mem.by     = "model";
            return mem;
        }
    }
    // measured: nvidia-smi per process, matched to this child's PID (on the host; in a container the PIDs do not match)
    if (in.procs_ok && m.port > 0) {
        for (const auto & [pid, port] : in.child_ports) {
            if (port != m.port) {
                continue;
            }
            for (const auto & p : in.procs) {
                const auto dev = in.pci_to_device.find(p.pci);
                if (p.pid == pid && dev != in.pci_to_device.end()) {
                    mem.bytes[dev->second] += p.used;
                }
            }
        }
        if (!mem.bytes.empty()) {
            mem.source = "measured";
            mem.by     = "nvidia-smi";
            return mem;
        }
    }
    // estimate: a dry-run check of exactly these settings, if one was made; else the GGUF formula.
    // Not a check made while this model ran: its fit saw the model's own memory as used and may have
    // planned fewer GPU layers or a smaller context than the running instance has.
    const admin_kv values = admin_effective_values(m);
    {
        const admin_inventory inv = admin_get_inventory(ctx);
        const json cached = admin_check_cached(ctx, admin_check_key(m.name, admin_render_args(ctx, m, values), inv.time_ms));
        const bool while_running = cached.is_object() && cached.contains("admin_seen") && cached.at("admin_seen").value("running", false);
        if (cached.is_object() && cached.value("ok", false) && !(while_running && admin_json_str(cached, "fit") != "off")) {
            for (const auto & d : cached.at("devices")) {
                const std::string name = admin_json_str(d, "device");
                for (const auto & dev : devices) {
                    if (admin_iequals(dev, name)) {
                        mem.bytes[dev] = (uint64_t) std::max<long long>(0, admin_json_int(d, "model", 0)) +
                                         (uint64_t) std::max<long long>(0, admin_json_int(d, "context", 0)) +
                                         (uint64_t) std::max<long long>(0, admin_json_int(d, "compute", 0));
                    }
                }
            }
            if (!mem.bytes.empty()) {
                mem.source = "estimate";
                mem.by     = "llama.cpp";
                return mem;
            }
            // the dry run put nothing on the GPUs this model is listed on: fall back to the formula below
        }
    }
    const auto rec = in.seen.find(m.name);
    if (rec != in.seen.end()) {
        json facts;
        if (admin_facts_lookup(ctx, admin_model_file(rec->second), facts) == "ready") {
            const uint64_t per_device = admin_formula_bytes(facts, values, devices.size());
            if (per_device > 0) {
                for (const auto & d : devices) {
                    mem.bytes[d] = per_device;
                }
                mem.source = "estimate";
                mem.by     = "formula";
            }
        }
    }
    return mem;
}

// models placed on a GPU are loading, or changed status a moment ago: its free memory is not final yet
static bool admin_gpu_settling(admin_context & ctx, const std::vector<std::string> & models_on_gpu) {
    std::lock_guard<std::mutex> lk(ctx.mtx_track);
    const int64_t now = admin_now_ms();
    for (const auto & id : models_on_gpu) {
        const auto it = ctx.track.find(id);
        if (it == ctx.track.end()) {
            continue;
        }
        if (it->second.status == "loading" || now - it->second.t_change < ADMIN_SETTLE_MS) {
            return true;
        }
    }
    return false;
}

static json admin_inventory_json(const admin_inventory & inv) {
    json devices = json::array();
    for (const auto & d : inv.devices) {
        devices.push_back({
            {"name",         d.name},
            {"backend",      d.backend},
            {"description",  d.description},
            {"type",         d.type},
            {"device_id",    d.device_id.empty() ? json(nullptr) : json(d.device_id)},
            {"pci_bus_id",   d.pci.empty() ? json(nullptr) : json(d.pci)},
            {"vendor",       d.vendor},
            {"memory_free",  d.memory_free},
            {"memory_total", d.memory_total},
        });
    }
    return {
        {"ran",         inv.ran},
        {"ok",          inv.ok},
        {"source",      inv.source},
        {"error",       inv.error},
        {"message",     inv.message},
        {"time",        inv.time_ms > 0 ? json(admin_iso_time(inv.time_ms)) : json(nullptr)},
        {"time_ms",     inv.time_ms},
        {"age_ms",      inv.time_ms > 0 ? admin_wall_ms() - inv.time_ms : 0},
        {"duration_ms", inv.duration_ms},
        {"devices",     devices},
        {"backends",    inv.backends},
    };
}

struct admin_gpu_card {
    std::string device;
    const admin_inv_device * inv = nullptr;
    const admin_vendor_gpu * row = nullptr;
    std::string mapping; // pci | single | unknown
};

//
// GET /admin/gpus, POST /admin/gpus/rescan
//

static admin_mem_inputs admin_memory_inputs(admin_context & ctx, const std::vector<server_model_meta> & models,
                                            const std::vector<admin_vendor_proc> & procs, bool procs_ok, const admin_inventory & inv) {
    admin_mem_inputs in;
    {
        std::lock_guard<std::mutex> lk(ctx.mtx_seen);
        in.seen = ctx.seen;
    }
    for (const auto & m : models) {
        admin_log_instance tail;
        if (m.is_running() && admin_log_copy(ctx, m.name, tail) && tail.port == m.port) {
            tail.lines.clear(); // only the breakdown is needed here
            in.tails[m.name] = tail;
        }
    }
    in.procs    = procs;
    in.procs_ok = procs_ok;
    if (procs_ok && !procs.empty()) {
        in.child_ports = admin_child_ports(); // scans /proc: only when there is something to match
    }
    for (const auto & d : inv.devices) {
        if (!d.pci.empty() && !in.pci_to_device.count(d.pci)) {
            in.pci_to_device[d.pci] = d.name;
        }
    }
    return in;
}

static json admin_gpus_body(admin_context & ctx, bool refresh) {
    const admin_live live = admin_refresh(ctx, false);
    const admin_inventory inv = admin_get_inventory(ctx);
    const json backend = admin_gpu_backend_json(ctx);
    const std::vector<std::string> build_devices = admin_build_devices(ctx);

    admin_facts_warm(ctx, live.models, live.pinned.applied);

    bool any_running = false;
    for (const auto & m : live.models) {
        any_running = any_running || m.is_running();
    }
#if defined(__linux__)
    const bool want_procs = any_running; // per-process numbers, matched to the children by PID (/proc)
#else
    const bool want_procs = false; // no /proc to match PIDs (and [N/A] under WDDM)
    (void) any_running;
#endif
    admin_probe_refresh(ctx, refresh, want_procs);

    std::vector<admin_tool_state> tools;
    std::string probe_mode;
    std::string probe_error;
    std::string probe_message;
    {
        std::lock_guard<std::mutex> lk(ctx.mtx_gpu);
        tools         = ctx.tools;
        probe_mode    = ctx.probe_mode;
        probe_error   = ctx.probe_error;
        probe_message = ctx.probe_message;
    }
    const int64_t now = admin_now_ms();

    // vendor rows (the latest good sample of each tool)
    std::vector<admin_vendor_gpu> rows;
    std::vector<int64_t> row_age;
    std::vector<admin_vendor_proc> procs;
    bool procs_ok = false;
    json j_tools = json::array();
    std::string first_error;
    std::string first_message;
    int64_t oldest = -1;
    bool stale = false;
    for (const auto & t : tools) {
        const int64_t age = t.has_sample ? now - t.t_sample : -1;
        if (t.has_sample) {
            for (const auto & g : t.gpus) {
                rows.push_back(g);
                row_age.push_back(age);
            }
            for (const auto & p : t.procs) {
                procs.push_back(p);
            }
            procs_ok = procs_ok || t.procs_ok;
            oldest = std::max(oldest, age);
            stale  = stale || !t.error.empty();
        }
        if (!t.error.empty() && first_error.empty()) {
            first_error   = t.error;
            first_message = t.message;
        }
        j_tools.push_back({
            {"tool",    t.canned ? std::string("file") : t.tool},
            {"path",    t.path},
            {"ok",      t.error.empty() && t.has_sample},
            {"error",   t.error},
            {"message", t.message},
            {"age_ms",  age >= 0 ? json(age) : json(nullptr)},
            {"rows",    t.gpus.size()},
        });
    }

    // cards: the inventory's GPUs (or the router's registry names when the inventory failed), matched by PCI id
    std::vector<admin_gpu_card> cards;
    if (inv.ok) {
        for (const auto & d : inv.devices) {
            if (d.type == "gpu" || d.type == "igpu") {
                cards.push_back({ d.name, &d, nullptr, "unknown" });
            }
        }
    } else {
        for (const auto & name : build_devices) {
            cards.push_back({ name, nullptr, nullptr, "unknown" });
        }
    }
    std::vector<bool> row_used(rows.size(), false);
    for (auto & card : cards) {
        if (card.inv == nullptr || card.inv->pci.empty()) {
            continue;
        }
        for (size_t i = 0; i < rows.size(); i++) {
            if (!row_used[i] && rows[i].pci == card.inv->pci) {
                card.row     = &rows[i];
                card.mapping = "pci";
                row_used[i]  = true;
                break;
            }
        }
    }
    if (cards.size() == 1 && rows.size() == 1 && cards[0].row == nullptr && !row_used[0] &&
        (cards[0].inv == nullptr || cards[0].inv->pci.empty() || rows[0].pci.empty())) {
        cards[0].row     = &rows[0];
        cards[0].mapping = "single";
        row_used[0]      = true;
    }

    const admin_mem_inputs mem_in = admin_memory_inputs(ctx, live.models, procs, procs_ok, inv);

    json j_gpus = json::array();
    std::set<std::string> mappings;
    for (const auto & card : cards) {
        mappings.insert(card.mapping);
        json j_models = json::array();
        std::vector<std::string> on_gpu;
        for (const auto & m : live.models) {
            bool all_gpus = false;
            const std::vector<std::string> devs = admin_model_devices_list(m, build_devices, all_gpus);
            bool here = false;
            for (const auto & d : devs) {
                here = here || admin_iequals(d, card.device);
            }
            if (!here) {
                continue;
            }
            on_gpu.push_back(m.name);
            const admin_model_memory mem = admin_model_memory_of(ctx, m, devs, mem_in);
            json j_mem = nullptr;
            for (const auto & [dev, bytes] : mem.bytes) {
                if (admin_iequals(dev, card.device)) {
                    j_mem = { {"bytes", bytes}, {"source", mem.source}, {"by", mem.by} };
                }
            }
            std::string facts_status = "unsupported";
            const auto rec = mem_in.seen.find(m.name);
            if (rec != mem_in.seen.end()) {
                json facts;
                facts_status = admin_facts_lookup(ctx, admin_model_file(rec->second), facts);
            }
            const bool pinned = std::find(live.pinned.applied.begin(), live.pinned.applied.end(), m.name) != live.pinned.applied.end();
            j_models.push_back({
                {"id",           m.name},
                {"status",       server_model_status_to_string(m.status)},
                {"all_gpus",     all_gpus},
                {"pinned",       pinned},
                {"memory",       j_mem},
                {"facts_status", facts_status},
            });
        }

        json j = {
            {"device",      card.device},
            {"backend",     card.inv ? json(card.inv->backend) : json(nullptr)},
            {"vendor",      card.inv ? json(card.inv->vendor) : json(nullptr)},
            {"type",        card.inv ? json(card.inv->type) : json(nullptr)},
            {"description", card.inv ? json(card.inv->description) : json(nullptr)},
            {"pci_bus_id",  card.inv && !card.inv->pci.empty() ? json(card.inv->pci) : (card.row && !card.row->pci.empty() ? json(card.row->pci) : json(nullptr))},
            {"mapping",     card.mapping},
            {"tool",        card.row ? json(card.row->tool) : json(nullptr)},
            {"index",       card.row ? json(card.row->index) : json(nullptr)},
            {"uuid",        card.row && !card.row->uuid.empty() ? json(card.row->uuid) : json(nullptr)},
            {"name",        card.row && !card.row->name.empty() ? json(card.row->name) : (card.inv ? json(card.inv->description) : json(card.device))},
            {"unsupported", card.row ? card.row->unsupported : false},
            {"settling",    admin_gpu_settling(ctx, on_gpu)},
            {"models",      j_models},
        };
        if (card.row && card.row->has_memory && !card.row->unsupported) {
            const size_t i = (size_t) (card.row - rows.data());
            j["total"]         = card.row->total;
            j["used"]          = card.row->used;
            j["free"]          = card.row->free;
            j["memory_source"] = "vendor_tool";
            j["memory_age_ms"] = row_age[i];
        } else if (card.inv && card.inv->memory_total > 0) {
            j["total"]         = card.inv->memory_total;
            j["free"]          = card.inv->memory_free;
            j["used"]          = card.inv->memory_total > card.inv->memory_free ? card.inv->memory_total - card.inv->memory_free : 0;
            j["memory_source"] = "inventory";
            j["memory_age_ms"] = admin_wall_ms() - inv.time_ms;
        } else {
            j["total"]         = nullptr;
            j["free"]          = nullptr;
            j["used"]          = nullptr;
            j["memory_source"] = "none";
            j["memory_age_ms"] = nullptr;
        }
        // the same physical GPU behind several backends (e.g. CUDA0 and Vulkan0)
        json same = json::array();
        if (card.inv && !card.inv->pci.empty()) {
            for (const auto & other : cards) {
                if (&other != &card && other.inv && other.inv->pci == card.inv->pci) {
                    same.push_back(other.device);
                }
            }
        }
        j["same_gpu_as"] = same;
        j_gpus.push_back(j);
    }

    json unmatched = json::array();
    for (size_t i = 0; i < rows.size(); i++) {
        if (row_used[i]) {
            continue;
        }
        const auto & r = rows[i];
        unmatched.push_back({
            {"tool",        r.tool},
            {"index",       r.index},
            {"pci_bus_id",  r.pci.empty() ? json(nullptr) : json(r.pci)},
            {"uuid",        r.uuid},
            {"name",        r.name},
            {"unsupported", r.unsupported},
            {"total",       r.has_memory ? json(r.total) : json(nullptr)},
            {"used",        r.has_memory ? json(r.used) : json(nullptr)},
            {"free",        r.has_memory ? json(r.free) : json(nullptr)},
        });
    }

    std::string mapping = "none";
    if (mappings.size() == 1) {
        mapping = *mappings.begin();
    } else if (mappings.size() > 1) {
        mapping = "mixed";
    }
    std::string source = "none";
    if (tools.size() == 1) {
        source = tools[0].canned ? "file" : tools[0].tool;
    } else if (tools.size() > 1) {
        source = "multiple";
    }
    const std::string error = !probe_error.empty() ? probe_error : first_error;
    const std::string message = !probe_error.empty() ? probe_message : (first_message.empty() ? probe_message : first_message);

    json warnings = json::array();
    if (admin_json_str(backend, "state") == "no_devices") {
        warnings.push_back("gpu_backend_not_initialised");
    }
    if (!unmatched.empty() && !cards.empty()) {
        warnings.push_back("vendor_rows_not_matched");
    }
    return {
        {"build_devices", build_devices},
        {"gpu_backend",   backend},
        {"inventory",     admin_inventory_json(inv)},
        {"probe", {
            {"mode",    probe_mode},
            {"source",  source},
            {"ok",      error.empty() && !tools.empty()},
            {"error",   error},
            {"message", message},
            {"age_ms",  oldest >= 0 ? json(oldest) : json(nullptr)},
            {"stale",   stale},
            {"mapping", mapping},
            {"tools",   j_tools},
        }},
        {"margin",    ADMIN_FIT_MARGIN},
        {"settle_ms", ADMIN_SETTLE_MS},
        {"gpus",      j_gpus},
        {"unmatched", unmatched},
        {"warnings",  warnings},
    };
}

static server_http_res_ptr admin_handle_gpus(admin_context & ctx, const server_http_req & req) {
    const std::string refresh = req.get_param("refresh");
    return admin_res_json(200, admin_gpus_body(ctx, !refresh.empty() && refresh != "0" && refresh != "false"));
}

// runs the inventory helper again (it briefly creates a context on every GPU)
static server_http_res_ptr admin_handle_gpus_rescan(admin_context & ctx, const server_http_req &) {
    {
        // check and reserve in one step: two requests close together must not run two helpers
        std::shared_ptr<admin_job> alive;
        bool busy = false;
        {
            std::lock_guard<std::mutex> lk(ctx.mtx_inv);
            if (ctx.inv_job && ctx.inv_job->alive.load()) {
                alive = ctx.inv_job;
            } else if (ctx.inv_rescan_running) {
                busy = true;
            } else {
                ctx.inv_rescan_running = true;
            }
        }
        if (alive && admin_job_is_stuck(alive)) { // outside mtx_inv: it takes the job's own mutex
            throw admin_http_error(409, "rescan_stuck", admin_stuck_message("device inventory", alive));
        }
        if (alive || busy) {
            throw admin_http_error(409, "rescan_busy", "a device inventory is still running");
        }
    }
    struct rescan_release {
        admin_context & ctx;
        ~rescan_release() {
            std::lock_guard<std::mutex> lk(ctx.mtx_inv);
            ctx.inv_rescan_running = false;
        }
    } release{ ctx };
    const admin_inventory inv = admin_run_inventory(ctx);
    admin_set_inventory(ctx, inv);
    {
        std::lock_guard<std::mutex> lk(ctx.mtx_gpu);
        ctx.tools_ready = false; // the vendors may have changed
    }
    if (inv.ok) {
        SRV_INF("admin: device inventory re-detected: %zu device(s)\n", inv.devices.size());
    } else {
        SRV_WRN("admin: device inventory re-detection failed (%s): %s\n", inv.error.c_str(), inv.message.c_str());
    }
    return admin_res_json(200, admin_gpus_body(ctx, true));
}

//
// GET /admin/models/log?model=<id>
//

static server_http_res_ptr admin_handle_model_log(admin_context & ctx, const server_http_req & req) {
    const std::string id = req.get_param("model");
    if (id.empty()) {
        throw admin_http_error(400, "invalid_request_error", "model is required");
    }
    std::optional<server_model_meta> meta = ctx.routes->models.get_meta(id);
    admin_log_instance tail;
    const bool has_tail = admin_log_copy(ctx, id, tail);
    const json failure = admin_get_failure(ctx, id);
    if (!meta.has_value() && !has_tail && failure.is_null()) {
        throw admin_http_error(404, "not_found_error", "model '" + id + "' is not found");
    }
    const bool running = meta.has_value() && meta->is_running() && has_tail && tail.port == meta->port;
    json lines = json::array();
    for (const auto & line : tail.lines) {
        lines.push_back(line);
    }
    json reason = nullptr;
    if (has_tail) {
        const admin_reason r = admin_classify(tail.lines);
        if (r.code != "none" || !running) {
            reason = admin_reason_json(r, meta.has_value() ? meta->exit_code : 0);
        }
    }
    return admin_res_json(200, {
        {"model",    meta.has_value() ? meta->name : id},
        {"status",   meta.has_value() ? json(server_model_status_to_string(meta->status)) : json(nullptr)},
        {"instance", has_tail ? json({
            {"port",       tail.port},
            {"running",    running},
            {"started",    admin_iso_time(tail.wall_start)},
            {"started_ms", tail.wall_start}, // wall clock ms, compare with last_failure.time_ms
            {"lines_total", tail.n_lines},
        }) : json(nullptr)},
        {"reason",       reason},
        {"lines",        lines},
        {"truncated",    tail.truncated},
        {"last_failure", failure},
    });
}

//
// GET /admin/diagnostics: what this server detected about itself, in plain terms
//

static bool admin_in_container() {
#if defined(_WIN32)
    return false;
#else
    std::error_code ec;
    if (fs::exists("/.dockerenv", ec) || fs::exists("/run/.containerenv", ec)) {
        return true;
    }
    std::string cgroup;
    if (admin_read_file("/proc/1/cgroup", cgroup, 256 * 1024)) {
        for (const char * w : { "docker", "containerd", "kubepods", "libpod", "lxc" }) {
            if (cgroup.find(w) != std::string::npos) {
                return true;
            }
        }
    }
    return false;
#endif
}

// /proc/self/mountinfo writes a blank, tab, newline or backslash in a path as \ooo (octal)
static std::string admin_mountinfo_unescape(const std::string & s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 3 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '3' && s[i + 2] >= '0' && s[i + 2] <= '7' &&
            s[i + 3] >= '0' && s[i + 3] <= '7') {
            out += (char) ((s[i + 1] - '0') * 64 + (s[i + 2] - '0') * 8 + (s[i + 3] - '0'));
            i += 3;
        } else {
            out += s[i];
        }
    }
    return out;
}

// In a container: is the directory on a volume or bind mount (kept when the container is recreated)?
// The mount that holds it is the longest mount point that is a prefix of it (on a path component
// boundary, the last one listed when several are stacked); the container's own root ("/", the image
// overlay) and a tmpfs are not kept. From /proc/self/mountinfo.
#if defined(__linux__)
static bool admin_mountinfo_on_volume(const std::string & info, const std::string & target) {
    std::string best;
    std::string best_type;
    for (const auto & line : admin_split_lines(info)) {
        // id parent major:minor root mount-point options [optional fields...] - fstype source super-options
        const std::vector<std::string> f = string_split<std::string>(line, ' ');
        if (f.size() < 5) {
            continue;
        }
        const std::string mp = admin_mountinfo_unescape(f[4]);
        const bool holds = mp == "/" || target == mp ||
                           (target.size() > mp.size() && target.compare(0, mp.size(), mp) == 0 && target[mp.size()] == '/');
        if (!holds || mp.size() < best.size()) {
            continue;
        }
        std::string type;
        for (size_t i = 5; i + 1 < f.size(); i++) {
            if (f[i] == "-") {
                type = f[i + 1];
                break;
            }
        }
        best      = mp;
        best_type = type;
    }
    return !best.empty() && best != "/" && best_type != "tmpfs" && best_type != "ramfs";
}
#endif

static bool admin_on_volume(const fs::path & dir) {
#if defined(__linux__)
    std::string info;
    if (!admin_read_file("/proc/self/mountinfo", info, 4 * 1024 * 1024)) {
        return false;
    }
    std::error_code ec;
    return admin_mountinfo_on_volume(info, admin_path_to_utf8(fs::weakly_canonical(dir, ec)));
#else
    (void) dir;
    return false;
#endif
}

// The version line of a vendor tool (diagnostics), bounded like a probe: a known version is kept, an
// unknown one is asked again after a minute; never while the tool's probe hangs or is stuck, and at
// most one version run per tool at a time (concurrent reports wait for the same one).
static constexpr int64_t ADMIN_TOOL_VERSION_RETRY_MS = 60000;

static std::string admin_tool_version(admin_context & ctx, const admin_tool_state & t) {
    if (t.canned) {
        return "canned output (" + t.path + ")";
    }
    std::shared_ptr<admin_job> job;
    {
        std::lock_guard<std::mutex> lk(ctx.mtx_gpu);
        const auto it = ctx.tool_versions.find(t.path);
        if (it != ctx.tool_versions.end() && (it->second.ok || admin_now_ms() - it->second.t_ms < ADMIN_TOOL_VERSION_RETRY_MS)) {
            return it->second.text;
        }
        if (t.error == "probe_stuck" || t.error == "timeout") {
            return "unknown (" + t.tool + " is not answering)"; // a second process would hang the same way
        }
        const auto jt = ctx.tool_version_jobs.find(t.path);
        if (jt != ctx.tool_version_jobs.end()) {
            bool recent = false;
            {
                std::lock_guard<std::mutex> lk_job(jt->second->mtx);
                recent = jt->second->done && admin_now_ms() - jt->second->t_done < ADMIN_TOOL_VERSION_RETRY_MS;
            }
            if (jt->second->alive.load() || recent) {
                job = jt->second; // running, stuck after its deadline, or answered after its caller gave up: use it
            }
        }
        if (!job) {
            std::vector<std::string> cmd = { t.path };
            if (t.tool == "nvidia-smi") {
                cmd.push_back("--query-gpu=driver_version");
                cmd.push_back("--format=csv,noheader");
            } else if (t.tool == "amd-smi") {
                cmd.push_back("version");
            } else {
                cmd.push_back("--showdriverversion");
            }
            job = admin_job_start("tool version", { cmd }, ctx.env_snapshot, ADMIN_PROBE_TIMEOUT_MS);
            ctx.tool_version_jobs[t.path] = job;
        }
    }
    if (!admin_job_wait(job, ADMIN_PROBE_WAIT_MS)) {
        return "unknown (no answer yet)"; // not remembered: the run goes on, a later report reads its answer
    }
    std::string version;
    {
        std::lock_guard<std::mutex> lk(job->mtx);
        if (job->spawned && !job->timed_out && job->exit_code == 0) {
            for (const auto & line : admin_output_tail(job->output, 20)) {
                if (!line.empty()) {
                    version = (t.tool == "nvidia-smi" ? "driver " : "") + line;
                    break;
                }
            }
        }
    }
    std::lock_guard<std::mutex> lk(ctx.mtx_gpu);
    admin_context::tool_version & v = ctx.tool_versions[t.path];
    v.ok   = !version.empty();
    v.text = v.ok ? version : "unknown";
    v.t_ms = admin_now_ms();
    return v.text;
}

static json admin_diag_item(const std::string & id, const std::string & label, const std::string & status, const json & value,
                            const std::string & hint) {
    return { {"id", id}, {"label", label}, {"status", status}, {"value", value}, {"hint", hint} };
}

static server_http_res_ptr admin_handle_diagnostics(admin_context & ctx, const server_http_req & req) {
    const bool container = admin_in_container();
    json items = json::array();

    items.push_back(admin_diag_item("deployment", "Deployment", "ok", container ? "container" : "host",
        container ? "running in a container (Docker / Podman / Kubernetes)" : "running directly on the host"));
    items.push_back(admin_diag_item("binary", "Server binary", "ok",
        json({ {"version", std::string(llama_build_info())}, {"path", ctx.bin_path} }), ""));

    const json backend = admin_gpu_backend_json(ctx);
    const std::string backend_state = admin_json_str(backend, "state");
    items.push_back(admin_diag_item("gpu_backend", "GPU backend", backend_state == "no_devices" ? "error" : "ok", backend,
        backend_state == "no_devices"
            ? std::string(container ? "the container may have been started without GPUs (docker: --gpus / device_ids); " : "") +
              "check the GPU driver and permissions (/dev/nvidia*, /dev/kfd, /dev/dri)"
            : admin_json_str(backend, "message")));

    const admin_inventory inv = admin_get_inventory(ctx);
    items.push_back(admin_diag_item("inventory", "Device inventory", inv.ok ? "ok" : (inv.error == "disabled" ? "warning" : "error"),
        admin_inventory_json(inv),
        inv.ok ? string_format("%zu device(s), measured %s; Re-detect GPUs measures again", inv.devices.size(),
                               inv.time_ms > 0 ? admin_iso_time(inv.time_ms).c_str() : "never")
               : inv.message));

    // vendor tools: run a probe now (bounded) so the report is current; on a Linux host with the
    // per-process query too, which the router-without-GPU self-check below needs
#if defined(__linux__)
    admin_probe_refresh(ctx, false, !container);
#else
    admin_probe_refresh(ctx, false, false);
#endif
    std::vector<admin_tool_state> tools;
    std::string probe_mode, probe_error, probe_message;
    {
        std::lock_guard<std::mutex> lk(ctx.mtx_gpu);
        tools         = ctx.tools;
        probe_mode    = ctx.probe_mode;
        probe_error   = ctx.probe_error;
        probe_message = ctx.probe_message;
    }
    {
        json value = json::array();
        std::string status = "ok";
        std::string hint;
        for (const auto & t : tools) {
            value.push_back({ {"tool", t.canned ? std::string("file") : t.tool}, {"path", t.path}, {"version", admin_tool_version(ctx, t)},
                              {"ok", t.error.empty() && t.has_sample}, {"error", t.error}, {"message", t.message} });
            if (!t.error.empty()) {
                status = "warning";
                hint   = t.message;
            }
        }
        if (!probe_error.empty()) {
            status = probe_error == "unsupported" ? "ok" : "warning";
            hint   = probe_message;
            bool nvidia = false;
            for (const auto & d : inv.devices) {
                nvidia = nvidia || d.vendor == "nvidia";
            }
            if (probe_error == "not_found" && container && nvidia) {
                hint += "; nvidia-smi is not available inside the container: add NVIDIA_DRIVER_CAPABILITIES=compute,utility";
            }
        } else if (!probe_message.empty() && hint.empty()) {
            hint = probe_message;
        }
        items.push_back(admin_diag_item("vendor_tool", "GPU memory tool", status,
            json({ {"mode", probe_mode}, {"setting", ctx.gpu_probe}, {"tools", value}, {"error", probe_error} }), hint));
    }

    // mapping of tool rows to devices and the router's own GPU use
    {
        const json gpus = admin_gpus_body(ctx, false);
        const std::string mapping = gpus.at("probe").value("mapping", std::string("none"));
        items.push_back(admin_diag_item("memory_mapping", "GPU memory matched to devices",
            mapping == "pci" || mapping == "single" || mapping == "none" ? "ok" : "warning", mapping,
            mapping == "pci" ? "matched by PCI bus id" : (mapping == "single" ? "one device and one tool row" :
            (mapping == "none" ? "no GPU devices" : "some GPUs could not be matched: their memory is shown as measured by the device inventory"))));

        std::string self_status = "info";
        std::string self_value  = "not checked";
        std::string self_hint   = "the router never queries a GPU device itself (names only); a check needs nvidia-smi on the host";
#if defined(__linux__)
        if (!container) {
            for (const auto & t : tools) {
                if (t.tool != "nvidia-smi" || t.canned || !t.procs_ok) {
                    continue;
                }
                bool found = false;
                for (const auto & p : t.procs) {
                    found = found || p.pid == (long long) getpid();
                }
                self_status = found ? "error" : "ok";
                self_value  = found ? "the router process uses a GPU" : "the router process uses no GPU";
                self_hint   = found ? "unexpected: please report it" : "checked with nvidia-smi --query-compute-apps";
            }
        }
#endif
        items.push_back(admin_diag_item("router_gpu_free", "Router without GPU context", self_status, self_value, self_hint));
    }

    {
        const bool writable = admin_dir_writable(ctx.state_dir);
        const std::string p = admin_path_to_utf8(ctx.state_dir);
        const bool tmp = string_starts_with(p, "/tmp") || string_starts_with(p, "/var/tmp");
        const bool persistent = container ? admin_on_volume(ctx.state_dir) : !tmp;
        items.push_back(admin_diag_item("state_dir", "Admin state directory", !writable ? "error" : (persistent ? "ok" : "warning"),
            json({ {"path", p}, {"writable", writable}, {"looks_persistent", persistent} }),
            !writable ? "set PROVISION_ADMIN_STATE_DIR to a writable directory" :
            (persistent ? "" : (container ? "not a volume: the settings are lost when the container is recreated"
                                          : "a temporary directory: the settings may be lost on reboot"))));
    }
    items.push_back(admin_diag_item("admin_key", "Admin key", ctx.key.empty() ? "error" : "ok", ctx.key_source,
        ctx.key.empty() ? "no admin key: set PROVISION_ADMIN_KEY or make the state directory writable" : ""));
    items.push_back(admin_diag_item("proxy", "Requests through a reverse proxy", ctx.proxy_seen.load() ? "ok" : "info",
        ctx.proxy_seen.load(), ctx.proxy_seen.load() ? "X-Forwarded-For seen" :
        "no X-Forwarded-For seen: requests reach the server directly; lock /admin in the reverse proxy or with "
        "PROVISION_ADMIN_ALLOW_FROM for remote use"));
    {
        const admin_access & acc = ctx.access;
        const admin_client client = admin_client_of(acc, req);
        std::string hint = !acc.enabled
            ? "off: any address can reach /admin (the admin key still applies); restrict /admin in the reverse proxy "
              "or set PROVISION_ADMIN_ALLOW_FROM"
            : (acc.trusted.empty() ? "only these addresses reach /admin; X-Forwarded-For is ignored (no trusted proxies)"
                                   : "only these addresses reach /admin; X-Forwarded-For is used only from the trusted proxies");
        if (!acc.warnings.empty()) {
            hint = admin_join(acc.warnings, "; ");
        }
        items.push_back(admin_diag_item("ip_allowlist", "Admin address allow-list",
            !acc.warnings.empty() ? "warning" : (acc.enabled ? "ok" : "info"),
            json({
                {"enabled",           acc.enabled},
                {"entries",           admin_range_texts(acc.allow)},
                {"trusted_proxies",   admin_range_texts(acc.trusted)},
                {"seen_from",         client.address},
                {"peer",              client.peer},
                {"via_forwarded_for", client.via_forwarded},
                {"note",              client.note},
            }), hint));
    }
    items.push_back(admin_diag_item("model_devices", "MODEL_DEVICES", "ok",
        ctx.model_devices.present ? json(ctx.model_devices.raw) : json(nullptr),
        ctx.model_devices.present ? "devices set here are locked in the panel" : "not set"));
    {
        std::lock_guard<std::mutex> lk(ctx.mtx_state);
        const admin_models_file & f = ctx.models_file;
        items.push_back(admin_diag_item("model_settings", "Per-model settings file", f.read_only ? "error" : (f.problems.empty() ? "ok" : "warning"),
            json({ {"file", admin_path_to_utf8(admin_models_path(ctx))}, {"entries", f.entries.size()}, {"read_only", f.read_only},
                   {"backup_loaded", f.backup_loaded}, {"error", f.error}, {"problems", f.problems} }),
            f.read_only ? "fix or remove the file, then restart the server" : ""));
    }
    // only while a helper hangs: the fit check and Re-detect GPUs answer 409 check_stuck / rescan_stuck until it exits
    {
        std::shared_ptr<admin_job> check_job;
        std::shared_ptr<admin_job> inv_job;
        {
            std::lock_guard<std::mutex> lk(ctx.mtx_check);
            check_job = ctx.check_job;
        }
        {
            std::lock_guard<std::mutex> lk(ctx.mtx_inv);
            inv_job = ctx.inv_job;
        }
        json stuck = json::array();
        if (check_job && admin_job_is_stuck(check_job)) {
            stuck.push_back(admin_stuck_message("fit check", check_job));
        }
        if (inv_job && admin_job_is_stuck(inv_job)) {
            stuck.push_back(admin_stuck_message("device inventory", inv_job));
        }
        if (!stuck.empty()) {
            items.push_back(admin_diag_item("stuck_helpers", "Helper processes", "error", stuck,
                "a helper process did not exit after it was stopped; the GPU driver may be hanging (check dmesg), restart the server if it does not clear"));
        }
    }
    return admin_res_json(200, {
        {"generated_at", admin_iso_time(admin_wall_ms())},
        {"items",        items},
    });
}

//
// GET / POST /admin/models/settings
//

struct admin_rows_inputs {
    std::map<std::string, admin_model_entry> entries;
    std::map<std::string, json>              kept_entries;
    bool        read_only     = false;
    bool        backup_loaded = false;
    std::string file_error;
    std::vector<std::string> problems;
    std::map<std::string, admin_seen_model> seen;
    admin_kv    cli;
    std::vector<std::string> devices;
    admin_inventory inv;
    std::map<std::string, json> failures;
    std::string fill_device;
    bool        default_device_missing = false;
};

static admin_rows_inputs admin_rows_gather(admin_context & ctx) {
    admin_rows_inputs in;
    {
        std::lock_guard<std::mutex> lk(ctx.mtx_state);
        in.entries       = ctx.models_file.entries;
        in.kept_entries  = ctx.models_file.kept_entries;
        in.read_only     = ctx.models_file.read_only;
        in.backup_loaded = ctx.models_file.backup_loaded;
        in.file_error    = ctx.models_file.error;
        in.problems      = ctx.models_file.problems;
        in.fill_device   = admin_overlay_device(ctx);
    }
    {
        std::lock_guard<std::mutex> lk(ctx.mtx_seen);
        in.seen = ctx.seen;
        in.cli  = ctx.cli_locks;
    }
    in.devices = admin_build_devices(ctx);
    in.inv     = admin_get_inventory(ctx);
    {
        std::lock_guard<std::mutex> lk(ctx.mtx_logs);
        in.failures = ctx.failures;
    }
    in.default_device_missing = admin_default_device_missing(in.fill_device, in.devices);
    return in;
}

static admin_eval_env admin_env_of(const admin_context & ctx, const admin_rows_inputs & in) {
    admin_eval_env env;
    env.cli         = in.cli;
    env.devices     = in.devices;
    env.md          = &ctx.model_devices;
    env.fill_device = in.fill_device;
    return env;
}

static json admin_state_json(const admin_context & ctx, const admin_rows_inputs & in) {
    return {
        {"file",          admin_path_to_utf8(admin_models_path(ctx))},
        {"error",         in.file_error},
        {"read_only",     in.read_only},
        {"backup_loaded", in.backup_loaded},
        {"problems",      in.problems},
    };
}

static json admin_schema_json() {
    json out = json::array();
    for (const auto & spec : ADMIN_KEYS) {
        json j = {
            {"key",     spec.key},
            {"group",   spec.group},
            {"default", spec.def != nullptr ? json(spec.def) : json(nullptr)},
        };
        switch (spec.kind) {
            case ADMIN_KIND_DEVICE:   j["type"] = "device"; break;
            case ADMIN_KIND_INT:      j["type"] = "int"; j["min"] = spec.min; j["max"] = spec.max; break;
            case ADMIN_KIND_PARALLEL: j["type"] = "int"; j["min"] = spec.min; j["max"] = spec.max; j["auto"] = -1; break;
            case ADMIN_KIND_NGL:      j["type"] = "ngl"; j["min"] = spec.min; j["max"] = spec.max; j["values"] = json::array({ "auto", "all" }); break;
            case ADMIN_KIND_ENUM:     j["type"] = "enum"; j["values"] = string_split<std::string>(spec.values, '|'); break;
            case ADMIN_KIND_BOOL:     j["type"] = "bool"; break;
        }
        if (std::string(spec.key) == "cache-type-k" || std::string(spec.key) == "cache-type-v") {
            j["pair"] = std::string(spec.key) == "cache-type-k" ? "cache-type-v" : "cache-type-k"; // must be equal
        }
        out.push_back(j);
    }
    return out;
}

static long long admin_pad256(long long n) {
    return (n + 255) / 256 * 256;
}

// what the context settings mean for one request, from the values the child gets
static json admin_context_json(const admin_kv & values, long long n_ctx_train) {
    long long v = 0;
    long long slots = 4;
    const bool auto_slots = !admin_parse_int(admin_value_or_default(values, "parallel"), v) || v <= 0;
    if (!auto_slots) {
        slots = v;
    }
    const bool unified = auto_slots || (values.count(ADMIN_KV_UNIFIED_KEY) && admin_is_truthy_text(values.at(ADMIN_KV_UNIFIED_KEY)));
    long long pool = 0;
    if (values.count("ctx-size") && admin_parse_int(values.at("ctx-size"), v) && v > 0) {
        pool = admin_pad256(v);
    }
    long long per_slot_cap = 0;
    if (values.count("kv-unified-per-slot") && admin_parse_int(values.at("kv-unified-per-slot"), v) && v > 0) {
        per_slot_cap = v;
    }
    json out = {
        {"slots",        slots},
        {"unified",      unified},
        {"pool",         pool > 0 ? json(pool) : json(nullptr)},
        {"per_slot_cap", per_slot_cap > 0 ? json(per_slot_cap) : json(nullptr)},
        {"n_ctx_train",  n_ctx_train > 0 ? json(n_ctx_train) : json(nullptr)},
    };
    if (pool > 0) {
        long long seq = unified ? pool : admin_pad256(pool / slots);
        long long max_req = seq;
        if (per_slot_cap > 0) {
            max_req = std::min(max_req, per_slot_cap);
        }
        if (n_ctx_train > 0) {
            max_req = std::min(max_req, n_ctx_train);
        }
        out["max_per_request"]      = max_req;
        out["per_request_all_busy"] = unified ? pool / slots : seq;
    } else {
        out["max_per_request"]      = nullptr;
        out["per_request_all_busy"] = nullptr;
    }
    return out;
}

// warnings about values that work but probably do not do what was meant
static void admin_value_warnings(const admin_kv & values, long long n_ctx_train, std::vector<std::string> & warnings) {
    auto warn = [&](const char * code) {
        if (std::find(warnings.begin(), warnings.end(), code) == warnings.end()) {
            warnings.push_back(code);
        }
    };
    const bool embeddings = admin_is_truthy_text(admin_value_or_default(values, "embeddings")) ||
                            admin_is_truthy_text(admin_value_or_default(values, "reranking"));
    if (values.count("pooling") && !embeddings) {
        warn("pooling_without_embeddings");
    }
    const json c = admin_context_json(values, n_ctx_train);
    long long v = 0;
    const long long slots = c.at("slots").get<long long>();
    if (c.at("pool").is_number()) {
        const long long pool = c.at("pool").get<long long>();
        if (n_ctx_train > 0 && pool > n_ctx_train * slots) {
            warn("ctx_above_train");
        }
        if (admin_parse_int(admin_value_or_default(values, "ubatch-size"), v) && v > pool) {
            warn("ubatch_above_ctx");
        }
        if (c.at("unified").get<bool>() && slots > 1 && n_ctx_train > 0 && pool / slots < std::min<long long>(n_ctx_train, 4096)) {
            warn("pool_overcommit");
        }
        if (c.at("per_slot_cap").is_number()) {
            const long long cap = c.at("per_slot_cap").get<long long>();
            const long long seq = c.at("unified").get<bool>() ? pool : admin_pad256(pool / slots);
            if (cap > seq) {
                warn("kv_per_slot_no_effect");
            }
        }
    }
}

static json admin_unattached_json(const std::string & id, const admin_model_entry & e, const std::string & kind) {
    return {
        {"id",        id},
        {"kind",      kind},
        {"overrides", admin_kv_to_json(e.settings)},
        {"phase",     e.phase},
        {"failures",  e.failures},
        {"updated",   e.updated.empty() ? json(nullptr) : json(e.updated)},
        {"reverted",  e.reverted.is_object() ? e.reverted : json(nullptr)},
        {"exists",    false},
    };
}

static json admin_model_row(admin_context & ctx, const server_model_meta & m, const admin_rows_inputs & in, bool compute_facts) {
    const auto e_it = in.entries.find(m.name);
    const admin_model_entry * e = e_it != in.entries.end() ? &e_it->second : nullptr;
    const auto s_it = in.seen.find(m.name);
    const admin_seen_model * rec = s_it != in.seen.end() ? &s_it->second : nullptr;
    const admin_eval_env env = admin_env_of(ctx, in);
    admin_eval ev;
    if (rec != nullptr) {
        ev = admin_evaluate(m.name, rec->source, rec->inherited, e != nullptr ? &e->settings : nullptr, env);
    }
    const admin_kv effective = admin_effective_values(m);

    std::vector<std::string> warnings;
    auto warn = [&](const std::string & code) {
        if (std::find(warnings.begin(), warnings.end(), code) == warnings.end()) {
            warnings.push_back(code);
        }
    };

    json fields = json::object();
    std::vector<std::string> apply_errors;
    for (const auto & spec : ADMIN_KEYS) {
        const std::string key = spec.key;
        json f;
        const auto eff = effective.find(key);
        f["effective"] = eff != effective.end() ? json(eff->second) : json(nullptr);
        f["default"]   = spec.def != nullptr ? json(spec.def) : json(nullptr);
        const bool inherited = rec != nullptr && rec->inherited.count(key);
        if (inherited) {
            f["inherited"]        = rec->inherited.at(key);
            f["inherited_origin"] = "models_ini";
        } else if (key == "device" && !ev.default_device.empty()) {
            f["inherited"]        = ev.default_device;
            f["inherited_origin"] = "default_device";
        } else {
            f["inherited"]        = nullptr;
            f["inherited_origin"] = "default";
        }
        json locked = nullptr;
        if (in.cli.count(key)) {
            locked = { {"by", "router_cli"}, {"value", in.cli.at(key)} };
        } else if (key == "device" && ev.model_devices_lock) {
            locked = { {"by", "model_devices"}, {"value", ev.model_devices_value} };
        }
        f["locked"] = locked;
        const bool has_override = e != nullptr && e->settings.count(key);
        const auto inactive = ev.inactive.find(key);
        f["override"]        = has_override ? json(e->settings.at(key)) : json(nullptr);
        f["active"]          = has_override && inactive == ev.inactive.end();
        f["inactive_reason"] = has_override && inactive != ev.inactive.end() ? json(inactive->second) : json(nullptr);
        if (has_override && inactive != ev.inactive.end() && inactive->second != "router_cli" && inactive->second != "model_devices") {
            apply_errors.push_back(key + ": " + inactive->second);
        }
        std::string origin = "default";
        if (!locked.is_null()) {
            origin = locked.at("by").get<std::string>();
        } else if (has_override && inactive == ev.inactive.end()) {
            origin = "admin";
        } else if (key == "device" && !ev.default_device.empty()) {
            origin = "default_device";
        } else if (inherited) {
            origin = "models_ini";
        }
        f["origin"] = origin;
        fields[key] = f;
    }
    if (!ev.model_devices_value.empty() && !ev.model_devices_lock) {
        warn("model_devices_not_applied");
    }

    // device identity: the PCI id the saved device had, against the current inventory
    json device_moved   = nullptr;
    json device_missing = nullptr;
    if (e != nullptr && e->settings.count("device")) {
        const auto inactive = ev.inactive.find("device");
        std::vector<std::string> missing;
        for (const auto & name : admin_device_list(e->settings.at("device"))) {
            if (name == "none") {
                continue;
            }
            bool found = false;
            for (const auto & d : in.devices) {
                found = found || admin_iequals(d, name);
            }
            if (!found) {
                missing.push_back(name);
                continue;
            }
            const auto saved = e->device_pci.find(name);
            const std::string current = admin_device_pci(in.inv, name);
            if (saved != e->device_pci.end() && !saved->second.empty() && !current.empty() && saved->second != current) {
                std::string now_named;
                for (const auto & d : in.inv.devices) {
                    if (d.pci == saved->second) {
                        now_named = d.name;
                    }
                }
                device_moved = {
                    {"device",      name},
                    {"saved_pci",   saved->second},
                    {"current_pci", current},
                    {"now_named",   now_named.empty() ? json(nullptr) : json(now_named)},
                };
                warn("device_moved");
            }
        }
        if (inactive != ev.inactive.end() && inactive->second == "device_missing") {
            device_missing = {
                {"devices", missing},
                {"message", "the saved device is not on this server: the model uses its inherited device"},
            };
            warn("device_missing");
        }
    }
    if (in.default_device_missing && m.source != SERVER_MODEL_SOURCE_PRESET && (rec == nullptr || !rec->inherited.count("device"))) {
        warn("default_device_not_present");
    }

    // facts (GGUF metadata) and the context summary
    json facts = nullptr;
    std::string facts_status = "unsupported";
    if (rec != nullptr) {
        const std::string path = admin_model_file(*rec);
        facts_status = compute_facts ? admin_facts_get(ctx, path, rec->mmproj, facts) : admin_facts_lookup(ctx, path, facts);
        if (facts_status == "pending" && !compute_facts) {
            admin_facts_enqueue(ctx, path, rec->mmproj);
        }
    }
    const long long n_ctx_train = facts.is_object() ? admin_json_int(facts, "n_ctx_train", 0) : 0;
    admin_value_warnings(effective, n_ctx_train, warnings);
    if (!apply_errors.empty()) {
        warn("apply_error");
    }

    json kept = json::object();
    if (e != nullptr) {
        for (const auto & [key, value] : e->kept) {
            kept[key] = value;
        }
    }
    const auto failure = in.failures.find(m.name);
    json row = {
        {"id",                  m.name},
        {"source",              server_model_source_to_string(m.source)},
        {"status",              server_model_status_to_string(m.status)},
        {"exists",              true},
        {"hidden",              m.hidden},
        {"overrides",           e != nullptr ? admin_kv_to_json(e->settings) : json::object()},
        {"kept",                kept},
        {"fields",              fields},
        {"apply_error",         admin_join(apply_errors, "; ")},
        {"phase",               e != nullptr ? json(e->phase) : json(nullptr)},
        {"failures",            e != nullptr ? e->failures : 0},
        {"updated",             e != nullptr && !e->updated.empty() ? json(e->updated) : json(nullptr)},
        {"last_good",           e != nullptr && e->last_good.has_value() ? admin_kv_to_json(*e->last_good) : json(nullptr)},
        {"reverted",            e != nullptr && e->reverted.is_object() ? e->reverted : json(nullptr)},
        {"reverted_at_startup", e != nullptr && e->reverted_at_startup},
        {"last_failure",        failure != in.failures.end() ? failure->second : json(nullptr)},
        {"last_run", {
            {"status",    server_model_status_to_string(m.status)},
            {"port",      m.port},
            {"running",   m.is_running()},
            {"failed",    m.is_failed()},
            {"exit_code", m.exit_code},
            {"reason",    failure != in.failures.end() ? failure->second.value("reason", json(nullptr)) : json(nullptr)},
        }},
        {"device_moved",        device_moved},
        {"device_missing",      device_missing},
        {"context",             admin_context_json(effective, n_ctx_train)},
        {"facts_status",        facts_status},
        {"facts",               facts},
        {"warnings",            warnings},
    };
    return row;
}

static server_http_res_ptr admin_handle_get_model_settings(admin_context & ctx, const server_http_req & req) {
    const std::string id = req.get_param("model");
    const admin_live live = admin_refresh(ctx, false);
    const admin_rows_inputs in = admin_rows_gather(ctx);
    if (!id.empty()) {
        std::optional<server_model_meta> meta;
        for (const auto & m : live.models) {
            if (m.name == id) {
                meta = m;
            }
        }
        if (!meta.has_value()) {
            meta = ctx.routes->models.get_meta(id); // a hidden model or an alias
            if (meta.has_value() && (meta->status == SERVER_MODEL_STATUS_DOWNLOADING || meta->status == SERVER_MODEL_STATUS_DOWNLOADED)) {
                meta.reset();
            }
        }
        if (meta.has_value()) {
            json row = admin_model_row(ctx, *meta, in, true);
            row["state"] = admin_state_json(ctx, in);
            return admin_res_json(200, row);
        }
        const auto e = in.entries.find(id);
        if (e != in.entries.end()) {
            json row = admin_unattached_json(id, e->second, "orphaned");
            row["state"] = admin_state_json(ctx, in);
            return admin_res_json(200, row);
        }
        throw admin_http_error(404, "not_found_error", "model '" + id + "' is not found");
    }

    admin_facts_warm(ctx, live.models, live.pinned.applied);
    json rows = json::array();
    std::set<std::string> listed;
    for (const auto & m : live.models) {
        rows.push_back(admin_model_row(ctx, m, in, false));
        listed.insert(m.name);
    }
    json unattached = json::array();
    for (const auto & [eid, e] : in.entries) {
        if (listed.count(eid)) {
            continue;
        }
        const std::optional<server_model_meta> meta = ctx.routes->models.get_meta(eid);
        unattached.push_back(admin_unattached_json(eid, e, meta.has_value() && meta->hidden ? "hidden" : "orphaned"));
    }
    for (const auto & [eid, raw] : in.kept_entries) {
        unattached.push_back({ {"id", eid}, {"kind", "unreadable"}, {"overrides", json::object()}, {"raw", raw}, {"exists", false} });
    }
    json warnings = json::array();
    if (in.default_device_missing) {
        warnings.push_back("default_device_not_present");
    }
    for (const auto & [section, dev] : ctx.model_devices.sections) {
        bool is_section = false;
        const auto rec = in.seen.find(section);
        is_section = rec != in.seen.end() && rec->second.source == SERVER_MODEL_SOURCE_PRESET;
        if (!is_section) {
            warnings.push_back("model_devices_unknown_section:" + section);
        }
    }
    return admin_res_json(200, {
        {"schema",        admin_schema_json()},
        {"build_devices", in.devices},
        {"models",        rows},
        {"unattached",    unattached},
        {"state",         admin_state_json(ctx, in)},
        {"warnings",      warnings},
    });
}

static bool admin_body_bool(const json & body, const char * key) {
    if (!body.contains(key) || body.at(key).is_null()) {
        return false;
    }
    if (!body.at(key).is_boolean()) {
        throw admin_http_error(400, "invalid_request_error", std::string(key) + " must be true or false");
    }
    return body.at(key).get<bool>();
}

// a 400 that names the offending field
static admin_http_error admin_bad_param(const std::string & param, const std::string & message) {
    admin_http_error e(400, "invalid_request_error", message);
    e.param = param;
    return e;
}

// the stored settings after applying `settings` ({key: value | null}) to `current`; throws on invalid input
// locked: 409 field_locked for a value on a locked key (null is allowed: it removes an old override);
// check_only: a locked value is ignored and reported in `ignored` instead
static admin_kv admin_merge_settings(const admin_context & ctx, const admin_kv & current, const json & settings,
                                     const admin_rows_inputs & in, const admin_eval & ev_current, bool check_only,
                                     std::vector<std::string> & touched, std::vector<std::string> & ignored) {
    admin_kv next = current;
    if (settings.is_null()) {
        return next;
    }
    if (!settings.is_object()) {
        throw admin_bad_param("settings", "settings must be an object of {key: value | null}");
    }
    for (const auto & [key, value] : settings.items()) {
        const admin_key_spec * spec = admin_key_find(key);
        if (spec == nullptr) {
            throw admin_bad_param(key, "unknown setting '" + key + "'" + (key == "cache-type" ? " (use cache-type-k and cache-type-v)" : ""));
        }
        touched.push_back(key);
        if (value.is_null()) {
            next.erase(key);
            continue;
        }
        std::string canon;
        std::string err;
        if (!admin_canon_value(*spec, value, canon, err, &in.devices)) {
            throw admin_bad_param(key, err);
        }
        std::string lock;
        if (in.cli.count(key)) {
            lock = key + " is set on the server command line (" + in.cli.at(key) + ") and cannot be changed here";
        } else if (key == "device" && ev_current.model_devices_lock) {
            lock = "device is set by MODEL_DEVICES (" + ev_current.model_devices_value + ") - change it there";
        }
        if (!lock.empty()) {
            if (check_only) {
                ignored.push_back(key);
                continue;
            }
            admin_http_error e(409, "field_locked", lock);
            e.param = key;
            throw e;
        }
        next[key] = canon;
    }
    (void) ctx;
    return next;
}

// cross-field rules on the values the child would get (only for the keys a request touches)
static void admin_check_cross_fields(const admin_kv & child, const std::vector<std::string> & touched, bool reset) {
    auto touches = [&](const char * key) {
        return reset || std::find(touched.begin(), touched.end(), key) != touched.end();
    };
    const std::string tk = admin_value_or_default(child, "cache-type-k");
    const std::string tv = admin_value_or_default(child, "cache-type-v");
    if ((touches("cache-type-k") || touches("cache-type-v")) && !admin_values_equal("cache-type-k", tk, tv)) {
        throw admin_bad_param(touches("cache-type-v") ? "cache-type-v" : "cache-type-k",
                              "cache-type-k and cache-type-v must be equal (" + tk + " / " + tv + ")");
    }
    const bool quantized_v = tv == "q8_0" || tv == "q4_0";
    if ((touches("cache-type-v") || touches("flash-attn")) && quantized_v && admin_value_or_default(child, "flash-attn") == "off") {
        throw admin_bad_param("flash-attn", "a quantized V cache (" + tv + ") needs flash attention on or auto");
    }
}

static json admin_previous_json(const admin_kv & before, const admin_kv & after) {
    json out = json::object();
    for (const auto & [key, value] : before) {
        out[key] = value;
    }
    for (const auto & [key, value] : after) {
        if (!before.count(key)) {
            out[key] = nullptr;
        }
    }
    return out;
}

static server_http_res_ptr admin_handle_post_model_settings(admin_context & ctx, const server_http_req & req) {
    server_models & models = ctx.routes->models;
    const json body = admin_parse_body_object(req);
    if (!body.contains("model") || !body.at("model").is_string() || body.at("model").get<std::string>().empty()) {
        throw admin_bad_param("model", "model must be a model id");
    }
    const std::string requested = body.at("model").get<std::string>();
    const bool reset   = admin_body_bool(body, "reset");
    const bool restart = admin_body_bool(body, "restart");
    const json settings = body.contains("settings") ? body.at("settings") : json(nullptr);
    if (!reset && !settings.is_object()) {
        throw admin_bad_param("settings", "settings must be an object of {key: value | null}");
    }

    // one change at a time: validate -> stop -> persist -> apply
    std::lock_guard<std::mutex> lk_apply(ctx.mtx_apply);

    admin_rows_inputs in = admin_rows_gather(ctx);
    if (in.read_only) {
        throw admin_http_error(409, "model_settings_read_only",
            "the per-model settings cannot be changed: " + in.file_error + " - fix or remove the file and restart the server");
    }
    std::optional<server_model_meta> meta = models.get_meta(requested);
    if (meta.has_value() && (meta->status == SERVER_MODEL_STATUS_DOWNLOADING || meta->status == SERVER_MODEL_STATUS_DOWNLOADED)) {
        meta.reset(); // a download in progress, not a model
    }
    const std::string id = meta.has_value() ? meta->name : requested;
    const auto e_it = in.entries.find(id);
    const bool had_entry = e_it != in.entries.end();
    const admin_kv old_settings = had_entry ? e_it->second.settings : admin_kv();

    json result = {
        {"changed", false}, {"reloaded", false}, {"restarted", false}, {"was_running", false},
        {"restarted_also", json::array()}, {"not_restarted", json::array()}, {"load_error", ""}, {"reload_error", ""},
    };

    if (!meta.has_value()) {
        if (reset && had_entry) {
            // an entry whose model is gone: just remove it
            std::string error;
            if (!admin_models_update(ctx, [&](admin_models_file & f) { return f.entries.erase(id) > 0; }, true, error)) {
                throw admin_http_error(500, "server_error", "the settings could not be saved: " + error);
            }
            SRV_INF("admin: removed the saved settings of %s (not in the model list)\n", id.c_str());
            result["changed"] = true;
            return admin_res_json(200, {
                {"model",    json({ {"id", id}, {"exists", false}, {"overrides", json::object()} })},
                {"result",   result},
                {"previous", admin_previous_json(old_settings, admin_kv())},
                {"warnings", json::array()},
            });
        }
        throw admin_http_error(404, "not_found_error", "model '" + requested + "' is not found");
    }
    if (!had_entry && (!admin_is_model_id(id))) {
        throw admin_bad_param("model", "the model id is not printable ASCII of at most 256 bytes");
    }

    const auto rec_it = in.seen.find(id);
    const admin_seen_model * rec = rec_it != in.seen.end() ? &rec_it->second : nullptr;
    const admin_eval_env env = admin_env_of(ctx, in);
    const admin_kv inherited = rec != nullptr ? rec->inherited : admin_kv();
    const server_model_source source = rec != nullptr ? rec->source : meta->source;
    const admin_eval ev_old = admin_evaluate(id, source, inherited, had_entry ? &old_settings : nullptr, env);

    // 1. validate strictly
    std::vector<std::string> touched;
    std::vector<std::string> ignored;
    admin_kv next = reset ? admin_kv() : admin_merge_settings(ctx, old_settings, settings, in, ev_old, false, touched, ignored);
    std::map<std::string, json> kept = had_entry && !reset ? e_it->second.kept : std::map<std::string, json>();
    for (const auto & key : touched) {
        kept.erase(key);
    }
    if (!had_entry && !next.empty() && in.entries.size() >= ADMIN_MODELS_MAX_ENTRIES) {
        throw admin_bad_param("model", string_format("at most %zu models can have saved settings", ADMIN_MODELS_MAX_ENTRIES));
    }
    const admin_eval ev_new = admin_evaluate(id, source, inherited, &next, env);
    const admin_kv child_old = admin_with_cli(ev_old.values, in.cli);
    const admin_kv child_new = admin_with_cli(ev_new.values, in.cli);
    admin_check_cross_fields(child_new, touched, reset);

    std::vector<std::string> warnings;
    {
        json facts;
        if (rec != nullptr) {
            admin_facts_lookup(ctx, admin_model_file(*rec), facts);
        }
        admin_value_warnings(child_new, facts.is_object() ? admin_json_int(facts, "n_ctx_train", 0) : 0, warnings);
    }
    if (ev_new.inactive.count("device") && ev_new.inactive.at("device") == "device_missing") {
        warnings.push_back("device_missing");
    }

    // 2. does it change anything for the child? (absent and the built-in default are the same)
    const bool changed = rec == nullptr ? next != old_settings : !admin_effect_equal(child_old, child_new);
    const bool stored_differs = next != old_settings || (had_entry && kept != e_it->second.kept) || (!had_entry && !next.empty());

    auto device_pci_of = [&](const admin_kv & s) {
        std::map<std::string, std::string> out;
        if (s.count("device")) {
            for (const auto & name : admin_device_list(s.at("device"))) {
                const std::string pci = admin_device_pci(in.inv, name);
                if (!pci.empty()) {
                    out[name] = pci;
                }
            }
        }
        return out;
    };

    if (!changed) {
        if (stored_differs) {
            std::string error;
            const bool ok = admin_models_update(ctx, [&](admin_models_file & f) {
                if (next.empty() && kept.empty()) {
                    return f.entries.erase(id) > 0;
                }
                admin_model_entry & e = f.entries[id];
                e.settings   = next;
                e.kept       = kept;
                e.updated    = admin_iso_time(admin_wall_ms());
                e.device_pci = device_pci_of(next);
                if (!had_entry) {
                    e.phase = meta->is_ready_or_sleep() ? "committed" : "pending";
                    if (meta->is_ready_or_sleep()) {
                        e.last_good = next;
                    }
                }
                return true;
            }, true, error);
            if (!ok) {
                throw admin_http_error(500, "server_error", "the settings could not be saved: " + error);
            }
        }
        in = admin_rows_gather(ctx);
        return admin_res_json(200, {
            {"model",    admin_model_row(ctx, *meta, in, false)},
            {"result",   result},
            {"previous", admin_previous_json(old_settings, next)},
            {"warnings", warnings},
        });
    }

    // 3. never unload a running model silently
    const bool running = meta->is_running();
    result["changed"]     = true;
    result["was_running"] = running;
    if (running && !restart) {
        throw admin_http_error(409, "restart_required",
            "model '" + id + "' is running: these settings take effect only after a restart; send \"restart\": true to restart it now");
    }

    // 4. stop it first, with a deadline: the reload in load_models() would wait for it without one
    if (running) {
        uint64_t n_unloaded = 0;
        {
            std::lock_guard<std::mutex> lk(ctx.mtx_track);
            n_unloaded = ctx.track[id].n_unloaded;
        }
        SRV_INF("admin: stopping %s to apply its new settings\n", id.c_str());
        models.unload(id);
        const int64_t deadline = admin_now_ms() + (int64_t) (std::max(0, meta->stop_timeout) + ADMIN_STOP_EXTRA_S) * 1000;
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const std::optional<server_model_meta> m = models.get_meta(id);
            bool stopped = !m.has_value() || !m->is_running();
            {
                std::lock_guard<std::mutex> lk(ctx.mtx_track);
                stopped = stopped || ctx.track[id].n_unloaded > n_unloaded;
            }
            if (stopped) {
                break;
            }
            if (admin_now_ms() > deadline) {
                SRV_ERR("admin: %s did not stop within %d s, its new settings are not saved\n", id.c_str(),
                        std::max(0, meta->stop_timeout) + ADMIN_STOP_EXTRA_S);
                throw admin_http_error(504, "stop_timeout", string_format(
                    "model '%s' did not stop within %d s (stop-timeout + %d s); nothing was saved. If its GPU stopped "
                    "responding, restart the server (or the host)", id.c_str(), std::max(0, meta->stop_timeout) + ADMIN_STOP_EXTRA_S, ADMIN_STOP_EXTRA_S));
            }
        }
    }

    // 5. remember what runs, persist, apply through a reload (it also rescans the sources, like Rescan)
    std::vector<std::string> loaded_before;
    for (const auto & m : admin_list_models(models)) {
        if (m.name != id && m.is_ready_or_sleep()) {
            loaded_before.push_back(m.name);
        }
    }
    const bool load_now = restart;
    admin_model_entry previous_entry;
    if (had_entry) {
        previous_entry = e_it->second;
    }
    std::string error;
    const bool saved = admin_models_update(ctx, [&](admin_models_file & f) {
        if (next.empty() && kept.empty()) {
            f.entries.erase(id);
            return true;
        }
        admin_model_entry & e = f.entries[id];
        e.settings            = next;
        e.kept                = kept;
        e.phase               = load_now ? "trial" : "pending";
        e.failures            = 0;
        e.updated             = admin_iso_time(admin_wall_ms());
        e.device_pci          = device_pci_of(next);
        e.reverted            = nullptr;
        e.reverted_at_startup = false;
        return true;
    }, true, error);
    if (!saved) {
        SRV_ERR("admin: the settings of %s are not saved: %s\n", id.c_str(), error.c_str());
        if (running) {
            try {
                models.load(id); // it was stopped for nothing: bring it back with its old settings
            } catch (const std::exception &) {
            }
        }
        throw admin_http_error(500, "server_error", "the settings could not be saved: " + error);
    }
    SRV_INF("admin: settings of %s saved (%s), reloading the model list\n", id.c_str(), load_now ? "starting it" : "applied at its next start");

    try {
        models.load_models();
        result["reloaded"] = true;
    } catch (const std::exception & e) {
        const std::string reload_error = e.what();
        SRV_ERR("admin: reloading the model list failed, the previous settings of %s are restored: %s\n", id.c_str(), e.what());
        std::string ignored_error;
        admin_models_update(ctx, [&](admin_models_file & f) {
            if (had_entry) {
                f.entries[id] = previous_entry;
            } else {
                f.entries.erase(id);
            }
            return true;
        }, false, ignored_error);
        admin_http_error err(500, "server_error", "reloading the model list failed, the previous settings are restored: " + reload_error);
        err.extra = { {"reload_error", reload_error} };
        // step 4 stopped it for these settings: bring it back with the old ones (the failed reload kept the old
        // model list), and the models that were loaded before, like step 6 does
        if (running) {
            try {
                models.get_meta(id); // rebuilds a stale preset first (a lazy reload that fails again changes nothing)
                models.load(id);
                err.extra["restarted"] = true;
            } catch (const std::exception & le) {
                err.extra["restarted"]     = false;
                err.extra["restart_error"] = le.what();
                SRV_ERR("admin: %s could not be started again with its previous settings: %s\n", id.c_str(), le.what());
            }
        }
        json restarted_also = json::array();
        json not_restarted  = json::array();
        for (const auto & name : loaded_before) {
            const std::optional<server_model_meta> m = models.get_meta(name);
            if (!m.has_value() || m->is_running()) {
                continue;
            }
            try {
                models.load(name);
                restarted_also.push_back(name);
            } catch (const std::exception & le) {
                not_restarted.push_back({ {"id", name}, {"error", le.what()} });
            }
        }
        if (!restarted_also.empty() || !not_restarted.empty()) {
            err.extra["restarted_also"] = restarted_also;
            err.extra["not_restarted"]  = not_restarted;
        }
        throw err;
    }

    // 6. models the rescan stopped as a side effect (e.g. their models.ini section changed meanwhile)
    for (const auto & name : loaded_before) {
        const std::optional<server_model_meta> m = models.get_meta(name);
        if (!m.has_value() || m->is_running()) {
            continue;
        }
        try {
            models.load(name);
            result["restarted_also"].push_back(name);
        } catch (const std::exception & e) {
            result["not_restarted"].push_back({ {"id", name}, {"error", e.what()} });
        }
    }

    // 7. start it (restart, or "Save and load" of a stopped model)
    if (load_now) {
        models.get_meta(id); // rebuilds a stale preset first
        try {
            models.load(id);
            result["restarted"] = true;
        } catch (const std::exception & e) {
            result["load_error"] = e.what();
            SRV_WRN("admin: %s could not be started now (%s), it starts on its next request\n", id.c_str(), e.what());
            std::string ignored_error;
            admin_models_update(ctx, [&](admin_models_file & f) {
                const auto it = f.entries.find(id);
                if (it == f.entries.end() || it->second.phase != "trial") {
                    return false;
                }
                it->second.phase = "pending"; // not tried yet
                return true;
            }, false, ignored_error);
        }
    }

    const std::optional<server_model_meta> after = models.get_meta(id);
    in = admin_rows_gather(ctx);
    return admin_res_json(200, {
        {"model",    admin_model_row(ctx, after.has_value() ? *after : *meta, in, false)},
        {"result",   result},
        {"previous", admin_previous_json(old_settings, next)},
        {"warnings", warnings},
    });
}

//
// POST /admin/models/settings/check: dry run by llama.cpp itself in a helper process
//

static std::string admin_verdict(uint64_t need, uint64_t available) {
    if (need + ADMIN_FIT_MARGIN <= available) {
        return "fits";
    }
    if (need <= available) {
        return "tight";
    }
    return "no";
}

static server_http_res_ptr admin_handle_check(admin_context & ctx, const server_http_req & req) {
    server_models & models = ctx.routes->models;
    const json body = admin_parse_body_object(req);
    if (!body.contains("model") || !body.at("model").is_string() || body.at("model").get<std::string>().empty()) {
        throw admin_bad_param("model", "model must be a model id");
    }
    std::optional<server_model_meta> meta = models.get_meta(body.at("model").get<std::string>());
    if (!meta.has_value() || meta->status == SERVER_MODEL_STATUS_DOWNLOADING || meta->status == SERVER_MODEL_STATUS_DOWNLOADED) {
        throw admin_http_error(404, "not_found_error", "model '" + body.at("model").get<std::string>() + "' is not found");
    }
    const std::string id = meta->name;
    const json settings = body.contains("settings") ? body.at("settings") : json(nullptr);
    const admin_rows_inputs in = admin_rows_gather(ctx);
    const auto e_it = in.entries.find(id);
    const admin_kv stored = e_it != in.entries.end() ? e_it->second.settings : admin_kv();
    const auto rec_it = in.seen.find(id);
    const admin_kv inherited = rec_it != in.seen.end() ? rec_it->second.inherited : admin_kv();
    const server_model_source source = rec_it != in.seen.end() ? rec_it->second.source : meta->source;
    const admin_eval_env env = admin_env_of(ctx, in);
    const admin_eval ev_cur = admin_evaluate(id, source, inherited, &stored, env);

    std::vector<std::string> touched;
    std::vector<std::string> ignored;
    const admin_kv draft = admin_merge_settings(ctx, stored, settings, in, ev_cur, true, touched, ignored);
    const admin_eval ev = admin_evaluate(id, source, inherited, &draft, env);
    const admin_kv child = admin_with_cli(ev.values, in.cli);
    admin_check_cross_fields(child, touched, false);

    json warnings = json::array();
    for (const auto & key : ignored) {
        warnings.push_back("field_locked:" + key);
    }
    if (rec_it != in.seen.end() && !rec_it->second.mmproj.empty()) {
        warnings.push_back("mmproj_not_included");
    }
    json out = {
        {"ok",          false},
        {"model",       id},
        {"checked_at",  admin_iso_time(admin_wall_ms())},
        {"cached",      false},
        {"devices",     json::array()},
        {"host_bytes",  nullptr},
        {"host",        nullptr},
        {"verdict",     "unknown"},
        {"margin",      ADMIN_FIT_MARGIN},
        {"error",       nullptr},
        {"warnings",    warnings},
    };
    auto fail = [&](const std::string & code, const std::string & message, const std::string & line = "") {
        out["error"] = { {"code", code}, {"message", message}, {"line", line} };
        return admin_res_json(200, out);
    };

    // a device that is not there: no dry run (the child would fail the same way)
    if (child.count("device")) {
        for (const auto & name : admin_device_list(child.at("device"))) {
            bool found = name == "none";
            for (const auto & d : in.devices) {
                found = found || admin_iequals(d, name);
            }
            if (!found) {
                return fail("device_missing", "device " + name + " is not on this server (" +
                            (in.devices.empty() ? std::string("no GPU devices") : admin_join(in.devices)) + ")");
            }
        }
    }
    if (!common_subproc::is_supported() || ctx.bin_path.empty()) {
        return fail("unsupported", "this build cannot start helper processes");
    }

    const std::vector<std::string> args = admin_render_args(ctx, *meta, child);
    out["args"] = args;
    const std::string key = admin_check_key(id, args, in.inv.time_ms);
    // the GPU cards now: live free memory for the verdict, and to tell whether a cached dry run still holds
    const json gpus = admin_gpus_body(ctx, false);
    const std::map<std::string, uint64_t> free_now = admin_live_free(gpus);
    const bool running_now = meta->is_running();
    json result = admin_check_cached(ctx, key);
    if (result.is_object() && !admin_check_reusable(result, running_now, free_now)) {
        result = nullptr; // llama.cpp's fit chose its placement for memory that is no longer free (or now is)
    }
    std::string output;
    if (result.is_object()) {
        out["cached"] = true;
    } else {
        std::shared_ptr<admin_job> job;
        std::shared_ptr<admin_job> other;
        {
            std::lock_guard<std::mutex> lk(ctx.mtx_check);
            if (ctx.check_job && ctx.check_job->alive.load()) {
                other = ctx.check_job;
            } else {
                job = admin_job_start("fit check", { args }, admin_helper_env(ctx, "fit"), ADMIN_FIT_TIMEOUT_MS);
                ctx.check_job = job;
            }
        }
        if (other) {
            // outside mtx_check: this takes the job's own mutex, which the request that runs it holds while it stores the result
            if (admin_job_is_stuck(other)) {
                throw admin_http_error(409, "check_stuck", admin_stuck_message("fit check", other));
            }
            throw admin_http_error(409, "check_busy", "another fit check is running, try again in a moment");
        }
        if (!admin_job_wait(job, ADMIN_FIT_TIMEOUT_MS + 2000)) {
            return fail("timeout", string_format("the fit check did not finish within %d s", ADMIN_FIT_TIMEOUT_MS / 1000));
        }
        std::lock_guard<std::mutex> lk(job->mtx);
        out["duration_ms"] = admin_now_ms() - job->t_start;
        if (!job->spawned) {
            return fail("failed", "cannot start the fit check helper (" + ctx.bin_path + ")");
        }
        if (job->timed_out) {
            return fail("timeout", string_format("the fit check did not finish within %d s", ADMIN_FIT_TIMEOUT_MS / 1000));
        }
        output = job->output;
        result = admin_result_line(output);
        if (!result.is_object()) {
            const std::vector<std::string> tail = admin_output_tail(output, ADMIN_LOG_MAX_LINES);
            const admin_reason r = admin_classify(tail);
            const std::string code = r.code == "none" || r.code == "unknown" ? "model_load_failed" : r.code;
            return fail(code, admin_reason_json(r, job->exit_code).at("message").get<std::string>(), r.line);
        }
        if (!result.value("ok", false)) {
            const std::vector<std::string> tail = admin_output_tail(output, ADMIN_LOG_MAX_LINES);
            const admin_reason r = admin_classify(tail);
            std::string code    = admin_json_str(result.value("error", json::object()), "code");
            std::string message = admin_json_str(result.value("error", json::object()), "message");
            if (r.code == "out_of_memory" || r.code == "invalid_settings" || r.code == "invalid_device") {
                code    = r.code; // the output names the cause better than the exception
                message = admin_reason_json(r, job->exit_code).at("message").get<std::string>();
            }
            if (code.empty()) {
                code = "model_load_failed";
            }
            return fail(code, message.empty() ? admin_reason_json(r, job->exit_code).at("message").get<std::string>() : message, r.line);
        }
        // what the dry run saw: its placement holds only while these stay about the same
        json seen_free = json::object();
        for (const auto & d : result.at("devices")) {
            const std::string dev = admin_to_lower(admin_json_str(d, "device"));
            const auto it = free_now.find(dev);
            if (it != free_now.end()) {
                seen_free[dev] = it->second;
            }
        }
        result["admin_seen"] = { {"t_ms", admin_now_ms()}, {"running", running_now}, {"free", seen_free} };
        admin_check_store(ctx, key, result);
    }
    out["checked_while_running"] = result.contains("admin_seen") ? result.at("admin_seen").value("running", false) : running_now;

    // verdict per device: available = live free memory (vendor tool, else the helper's own reading)
    // + what this model uses there now when it runs and would be restarted
    std::vector<std::string> verdicts;
    json devices = json::array();
    for (const auto & d : result.at("devices")) {
        const std::string dev = admin_json_str(d, "device");
        const uint64_t need_model   = (uint64_t) std::max<long long>(0, admin_json_int(d, "model", 0));
        const uint64_t need_context = (uint64_t) std::max<long long>(0, admin_json_int(d, "context", 0));
        const uint64_t need_compute = (uint64_t) std::max<long long>(0, admin_json_int(d, "compute", 0));
        const uint64_t need = need_model + need_context + need_compute;
        uint64_t free  = (uint64_t) std::max<long long>(0, admin_json_int(d, "free", 0));
        uint64_t total = (uint64_t) std::max<long long>(0, admin_json_int(d, "total", 0));
        std::string free_source = "helper";
        bool settling = false;
        uint64_t own = 0;
        std::string own_source;
        for (const auto & card : gpus.at("gpus")) {
            if (!admin_iequals(admin_json_str(card, "device"), dev)) {
                continue;
            }
            settling = card.value("settling", false);
            if (admin_json_str(card, "memory_source") == "vendor_tool" && card.at("free").is_number()) {
                free        = card.at("free").get<uint64_t>();
                total       = card.at("total").get<uint64_t>();
                free_source = "vendor_tool";
            }
            if (meta->is_running()) {
                for (const auto & m : card.at("models")) {
                    if (admin_json_str(m, "id") == id && m.at("memory").is_object()) {
                        own        = m.at("memory").value("bytes", (uint64_t) 0);
                        own_source = m.at("memory").value("source", std::string());
                    }
                }
            }
        }
        const uint64_t available = free + own;
        std::string verdict = total == 0 && free == 0 ? "unknown" : admin_verdict(need, available);
        if (settling) {
            verdict = "unknown";
        }
        verdicts.push_back(verdict);
        devices.push_back({
            {"device",      dev},
            {"description", admin_json_str(d, "description")},
            {"need", {
                {"model",   need_model},
                {"context", need_context},
                {"compute", need_compute},
                {"total",   need},
            }},
            {"free",             free},
            {"total",            total},
            {"free_source",      free_source},
            {"own_usage",        own},
            {"own_usage_source", own_source.empty() ? json(nullptr) : json(own_source)},
            {"available",        available},
            {"settling",         settling},
            {"verdict",          verdict},
        });
    }
    std::string overall = "fits";
    if (verdicts.empty()) {
        overall = "cpu_only";
    }
    for (const char * worst : { "tight", "unknown", "no" }) {
        if (std::find(verdicts.begin(), verdicts.end(), worst) != verdicts.end()) {
            overall = worst;
        }
    }
    const json host = result.value("host", json::object());
    out["ok"]           = true;
    out["devices"]      = devices;
    out["host"]         = host;
    out["host_bytes"]   = admin_json_int(host, "model", 0) + admin_json_int(host, "context", 0) + admin_json_int(host, "compute", 0);
    out["verdict"]      = overall;
    out["n_ctx"]        = result.value("n_ctx", json(nullptr));
    out["n_ctx_train"]  = result.value("n_ctx_train", json(nullptr));
    out["n_layer"]      = result.value("n_layer", json(nullptr));
    out["n_gpu_layers"] = result.value("n_gpu_layers", json(nullptr));
    out["fit"]          = result.value("fit", json(nullptr));
    return admin_res_json(200, out);
}

//
// model status feed: two-phase commit of new settings and the circuit breaker
//
// A background thread reads the same feed as GET /models/sse (server_models::sse, as one more
// waiting client; no upstream change). pending -> trial when a start begins, trial -> committed
// (last_good = settings) when the model reaches LOADED, trial -> failed when its child exits with
// an error before that. ADMIN_MAX_FAILURES failed starts in a row revert the settings to last_good
// (or to none) and reload the presets, so a bad setting does not make every request re-spawn a
// failing model forever. A child that exits without an error line (stopped or killed while
// loading) is not counted.
//

static void admin_on_model_spawn(admin_context & ctx, const std::string & id) {
    std::string error;
    admin_models_update(ctx, [&](admin_models_file & f) {
        const auto it = f.entries.find(id);
        if (it == f.entries.end() || it->second.phase != "pending") {
            return false;
        }
        it->second.phase = "trial";
        return true;
    }, false, error);
}

static void admin_on_model_loaded(admin_context & ctx, const std::string & id) {
    std::string error;
    admin_models_update(ctx, [&](admin_models_file & f) {
        const auto it = f.entries.find(id);
        if (it == f.entries.end()) {
            return false;
        }
        admin_model_entry & e = it->second;
        if (e.phase == "committed" && e.last_good.has_value() && *e.last_good == e.settings && e.failures == 0) {
            return false;
        }
        e.phase     = "committed";
        e.last_good = e.settings;
        e.failures  = 0;
        return true;
    }, false, error);
    // the GPU card shows its memory: read the facts now
    admin_seen_model rec;
    bool found = false;
    {
        std::lock_guard<std::mutex> lk(ctx.mtx_seen);
        const auto it = ctx.seen.find(id);
        if (it != ctx.seen.end()) {
            rec   = it->second;
            found = true;
        }
    }
    if (found) {
        admin_facts_enqueue(ctx, admin_model_file(rec), rec.mmproj);
    }
}

static void admin_revert(admin_context & ctx, const std::string & id, const json & reason) {
    std::lock_guard<std::mutex> lk_apply(ctx.mtx_apply); // serialised with the settings POSTs
    bool reverted = false;
    bool had_last_good = false;
    std::string error;
    admin_models_update(ctx, [&](admin_models_file & f) {
        const auto it = f.entries.find(id);
        if (it == f.entries.end() || it->second.phase != "failed" || it->second.failures < ADMIN_MAX_FAILURES) {
            return false;
        }
        had_last_good = it->second.last_good.has_value();
        admin_entry_revert(it->second, admin_json_str(reason, "code"), admin_json_str(reason, "message"));
        reverted = true;
        return true;
    }, false, error);
    if (!reverted) {
        return;
    }
    SRV_WRN("admin: model %s failed to start %d times in a row with its saved settings (%s), reverted to %s\n",
            id.c_str(), ADMIN_MAX_FAILURES, admin_json_str(reason, "code").c_str(),
            had_last_good ? "its last good settings" : "its models.ini values");
    if (ctx.stop_threads.load()) {
        return;
    }
    try {
        ctx.routes->models.load_models();
    } catch (const std::exception & e) {
        SRV_ERR("admin: reloading the model list after the revert of %s failed: %s\n", id.c_str(), e.what());
    }
}

static void admin_on_model_exit(admin_context & ctx, const std::string & id, int exit_code, bool was_loaded) {
    if (exit_code == 0) {
        return;
    }
    admin_log_instance tail;
    const bool has_tail = admin_log_copy(ctx, id, tail);
    const admin_reason r = has_tail ? admin_classify(tail.lines) : admin_reason{ "none", "", {} };
    if (r.code == "none") {
        SRV_DBG("admin: %s exited with status %d without an error line (stopped or killed)\n", id.c_str(), exit_code);
        return;
    }
    const json reason = admin_reason_json(r, exit_code);
    bool counted = false;
    int  failures = 0;
    if (!was_loaded) {
        std::string error;
        admin_models_update(ctx, [&](admin_models_file & f) {
            const auto it = f.entries.find(id);
            if (it == f.entries.end() || it->second.settings.empty() ||
                (it->second.phase != "pending" && it->second.phase != "trial" && it->second.phase != "failed")) {
                return false;
            }
            it->second.failures++;
            it->second.phase = "failed";
            failures = it->second.failures;
            counted  = true;
            return true;
        }, false, error);
    }
    json lines = json::array();
    for (const auto & line : tail.lines) {
        lines.push_back(line);
    }
    const int64_t wall = admin_wall_ms();
    const bool revert = counted && failures >= ADMIN_MAX_FAILURES;
    json snapshot = {
        {"time",             admin_iso_time(wall)},
        {"time_ms",          wall},
        {"reason",           reason},
        {"port",             tail.port},
        {"exit_code",        exit_code},
        {"before_loaded",    !was_loaded},
        {"lines",            lines},
        {"truncated",        tail.truncated},
        {"failures",         failures},
        {"settings_reverted", revert},
    };
    {
        std::lock_guard<std::mutex> lk(ctx.mtx_logs);
        if (ctx.failures.size() >= ADMIN_LOG_MAX_NAMES && !ctx.failures.count(id)) {
            auto oldest = ctx.failures.begin();
            for (auto it = ctx.failures.begin(); it != ctx.failures.end(); ++it) {
                if (admin_json_int(it->second, "time_ms", 0) < admin_json_int(oldest->second, "time_ms", 0)) {
                    oldest = it;
                }
            }
            ctx.failures.erase(oldest);
        }
        ctx.failures[id] = snapshot; // replaced only by a later failure: an autoload retry does not wipe it
    }
    SRV_WRN("admin: model %s %s (%s): %s\n", id.c_str(), was_loaded ? "stopped with an error" : "failed to start",
            r.code.c_str(), r.line.c_str());
    if (revert) {
        admin_revert(ctx, id, reason);
    }
}

static void admin_on_status(admin_context & ctx, const std::string & id, const std::string & status, int exit_code, bool spawn) {
    std::string prev;
    bool was_loaded = false;
    {
        std::lock_guard<std::mutex> lk(ctx.mtx_track);
        admin_track & t = ctx.track[id];
        prev = t.status;
        if (spawn) {
            t.reached_loaded = false;
        }
        if (status == "loaded" || status == "sleeping") {
            if (status == "loaded" && prev != "loaded") {
                t.n_loaded++;
            }
            t.reached_loaded = true;
        }
        if (status == "unloaded") {
            was_loaded       = t.reached_loaded;
            t.reached_loaded = false;
            if (prev != "unloaded") {
                t.n_unloaded++;
            }
        }
        t.status   = status;
        t.t_change = admin_now_ms();
    }
    if (spawn || (status == "loading" && prev != "loading")) {
        admin_on_model_spawn(ctx, id);
    }
    if (status == "loaded" && prev != "loaded" && prev != "sleeping") {
        admin_on_model_loaded(ctx, id);
    }
    if (status == "unloaded" && prev != "unloaded") {
        admin_on_model_exit(ctx, id, exit_code, was_loaded);
    }
}

static void admin_on_event(admin_context & ctx, const json & data) {
    const std::string event = admin_json_str(data, "event");
    const std::string model = admin_json_str(data, "model");
    if (model.empty() || model == "*") {
        return;
    }
    const json d = data.contains("data") && data.at("data").is_object() ? data.at("data") : json::object();
    const std::string status = admin_json_str(d, "status");
    if (event == "model_status") {
        if (status == "loading") {
            admin_on_status(ctx, model, "loading", 0, true); // load() spawned a child
        }
    } else if (event == "status_change") {
        if (status.empty() || status == "downloading" || status == "downloaded") {
            return;
        }
        admin_on_status(ctx, model, status, (int) admin_json_int(d, "exit_code", 0), false);
    }
}

static void admin_events_thread() {
    admin_context & ctx = admin_ctx();
    server_models & models = ctx.routes->models;
    while (!ctx.stop_threads.load()) {
        admin_upload_tick(); // stage 3: wakes the upload worker when its sweep is due (cheap, deletes nothing here)
        server_task_result_ptr res = models.sse.recv_with_timeout({ ctx.sse_id }, 1);
        if (res == nullptr) {
            continue;
        }
        try {
            admin_on_event(ctx, res->to_json());
        } catch (const std::exception & e) {
            SRV_ERR("admin: handling a model status event failed: %s\n", e.what());
        }
    }
}

static void admin_start_threads(admin_context & ctx) {
    if (ctx.threads_started) {
        return;
    }
    ctx.stop_threads.store(false);
    ctx.routes->models.sse.add_waiting_task_id(ctx.sse_id);
    ctx.sse_subscribed  = true;
    ctx.th_events       = std::thread(admin_events_thread);
    ctx.th_facts        = std::thread(admin_facts_thread);
    ctx.threads_started = true;
}

static void admin_stop_threads() {
    admin_context & ctx = admin_ctx();
    if (!ctx.threads_started) {
        return;
    }
    ctx.stop_threads.store(true);
    ctx.cv_facts.notify_all();
    admin_upload_stop(); // stage 3: the upload worker calls into server_models too
    try {
        if (ctx.th_events.joinable()) {
            ctx.th_events.join();
        }
        if (ctx.th_facts.joinable()) {
            ctx.th_facts.join();
        }
    } catch (const std::exception & e) {
        SRV_ERR("admin: stopping the background threads failed: %s\n", e.what());
    }
    if (ctx.sse_subscribed && ctx.routes != nullptr) {
        ctx.routes->models.sse.remove_waiting_task_id(ctx.sse_id);
        ctx.sse_subscribed = false;
    }
    ctx.threads_started = false;
}

// The background threads use server_models, which is destroyed when llama_server() returns. An
// instance of this lives in the handler of an admin route, inside the HTTP server: that is
// destroyed first (server_http_context is declared after server_models_routes in llama_server(),
// on every exit path), so the threads are stopped and joined before server_models goes away,
// without a call-out in the upstream shutdown code.
struct admin_lifetime_sentinel {
    admin_lifetime_sentinel() = default;
    admin_lifetime_sentinel(const admin_lifetime_sentinel &) = delete;
    admin_lifetime_sentinel & operator=(const admin_lifetime_sentinel &) = delete;
    ~admin_lifetime_sentinel() {
        admin_stop_threads();
    }
};

//
// helper modes of this binary (PROVISION_ADMIN_HELPER), see server_admin_helper_main()
//

void admin_print_result(const json & result) {
    fflush(stderr);
    const std::string line = std::string("\n") + ADMIN_RESULT_PREFIX + result.dump() + "\n";
    fwrite(line.data(), 1, line.size(), stdout);
    fflush(stdout);
}

static const char * admin_dev_type_name(enum ggml_backend_dev_type type) {
    switch (type) {
        case GGML_BACKEND_DEVICE_TYPE_GPU:   return "gpu";
        case GGML_BACKEND_DEVICE_TYPE_IGPU:  return "igpu";
        case GGML_BACKEND_DEVICE_TYPE_ACCEL: return "accel";
        default:                             return "other";
    }
}

// device inventory: loads every backend, reads name, PCI id and memory of each non-CPU device
// (this creates a context on every GPU: it is why it runs here and never in the router)
static int admin_helper_inventory() {
    std::setlocale(LC_NUMERIC, "C");
    json out;
    try {
        llama_backend_init();
        json backends = json::array();
        for (size_t i = 0; i < ggml_backend_reg_count(); i++) {
            ggml_backend_reg_t reg = ggml_backend_reg_get(i);
            backends.push_back({ {"name", ggml_backend_reg_name(reg)}, {"devices", ggml_backend_reg_dev_count(reg)} });
        }
        json devices = json::array();
        for (size_t i = 0; i < ggml_backend_dev_count(); i++) {
            ggml_backend_dev_t dev = ggml_backend_dev_get(i);
            const enum ggml_backend_dev_type type = ggml_backend_dev_type(dev);
            if (type == GGML_BACKEND_DEVICE_TYPE_CPU || type == GGML_BACKEND_DEVICE_TYPE_META) {
                continue;
            }
            ggml_backend_dev_props props = {};
            ggml_backend_dev_get_props(dev, &props);
            ggml_backend_reg_t reg = ggml_backend_dev_backend_reg(dev);
            devices.push_back({
                {"name",         props.name != nullptr ? props.name : ggml_backend_dev_name(dev)},
                {"backend",      reg != nullptr ? ggml_backend_reg_name(reg) : ""},
                {"description",  props.description != nullptr ? props.description : ""},
                {"type",         admin_dev_type_name(type)},
                {"device_id",    props.device_id != nullptr ? json(props.device_id) : json(nullptr)},
                {"memory_free",  (uint64_t) props.memory_free},
                {"memory_total", (uint64_t) props.memory_total},
            });
        }
        out = {
            {"ok",       true},
            {"devices",  devices},
            {"backends", backends},
            {"build",    std::string(llama_build_info())},
        };
        llama_backend_free();
    } catch (const std::exception & e) {
        out = { {"ok", false}, {"error", { {"code", "failed"}, {"message", e.what()} }} };
    }
    admin_print_result(out);
    return out.value("ok", false) ? 0 : 1;
}

// dry-run fit check: the child's own argument parsing and fit, then llama.cpp's no_alloc load
// (common_get_device_memory_data): the memory each device needs, no weights are read
static int admin_helper_fit(int argc, char ** argv) {
    std::setlocale(LC_NUMERIC, "C");
    common_init();
    auto fail = [](const std::string & code, const std::string & message) {
        admin_print_result({ {"ok", false}, {"error", { {"code", code}, {"message", message} }} });
        return 1;
    };
    common_params params;
    if (!common_params_parse(argc, argv, params, LLAMA_EXAMPLE_SERVER)) {
        return fail("invalid_settings", "the arguments of the model were rejected (see the output)");
    }
    llama_backend_init();
    try {
        if (!params.model.hf_repo.empty() || !params.model.url.empty()) {
            // like a router child, but never downloads: the file must be in the cache already
            params.offline = true;
            common_models_handler handler = common_models_handler_init(params, LLAMA_EXAMPLE_SERVER);
            common_models_handler_apply(handler, params);
        }
    } catch (const std::exception & e) {
        return fail("model_load_failed", std::string("the model file is not available locally: ") + e.what());
    }
    if (params.model.path.empty()) {
        return fail("model_load_failed", "the model has no model file");
    }

    // what llama_server() changes before it loads a model (tools/server/server.cpp)
    if (params.embedding && params.n_batch > params.n_ubatch) {
        params.n_batch = params.n_ubatch;
    }
    if (params.n_parallel < 0) {
        params.n_parallel = 4;
        params.kv_unified = true;
    }
    if (params.kv_unified_per_slot > 0 && params.n_ctx == 0 && (uint32_t) params.fit_params_min_ctx != UINT32_MAX) {
        params.n_ctx = params.n_parallel * params.kv_unified_per_slot;
    }
    // ... and server_context::load_model() (output buffers, copy of server_output_limits())
    if (params.embedding || (params.pooling_type != LLAMA_POOLING_TYPE_UNSPECIFIED && params.pooling_type != LLAMA_POOLING_TYPE_NONE)) {
        params.n_outputs_max         = params.n_batch;
        params.n_outputs_max_per_seq = 1;
    } else {
        const common_speculative_output_limits limits = common_speculative_get_output_limits(
            params.n_batch, params.n_parallel, common_speculative_n_max(&params.speculative));
        params.n_outputs_max         = std::max<int32_t>(1, limits.total);
        params.n_outputs_max_per_seq = std::max<int32_t>(1, limits.per_seq);
    }

    json out;
    try {
        llama_model_params   mparams = common_model_params_to_llama(params);
        llama_context_params cparams = common_context_params_to_llama(params);
        std::string fit = "off";
        if (params.fit_params) {
            const common_params_fit_status st = common_fit_params(params.model.path.c_str(), &mparams, &cparams,
                params.tensor_split, params.tensor_buft_overrides.data(), params.fit_params_target.data(),
                params.fit_params_min_ctx, nullptr, GGML_LOG_LEVEL_ERROR);
            fit = st == COMMON_PARAMS_FIT_STATUS_SUCCESS ? "success" : (st == COMMON_PARAMS_FIT_STATUS_FAILURE ? "failure" : "error");
        }
        std::vector<ggml_backend_dev_t> devs;
        uint32_t hp_ngl = 0;
        uint32_t hp_nct = 0;
        uint32_t hp_nex = 0;
        const common_device_memory_data_vec dmd = common_get_device_memory_data(params.model.path.c_str(), &mparams, &cparams,
                                                                                devs, hp_ngl, hp_nct, hp_nex, GGML_LOG_LEVEL_ERROR);
        json devices = json::array();
        for (size_t i = 0; i < devs.size() && i < dmd.size(); i++) {
            ggml_backend_dev_props props = {};
            ggml_backend_dev_get_props(devs[i], &props);
            devices.push_back({
                {"device",      ggml_backend_dev_name(devs[i])},
                {"description", ggml_backend_dev_description(devs[i])},
                {"device_id",   props.device_id != nullptr ? json(props.device_id) : json(nullptr)},
                {"model",       (uint64_t) dmd[i].model},
                {"context",     (uint64_t) dmd[i].context},
                {"compute",     (uint64_t) dmd[i].compute},
                {"free",        (int64_t) dmd[i].free},
                {"total",       (int64_t) dmd[i].total},
            });
        }
        const common_device_memory_data & host = dmd.back();
        out = {
            {"ok",           true},
            {"devices",      devices},
            {"host",         { {"model", (uint64_t) host.model}, {"context", (uint64_t) host.context}, {"compute", (uint64_t) host.compute},
                               {"free", (int64_t) host.free}, {"total", (int64_t) host.total} }},
            {"n_ctx",        cparams.n_ctx},
            {"n_gpu_layers", mparams.n_gpu_layers},
            {"n_ctx_train",  hp_nct},
            {"n_layer",      hp_ngl},
            {"fit",          fit},
        };
    } catch (const std::exception & e) {
        const std::string what = e.what();
        llama_backend_free();
        return fail(what.find("llama_context") != std::string::npos ? "invalid_settings" : "model_load_failed", what);
    }
    llama_backend_free();
    admin_print_result(out);
    return 0;
}

// prints a file (canned GPU / inventory output for tests) through a real process: "# sleep <ms>",
// "# exit <code>" and other "# " lines are directives and are not printed
static int admin_helper_cat() {
    const std::string path = admin_getenv(ADMIN_HELPER_FILE_ENV);
    std::string text;
    if (path.empty() || !admin_read_file(admin_path_from_utf8(path), text, 16 * 1024 * 1024)) {
        fprintf(stderr, "cannot read %s\n", path.c_str());
        return 2;
    }
    int exit_code = 0;
    for (const auto & line : admin_split_lines(text)) {
        const std::string s = admin_strip(line);
        long long n = 0;
        if (string_starts_with(s, "# sleep ") && admin_parse_int(s.substr(8), n)) {
            fflush(stdout);
            std::this_thread::sleep_for(std::chrono::milliseconds(std::max<long long>(0, n)));
            continue;
        }
        if (string_starts_with(s, "# exit ") && admin_parse_int(s.substr(7), n)) {
            exit_code = (int) n;
            continue;
        }
        if (string_starts_with(s, "# ")) {
            continue;
        }
        fwrite(line.data(), 1, line.size(), stdout);
        fputc('\n', stdout);
    }
    fflush(stdout);
    return exit_code;
}

bool server_admin_helper_main(int argc, char ** argv, int & exit_code) {
    const std::string mode = admin_getenv(ADMIN_HELPER_ENV);
    if (mode.empty()) {
        return false;
    }
    if (mode == "inventory") {
        exit_code = admin_helper_inventory();
    } else if (mode == "fit") {
        exit_code = admin_helper_fit(argc, argv);
    } else if (mode == "cat") {
        exit_code = admin_helper_cat();
    } else if (mode == "validate") {
        exit_code = admin_upload_helper_validate(argc, argv); // stage 3: GGUF checks of an upload
    } else {
        fprintf(stderr, "%s: unknown helper mode '%s'\n", ADMIN_HELPER_ENV, mode.c_str());
        exit_code = 2;
    }
    return true;
}

//
// routes
//

void server_admin_register_routes(server_http_context & ctx_http, server_models_routes & routes) {
    admin_context & ctx = admin_ctx();
    if (!ctx.enabled.load()) {
        return;
    }
    ctx.routes = &routes;

    // models_max from the state file already went into the constructor through params;
    // the pinned set is pushed here, before the HTTP server starts and before the startup models load
    {
        std::lock_guard<std::mutex> lk_apply(ctx.mtx_apply);
        admin_refresh(ctx, true);
    }

    // the status feed (two-phase commit of model settings) and the facts reader run from now on;
    // they are stopped through the sentinel below before server_models is destroyed
    admin_start_threads(ctx);
    std::shared_ptr<admin_lifetime_sentinel> sentinel = std::make_shared<admin_lifetime_sentinel>();
    {
        const admin_live live = admin_refresh(ctx, false);
        admin_facts_warm(ctx, live.models, live.pinned.applied);
    }

    static const char * const path_status         = "/admin/status";
    static const char * const path_settings       = "/admin/settings";
    static const char * const path_storage        = "/admin/storage";
    static const char * const path_check          = "/admin/models/download/check";
    static const char * const path_model_settings = "/admin/models/settings";
    static const char * const path_fit_check      = "/admin/models/settings/check";
    static const char * const path_model_log      = "/admin/models/log";
    static const char * const path_gpus           = "/admin/gpus";
    static const char * const path_gpus_rescan    = "/admin/gpus/rescan";
    static const char * const path_diagnostics    = "/admin/diagnostics";

    auto status_handler = [sentinel](admin_context & c, const server_http_req & r) {
        return admin_handle_status(c, r);
    };
    ctx_http.get (path_status,         admin_wrap(status_handler,                   false)); // the probe, no key required
    ctx_http.get (path_settings,       admin_wrap(admin_handle_get_settings,        true));
    ctx_http.post(path_settings,       admin_wrap(admin_handle_post_settings,       true));
    ctx_http.get (path_storage,        admin_wrap(admin_handle_storage,             true));
    ctx_http.post(path_check,          admin_wrap(admin_handle_download_check,      true));
    ctx_http.get (path_model_settings, admin_wrap(admin_handle_get_model_settings,  true));
    ctx_http.post(path_model_settings, admin_wrap(admin_handle_post_model_settings, true));
    ctx_http.post(path_fit_check,      admin_wrap(admin_handle_check,               true));
    ctx_http.get (path_model_log,      admin_wrap(admin_handle_model_log,           true));
    ctx_http.get (path_gpus,           admin_wrap(admin_handle_gpus,                true));
    ctx_http.post(path_gpus_rescan,    admin_wrap(admin_handle_gpus_rescan,         true));
    ctx_http.get (path_diagnostics,    admin_wrap(admin_handle_diagnostics,         true));

    // stage 3: upload into --models-dir, delete of models_dir models (server-admin-upload.cpp)
    std::vector<std::string> paths_upload;
    admin_upload_init(routes.params);
    admin_upload_register(ctx_http, paths_upload);

    // Keep the admin handlers out of the generic handler table: the Vertex AI
    // compat route (/predict, server_http_context::register_gcp_compat) dispatches
    // to anything listed there by name, which would make /admin reachable through
    // a path that a reverse proxy rule on /admin does not cover.
    for (const char * path : { path_status, path_settings, path_storage, path_check, path_model_settings, path_fit_check,
                               path_model_log, path_gpus, path_gpus_rescan, path_diagnostics }) {
        ctx_http.handlers.erase(path);
    }
    for (const auto & path : paths_upload) {
        ctx_http.handlers.erase(path);
    }

    SRV_INF("%s", "admin: routes registered under /admin\n");
}

//
// shared with the other fork-owned admin files (server-admin-internal.h)
//

server_http_context::handler_t admin_internal_wrap(admin_route_fn fn) {
    return admin_wrap([fn = std::move(fn)](admin_context &, const server_http_req & req) { return fn(req); }, true);
}

server_http_res_ptr admin_internal_refusal(const server_http_req & req) {
    admin_context & ctx = admin_ctx();
    try {
        server_http_res_ptr refused = admin_check_access(ctx, req);
        if (refused) {
            return refused;
        }
        const admin_auth auth = admin_check_key(ctx, req);
        if (auth == ADMIN_AUTH_NO_KEY) {
            return admin_res_error(503, "admin_key_unavailable",
                "no admin key is available: set PROVISION_ADMIN_KEY or make the admin state directory writable");
        }
        if (auth != ADMIN_AUTH_OK) {
            SRV_WRN("admin: rejected %s (missing or wrong X-Admin-Key)\n", req.path.c_str());
            return admin_res_error(401, "admin_key_required", "a valid X-Admin-Key header is required");
        }
    } catch (const std::exception & e) {
        return admin_res_error(500, "server_error", e.what());
    }
    return nullptr;
}

server_http_res_ptr admin_internal_json(int status, const json & data) {
    return admin_res_json(status, data);
}

server_http_res_ptr admin_internal_error(int status, const std::string & type, const std::string & message,
                                         const std::string & param, const json & extra) {
    return admin_res_error(status, type, message, param, extra);
}

json admin_internal_body_object(const server_http_req & req) {
    return admin_parse_body_object(req);
}

server_models_routes * admin_internal_routes() {
    return admin_ctx().routes;
}

fs::path admin_internal_state_dir() {
    return admin_ctx().state_dir;
}

std::mutex & admin_internal_apply_mutex() {
    return admin_ctx().mtx_apply;
}

bool admin_internal_stopping() {
    return admin_ctx().stop_threads.load();
}

std::vector<std::string> admin_internal_self_command() {
    return admin_self_command(admin_ctx());
}

std::vector<std::string> admin_internal_helper_env(const std::string & mode) {
    return admin_helper_env(admin_ctx(), mode);
}

json admin_internal_read_facts(const std::string & path, const std::string & mmproj) {
    try {
        return admin_read_facts(path, mmproj);
    } catch (const std::exception & e) {
        return json { {"error", "cannot read the GGUF metadata of " + path + ": " + e.what()} };
    }
}

void admin_internal_facts_enqueue(const std::string & path, const std::string & mmproj) {
    admin_facts_enqueue(admin_ctx(), path, mmproj);
}

admin_kv admin_internal_validate_settings(const std::string & id, const json & settings, const admin_kv * base,
                                          std::vector<std::string> & warnings, json & locks) {
    admin_context & ctx = admin_ctx();
    const admin_rows_inputs in = admin_rows_gather(ctx);

    // what a new models_dir model inherits: the [*] section of the preset file (load_models() cascades it
    // over every models_dir model); the docker-dual entrypoint writes MODEL_DEVICES "*=" into it
    admin_kv inherited;
    const common_params & params = ctx.routes->params;
    if (!params.models_preset.empty()) {
        try {
            const common_preset_context ctx_preset(LLAMA_EXAMPLE_SERVER);
            common_preset global;
            ctx_preset.load_from_ini(params.models_preset, global);
            for (const auto & spec : ADMIN_KEYS) {
                std::string value;
                if (global.get_option(spec.env, value)) {
                    inherited[spec.key] = value;
                }
            }
            std::string kv_unified;
            if (global.get_option(ADMIN_KV_UNIFIED_ENV, kv_unified)) {
                inherited[ADMIN_KV_UNIFIED_KEY] = kv_unified;
            }
        } catch (const std::exception & e) {
            SRV_WRN("admin: cannot read the [*] section of %s for the locks of a new model: %s\n", params.models_preset.c_str(), e.what());
        }
    }
    const admin_eval_env env = admin_env_of(ctx, in);
    const admin_eval ev_current = admin_evaluate(id, SERVER_MODEL_SOURCE_MODELS_DIR, inherited, nullptr, env);

    std::vector<std::string> touched;
    std::vector<std::string> ignored;
    const admin_kv next = admin_merge_settings(ctx, base != nullptr ? *base : admin_kv(), settings, in, ev_current, false, touched, ignored);
    const admin_eval ev_new = admin_evaluate(id, SERVER_MODEL_SOURCE_MODELS_DIR, inherited, &next, env);
    const admin_kv child = admin_with_cli(ev_new.values, in.cli);
    admin_check_cross_fields(child, touched, false);
    admin_value_warnings(child, 0, warnings);
    if (ev_new.inactive.count("device") && ev_new.inactive.at("device") == "device_missing") {
        warnings.push_back("device_missing");
    }

    locks = json::object();
    for (const auto & [key, value] : in.cli) {
        if (admin_key_find(key) != nullptr) {
            locks[key] = { {"by", "router_cli"}, {"value", value} };
        }
    }
    if (ev_current.model_devices_lock) {
        locks["device"] = { {"by", "model_devices"}, {"value", ev_current.model_devices_value} };
    }
    return next;
}

bool admin_internal_entry(const std::string & id, json & overrides, admin_kv & settings) {
    admin_context & ctx = admin_ctx();
    std::lock_guard<std::mutex> lk(ctx.mtx_state);
    const auto it = ctx.models_file.entries.find(id);
    if (it == ctx.models_file.entries.end()) {
        overrides = nullptr;
        settings.clear();
        return false;
    }
    settings  = it->second.settings;
    overrides = admin_kv_to_json(it->second.settings);
    return true;
}

bool admin_internal_settings_writable(std::string & why) {
    admin_context & ctx = admin_ctx();
    {
        std::lock_guard<std::mutex> lk(ctx.mtx_state);
        if (ctx.models_file.read_only) {
            why = "the per-model settings are read-only: " + ctx.models_file.error;
            return false;
        }
    }
    if (!admin_dir_writable(ctx.state_dir)) {
        why = "the admin state directory " + admin_path_to_utf8(ctx.state_dir) + " is not writable";
        return false;
    }
    return true;
}

bool admin_internal_put_upload_entry(const std::string & id, const admin_kv & settings, json & previous, std::string & error) {
    admin_context & ctx = admin_ctx();
    const admin_inventory inv = admin_get_inventory(ctx);
    previous = nullptr;
    bool too_many = false;
    const bool ok = admin_models_update(ctx, [&](admin_models_file & f) {
        const auto it = f.entries.find(id);
        if (it != f.entries.end()) {
            previous = admin_entry_to_json(it->second);
        }
        if (settings.empty()) {
            return f.entries.erase(id) > 0;
        }
        if (it == f.entries.end() && f.entries.size() >= ADMIN_MODELS_MAX_ENTRIES) {
            too_many = true;
            return false;
        }
        admin_model_entry e;
        e.settings = settings;
        e.phase    = "pending"; // the stage-2 two-phase commit: tried at the first start, reverted after repeated failures
        e.updated  = admin_iso_time(admin_wall_ms());
        if (settings.count("device")) {
            for (const auto & name : admin_device_list(settings.at("device"))) {
                const std::string pci = admin_device_pci(inv, name);
                if (!pci.empty()) {
                    e.device_pci[name] = pci;
                }
            }
        }
        e.raw = json::object();
        e.raw["origin"] = "upload";
        f.entries[id] = e;
        return true;
    }, true, error);
    if (too_many) {
        error = string_format("at most %zu models can have saved settings", ADMIN_MODELS_MAX_ENTRIES);
        return false;
    }
    return ok;
}

bool admin_internal_restore_entry(const std::string & id, const json & previous, std::string & error) {
    admin_context & ctx = admin_ctx();
    return admin_models_update(ctx, [&](admin_models_file & f) {
        if (previous.is_object()) {
            std::vector<std::string> problems;
            f.entries[id] = admin_entry_from_json(id, previous, problems);
            return true;
        }
        return f.entries.erase(id) > 0;
    }, false, error);
}

void admin_internal_stop_model(const std::string & id) {
    admin_context & ctx = admin_ctx();
    server_models & models = ctx.routes->models;
    const std::optional<server_model_meta> meta = models.get_meta(id);
    if (!meta.has_value() || !meta->is_running()) {
        return;
    }
    // the same deadline as a settings restart: load_models() would wait for the child without one
    uint64_t n_unloaded = 0;
    {
        std::lock_guard<std::mutex> lk(ctx.mtx_track);
        n_unloaded = ctx.track[id].n_unloaded;
    }
    SRV_INF("admin: stopping %s\n", id.c_str());
    models.unload(id);
    const int wait_s = std::max(0, meta->stop_timeout) + ADMIN_STOP_EXTRA_S;
    const int64_t deadline = admin_now_ms() + (int64_t) wait_s * 1000;
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const std::optional<server_model_meta> m = models.get_meta(id);
        bool stopped = !m.has_value() || !m->is_running();
        {
            std::lock_guard<std::mutex> lk(ctx.mtx_track);
            stopped = stopped || ctx.track[id].n_unloaded > n_unloaded;
        }
        if (stopped) {
            return;
        }
        if (admin_now_ms() > deadline) {
            SRV_ERR("admin: %s did not stop within %d s\n", id.c_str(), wait_s);
            throw admin_http_error(504, "stop_timeout", string_format(
                "model '%s' did not stop within %d s (stop-timeout + %d s); nothing was changed. If its GPU stopped "
                "responding, restart the server (or the host)", id.c_str(), wait_s, ADMIN_STOP_EXTRA_S));
        }
    }
}

json admin_internal_forget_model(const std::string & id, const std::vector<std::string> & paths, bool remove_settings) {
    admin_context & ctx = admin_ctx();
    json out = {
        {"settings_removed", false},
        {"pin_removed",      false},
    };
    if (remove_settings) {
        std::string error;
        bool erased = false;
        const bool ok = admin_models_update(ctx, [&](admin_models_file & f) {
            erased = f.entries.erase(id) > 0;
            return erased;
        }, true, error);
        out["settings_removed"] = ok && erased;
        if (!ok) {
            out["settings_error"] = error;
            SRV_ERR("admin: the saved settings of the deleted model %s could not be removed: %s\n", id.c_str(), error.c_str());
        }
        admin_settings next;
        bool pinned = false;
        {
            std::lock_guard<std::mutex> lk(ctx.mtx_state);
            next = ctx.saved;
            if (next.pinned.has_value()) {
                auto & list = *next.pinned;
                const auto it = std::find(list.begin(), list.end(), id);
                if (it != list.end()) {
                    list.erase(it);
                    pinned = true;
                }
            }
        }
        if (pinned) {
            std::string state_error;
            if (admin_save_state(ctx, next, state_error)) {
                {
                    std::lock_guard<std::mutex> lk(ctx.mtx_state);
                    ctx.saved = next;
                }
                out["pin_removed"] = true;
                admin_refresh(ctx, true); // the caller holds mtx_apply
            } else {
                out["pin_error"] = state_error;
            }
        }
        if (std::find(ctx.env_pinned.begin(), ctx.env_pinned.end(), id) != ctx.env_pinned.end()) {
            out["pinned_by_env"] = true; // PROVISION_ADMIN_PINNED names it; only the environment can change that
        }
        {
            std::lock_guard<std::mutex> lk(ctx.mtx_logs);
            ctx.logs.erase(id);
            ctx.failures.erase(id);
        }
        {
            std::lock_guard<std::mutex> lk(ctx.mtx_track);
            ctx.track.erase(id);
        }
    }
    {
        std::lock_guard<std::mutex> lk(ctx.mtx_facts);
        for (const auto & p : paths) {
            ctx.facts.erase(p);
        }
    }
    {
        std::lock_guard<std::mutex> lk(ctx.mtx_check);
        const std::string prefix = id + "\x1f";
        for (auto it = ctx.check_cache.begin(); it != ctx.check_cache.end();) {
            if (string_starts_with(it->first, prefix)) {
                const std::string key = it->first;
                it = ctx.check_cache.erase(it);
                ctx.check_order.erase(std::remove(ctx.check_order.begin(), ctx.check_order.end(), key), ctx.check_order.end());
            } else {
                ++it;
            }
        }
    }
    return out;
}

void admin_internal_model_flags(const std::string & id, bool & pinned, bool & has_settings) {
    admin_context & ctx = admin_ctx();
    std::lock_guard<std::mutex> lk(ctx.mtx_state);
    has_settings = ctx.models_file.entries.count(id) > 0;
    const std::vector<std::string> & configured = ctx.saved.pinned.has_value() ? *ctx.saved.pinned : ctx.env_pinned;
    pinned = std::find(configured.begin(), configured.end(), id) != configured.end();
}

std::string admin_internal_getenv(const char * name) {
    return admin_getenv(name);
}

fs::path admin_internal_path_from_utf8(const std::string & s) {
    return admin_path_from_utf8(s);
}

std::string admin_internal_path_to_utf8(const fs::path & p) {
    return admin_path_to_utf8(p);
}

bool admin_internal_random_bytes(unsigned char * out, size_t n) {
    return admin_random_bytes(out, n);
}

int64_t admin_internal_now_ms() {
    return admin_now_ms();
}

int64_t admin_internal_wall_ms() {
    return admin_wall_ms();
}

std::string admin_internal_iso_time(int64_t wall_ms) {
    return admin_iso_time(wall_ms);
}

bool admin_internal_read_file(const fs::path & path, std::string & out, size_t max_size) {
    return admin_read_file(path, out, max_size);
}

bool admin_internal_write_file(const fs::path & path, const std::string & content, bool private_mode) {
    return admin_write_file(path, content, private_mode);
}

bool admin_internal_dir_writable(const fs::path & dir) {
    return admin_dir_writable(dir);
}

void admin_internal_fsync_dir(const fs::path & dir) {
    admin_fsync_dir(dir);
}

void admin_internal_dir_space(const fs::path & dir, uint64_t & total_bytes, uint64_t & free_bytes) {
    admin_dir_space(dir, total_bytes, free_bytes);
}

std::string admin_internal_client_text(const server_http_req & req) {
    const admin_client client = admin_client_of(admin_ctx().access, req);
    if (client.known) {
        return client.via_forwarded ? client.address + " (via " + admin_cut(client.peer, 64) + ")" : client.address;
    }
    return client.peer.empty() ? std::string("unknown") : admin_cut(client.peer, 64);
}

void admin_audit(const server_http_req * req, const std::string & action, const std::string & target,
                 const std::string & result, const json & details) {
    // milestone M3 keeps these records in <state>/admin-audit.jsonl and shows them in an Activity tab;
    // until then the hook only logs one line per action
    try {
        const std::string who = req != nullptr ? admin_internal_client_text(*req) : std::string("server");
        const std::string extra = details.is_object() && !details.empty() ? " " + admin_cut(details.dump(), 512) : std::string();
        SRV_INF("admin: audit: %s %s -> %s (by %s)%s\n", action.c_str(), admin_cut(target, 128).c_str(), result.c_str(),
                who.c_str(), extra.c_str());
    } catch (...) {
        // never fails an admin action
    }
}
