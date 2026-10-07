// Provision admin layer, stage 3 milestone M1 (fork-owned, see server-admin-upload.h and docs/provision-admin.md):
// GGUF upload into --models-dir, delete of models_dir models, "Remove projector", the validate helper and the
// typed pre-check of a projector that the router reads on every reload.

#if defined(_WIN32) && !defined(_CRT_RAND_S)
#define _CRT_RAND_S
#endif
#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX // std::min / std::max below, before any header includes windows.h
#endif

#include "server-admin-upload.h"
#include "server-admin.h"
#include "server-admin-internal.h"

#include "server-common.h"
#include "server-http.h"
#include "server-models.h"

#include "common.h"
#include "ggml.h"
#include "gguf.h"
#include "llama.h"
#include "log.h"
#include "mtmd.h"

extern "C" {
#include "hash/sha256/sha256.h"
}

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <clocale>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#include <sys/types.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#if defined(__APPLE__)
#include <stdio.h> // renameatx_np
#endif
#endif

namespace fs = std::filesystem;

//
// constants (fixed: nothing here is tuned per host)
//

static constexpr uint64_t UP_MIB            = 1024ull * 1024;
static constexpr uint64_t UP_CHUNK_MAX      = 16 * UP_MIB;       // a larger body gets the JSON 413 chunk_too_large
static constexpr uint64_t UP_CHUNK_DEFAULT  = 8 * UP_MIB;
static constexpr uint64_t UP_CHUNK_MIN      = 240 * 1024;        // the client's floor when a proxy limits the body size
static constexpr int      UP_MAX_SESSIONS   = 4;                 // unfinished sessions
static constexpr int      UP_MAX_WRITERS    = 4;                 // bodies admitted at once = sessions: no post-body 429
static constexpr size_t   UP_MAX_FILES      = 256;
static constexpr int64_t  UP_TTL_MS         = 72ll * 3600 * 1000; // without progress, counted while the router runs
static constexpr int64_t  UP_KEEP_FINAL_MS  = 24ll * 3600 * 1000; // records of finished sessions, for the UI
static constexpr int64_t  UP_ORPHAN_MS      = 3600ll * 1000;     // a staging folder without a session
static constexpr int64_t  UP_SWEEP_MS       = 60ll * 1000;
static constexpr uint64_t UP_CHECKPOINT     = 256 * UP_MIB;      // fdatasync + durable_offset at least this often ...
static constexpr int64_t  UP_CHECKPOINT_MS  = 15000;             // ... or after this long
static constexpr int64_t  UP_CLIENT_MS      = 30000;             // another tab within this time: other_client_active
static constexpr uint64_t UP_HEAD_CRC_BYTES = UP_MIB;            // resume identity: CRC32 of the first MiB
static constexpr uint64_t UP_MAX_HEADER     = 64 * UP_MIB;       // GGUF metadata + tensor infos read anywhere
static constexpr int      UP_HELPER_MS      = 120000;            // per helper run
static constexpr uint64_t UP_HELPER_AS      = 8ull * 1024 * UP_MIB; // address space of the validate helper
static constexpr size_t   UP_NAME_MAX       = 128;

static const char * const UP_STAGING    = ".provision-upload";
static const char * const UP_RECORD_DIR = "uploads";      // in the admin state directory
static const char * const UP_HINT_FILE  = "session.json"; // in the staging folder: offsets only, revalidated
static const char * const UP_LOCK_FILE  = ".lock";
static const char * const UP_OUT_DIR    = "out";
static const char * const UP_OWNER_FILE = "owner";        // in <state>/uploads: the owner id of this router's hints
static constexpr int64_t  UP_FOREIGN_MARGIN_MS = 24ll * 3600 * 1000; // another router's session: kept TTL + this
static constexpr int      UP_RECORD_VERSION = 1;

//
// small helpers
//

static std::string up_lower(const std::string & s) {
    std::string out = s;
    for (char & c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = (char) (c - 'A' + 'a');
        }
    }
    return out;
}

static bool up_is_hex(const std::string & s, size_t len) {
    if (s.size() != len) {
        return false;
    }
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

static std::string up_hex(const unsigned char * d, size_t n) {
    static const char hex[] = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < n; i++) {
        out += hex[d[i] >> 4];
        out += hex[d[i] & 0x0F];
    }
    return out;
}

static bool up_parse_u64(const std::string & s, uint64_t & out) {
    if (s.empty() || s.size() > 19) {
        return false;
    }
    uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') {
            return false;
        }
        v = v * 10 + (uint64_t) (c - '0');
    }
    out = v;
    return true;
}

static std::string up_errno_text(int err) {
    switch (err) {
        case ENOSPC: return "no space left on the device";
#ifdef EDQUOT
        case EDQUOT: return "disk quota exceeded";
#endif
        case EACCES: return "permission denied";
        case EROFS:  return "read-only file system";
        case EEXIST: return "already exists";
        case ENOENT: return "not found";
        default:     break;
    }
    const char * s = std::strerror(err);
    return s != nullptr ? std::string(s) : string_format("error %d", err);
}

static bool up_is_disk_full(int err) {
#ifdef EDQUOT
    if (err == EDQUOT) {
        return true;
    }
#endif
    return err == ENOSPC;
}

static std::string up_cut(const std::string & s, size_t n) {
    return s.size() <= n ? s : s.substr(0, n) + "...";
}

static std::string up_json_str(const json & j, const char * key) {
    return j.is_object() && j.contains(key) && j.at(key).is_string() ? j.at(key).get<std::string>() : std::string();
}

static int64_t up_json_i64(const json & j, const char * key, int64_t def) {
    if (!j.is_object() || !j.contains(key) || !j.at(key).is_number_integer()) {
        return def;
    }
    try {
        return j.at(key).get<int64_t>();
    } catch (const std::exception &) {
        return def;
    }
}

static bool up_json_bool(const json & j, const char * key, bool def) {
    return j.is_object() && j.contains(key) && j.at(key).is_boolean() ? j.at(key).get<bool>() : def;
}

static std::string up_new_id() {
    unsigned char raw[16];
    if (!admin_internal_random_bytes(raw, sizeof(raw))) {
        throw admin_http_error(500, "server_error", "no secure random source available");
    }
    return up_hex(raw, sizeof(raw));
}

static std::string up_iso(int64_t wall_ms) {
    return wall_ms > 0 ? admin_internal_iso_time(wall_ms) : std::string();
}

//
// CRC32 (IEEE 802.3, the zlib one): X-Chunk-CRC32 and the resume identity of a file
//

static uint32_t up_crc32_update(uint32_t crc, const unsigned char * data, size_t n) {
    static uint32_t table[256];
    static std::once_flag once;
    std::call_once(once, []() {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) {
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            table[i] = c;
        }
    });
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

static std::string up_crc_text(uint32_t crc) {
    return string_format("%08x", (unsigned) crc);
}

//
// test-only hooks (PROVISION_ADMIN_TEST_*), read by admin_upload_read_env() before the environment is scrubbed
//

struct up_test_hooks {
    int64_t  fail_write_at    = -1;      // a write that reaches this file offset fails ...
    int      fail_write_count = 0;       // ... this many times
    int      fail_write_errno = ENOSPC;
    bool     fail_at_sync     = false;   // ... at the fdatasync of a checkpoint instead of at the write
    int64_t  ttl_ms           = -1;      // replaces UP_TTL_MS
    int64_t  keep_final_ms    = -1;      // replaces UP_KEEP_FINAL_MS
    int64_t  orphan_ms        = -1;      // replaces UP_ORPHAN_MS
    int64_t  sweep_ms         = -1;      // replaces UP_SWEEP_MS
    int64_t  checkpoint_bytes = -1;      // replaces UP_CHECKPOINT
    int      rename_errno     = 0;       // the first no-replace rename of a publish fails with this errno
    int      link_errno       = 0;       // ... and its link() fallback with this one
    std::string validate;                // hang | exit | spawn: the next validate helper run(s) fail like that
    int      validate_count   = 0;
    int      helper_ms        = -1;      // replaces UP_HELPER_MS
    int64_t  check_delay_ms   = 0;       // the worker waits this long before the checks of a complete upload
    int      stage_fail_at    = -1;      // staging: the move of the file at this position of the plan fails (EIO) ...
    int      stage_fail_count = 0;       // ... this many times
    int64_t  foreign_margin_ms = -1;     // replaces UP_FOREIGN_MARGIN_MS
};

static std::mutex    g_test_mtx;
static up_test_hooks g_test;

static int up_errno_from_name(const std::string & name, int def) {
    const std::string n = up_lower(name);
    if (n == "enospc") return ENOSPC;
#ifdef EDQUOT
    if (n == "edquot") return EDQUOT;
#endif
    if (n == "eio")    return EIO;
    if (n == "einval") return EINVAL;
    if (n == "enosys") return ENOSYS;
    if (n == "eperm")  return EPERM;
    if (n == "exdev")  return EXDEV;
    if (n == "eexist") return EEXIST;
#ifdef ENOTSUP
    if (n == "enotsup") return ENOTSUP;
#endif
    if (n == "eacces") return EACCES;
    return def;
}

void admin_upload_read_env() {
    std::lock_guard<std::mutex> lk(g_test_mtx);
    g_test = up_test_hooks();
    auto num = [](const char * name, int64_t & out) {
        const std::string v = admin_internal_getenv(name);
        uint64_t n = 0;
        if (!v.empty() && up_parse_u64(v, n)) {
            out = (int64_t) n;
        }
    };
    // <offset>[,<count>[,<errno>[,sync]]]
    const std::string fail = admin_internal_getenv("PROVISION_ADMIN_TEST_FAIL_WRITE_AT");
    if (!fail.empty()) {
        const std::vector<std::string> parts = string_split<std::string>(fail, ',');
        uint64_t n = 0;
        if (!parts.empty() && up_parse_u64(parts[0], n)) {
            g_test.fail_write_at    = (int64_t) n;
            g_test.fail_write_count = 1;
        }
        if (parts.size() > 1 && up_parse_u64(parts[1], n)) {
            g_test.fail_write_count = (int) n;
        }
        if (parts.size() > 2) {
            g_test.fail_write_errno = up_errno_from_name(parts[2], ENOSPC);
        }
        if (parts.size() > 3) {
            g_test.fail_at_sync = up_lower(parts[3]) == "sync";
        }
    }
    int64_t v = -1;
    num("PROVISION_ADMIN_TEST_UPLOAD_TTL_S", v);          if (v >= 0) { g_test.ttl_ms = v * 1000; }          v = -1;
    num("PROVISION_ADMIN_TEST_UPLOAD_KEEP_S", v);         if (v >= 0) { g_test.keep_final_ms = v * 1000; }   v = -1;
    num("PROVISION_ADMIN_TEST_UPLOAD_ORPHAN_S", v);       if (v >= 0) { g_test.orphan_ms = v * 1000; }       v = -1;
    num("PROVISION_ADMIN_TEST_UPLOAD_SWEEP_MS", v);       if (v >= 0) { g_test.sweep_ms = v; }               v = -1;
    num("PROVISION_ADMIN_TEST_CHECKPOINT_BYTES", v);      if (v > 0)  { g_test.checkpoint_bytes = v; }       v = -1;
    num("PROVISION_ADMIN_TEST_HELPER_TIMEOUT_MS", v);     if (v > 0)  { g_test.helper_ms = (int) v; }        v = -1;
    num("PROVISION_ADMIN_TEST_CHECK_DELAY_MS", v);        if (v > 0)  { g_test.check_delay_ms = v; }         v = -1;
    num("PROVISION_ADMIN_TEST_UPLOAD_FOREIGN_MARGIN_S", v); if (v >= 0) { g_test.foreign_margin_ms = v * 1000; } v = -1;
    g_test.rename_errno = up_errno_from_name(admin_internal_getenv("PROVISION_ADMIN_TEST_RENAME_ERRNO"), 0);
    g_test.link_errno   = up_errno_from_name(admin_internal_getenv("PROVISION_ADMIN_TEST_LINK_ERRNO"), 0);
    // <position>[,<count>]
    const std::string stage = admin_internal_getenv("PROVISION_ADMIN_TEST_STAGE_FAIL_AT");
    if (!stage.empty()) {
        const std::vector<std::string> parts = string_split<std::string>(stage, ',');
        uint64_t n = 0;
        if (!parts.empty() && up_parse_u64(parts[0], n)) {
            g_test.stage_fail_at    = (int) n;
            g_test.stage_fail_count = 1;
        }
        if (parts.size() > 1 && up_parse_u64(parts[1], n)) {
            g_test.stage_fail_count = (int) n;
        }
    }
    // hang | exit | spawn [,<count>]
    const std::string val = admin_internal_getenv("PROVISION_ADMIN_TEST_VALIDATE");
    if (!val.empty()) {
        const std::vector<std::string> parts = string_split<std::string>(val, ',');
        g_test.validate       = up_lower(parts[0]);
        g_test.validate_count = 1;
        uint64_t n = 0;
        if (parts.size() > 1 && up_parse_u64(parts[1], n)) {
            g_test.validate_count = (int) n;
        }
    }
}

static int64_t up_ttl_ms()        { std::lock_guard<std::mutex> lk(g_test_mtx); return g_test.ttl_ms >= 0 ? g_test.ttl_ms : UP_TTL_MS; }
static int64_t up_keep_final_ms() { std::lock_guard<std::mutex> lk(g_test_mtx); return g_test.keep_final_ms >= 0 ? g_test.keep_final_ms : UP_KEEP_FINAL_MS; }
static int64_t up_orphan_ms()     { std::lock_guard<std::mutex> lk(g_test_mtx); return g_test.orphan_ms >= 0 ? g_test.orphan_ms : UP_ORPHAN_MS; }
static int64_t up_sweep_ms()      { std::lock_guard<std::mutex> lk(g_test_mtx); return g_test.sweep_ms >= 0 ? g_test.sweep_ms : UP_SWEEP_MS; }
static uint64_t up_checkpoint()   { std::lock_guard<std::mutex> lk(g_test_mtx); return g_test.checkpoint_bytes > 0 ? (uint64_t) g_test.checkpoint_bytes : UP_CHECKPOINT; }
static int     up_helper_ms()     { std::lock_guard<std::mutex> lk(g_test_mtx); return g_test.helper_ms > 0 ? g_test.helper_ms : UP_HELPER_MS; }
static int64_t up_foreign_margin_ms() { std::lock_guard<std::mutex> lk(g_test_mtx); return g_test.foreign_margin_ms >= 0 ? g_test.foreign_margin_ms : UP_FOREIGN_MARGIN_MS; }

// a simulated I/O failure for a write that reaches `end` (test hook), 0 = none
static int up_test_write_failure(uint64_t end, bool at_sync) {
    std::lock_guard<std::mutex> lk(g_test_mtx);
    if (g_test.fail_write_at < 0 || g_test.fail_write_count <= 0 || g_test.fail_at_sync != at_sync || end < (uint64_t) g_test.fail_write_at) {
        return 0;
    }
    g_test.fail_write_count--;
    return g_test.fail_write_errno;
}

// a simulated failure of the move of the file at `position` of a staging (test hook), 0 = none
static int up_test_stage_failure(int position) {
    std::lock_guard<std::mutex> lk(g_test_mtx);
    if (g_test.stage_fail_at < 0 || g_test.stage_fail_count <= 0 || position != g_test.stage_fail_at) {
        return 0;
    }
    g_test.stage_fail_count--;
    return EIO;
}

static std::string up_test_validate_take() {
    std::lock_guard<std::mutex> lk(g_test_mtx);
    if (g_test.validate.empty() || g_test.validate_count <= 0) {
        return "";
    }
    g_test.validate_count--;
    return g_test.validate;
}

//
// GGUF header scanner: a bounded streaming read of the metadata and the tensor infos. It allocates nothing per
// value (only the few strings it keeps, cut short), so it is safe on any file, in the router: it finds where the
// header ends (for the early check on a partial upload), the keys the upload needs, the types of the keys the
// projector loader reads, and refuses a header larger than UP_MAX_HEADER.
//

enum up_scan_status {
    UP_SCAN_OK,
    UP_SCAN_NEED_MORE, // the file does not hold the whole header yet
    UP_SCAN_INVALID,
};

struct up_gguf_info {
    up_scan_status status = UP_SCAN_INVALID;
    std::string code;       // invalid: not_gguf | unsupported_gguf_version | header_too_large | malformed
    std::string message;
    uint64_t need = 0;      // need_more: the file must hold at least this many bytes
    uint32_t version   = 0;
    uint64_t n_tensors = 0;
    uint64_t n_kv      = 0;
    uint64_t header_end = 0; // first byte of the data section
    uint32_t alignment  = 32;
    std::string arch;
    std::string type;
    std::string name;
    long long split_no      = -1;
    long long split_count   = -1;
    long long split_tensors = -1;
    long long file_type     = -1;
    long long ctx_train     = -1;
    long long n_layer       = -1;
    uint64_t  n_params      = 0;
    std::map<std::string, uint32_t> clip_types; // the keys clip_model_loader reads with a typed getter -> their GGUF type
    std::map<std::string, long long> ints;      // <anything>.context_length / .block_count, resolved with arch
};

struct up_reader {
    std::function<bool(uint64_t, void *, size_t)> read_at; // false on an I/O error
    uint64_t avail = 0;  // bytes the file holds (or that are durable)
    std::vector<unsigned char> buf;
    uint64_t buf_off = 0;
    size_t   buf_len = 0;
    bool     io_error = false;

    // 1 = ok, 0 = beyond avail, -1 = I/O error
    int get(uint64_t off, void * out, size_t n) {
        if (off + n > avail) {
            return 0;
        }
        unsigned char * dst = (unsigned char *) out;
        while (n > 0) {
            if (off >= buf_off && off < buf_off + buf_len) {
                const size_t k = std::min(n, (size_t) (buf_off + buf_len - off));
                memcpy(dst, buf.data() + (off - buf_off), k);
                dst += k;
                off += k;
                n   -= k;
                continue;
            }
            if (buf.empty()) {
                buf.resize(256 * 1024);
            }
            const size_t want = (size_t) std::min<uint64_t>(buf.size(), avail - off);
            if (!read_at(off, buf.data(), want)) {
                io_error = true;
                return -1;
            }
            buf_off = off;
            buf_len = want;
        }
        return 1;
    }
};

static up_gguf_info up_gguf_scan(up_reader & r, bool file_complete) {
    up_gguf_info g;
    uint64_t pos = 0;
    bool stop = false;

    auto fail = [&](const char * code, const std::string & message) {
        g.status  = UP_SCAN_INVALID;
        g.code    = code;
        g.message = message;
        stop = true;
        return false;
    };
    // reads n bytes at pos (or only checks they are there when out is null)
    auto take = [&](void * out, uint64_t n) -> bool {
        if (stop) {
            return false;
        }
        if (pos + n > UP_MAX_HEADER) {
            return fail("header_too_large", string_format("the GGUF metadata is larger than %llu MiB",
                (unsigned long long) (UP_MAX_HEADER / UP_MIB)));
        }
        if (pos + n > r.avail) {
            if (file_complete) {
                return fail("malformed", "the file ends inside its GGUF header");
            }
            g.status = UP_SCAN_NEED_MORE;
            g.need   = pos + n;
            stop = true;
            return false;
        }
        if (out != nullptr) {
            const int rc = r.get(pos, out, (size_t) n);
            if (rc < 0) {
                return fail("io_error", "the file cannot be read");
            }
        }
        pos += n;
        return true;
    };
    // host byte order, as gguf.cpp reads a file (a big-endian host such as s390x reads big-endian GGUF files)
    auto u32 = [&](uint32_t & v) { return take(&v, 4); };
    auto u64 = [&](uint64_t & v) { return take(&v, 8); };
    // a string: kept (cut to keep_max) when keep is given, else skipped
    auto str = [&](std::string * keep, size_t keep_max, uint64_t max_len) -> bool {
        uint64_t len = 0;
        if (!u64(len)) {
            return false;
        }
        if (len > max_len) {
            return fail("malformed", string_format("a string of %llu bytes in the GGUF metadata", (unsigned long long) len));
        }
        if (keep != nullptr) {
            const uint64_t k = std::min<uint64_t>(len, keep_max);
            keep->assign((size_t) k, '\0');
            if (k > 0 && !take(&(*keep)[0], k)) {
                return false;
            }
            return take(nullptr, len - k);
        }
        return take(nullptr, len);
    };
    static const int scalar_size[13] = { 1, 1, 2, 2, 4, 4, 4, 1, 0, 0, 8, 8, 8 }; // GGUF_TYPE_*, 8 = string, 9 = array
    auto read_int = [&](uint32_t type, long long & out) -> bool {
        const int n = type < 13 ? scalar_size[type] : 0;
        if (n == 0 || type == 6 || type == 12) {
            return take(nullptr, (uint64_t) n);
        }
        switch (type) {
            case 0: case 7: { uint8_t  v = 0; if (!take(&v, 1)) { return false; } out = v; break; } // uint8, bool
            case 1:         { int8_t   v = 0; if (!take(&v, 1)) { return false; } out = v; break; }
            case 2:         { uint16_t v = 0; if (!take(&v, 2)) { return false; } out = v; break; }
            case 3:         { int16_t  v = 0; if (!take(&v, 2)) { return false; } out = v; break; }
            case 4:         { uint32_t v = 0; if (!take(&v, 4)) { return false; } out = v; break; }
            case 5:         { int32_t  v = 0; if (!take(&v, 4)) { return false; } out = v; break; }
            case 10:        { uint64_t v = 0; if (!take(&v, 8)) { return false; } out = (long long) v; break; }
            case 11:        { int64_t  v = 0; if (!take(&v, 8)) { return false; } out = (long long) v; break; }
            default:        return take(nullptr, (uint64_t) n);
        }
        return true;
    };

    unsigned char magic[4];
    if (!take(magic, 4)) {
        return g;
    }
    if (memcmp(magic, "GGUF", 4) != 0) {
        fail("not_gguf", "the file does not start with the GGUF magic bytes");
        return g;
    }
    if (!u32(g.version)) {
        return g;
    }
    if (g.version != 2 && g.version != 3) {
        // gguf.cpp refuses the other byte order the same way
        const bool swapped = g.version == 0x02000000u || g.version == 0x03000000u;
        fail("unsupported_gguf_version", swapped ? "a GGUF file of the other byte order (endianness) than this server" :
             string_format("GGUF version %u, this server reads versions 2 and 3", (unsigned) g.version));
        return g;
    }
    if (!u64(g.n_tensors) || !u64(g.n_kv)) {
        return g;
    }
    if (g.n_kv > UP_MAX_HEADER / 12 || g.n_tensors > UP_MAX_HEADER / 32) {
        fail("header_too_large", "the GGUF header declares more entries than its size limit allows");
        return g;
    }
    static const std::set<std::string> clip_keys = {
        "general.name", "general.description", "clip.has_vision_encoder", "clip.has_audio_encoder", "clip.has_gen_audio_encoder",
    };
    for (uint64_t i = 0; i < g.n_kv && !stop; i++) {
        std::string key;
        if (!str(&key, 256, 65535)) {
            break;
        }
        uint32_t type = 0;
        if (!u32(type)) {
            break;
        }
        if (clip_keys.count(key)) {
            g.clip_types[key] = type;
        }
        if (type == 8) {
            std::string value;
            const bool keep = key == "general.architecture" || key == "general.type" || key == "general.name";
            if (!str(keep ? &value : nullptr, 256, UP_MAX_HEADER)) {
                break;
            }
            if (key == "general.architecture") {
                g.arch = value;
            } else if (key == "general.type") {
                g.type = value;
            } else if (key == "general.name") {
                g.name = value;
            }
        } else if (type == 9) {
            uint32_t et = 0;
            uint64_t n  = 0;
            if (!u32(et) || !u64(n)) {
                break;
            }
            if (et == 9 || et > 12) {
                fail("malformed", "an array of arrays or of an unknown type in the GGUF metadata");
                break;
            }
            if (n > UP_MAX_HEADER) {
                fail("header_too_large", "an array in the GGUF metadata is larger than the size limit");
                break;
            }
            if (et == 8) {
                for (uint64_t k = 0; k < n && !stop; k++) {
                    str(nullptr, 0, UP_MAX_HEADER);
                }
            } else {
                take(nullptr, n * (uint64_t) scalar_size[et]);
            }
        } else if (type <= 12) {
            long long v = 0;
            if (!read_int(type, v)) {
                break;
            }
            if (type != 6 && type != 12) {
                if (key == "split.no") {
                    g.split_no = v;
                } else if (key == "split.count") {
                    g.split_count = v;
                } else if (key == "split.tensors.count") {
                    g.split_tensors = v;
                } else if (key == "general.file_type") {
                    g.file_type = v;
                } else if (key == "general.alignment") {
                    if (v <= 0 || v > (1 << 20) || (v & (v - 1)) != 0) {
                        fail("malformed", "general.alignment is not a power of two");
                        break;
                    }
                    g.alignment = (uint32_t) v;
                } else if (string_ends_with(key, ".context_length") || string_ends_with(key, ".block_count")) {
                    g.ints[key] = v;
                }
            }
        } else {
            fail("malformed", string_format("unknown GGUF value type %u", (unsigned) type));
            break;
        }
    }
    for (uint64_t i = 0; i < g.n_tensors && !stop; i++) {
        if (!str(nullptr, 0, 65535)) {
            break;
        }
        uint32_t n_dims = 0;
        if (!u32(n_dims)) {
            break;
        }
        if (n_dims == 0 || n_dims > 4) {
            fail("malformed", "a tensor with more than 4 dimensions");
            break;
        }
        uint64_t n_el = 1;
        for (uint32_t d = 0; d < n_dims; d++) {
            uint64_t ne = 0;
            if (!u64(ne)) {
                break;
            }
            n_el = (ne != 0 && n_el > UINT64_MAX / ne) ? UINT64_MAX : n_el * ne;
        }
        uint32_t ttype = 0;
        uint64_t offset = 0;
        if (!u32(ttype) || !u64(offset)) {
            break;
        }
        g.n_params = UINT64_MAX - g.n_params < n_el ? UINT64_MAX : g.n_params + n_el;
    }
    if (stop) {
        return g;
    }
    g.header_end = pos;
    if (g.n_tensors > 0) {
        const uint64_t a = g.alignment;
        g.header_end = (pos + a - 1) / a * a;
        if (g.header_end > r.avail && !file_complete) {
            g.status = UP_SCAN_NEED_MORE;
            g.need   = g.header_end;
            return g;
        }
    }
    if (!g.arch.empty()) {
        const auto c = g.ints.find(g.arch + ".context_length");
        const auto b = g.ints.find(g.arch + ".block_count");
        g.ctx_train = c != g.ints.end() ? c->second : -1;
        g.n_layer   = b != g.ints.end() ? b->second : -1;
    }
    g.status = UP_SCAN_OK;
    return g;
}

// the clip loader reads these keys with typed getters that abort the process on another type
static bool up_clip_types_ok(const up_gguf_info & g, std::string & why) {
    for (const auto & [key, type] : g.clip_types) {
        const bool is_string = key == "general.name" || key == "general.description";
        const uint32_t want  = is_string ? 8u /* GGUF_TYPE_STRING */ : 7u /* GGUF_TYPE_BOOL */;
        if (type != want) {
            why = string_format("%s has GGUF type %u, the projector loader needs %s", key.c_str(), (unsigned) type,
                                is_string ? "a string" : "a bool");
            return false;
        }
    }
    return true;
}

// scans a whole file on disk (complete), through the C runtime
static up_gguf_info up_gguf_scan_path(const std::string & path) {
    up_gguf_info g;
    FILE * f = nullptr;
#if defined(_WIN32)
    const fs::path p = admin_internal_path_from_utf8(path);
    f = _wfopen(p.c_str(), L"rb");
#else
    f = std::fopen(path.c_str(), "rb");
#endif
    if (f == nullptr) {
        g.code    = "io_error";
        g.message = "the file cannot be opened";
        return g;
    }
    uint64_t size = 0;
#if defined(_WIN32)
    if (_fseeki64(f, 0, SEEK_END) == 0) {
        size = (uint64_t) _ftelli64(f);
    }
#else
    if (fseeko(f, 0, SEEK_END) == 0) {
        size = (uint64_t) ftello(f);
    }
#endif
    up_reader r;
    r.avail   = size;
    r.read_at = [f](uint64_t off, void * buf, size_t n) {
#if defined(_WIN32)
        if (_fseeki64(f, (long long) off, SEEK_SET) != 0) {
            return false;
        }
#else
        if (fseeko(f, (off_t) off, SEEK_SET) != 0) {
            return false;
        }
#endif
        return std::fread(buf, 1, n, f) == n;
    };
    g = up_gguf_scan(r, true);
    std::fclose(f);
    return g;
}

bool admin_gguf_header_within_limit(const std::string & path, std::string & why) {
    const up_gguf_info g = up_gguf_scan_path(path);
    if (g.status == UP_SCAN_OK) {
        return true;
    }
    why = g.message.empty() ? g.code : g.message;
    return false;
}

// provision-admin crash fix (called by server_model_meta::update_caps() on every reload, also with the admin
// layer disabled): mtmd_get_cap_from_file() aborts the router for a projector whose keys have another type
bool server_admin_mmproj_usable(const std::string & path) {
    try {
        const up_gguf_info g = up_gguf_scan_path(path);
        std::string why;
        if (g.status != UP_SCAN_OK) {
            why = g.message.empty() ? g.code : g.message;
        } else if (!up_clip_types_ok(g, why)) {
            // why is set
        } else {
            return true;
        }
        LOG_WRN("projector %s is not used (no multimodal input for its model): %s\n", path.c_str(), why.c_str());
    } catch (const std::exception & e) {
        LOG_WRN("projector %s is not used: %s\n", path.c_str(), e.what());
    }
    return false;
}

//
// file system layer: on POSIX every staging operation goes through directory descriptors opened without
// following symlinks (openat O_NOFOLLOW), so a folder or file swapped for a symlink is refused instead of
// being followed (the router may run as root, the staging folder is group-writable). Windows (development
// box) uses paths.
//

#if defined(_WIN32)
static std::wstring up_wide(const fs::path & p) {
    return p.wstring();
}

// the long-path form (\\?\D:\..., \\?\UNC\server\share\...) of an absolute path: staging adds about 56 characters
// (\.provision-upload\<id>\out) to a published path, which must not hit MAX_PATH for a name whose published path
// fits; the wide file APIs, ggml_fopen (_wfopen) and the loader of the validate helper all accept it
static fs::path up_long_path(const fs::path & p) {
    const std::wstring w = p.wstring();
    if (w.rfind(L"\\\\?\\", 0) == 0 || !p.is_absolute()) {
        return p;
    }
    if (w.rfind(L"\\\\", 0) == 0) {
        return fs::path(L"\\\\?\\UNC\\" + w.substr(2));
    }
    return fs::path(L"\\\\?\\" + w);
}
#endif

struct up_dir {
    fs::path path; // for logs and the Windows calls
#if !defined(_WIN32)
    int fd = -1;
#endif
    up_dir() = default;
    up_dir(const up_dir &) = delete;
    up_dir & operator=(const up_dir &) = delete;
    up_dir(up_dir && o) noexcept : path(std::move(o.path)) {
#if !defined(_WIN32)
        fd = o.fd;
        o.fd = -1;
#endif
    }
    up_dir & operator=(up_dir && o) noexcept {
        if (this != &o) {
            reset();
            path = std::move(o.path);
#if !defined(_WIN32)
            fd = o.fd;
            o.fd = -1;
#endif
        }
        return *this;
    }
    ~up_dir() {
        reset();
    }
    void reset() {
#if !defined(_WIN32)
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
#endif
        path.clear();
    }
    bool valid() const {
#if defined(_WIN32)
        return !path.empty();
#else
        return fd >= 0;
#endif
    }
};

struct up_stat {
    bool     exists = false;
    bool     is_dir = false;
    bool     is_reg = false;
    bool     is_link = false;
    uint64_t size  = 0;
    uint64_t dev   = 0;
    uint64_t ino   = 0;
    int64_t  mtime = 0; // seconds
};

static bool up_dir_open_root(const fs::path & p, up_dir & d, int & err) {
    d.reset();
#if defined(_WIN32)
    std::error_code ec;
    if (!fs::is_directory(p, ec)) {
        err = ENOENT;
        return false;
    }
    d.path = up_long_path(p.lexically_normal());
    return true;
#else
    const int fd = ::open(p.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        err = errno;
        return false;
    }
    d.fd   = fd;
    d.path = p;
    return true;
#endif
}

static bool up_dir_open(const up_dir & parent, const std::string & name, bool create, int mode, up_dir & d, int & err) {
    d.reset();
#if defined(_WIN32)
    (void) mode;
    const fs::path p = parent.path / admin_internal_path_from_utf8(name);
    std::error_code ec;
    const fs::file_status st = fs::symlink_status(p, ec);
    if (ec || !fs::exists(st)) {
        if (!create) {
            err = ENOENT;
            return false;
        }
        fs::create_directory(p, ec);
        if (ec) {
            err = ec.value() == ERROR_ACCESS_DENIED ? EACCES : EIO;
            return false;
        }
    } else if (fs::is_symlink(st) || !fs::is_directory(st)) {
        err = ENOTDIR;
        return false;
    }
    d.path = p;
    return true;
#else
    if (create && ::mkdirat(parent.fd, name.c_str(), (mode_t) mode) != 0 && errno != EEXIST) {
        err = errno;
        return false;
    }
    const int fd = ::openat(parent.fd, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        err = errno == ELOOP ? ENOTDIR : errno;
        return false;
    }
    d.fd   = fd;
    d.path = parent.path / name;
    return true;
#endif
}

static bool up_stat_at(const up_dir & d, const std::string & name, up_stat & out) {
    out = up_stat();
#if defined(_WIN32)
    const fs::path p = d.path / admin_internal_path_from_utf8(name);
    std::error_code ec;
    const fs::file_status st = fs::symlink_status(p, ec);
    if (ec || !fs::exists(st)) {
        return false;
    }
    out.exists  = true;
    out.is_link = fs::is_symlink(st);
    out.is_dir  = !out.is_link && fs::is_directory(st);
    out.is_reg  = !out.is_link && fs::is_regular_file(st);
    if (out.is_reg) {
        out.size = (uint64_t) fs::file_size(p, ec);
    }
    struct _stat64 sb;
    if (_wstat64(p.c_str(), &sb) == 0) {
        out.mtime = (int64_t) sb.st_mtime;
        out.dev   = (uint64_t) sb.st_dev;
    }
    return true;
#else
    struct stat sb;
    if (::fstatat(d.fd, name.c_str(), &sb, AT_SYMLINK_NOFOLLOW) != 0) {
        return false;
    }
    out.exists  = true;
    out.is_link = S_ISLNK(sb.st_mode);
    out.is_dir  = S_ISDIR(sb.st_mode);
    out.is_reg  = S_ISREG(sb.st_mode);
    out.size    = (uint64_t) sb.st_size;
    out.dev     = (uint64_t) sb.st_dev;
    out.ino     = (uint64_t) sb.st_ino;
    out.mtime   = (int64_t) sb.st_mtime;
    return true;
#endif
}

static bool up_dir_stat(const up_dir & d, up_stat & out) {
    out = up_stat();
#if defined(_WIN32)
    struct _stat64 sb;
    if (_wstat64(d.path.c_str(), &sb) != 0) {
        return false;
    }
    out.exists = true;
    out.is_dir = true;
    out.dev    = (uint64_t) sb.st_dev;
    out.mtime  = (int64_t) sb.st_mtime;
    return true;
#else
    struct stat sb;
    if (::fstat(d.fd, &sb) != 0) {
        return false;
    }
    out.exists = true;
    out.is_dir = true;
    out.dev    = (uint64_t) sb.st_dev;
    out.ino    = (uint64_t) sb.st_ino;
    out.mtime  = (int64_t) sb.st_mtime;
    return true;
#endif
}

enum up_open_mode {
    UP_OPEN_READ,
    UP_OPEN_WRITE,  // existing file, read + write
    UP_OPEN_CREATE, // created when missing, read + write
};

static int up_file_open(const up_dir & d, const std::string & name, up_open_mode mode, int & err) {
#if defined(_WIN32)
    const fs::path p = d.path / admin_internal_path_from_utf8(name);
    std::error_code ec;
    if (fs::is_symlink(fs::symlink_status(p, ec))) {
        err = ELOOP;
        return -1;
    }
    int flags = _O_BINARY | _O_NOINHERIT;
    flags |= mode == UP_OPEN_READ ? _O_RDONLY : _O_RDWR;
    if (mode == UP_OPEN_CREATE) {
        flags |= _O_CREAT;
    }
    int fd = -1;
    const errno_t rc = _wsopen_s(&fd, p.c_str(), flags, _SH_DENYNO, _S_IREAD | _S_IWRITE);
    if (rc != 0 || fd < 0) {
        err = rc != 0 ? (int) rc : EIO;
        return -1;
    }
    return fd;
#else
    int flags = O_CLOEXEC | O_NOFOLLOW;
    flags |= mode == UP_OPEN_READ ? O_RDONLY : O_RDWR;
    if (mode == UP_OPEN_CREATE) {
        flags |= O_CREAT;
    }
    const int fd = ::openat(d.fd, name.c_str(), flags, 0660);
    if (fd < 0) {
        err = errno;
        return -1;
    }
    struct stat sb;
    if (::fstat(fd, &sb) != 0 || !S_ISREG(sb.st_mode)) {
        ::close(fd);
        err = EINVAL;
        return -1;
    }
    return fd;
#endif
}

static bool up_close(int fd, int & err) {
#if defined(_WIN32)
    if (_close(fd) != 0) {
        err = errno;
        return false;
    }
#else
    if (::close(fd) != 0) {
        err = errno;
        return false;
    }
#endif
    return true;
}

static bool up_pwrite_all(int fd, uint64_t off, const char * data, size_t n, int & err) {
#if defined(_WIN32)
    if (_lseeki64(fd, (long long) off, SEEK_SET) < 0) {
        err = errno;
        return false;
    }
    while (n > 0) {
        const unsigned int k = (unsigned int) std::min<size_t>(n, 1u << 30);
        const int w = _write(fd, data, k);
        if (w <= 0) {
            err = w < 0 ? errno : EIO;
            return false;
        }
        data += w;
        n    -= (size_t) w;
    }
    return true;
#else
    while (n > 0) {
        const ssize_t w = ::pwrite(fd, data, n, (off_t) off);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            err = errno;
            return false;
        }
        if (w == 0) {
            err = EIO;
            return false;
        }
        data += w;
        off  += (uint64_t) w;
        n    -= (size_t) w;
    }
    return true;
#endif
}

static bool up_pread_all(int fd, uint64_t off, char * data, size_t n, int & err) {
#if defined(_WIN32)
    if (_lseeki64(fd, (long long) off, SEEK_SET) < 0) {
        err = errno;
        return false;
    }
    while (n > 0) {
        const unsigned int k = (unsigned int) std::min<size_t>(n, 1u << 30);
        const int r = _read(fd, data, k);
        if (r <= 0) {
            err = r < 0 ? errno : EIO;
            return false;
        }
        data += r;
        n    -= (size_t) r;
    }
    return true;
#else
    while (n > 0) {
        const ssize_t r = ::pread(fd, data, n, (off_t) off);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            err = errno;
            return false;
        }
        if (r == 0) {
            err = EIO;
            return false;
        }
        data += r;
        off  += (uint64_t) r;
        n    -= (size_t) r;
    }
    return true;
#endif
}

static bool up_datasync(int fd, int & err) {
#if defined(_WIN32)
    if (_commit(fd) != 0) {
        err = errno;
        return false;
    }
#elif defined(__APPLE__)
    if (::fcntl(fd, F_FULLFSYNC) != 0 && ::fsync(fd) != 0) {
        err = errno;
        return false;
    }
#elif defined(__linux__)
    if (::fdatasync(fd) != 0) {
        err = errno;
        return false;
    }
#else
    if (::fsync(fd) != 0) {
        err = errno;
        return false;
    }
#endif
    return true;
}

static bool up_truncate(int fd, uint64_t size, int & err) {
#if defined(_WIN32)
    const errno_t rc = _chsize_s(fd, (long long) size);
    if (rc != 0) {
        err = (int) rc;
        return false;
    }
#else
    if (::ftruncate(fd, (off_t) size) != 0) {
        err = errno;
        return false;
    }
#endif
    return true;
}

static bool up_fsize(int fd, uint64_t & size) {
#if defined(_WIN32)
    struct _stat64 sb;
    if (_fstat64(fd, &sb) != 0) {
        return false;
    }
    size = (uint64_t) sb.st_size;
#else
    struct stat sb;
    if (::fstat(fd, &sb) != 0) {
        return false;
    }
    size = (uint64_t) sb.st_size;
#endif
    return true;
}

// the written range does not need to stay in the page cache (Linux): 40 GB of upload must not push the
// memory-mapped weights of running models out
static void up_drop_cache(int fd, uint64_t off, uint64_t len) {
#if defined(__linux__) && defined(POSIX_FADV_DONTNEED)
    ::posix_fadvise(fd, (off_t) off, (off_t) len, POSIX_FADV_DONTNEED);
#else
    (void) fd;
    (void) off;
    (void) len;
#endif
}

static void up_fsync_dir(const up_dir & d) {
#if !defined(_WIN32)
    if (d.fd >= 0) {
        ::fsync(d.fd);
    }
#else
    (void) d;
#endif
}

static bool up_unlink_at(const up_dir & d, const std::string & name, int & err) {
#if defined(_WIN32)
    std::error_code ec;
    if (!fs::remove(d.path / admin_internal_path_from_utf8(name), ec) || ec) {
        err = ec ? EIO : ENOENT;
        return false;
    }
    return true;
#else
    if (::unlinkat(d.fd, name.c_str(), 0) != 0) {
        err = errno;
        return false;
    }
    return true;
#endif
}

static bool up_rmdir_at(const up_dir & d, const std::string & name, int & err) {
#if defined(_WIN32)
    std::error_code ec;
    if (!fs::remove(d.path / admin_internal_path_from_utf8(name), ec) || ec) {
        err = ec ? ENOTEMPTY : ENOENT;
        return false;
    }
    return true;
#else
    if (::unlinkat(d.fd, name.c_str(), AT_REMOVEDIR) != 0) {
        err = errno;
        return false;
    }
    return true;
#endif
}

struct up_dirent {
    std::string name;
    up_stat     st;
};

static std::vector<up_dirent> up_list(const up_dir & d) {
    std::vector<up_dirent> out;
#if defined(_WIN32)
    std::error_code ec;
    for (fs::directory_iterator it(d.path, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        up_dirent e;
        e.name = admin_internal_path_to_utf8(it->path().filename());
        up_stat_at(d, e.name, e.st);
        out.push_back(e);
    }
#else
    const int fd = ::openat(d.fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        return out;
    }
    DIR * dir = ::fdopendir(fd);
    if (dir == nullptr) {
        ::close(fd);
        return out;
    }
    while (struct dirent * de = ::readdir(dir)) {
        const std::string name = de->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        up_dirent e;
        e.name = name;
        up_stat_at(d, name, e.st);
        out.push_back(e);
    }
    ::closedir(dir);
#endif
    return out;
}

// removes a file or a folder tree inside d, never following a symlink (a symlink itself is removed)
static bool up_remove_tree_at(const up_dir & d, const std::string & name, int depth = 0) {
    up_stat st;
    if (!up_stat_at(d, name, st)) {
        return true;
    }
    int err = 0;
    if (!st.is_dir) {
        return up_unlink_at(d, name, err);
    }
    if (depth > 16) {
        return false;
    }
    up_dir sub;
    if (!up_dir_open(d, name, false, 0, sub, err)) {
        return false;
    }
    bool ok = true;
    for (const auto & e : up_list(sub)) {
        ok = up_remove_tree_at(sub, e.name, depth + 1) && ok;
    }
    sub.reset();
    return up_rmdir_at(d, name, err) && ok;
}

static uint64_t up_tree_bytes(const up_dir & d, int depth = 0) {
    uint64_t total = 0;
    if (depth > 16) {
        return 0;
    }
    for (const auto & e : up_list(d)) {
        if (e.st.is_reg) {
            total += e.st.size;
        } else if (e.st.is_dir) {
            up_dir sub;
            int err = 0;
            if (up_dir_open(d, e.name, false, 0, sub, err)) {
                total += up_tree_bytes(sub, depth + 1);
            }
        }
    }
    return total;
}

// the owner the router gives what it creates in models_dir (Docker runs it as root: the entrypoint chowns new
// folders to the owner of the mount, and so does the upload); POSIX only
struct up_owner {
    bool     chown = false;
    unsigned uid   = 0;
    unsigned gid   = 0;
    bool     setgid_dirs = false;
};

static void up_apply_owner_fd(int fd, const up_owner & o, bool is_dir, std::vector<std::string> * warnings) {
#if defined(_WIN32)
    (void) fd;
    (void) o;
    (void) is_dir;
    (void) warnings;
#else
    if (o.chown && ::fchown(fd, (uid_t) o.uid, (gid_t) o.gid) != 0 && warnings != nullptr) {
        const std::string w = "chown_failed";
        if (std::find(warnings->begin(), warnings->end(), w) == warnings->end()) {
            warnings->push_back(w);
        }
    }
    const mode_t mode = is_dir ? (mode_t) (0770 | (o.setgid_dirs ? S_ISGID : 0)) : (mode_t) 0660;
    ::fchmod(fd, mode);
#endif
}

static void up_apply_owner_dir(const up_dir & d, const up_owner & o, std::vector<std::string> * warnings) {
#if defined(_WIN32)
    (void) d;
    (void) o;
    (void) warnings;
#else
    if (d.fd >= 0) {
        up_apply_owner_fd(d.fd, o, true, warnings);
    }
#endif
}

enum up_rename_result {
    UP_RENAME_OK,
    UP_RENAME_EXISTS,
    UP_RENAME_FAILED,
};

// a rename that never replaces anything: renameat2(RENAME_NOREPLACE); where the file system does not support
// it, link() + unlink() for a file; where that is not possible either, a check followed by a rename (the
// caller holds the admin apply mutex, so no other publish runs at the same time)
static up_rename_result up_rename_noreplace(const up_dir & from, const std::string & from_name,
                                            const up_dir & to, const std::string & to_name,
                                            bool is_dir, bool test_hooks, int & err, std::string & method) {
    int inject_rename = 0;
    int inject_link   = 0;
    if (test_hooks) {
        std::lock_guard<std::mutex> lk(g_test_mtx);
        inject_rename = g_test.rename_errno;
        inject_link   = g_test.link_errno;
    }
#if defined(_WIN32)
    (void) is_dir;
    (void) inject_link;
    const fs::path src = from.path / admin_internal_path_from_utf8(from_name);
    const fs::path dst = to.path / admin_internal_path_from_utf8(to_name);
    method = "MoveFileEx";
    if (inject_rename == EEXIST) {
        err = EEXIST;
        return UP_RENAME_EXISTS;
    }
    // a file the facts reader holds open for a moment: retried for up to 5 s
    for (int attempt = 0; attempt < 50; attempt++) {
        if (MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_WRITE_THROUGH)) {
            return UP_RENAME_OK;
        }
        const DWORD e = GetLastError();
        if (e == ERROR_ALREADY_EXISTS || e == ERROR_FILE_EXISTS) {
            err = EEXIST;
            return UP_RENAME_EXISTS;
        }
        if (e != ERROR_SHARING_VIOLATION && e != ERROR_ACCESS_DENIED && e != ERROR_LOCK_VIOLATION) {
            err = e == ERROR_NOT_SAME_DEVICE ? EXDEV : EIO;
            return UP_RENAME_FAILED;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    err = EBUSY;
    return UP_RENAME_FAILED;
#else
    int e = 0;
    bool try_link = true;
#if defined(__linux__) && defined(SYS_renameat2)
    method = "renameat2";
    long rc = -1;
    if (inject_rename != 0) {
        errno = inject_rename;
    } else {
        rc = ::syscall(SYS_renameat2, from.fd, from_name.c_str(), to.fd, to_name.c_str(), 1 /* RENAME_NOREPLACE */);
    }
    if (rc == 0) {
        return UP_RENAME_OK;
    }
    e = errno;
    if (e == EEXIST || e == ENOTEMPTY) {
        err = EEXIST;
        return UP_RENAME_EXISTS;
    }
    if (e != EINVAL && e != ENOSYS
#ifdef ENOTSUP
        && e != ENOTSUP
#endif
#ifdef EOPNOTSUPP
        && e != EOPNOTSUPP
#endif
        ) {
        err = e;
        return UP_RENAME_FAILED;
    }
#elif defined(__APPLE__) && defined(RENAME_EXCL)
    method = "renameatx_np";
    int rc = -1;
    if (inject_rename != 0) {
        errno = inject_rename;
    } else {
        rc = ::renameatx_np(from.fd, from_name.c_str(), to.fd, to_name.c_str(), RENAME_EXCL);
    }
    if (rc == 0) {
        return UP_RENAME_OK;
    }
    e = errno;
    if (e == EEXIST) {
        err = EEXIST;
        return UP_RENAME_EXISTS;
    }
    if (e != EINVAL && e != ENOTSUP) {
        err = e;
        return UP_RENAME_FAILED;
    }
#else
    (void) inject_rename;
#endif
    if (!is_dir && try_link) {
        method = "link";
        int rc_link = -1;
        if (inject_link != 0) {
            errno = inject_link;
        } else {
            rc_link = ::linkat(from.fd, from_name.c_str(), to.fd, to_name.c_str(), 0);
        }
        if (rc_link == 0) {
            int ignored = 0;
            up_unlink_at(from, from_name, ignored);
            return UP_RENAME_OK;
        }
        e = errno;
        if (e == EEXIST) {
            err = EEXIST;
            return UP_RENAME_EXISTS;
        }
        if (e != EPERM && e != EXDEV && e != EMLINK && e != ENOSYS && e != EACCES
#ifdef ENOTSUP
            && e != ENOTSUP
#endif
#ifdef EOPNOTSUPP
            && e != EOPNOTSUPP
#endif
            ) {
            err = e;
            return UP_RENAME_FAILED;
        }
    }
    method = "check+rename";
    up_stat st;
    if (up_stat_at(to, to_name, st)) {
        err = EEXIST;
        return UP_RENAME_EXISTS;
    }
    if (::renameat(from.fd, from_name.c_str(), to.fd, to_name.c_str()) != 0) {
        err = errno;
        return UP_RENAME_FAILED;
    }
    return UP_RENAME_OK;
#endif
}

// a plain rename inside the staging area (both names are ours); replace: an existing target is replaced (POSIX
// renameat() always does, Windows only with MOVEFILE_REPLACE_EXISTING)
static bool up_rename_at(const up_dir & from, const std::string & from_name, const up_dir & to, const std::string & to_name, int & err,
                         bool replace = false) {
#if defined(_WIN32)
    const fs::path src = from.path / admin_internal_path_from_utf8(from_name);
    const fs::path dst = to.path / admin_internal_path_from_utf8(to_name);
    const DWORD flags = MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0);
    for (int attempt = 0; attempt < 50; attempt++) {
        if (MoveFileExW(src.c_str(), dst.c_str(), flags)) {
            return true;
        }
        const DWORD e = GetLastError();
        if (e != ERROR_SHARING_VIOLATION && e != ERROR_ACCESS_DENIED && e != ERROR_LOCK_VIOLATION) {
            err = e == ERROR_ALREADY_EXISTS || e == ERROR_FILE_EXISTS ? EEXIST : (e == ERROR_NOT_SAME_DEVICE ? EXDEV : EIO);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    err = EBUSY;
    return false;
#else
    (void) replace; // renameat() replaces an existing target
    if (::renameat(from.fd, from_name.c_str(), to.fd, to_name.c_str()) != 0) {
        err = errno;
        return false;
    }
    return true;
#endif
}

// advisory lock held by the process that owns a session, for the whole life of the session: recovery and the
// sweep of another router that shares the models dir skip a locked session
struct up_lock {
#if defined(_WIN32)
    HANDLE h = INVALID_HANDLE_VALUE;
#else
    int fd = -1;
#endif
    bool held() const {
#if defined(_WIN32)
        return h != INVALID_HANDLE_VALUE;
#else
        return fd >= 0;
#endif
    }
    void release() {
#if defined(_WIN32)
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            h = INVALID_HANDLE_VALUE;
        }
#else
        if (fd >= 0) {
            ::flock(fd, LOCK_UN);
            ::close(fd);
            fd = -1;
        }
#endif
    }
};

static const up_owner & up_fs_owner();

// false: not taken (elsewhere = another process holds it; else the lock file could not be opened, open_err)
static bool up_lock_take(const up_dir & d, const std::string & owner, up_lock & lock, bool & elsewhere, int * open_err = nullptr) {
    elsewhere = false;
#if defined(_WIN32)
    const fs::path p = d.path / UP_LOCK_FILE;
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        elsewhere = e == ERROR_SHARING_VIOLATION;
        if (open_err != nullptr) {
            *open_err = e == ERROR_ACCESS_DENIED ? EACCES : EIO;
        }
        return false;
    }
    SetFilePointer(h, 0, nullptr, FILE_BEGIN);
    SetEndOfFile(h);
    DWORD written = 0;
    WriteFile(h, owner.data(), (DWORD) owner.size(), &written, nullptr);
    lock.h = h;
    return true;
#else
    bool writable = true;
    int fd = ::openat(d.fd, UP_LOCK_FILE, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0660);
    if (fd < 0 && errno == EACCES) {
        // a lock file another user created (an older router running as root): flock() works on a read-only
        // descriptor as well, so the lock can still be probed and held
        writable = false;
        fd = ::openat(d.fd, UP_LOCK_FILE, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    }
    if (fd < 0) {
        if (open_err != nullptr) {
            *open_err = errno;
        }
        return false;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        elsewhere = errno == EWOULDBLOCK || errno == EAGAIN;
        if (open_err != nullptr) {
            *open_err = errno;
        }
        ::close(fd);
        return false;
    }
    if (writable) {
        // like everything else in staging: the owner of models_dir, group read + write (another router sharing
        // the models dir must be able to open it)
        up_apply_owner_fd(fd, up_fs_owner(), false, nullptr);
        if (::ftruncate(fd, 0) == 0) {
            int err = 0;
            up_pwrite_all(fd, 0, owner.data(), owner.size(), err);
        }
    }
    lock.fd = fd;
    return true;
#endif
}

static std::string up_owner_id_now();

static std::string up_lock_owner_text() {
    char host[256] = {};
#if defined(_WIN32)
    DWORD n = sizeof(host);
    GetComputerNameA(host, &n);
    const long long pid = (long long) GetCurrentProcessId();
#else
    gethostname(host, sizeof(host) - 1);
    const long long pid = (long long) getpid();
#endif
    std::string boot_id;
#if defined(__linux__)
    std::string text;
    if (admin_internal_read_file("/proc/sys/kernel/random/boot_id", text, 4096)) {
        boot_id = text.substr(0, text.find('\n'));
    }
#endif
    json j = {
        {"pid",     pid},
        {"host",    std::string(host)},
        {"boot_id", boot_id},
        {"owner",   up_owner_id_now()},
        {"since",   admin_internal_iso_time(admin_internal_wall_ms())},
    };
    return j.dump() + "\n";
}

//
// sessions
//

enum up_head_state {
    UP_HEAD_NONE,     // no bytes yet
    UP_HEAD_SCANNING, // waiting for the whole header (head_need)
    UP_HEAD_QUEUED,   // the header is here, the helper check is queued
    UP_HEAD_OK,
    UP_HEAD_WARNING,  // metadata alone gave a load error: confirmed when the upload is complete
    UP_HEAD_FAILED,   // the helper did not answer (timeout, killed): checked again at the end
    UP_HEAD_ERROR,    // definitive: see up_file::error
};

static const char * up_head_name(up_head_state s) {
    switch (s) {
        case UP_HEAD_NONE:     return "none";
        case UP_HEAD_SCANNING: return "scanning";
        case UP_HEAD_QUEUED:   return "queued";
        case UP_HEAD_OK:       return "ok";
        case UP_HEAD_WARNING:  return "warning";
        case UP_HEAD_FAILED:   return "failed";
        case UP_HEAD_ERROR:    return "error";
    }
    return "none";
}

struct up_file {
    int         index = 0;
    std::string name;            // as the client named it (display only, never part of a path)
    uint64_t    size  = 0;
    int64_t     last_modified = 0;
    std::string sha_expected;    // lower-case hex, "" = none

    uint64_t    offset  = 0;     // bytes accepted
    uint64_t    durable = 0;     // bytes known to be on disk (fdatasync)
    bool        complete = false;
    bool        dropped  = false;
    int64_t     t_sync   = 0;    // steady ms of the latest checkpoint

    sha256_t    sha;             // over [0, offset) when sha_valid
    bool        sha_valid = true;
    std::string sha_hex;         // final

    uint32_t    head_crc = 0;    // CRC32 of [0, min(offset, UP_HEAD_CRC_BYTES))
    bool        head_crc_valid = true;

    up_head_state head_state = UP_HEAD_NONE;
    uint64_t    head_need = 8;
    up_gguf_info head;           // the scan of the header
    json        head_result;     // what the helper said about it
    std::string role;            // model | projector, "" = not known yet
    json        error;           // null, or {code, message}: this file cannot be used (drop it or cancel)
    std::string final_name;      // name inside the published layout
};

enum up_state {
    UP_RECEIVING,
    UP_CHECKING,
    UP_PUBLISHING,
    UP_READY,
    UP_PUBLISHED,
    UP_REJECTED,
    UP_CANCELLED,
    UP_EXPIRED,
};

static const char * up_state_name(up_state s) {
    switch (s) {
        case UP_RECEIVING:  return "receiving";
        case UP_CHECKING:   return "checking";
        case UP_PUBLISHING: return "publishing";
        case UP_READY:      return "ready";
        case UP_PUBLISHED:  return "published";
        case UP_REJECTED:   return "rejected";
        case UP_CANCELLED:  return "cancelled";
        case UP_EXPIRED:    return "expired";
    }
    return "receiving";
}

static bool up_state_from_name(const std::string & n, up_state & out) {
    for (up_state s : { UP_RECEIVING, UP_CHECKING, UP_PUBLISHING, UP_READY, UP_PUBLISHED, UP_REJECTED, UP_CANCELLED, UP_EXPIRED }) {
        if (n == up_state_name(s)) {
            out = s;
            return true;
        }
    }
    return false;
}

static bool up_is_final(up_state s) {
    return s == UP_PUBLISHED || s == UP_REJECTED || s == UP_CANCELLED || s == UP_EXPIRED;
}

struct up_session;

// a chunk body admitted by the pre-body check: released when the handler is done, or when the request ends
// without it (the body never arrived)
struct up_admission {
    std::weak_ptr<up_session> s;
    std::atomic<bool> released{false};
    void release();
    ~up_admission() {
        release();
    }
};

struct up_session {
    std::string id;
    std::string name;
    up_state    state = UP_RECEIVING;
    std::string substate;          // checking: queued | hashing | classifying | load_check
    std::string blocked_by;        // ready: manual | name_taken | publish_error | sha256_mismatch | check_failed | settings_not_saved | settings_invalid
    json        blocked;           // details of blocked_by
    json        error;             // rejected: {code, message, file?}
    admin_kv    settings;          // canonical per-model settings to apply at publish
    json        settings_request;  // as the client sent them (re-validated at publish)
    std::string settings_mode = "replace";
    bool        auto_publish = true;
    bool        force_sha = false;         // "Publish anyway" after sha256_mismatch
    bool        without_settings = false;  // "Publish without the placement" after settings_not_saved
    std::vector<up_file> files;
    json        plan;              // the final layout
    json        facts;             // what the checks found (architecture, ...)
    json        result;            // published: final names, sizes, SHA-256
    std::vector<std::string> warnings;
    std::string creator;           // the client that created it (audit)
    json        creator_actor;     // ... as the audit records it (admin_audit_actor), for the records the worker writes
    int64_t     created = 0;       // wall ms
    int64_t     updated = 0;       // wall ms
    int64_t     last_activity = 0; // wall ms
    int64_t     idle_ms = 0;       // without progress, counted while the router runs
    int64_t     t_idle  = 0;       // steady ms of the latest idle accounting
    int64_t     t_final = 0;       // steady ms when it reached a final state
    std::string client;            // X-Upload-Client of the latest chunk
    int64_t     t_client = 0;      // steady ms of the latest chunk
    bool        recovered = false; // continued after a router restart
    bool        recovered_without_record = false; // rebuilt from the staging hint only (no settings, manual publish)
    bool        record_persisted = false;
    bool        cancel_requested = false;
    bool        retry_checks = false;
    int64_t     t_checks_after = 0; // steady ms: test hook delay
    uint64_t    hash_done = 0;
    uint64_t    hash_total = 0;
    int64_t     t_hash_start = 0;
    bool        staged = false;    // the parts are renamed into out/
    std::string staged_id;         // the id the staged names were made for

    up_dir      dir;               // <staging>/<id>
    up_lock     lock;

    std::mutex  mtx;               // leaf: every field above
    std::mutex  mtx_io;            // try-locked by the chunk writer (one chunk per session at a time)
    std::mutex  mtx_record;        // serialises the writes of the record and the hint
    uint64_t    record_seq = 0;    // under mtx
    uint64_t    record_written = 0; // under mtx_record

    bool        in_flight = false; // under up_ctx::mtx: a body is being received for it
    std::weak_ptr<up_admission> adm;
};

struct up_context {
    std::mutex mtx; // leaf: sessions, admitted, the availability fields
    // outermost, never taken under another lock: a create (name check, session limit, insert) and a rename of a
    // session are one step each, so two concurrent creates cannot both take a name or go past UP_MAX_SESSIONS
    std::mutex mtx_names;
    std::condition_variable cv;
    std::map<std::string, std::shared_ptr<up_session>> sessions;
    int  admitted = 0;
    bool ready    = false; // admin_upload_init() ran
    std::atomic<bool> stop{false};
    std::thread worker;
    bool worker_started = false;
    bool wake = false;
    int64_t t_sweep = 0; // steady ms of the latest sweep
    bool sweep_due = true;

    fs::path models_dir;   // canonical, "" = none
    std::string models_dir_text;
    up_dir root;           // models_dir
    up_dir staging;        // <models_dir>/.provision-upload
    bool available = false;
    std::string reason;    // no_models_dir | models_dir_missing | not_writable | staging_unusable | staging_other_filesystem
    std::string message;
    up_owner owner;
    fs::path records_dir;  // <state>/uploads
    bool records_writable = false;
    std::vector<std::string> cleanup; // staging entries to remove (session folders, trash), by the worker
    // worker thread only: abandoned folders the sweep handed to the cleanup -> the details of their upload.swept
    // record (written once the removal is done), and the entries whose removal failed and was reported (they are
    // retried by every sweep, but reported again only once they are gone)
    std::map<std::string, json> swept;
    std::set<std::string> cleanup_failed;

    // the identity of this router in the staging hints it writes (several routers may share one models dir)
    std::mutex  mtx_owner;      // leaf: the three fields below
    std::string owner_id;       // 32 hex digits
    bool        owner_saved  = false; // in <state>/uploads/owner
    bool        owner_random = false; // a new random id, replaced by the derived one when it cannot be saved
};

static up_context & up_ctx() {
    static up_context * ctx = new up_context(); // never destroyed: detached helper threads may outlive main
    return *ctx;
}

// set once by admin_upload_init(), read-only afterwards
static const up_owner & up_fs_owner() {
    return up_ctx().owner;
}

// an owner id that needs no file: the same for the same host name, state directory and models directory, so a
// router whose state directory is not writable still recognises its own hints after a restart
static std::string up_derived_owner_id(const up_context & u) {
    char host[256] = {};
#if defined(_WIN32)
    DWORD n = sizeof(host) - 1;
    GetComputerNameA(host, &n);
#else
    gethostname(host, sizeof(host) - 1);
#endif
    const std::string text = std::string("provision-upload-owner\n") + host + "\n" +
                             admin_internal_path_to_utf8(admin_internal_state_dir()) + "\n" + u.models_dir_text + "\n";
    sha256_t sha;
    sha256_init(&sha);
    sha256_update(&sha, (const unsigned char *) text.data(), text.size());
    unsigned char digest[SHA256_DIGEST_SIZE];
    sha256_final(&sha, digest);
    return up_hex(digest, 16);
}

// at startup: the saved owner id, else a new one (random where the state directory is writable)
static void up_owner_init(up_context & u) {
    std::lock_guard<std::mutex> lk(u.mtx_owner);
    u.owner_saved  = false;
    u.owner_random = false;
    std::string text;
    if (!u.records_dir.empty() && admin_internal_read_file(u.records_dir / UP_OWNER_FILE, text, 4096)) {
        const std::string id = up_lower(text.substr(0, text.find_first_of("\r\n")));
        if (up_is_hex(id, 32)) {
            u.owner_id    = id;
            u.owner_saved = true;
            return;
        }
    }
    if (u.records_writable) {
        u.owner_id     = up_new_id();
        u.owner_random = true;
    } else {
        u.owner_id = up_derived_owner_id(u);
    }
}

// the owner id for a hint about to be written: saved next to the records the first time (one file of 33 bytes)
static std::string up_owner_id() {
    up_context & u = up_ctx();
    std::lock_guard<std::mutex> lk(u.mtx_owner);
    if (!u.owner_saved && !u.records_dir.empty()) {
        std::error_code ec;
        fs::create_directories(u.records_dir, ec);
        const fs::path path = u.records_dir / UP_OWNER_FILE;
        const fs::path tmp  = u.records_dir / (std::string(UP_OWNER_FILE) + ".tmp");
        if (admin_internal_write_file(tmp, u.owner_id + "\n", true)) {
            fs::rename(tmp, path, ec);
            u.owner_saved = !ec;
            if (ec) {
                fs::remove(tmp, ec);
            } else {
                admin_internal_fsync_dir(u.records_dir);
            }
        }
        if (!u.owner_saved && u.owner_random) {
            u.owner_id     = up_derived_owner_id(u); // stays the same across restarts without the file
            u.owner_random = false;
        }
    }
    return u.owner_id;
}

static std::string up_owner_id_now() {
    up_context & u = up_ctx();
    std::lock_guard<std::mutex> lk(u.mtx_owner);
    return u.owner_id;
}

void up_admission::release() {
    if (released.exchange(true)) {
        return;
    }
    up_context & u = up_ctx();
    std::shared_ptr<up_session> sp = s.lock();
    std::lock_guard<std::mutex> lk(u.mtx);
    if (u.admitted > 0) {
        u.admitted--;
    }
    if (sp) {
        sp->in_flight = false;
    }
}

//
// names (base design 3.10): the scanner's own rules, so a published model is listed under exactly this id
//

static bool up_name_valid(const std::string & name, std::string & reason) {
    if (name.empty()) {
        reason = "empty";
        return false;
    }
    if (name.size() > UP_NAME_MAX) {
        reason = "too_long";
        return false;
    }
    const auto alnum = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
    if (!alnum(name[0])) {
        reason = "charset";
        return false;
    }
    for (char c : name) {
        if (!alnum(c) && c != '.' && c != '_' && c != '-') {
            reason = "charset";
            return false;
        }
    }
    const std::string lower = up_lower(name);
    if (lower.find("mmproj") != std::string::npos) {
        reason = "mmproj"; // inside a folder such a file is taken as the projector
        return false;
    }
    if (lower.find(".gguf") != std::string::npos) {
        reason = "gguf"; // the scanner removes ".gguf" from ids
        return false;
    }
    for (const char * prefix : { "mtp-", "dspark-", "dflash-", "eagle3-" }) {
        if (string_starts_with(lower, prefix)) {
            reason = "draft_prefix"; // taken as a draft model by the scanner
            return false;
        }
    }
    for (size_t i = 0; i + 15 <= lower.size(); i++) {
        // -NNNNN-of-NNNNN
        const std::string s = lower.substr(i, 15);
        bool shard = s[0] == '-' && s.substr(6, 4) == "-of-";
        for (size_t k : { 1, 2, 3, 4, 5, 10, 11, 12, 13, 14 }) {
            shard = shard && s[k] >= '0' && s[k] <= '9';
        }
        if (shard) {
            reason = "shard_suffix";
            return false;
        }
    }
    if (name.back() == '.') {
        reason = "trailing_dot";
        return false;
    }
    const std::string base = lower.substr(0, lower.find('.'));
    static const std::set<std::string> reserved = {
        "con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
        "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9",
    };
    if (reserved.count(base)) {
        reason = "reserved";
        return false;
    }
    return true;
}

static std::string up_name_reason_text(const std::string & reason) {
    if (reason == "empty")        return "the name is empty";
    if (reason == "too_long")     return "the name is longer than 128 characters";
    if (reason == "charset")      return "the name may only use letters, digits, '.', '_' and '-' and must start with a letter or digit";
    if (reason == "mmproj")       return "the name must not contain 'mmproj' (such files are taken as projectors)";
    if (reason == "gguf")         return "the name must not contain '.gguf'";
    if (reason == "draft_prefix") return "the name must not start with mtp-, dspark-, dflash- or eagle3- (such files are taken as draft models)";
    if (reason == "shard_suffix") return "the name must not contain a shard suffix like -00001-of-00003";
    if (reason == "trailing_dot") return "the name must not end with '.'";
    if (reason == "reserved")     return "the name is reserved on Windows";
    return "the name is not allowed";
}

// the id the models_dir scanner gives a first-level entry ("" = none): the folder name, or the file name with
// every ".gguf" removed (common/preset.cpp)
static std::string up_scanner_id(const up_dirent & e) {
    if (e.st.is_dir || (e.st.is_link && !e.st.is_reg)) {
        return e.name;
    }
    if (!string_ends_with(e.name, ".gguf")) {
        return "";
    }
    if (e.name.find("mmproj") != std::string::npos) {
        return "";
    }
    for (const char * prefix : { "mtp-", "dspark-", "dflash-" }) {
        if (string_starts_with(e.name, prefix)) {
            return "";
        }
    }
    std::string id = e.name;
    string_replace_all(id, ".gguf", "");
    return id;
}

// throws 400 invalid_name or 409 name_taken; own_id: the session the name belongs to (its own name is free)
static void up_check_name(const std::string & name, const std::string & own_id) {
    std::string reason;
    if (!up_name_valid(name, reason)) {
        admin_http_error e(400, "invalid_name", up_name_reason_text(reason));
        e.param = "name";
        e.extra = { {"reason", reason} };
        throw e;
    }
    const std::string lower = up_lower(name);
    auto taken = [&](const std::string & other, const char * by) {
        if (up_lower(other) != lower) {
            return;
        }
        admin_http_error e(409, "name_taken", other == name ?
            string_format("the name '%s' is already used (%s)", name.c_str(), by) :
            string_format("the name '%s' differs only in case from '%s' (%s)", name.c_str(), other.c_str(), by));
        e.param = "name";
        e.extra = { {"by", by}, {"case_variant", other != name}, {"other", other} };
        throw e;
    };
    server_models_routes * routes = admin_internal_routes();
    if (routes != nullptr) {
        for (const auto & m : routes->models.get_all_meta()) {
            taken(m.name, m.source == SERVER_MODEL_SOURCE_PRESET ? "preset" : "model");
            for (const auto & alias : m.aliases) {
                taken(alias, "alias");
            }
        }
    }
    up_context & u = up_ctx();
    std::vector<up_dirent> entries;
    {
        std::lock_guard<std::mutex> lk(u.mtx);
        if (u.root.valid()) {
            entries = up_list(u.root);
        }
    }
    {
        for (const auto & e : entries) {
            if (e.name == UP_STAGING) {
                continue;
            }
            const std::string id = up_scanner_id(e);
            if (!id.empty()) {
                taken(id, "file");
            }
            taken(e.name, "file");
            if (string_ends_with(e.name, ".gguf")) {
                taken(e.name.substr(0, e.name.size() - 5), "file");
            }
        }
    }
    std::vector<std::shared_ptr<up_session>> others;
    {
        std::lock_guard<std::mutex> lk(u.mtx);
        for (const auto & [id, s] : u.sessions) {
            if (id != own_id) {
                others.push_back(s);
            }
        }
    }
    for (const auto & s : others) {
        std::string other;
        {
            std::lock_guard<std::mutex> lk(s->mtx);
            if (up_is_final(s->state)) {
                continue;
            }
            other = s->name;
        }
        taken(other, "upload");
    }
}

//
// session record (authoritative, <state>/uploads/<id>.json, private) and staging hint (<staging>/<id>/session.json:
// offsets only, a hint that is validated against the file lengths and the record)
//

static uint64_t up_total_bytes(const up_session & s) {
    uint64_t total = 0;
    for (const auto & f : s.files) {
        if (!f.dropped) {
            total += f.size;
        }
    }
    return total;
}

static uint64_t up_received_bytes(const up_session & s) {
    uint64_t total = 0;
    for (const auto & f : s.files) {
        if (!f.dropped) {
            total += f.offset;
        }
    }
    return total;
}

static json up_kv_json(const admin_kv & kv) {
    json out = json::object();
    for (const auto & [k, v] : kv) {
        out[k] = v;
    }
    return out;
}

static json up_record_json(const up_session & s) {
    json files = json::array();
    for (const auto & f : s.files) {
        json jf = {
            {"index",          f.index},
            {"name",           f.name},
            {"size",           f.size},
            {"last_modified",  f.last_modified},
            {"sha256_expected", f.sha_expected},
            {"durable_offset", f.durable},
            {"complete",       f.complete},
            {"dropped",        f.dropped},
            {"sha256",         f.sha_hex},
            {"head_crc32",     f.head_crc_valid && f.durable >= std::min<uint64_t>(f.size, UP_HEAD_CRC_BYTES) ? up_crc_text(f.head_crc) : std::string()},
            {"role",           f.role},
            {"final_name",     f.final_name},
        };
        jf["error"] = f.error;
        files.push_back(jf);
    }
    json warnings = json::array();
    for (const auto & w : s.warnings) {
        warnings.push_back(w);
    }
    json j = {
        {"version",        UP_RECORD_VERSION},
        {"id",             s.id},
        {"name",           s.name},
        {"state",          up_state_name(s.state)},
        {"blocked_by",     s.blocked_by},
        {"settings_mode",  s.settings_mode},
        {"auto_publish",   s.auto_publish},
        {"creator",        s.creator},
        {"creator_actor",  s.creator_actor.is_object() ? s.creator_actor : json(nullptr)},
        {"created",        s.created},
        {"updated",        s.updated},
        {"last_activity",  s.last_activity},
        {"idle_ms",        s.idle_ms},
        {"recovered_without_record", s.recovered_without_record},
        {"staged",         s.staged},
        {"staged_id",      s.staged_id},
    };
    j["blocked"]  = s.blocked;
    j["error"]    = s.error;
    j["settings"] = up_kv_json(s.settings);
    j["settings_request"] = s.settings_request;
    j["files"]    = files;
    j["plan"]     = s.plan;
    j["facts"]    = s.facts;
    j["result"]   = s.result;
    j["warnings"] = warnings;
    return j;
}

// owner: the router that writes it (another router sharing the models dir leaves the session alone)
static json up_hint_json(const up_session & s, const std::string & owner) {
    json files = json::array();
    for (const auto & f : s.files) {
        files.push_back({
            {"index",          f.index},
            {"name",           f.name},
            {"size",           f.size},
            {"durable_offset", f.durable},
            {"dropped",        f.dropped},
        });
    }
    json j = {
        {"version", UP_RECORD_VERSION},
        {"id",      s.id},
        {"name",    s.name},
        {"owner",   owner},
        {"idle_ms", s.idle_ms}, // the expiry of the session as far as it counted (while its router runs)
        {"note",    "upload in progress (Provision LLM Engine); do not edit, delete it from the admin page"},
    };
    j["files"] = files;
    return j;
}

// writes the record (and the staging hint) of a session; snapshots under s->mtx, writes outside it
static void up_persist(const std::shared_ptr<up_session> & sp) {
    up_context & u = up_ctx();
    std::string record;
    std::string hint;
    uint64_t seq = 0;
    bool write_hint = false;
    std::string id;
    const std::string owner = up_owner_id();
    {
        std::lock_guard<std::mutex> lk(sp->mtx);
        sp->updated = admin_internal_wall_ms();
        record = up_record_json(*sp).dump(1) + "\n";
        write_hint = !up_is_final(sp->state) && sp->dir.valid();
        if (write_hint) {
            hint = up_hint_json(*sp, owner).dump(1) + "\n";
        }
        seq = ++sp->record_seq;
        id  = sp->id;
    }
    std::lock_guard<std::mutex> lk_rec(sp->mtx_record);
    if (seq <= sp->record_written) {
        return; // a newer snapshot was written already
    }
    sp->record_written = seq;
    bool ok = false;
    if (!u.records_dir.empty()) {
        std::error_code ec;
        fs::create_directories(u.records_dir, ec);
        const fs::path path = u.records_dir / (id + ".json");
        const fs::path tmp  = u.records_dir / (id + ".json.tmp");
        if (admin_internal_write_file(tmp, record, true)) {
            fs::rename(tmp, path, ec);
            ok = !ec;
            if (ec) {
                fs::remove(tmp, ec);
            } else {
                // the rename itself durable too: the state dir may be another file system than the staged data,
                // whose renames are synced (a record that lags behind them would restart a staged upload at 0)
                admin_internal_fsync_dir(u.records_dir);
            }
        }
    }
    {
        std::lock_guard<std::mutex> lk(sp->mtx);
        sp->record_persisted = ok;
    }
    if (write_hint) {
        // group-readable, in the staging folder (no 0700 there: every router sharing the models dir must list it)
        int err = 0;
        const std::string tmp_name = std::string(UP_HINT_FILE) + ".tmp";
        std::shared_ptr<up_session> keep = sp;
        const int fd = up_file_open(sp->dir, tmp_name, UP_OPEN_CREATE, err);
        if (fd >= 0) {
            bool w = up_truncate(fd, 0, err) && up_pwrite_all(fd, 0, hint.data(), hint.size(), err) && up_datasync(fd, err);
            up_apply_owner_fd(fd, u.owner, false, nullptr);
            w = up_close(fd, err) && w;
            if (w) {
                up_rename_at(sp->dir, tmp_name, sp->dir, UP_HINT_FILE, err, true);
            }
        }
    }
}

static void up_remove_record(const std::string & id) {
    up_context & u = up_ctx();
    if (u.records_dir.empty()) {
        return;
    }
    std::error_code ec;
    fs::remove(u.records_dir / (id + ".json"), ec);
}

static void up_wake_worker() {
    up_context & u = up_ctx();
    {
        std::lock_guard<std::mutex> lk(u.mtx);
        u.wake = true;
    }
    u.cv.notify_all();
}

//
// JSON views
//

static json up_file_json(const up_file & f) {
    json j = {
        {"index",          f.index},
        {"name",           f.name},
        {"size",           f.size},
        {"offset",         f.offset},
        {"durable_offset", f.durable},
        {"complete",       f.complete},
        {"dropped",        f.dropped},
        {"role",           f.role.empty() ? json(nullptr) : json(f.role)},
        {"sha256",         f.sha_hex.empty() ? json(nullptr) : json(f.sha_hex)},
        {"sha256_expected", f.sha_expected.empty() ? json(nullptr) : json(f.sha_expected)},
        {"head_crc32",     f.head_crc_valid && f.offset >= std::min<uint64_t>(f.size, UP_HEAD_CRC_BYTES) ? json(up_crc_text(f.head_crc)) : json(nullptr)},
        {"final_name",     f.final_name.empty() ? json(nullptr) : json(f.final_name)},
    };
    json head = {
        {"state", up_head_name(f.head_state)},
    };
    if (f.head.status == UP_SCAN_OK) {
        head["arch"] = f.head.arch.empty() ? json(nullptr) : json(f.head.arch);
        head["type"] = f.head.type.empty() ? json(nullptr) : json(f.head.type);
        if (f.head.split_count > 1) {
            head["split"] = { {"no", f.head.split_no}, {"count", f.head.split_count} };
        }
    }
    if (f.head_result.is_object()) {
        const std::string msg = up_json_str(f.head_result, "message");
        if (!msg.empty()) {
            head["message"] = msg;
        }
    }
    j["header"] = head;
    j["error"]  = f.error;
    return j;
}

static json up_session_json(const up_session & s, bool full) {
    const int64_t now = admin_internal_now_ms();
    const int64_t ttl = up_ttl_ms();
    json j = {
        {"id",             s.id},
        {"name",           s.name},
        {"state",          up_state_name(s.state)},
        {"substate",       s.substate.empty() ? json(nullptr) : json(s.substate)},
        {"blocked_by",     s.blocked_by.empty() ? json(nullptr) : json(s.blocked_by)},
        {"total_bytes",    up_total_bytes(s)},
        {"received_bytes", up_received_bytes(s)},
        {"n_files",        s.files.size()},
        {"auto_publish",   s.auto_publish},
        {"created",        up_iso(s.created)},
        {"last_activity",  up_iso(s.last_activity)},
    };
    j["error"]   = s.error;
    j["blocked"] = s.blocked;
    const bool counting = s.state == UP_RECEIVING || s.state == UP_READY;
    j["expires_at"] = counting ? json(up_iso(admin_internal_wall_ms() + std::max<int64_t>(0, ttl - s.idle_ms))) : json(nullptr);
    if (!full) {
        return j;
    }
    json files = json::array();
    for (const auto & f : s.files) {
        files.push_back(up_file_json(f));
    }
    json warnings = json::array();
    for (const auto & w : s.warnings) {
        warnings.push_back(w);
    }
    j["files"]          = files;
    j["settings"]       = up_kv_json(s.settings);
    j["settings_mode"]  = s.settings_mode;
    j["plan"]           = s.plan;
    j["facts"]          = s.facts;
    j["result"]         = s.result;
    j["warnings"]       = warnings;
    j["idle_ms"]        = s.idle_ms;
    j["last_client_idle_ms"] = s.t_client > 0 ? json(now - s.t_client) : json(nullptr);
    j["recovered"]      = s.recovered;
    j["recovered_without_record"] = s.recovered_without_record;
    j["record_persisted"] = s.record_persisted;
    j["chunk_max"]      = UP_CHUNK_MAX;
    j["chunk_default"]  = UP_CHUNK_DEFAULT;
    if (s.state == UP_CHECKING && s.substate == "hashing") {
        j["progress"] = { {"done_bytes", s.hash_done}, {"total_bytes", s.hash_total} };
    } else {
        j["progress"] = nullptr;
    }
    json checks = json::array();
    for (const auto & f : s.files) {
        if (f.dropped) {
            continue;
        }
        if (f.head_state != UP_HEAD_NONE) {
            checks.push_back({
                {"step",    "header"},
                {"file",    f.index},
                {"status",  up_head_name(f.head_state)},
                {"message", up_json_str(f.head_result, "message")},
            });
        }
    }
    j["checks"] = checks;
    return j;
}

static json up_limits_json() {
    return {
        {"chunk_max",     UP_CHUNK_MAX},
        {"chunk_default", UP_CHUNK_DEFAULT},
        {"chunk_min",     UP_CHUNK_MIN},
        {"max_sessions",  UP_MAX_SESSIONS},
        {"max_writers",   UP_MAX_WRITERS},
        {"max_files",     UP_MAX_FILES},
        {"ttl_h",         (long long) (up_ttl_ms() / 3600000)},
        {"ttl_s",         (long long) (up_ttl_ms() / 1000)},
        {"max_header_bytes", UP_MAX_HEADER},
    };
}

//
// availability of uploads (base design 3.12): a writable --models-dir; staging on the same file system
//

static void up_set_unavailable(up_context & u, const char * reason, const std::string & message) {
    u.available = false;
    u.reason    = reason;
    u.message   = message;
}

// (re)opens models_dir and the staging folder; caller holds no lock (it takes u.mtx for the fields).
// create_staging: the first upload (or delete) creates <models_dir>/.provision-upload; until then a models dir
// stays as the operator left it, and an existing staging folder (an earlier run) is only opened
static void up_open_dirs(up_context & u, bool create_staging) {
    up_dir root;
    up_dir staging;
    int err = 0;
    std::string reason;
    std::string message;
    if (u.models_dir.empty()) {
        reason  = "no_models_dir";
        message = "this server has no --models-dir: uploads need one (Docker: the entrypoint could not create /models-rw/local, see the container log; "
                  "host: add --models-dir to PROVISION_LLM_ARGS), then restart";
    } else if (!up_dir_open_root(u.models_dir, root, err)) {
        reason  = "models_dir_missing";
        message = "the models directory " + u.models_dir_text + " does not exist or cannot be opened: " + up_errno_text(err);
    } else if (!admin_internal_dir_writable(u.models_dir)) {
        reason  = "not_writable";
        message = "the models directory " + u.models_dir_text + " is not writable for the server";
    } else {
        up_stat st;
        const bool exists = up_stat_at(root, UP_STAGING, st);
        if (exists && (st.is_link || !st.is_dir)) {
            reason  = "staging_unusable";
            message = std::string(UP_STAGING) + " in the models directory is not a folder (a symlink or a file): remove it";
        } else if (!exists && !create_staging) {
            // available: the folder is created by the first upload
        } else if (!up_dir_open(root, UP_STAGING, true, 0770, staging, err)) {
            reason  = err == EACCES || err == EROFS ? "not_writable" : "staging_unusable";
            message = std::string("cannot create ") + UP_STAGING + " in the models directory: " + up_errno_text(err);
        } else {
            up_stat a;
            up_stat b;
            if (up_dir_stat(root, a) && up_dir_stat(staging, b) && a.dev != b.dev) {
                reason  = "staging_other_filesystem";
                message = std::string(UP_STAGING) + " is on another file system than the models directory (something is mounted over it)";
            }
        }
    }
    if (reason.empty() && staging.valid()) {
        up_apply_owner_dir(staging, u.owner, nullptr);
    }
    std::lock_guard<std::mutex> lk(u.mtx);
    if (reason.empty()) {
        u.available = true;
        u.reason.clear();
        u.message.clear();
        u.root = std::move(root);
        if (staging.valid()) {
            u.staging = std::move(staging);
        }
    } else {
        up_set_unavailable(u, reason.c_str(), message);
        if (root.valid()) {
            u.root = std::move(root);
        }
    }
}

// throws 409 upload_unavailable
static void up_require_available() {
    up_context & u = up_ctx();
    bool ok = false;
    {
        std::lock_guard<std::mutex> lk(u.mtx);
        ok = u.available && u.staging.valid();
    }
    if (!ok) {
        up_open_dirs(u, true); // a directory that appeared or became writable since; the staging folder now
    }
    std::lock_guard<std::mutex> lk(u.mtx);
    if (!u.available || !u.staging.valid()) {
        admin_http_error e(409, "upload_unavailable", u.message);
        e.extra = { {"reason", u.reason} };
        throw e;
    }
}

//
// session lookup
//

static std::shared_ptr<up_session> up_find(const std::string & id) {
    up_context & u = up_ctx();
    std::lock_guard<std::mutex> lk(u.mtx);
    const auto it = u.sessions.find(id);
    return it != u.sessions.end() ? it->second : nullptr;
}

// ?id= or "id" in a body: only ^[0-9a-f]{32}$, never joined into a path before it was found in the table
static std::shared_ptr<up_session> up_find_or_throw(const std::string & id) {
    if (!up_is_hex(id, 32)) {
        admin_http_error e(400, "invalid_request_error", "id must be an upload id (32 hex digits)");
        e.param = "id";
        throw e;
    }
    std::shared_ptr<up_session> s = up_find(up_lower(id));
    if (!s) {
        admin_http_error e(410, "upload_gone", "this upload does not exist (any more): it expired, was cancelled, or the id is wrong");
        e.extra = { {"state", nullptr} };
        throw e;
    }
    return s;
}

static int up_count_unfinished() {
    up_context & u = up_ctx();
    std::vector<std::shared_ptr<up_session>> all;
    {
        std::lock_guard<std::mutex> lk(u.mtx);
        for (const auto & [id, s] : u.sessions) {
            all.push_back(s);
        }
    }
    int n = 0;
    for (const auto & s : all) {
        std::lock_guard<std::mutex> lk(s->mtx);
        n += up_is_final(s->state) ? 0 : 1;
    }
    return n;
}

//
// moving a session to a final state; its data is removed by the worker (never on the events thread, never
// under the apply mutex)
//

static void up_finish(const std::shared_ptr<up_session> & sp, up_state state, const json & error, const server_http_req * req,
                      const char * action) {
    up_context & u = up_ctx();
    std::string id;
    std::string name;
    json details = json::object();
    json creator;
    {
        std::lock_guard<std::mutex> lk(sp->mtx);
        if (up_is_final(sp->state)) {
            return;
        }
        sp->state    = state;
        sp->substate.clear();
        sp->error    = error;
        sp->t_final  = admin_internal_now_ms();
        sp->last_activity = admin_internal_wall_ms();
        id   = sp->id;
        name = sp->name;
        creator = sp->creator_actor;
        details["id"]    = id;
        details["bytes"] = up_received_bytes(*sp);
        if (error.is_object()) {
            details["code"] = up_json_str(error, "code");
            if (error.contains("file") && error.at("file").is_number_integer()) {
                details["file"] = error.at("file");
            }
        }
    }
    up_persist(sp);
    {
        std::lock_guard<std::mutex> lk(u.mtx);
        u.cleanup.push_back(id);
    }
    up_wake_worker();
    const char * result = state == UP_REJECTED ? "rejected" : "ok";
    if (req == nullptr && state == UP_REJECTED && creator.is_object()) {
        admin_audit_for(creator, action, name, result, details); // the checks of the creator's upload refused it
    } else {
        admin_audit(req, action, name, result, details);
    }
}

//
// the chunk path
//

static std::string up_header(const server_http_req & req, const char * name) {
    const std::string want = up_lower(name);
    for (const auto & [k, v] : req.headers) {
        if (up_lower(k) == want) {
            return v;
        }
    }
    return "";
}

[[noreturn]] static void up_throw(int status, const char * type, const std::string & message, const json & extra = json()) {
    admin_http_error e(status, type, message);
    if (extra.is_object()) {
        e.extra = extra;
    }
    throw e;
}

// the cheap refusals, before the body is read (pre-body hook) and again in the handler
// returns the session; throws admin_http_error
static std::shared_ptr<up_session> up_chunk_precheck(const server_http_req & req, bool in_handler) {
    const std::string id = req.get_param("id");
    std::shared_ptr<up_session> sp = up_find_or_throw(id);
    std::string client = up_header(req, "X-Upload-Client");
    const bool takeover = req.get_param("takeover") == "1";
    std::lock_guard<std::mutex> lk(sp->mtx);
    if (up_is_final(sp->state)) {
        up_throw(410, "upload_gone", string_format("this upload is %s", up_state_name(sp->state)), { {"state", up_state_name(sp->state)} });
    }
    if (sp->state != UP_RECEIVING) {
        up_throw(409, "upload_busy", string_format("this upload is %s: no more data is accepted", up_state_name(sp->state)),
                 { {"state", up_state_name(sp->state)} });
    }
    if (!in_handler && !client.empty() && !sp->client.empty() && client != sp->client && !takeover && sp->t_client > 0) {
        const int64_t idle = admin_internal_now_ms() - sp->t_client;
        if (idle < UP_CLIENT_MS) {
            up_throw(409, "other_client_active", "another browser tab or client is uploading this file right now; take over with takeover=1",
                     { {"idle_ms", idle} });
        }
    }
    return sp;
}

// pre-body hook of the chunk route (server_http_context::set_pre_body_handler): answers the cheap refusals
// before httplib reads the body, and admits at most one body per session and UP_MAX_WRITERS in all, so the
// memory a chunk costs is bounded by the number of sessions, not by the number of requests
static server_http_res_ptr up_pre_body(const server_http_req & req, std::shared_ptr<void> & hold) {
    server_http_res_ptr refused = admin_internal_refusal(req);
    if (refused) {
        return refused;
    }
    try {
        // the length decides what is admitted, so it must be the real framing of the body: a Content-Length, and no
        // transfer coding next to it (httplib accepts "Content-Length: 0" with "Transfer-Encoding: chunked" and then
        // reads the chunked body; without a length it reads until the connection closes) and no content coding (a
        // compressed body is larger once decoded)
        const std::string len_text = up_header(req, "Content-Length");
        uint64_t len = 0;
        if (!up_header(req, "Transfer-Encoding").empty() || len_text.empty() || !up_parse_u64(len_text, len)) {
            up_throw(411, "length_required", "a chunk needs a Content-Length (no chunked transfer encoding)");
        }
        const std::string coding = up_lower(up_header(req, "Content-Encoding"));
        if (!coding.empty() && coding != "identity") {
            up_throw(415, "unsupported_media_type", "a chunk must be sent as raw bytes (no Content-Encoding)");
        }
        if (len > UP_CHUNK_MAX) {
            up_throw(413, "chunk_too_large", string_format("a chunk may hold at most %llu bytes", (unsigned long long) UP_CHUNK_MAX),
                     { {"chunk_max", UP_CHUNK_MAX} });
        }
        std::shared_ptr<up_session> sp = up_chunk_precheck(req, false);
        up_context & u = up_ctx();
        auto adm = std::make_shared<up_admission>();
        adm->s = sp;
        {
            std::lock_guard<std::mutex> lk(u.mtx);
            if (sp->in_flight) {
                adm->released.store(true); // nothing was admitted
                up_throw(409, "chunk_in_progress", "a chunk of this upload is being received right now; send one chunk at a time");
            }
            if (u.admitted >= UP_MAX_WRITERS) {
                adm->released.store(true);
                up_throw(429, "upload_slots_busy", "the server is receiving the maximum number of chunks at once; retry shortly",
                         { {"retry_after_s", 2} });
            }
            u.admitted++;
            sp->in_flight = true;
            sp->adm = adm;
        }
        hold = adm;
    } catch (const admin_http_error & e) {
        return admin_internal_error(e.status, e.type, e.what(), e.param, e.extra);
    } catch (const std::exception & e) {
        return admin_internal_error(500, "server_error", e.what());
    }
    return nullptr;
}

// the handler is done with the body: the next chunk of this session may be admitted at once
static void up_release_admission(const std::shared_ptr<up_session> & sp) {
    std::shared_ptr<up_admission> adm;
    {
        std::lock_guard<std::mutex> lk(up_ctx().mtx);
        adm = sp->adm.lock();
    }
    if (adm) {
        adm->release();
    }
}

struct up_admission_guard {
    std::shared_ptr<up_session> sp;
    ~up_admission_guard() {
        if (sp) {
            up_release_admission(sp);
        }
    }
};

// runs the header scan of a file when enough bytes arrived; caller holds mtx_io (not mtx)
static void up_maybe_scan_header(const std::shared_ptr<up_session> & sp, int file_index, int fd, uint64_t offset, bool complete) {
    up_file * f = nullptr;
    {
        std::lock_guard<std::mutex> lk(sp->mtx);
        f = &sp->files[(size_t) file_index];
        if (f->head_state != UP_HEAD_NONE && f->head_state != UP_HEAD_SCANNING) {
            return;
        }
        if (offset < f->head_need && !complete) {
            return;
        }
    }
    up_reader r;
    r.avail   = offset;
    r.read_at = [fd](uint64_t off, void * buf, size_t n) {
        int err = 0;
        return up_pread_all(fd, off, (char *) buf, n, err);
    };
    const up_gguf_info g = up_gguf_scan(r, complete);
    bool queued = false;
    {
        std::lock_guard<std::mutex> lk(sp->mtx);
        f = &sp->files[(size_t) file_index];
        f->head = g;
        if (g.status == UP_SCAN_NEED_MORE) {
            f->head_state = UP_HEAD_SCANNING;
            f->head_need  = g.need;
        } else if (g.status == UP_SCAN_INVALID) {
            f->head_state = UP_HEAD_ERROR;
            const std::string code = g.code == "unsupported_gguf_version" ? "not_gguf" : g.code;
            f->error = { {"code", code}, {"message", g.message} };
        } else {
            f->head_state = UP_HEAD_QUEUED;
            f->role       = g.arch == "clip" ? "projector" : "model";
            queued = true;
        }
    }
    if (queued) {
        up_wake_worker();
    }
}

static server_http_res_ptr up_handle_chunk(const server_http_req & req) {
    std::shared_ptr<up_session> sp = up_chunk_precheck(req, true);
    up_admission_guard guard{ sp };

    const std::string ct = up_lower(up_header(req, "Content-Type"));
    if (!string_starts_with(ct, "application/octet-stream")) {
        up_throw(415, "unsupported_media_type", "a chunk must be sent as Content-Type: application/octet-stream");
    }
    if (req.body.size() > UP_CHUNK_MAX) {
        up_throw(413, "chunk_too_large", string_format("a chunk may hold at most %llu bytes", (unsigned long long) UP_CHUNK_MAX),
                 { {"chunk_max", UP_CHUNK_MAX} });
    }
    uint64_t file_index = 0;
    uint64_t offset = 0;
    if (!up_parse_u64(req.get_param("file"), file_index)) {
        admin_http_error e(400, "invalid_request_error", "file must be the index of a file of the upload");
        e.param = "file";
        throw e;
    }
    if (!up_parse_u64(req.get_param("offset"), offset)) {
        admin_http_error e(400, "invalid_request_error", "offset must be a byte offset");
        e.param = "offset";
        throw e;
    }
    const bool verify   = req.get_param("verify") == "1";
    const bool takeover = req.get_param("takeover") == "1";
    const std::string client = up_header(req, "X-Upload-Client");
    const std::string crc_text = up_lower(up_header(req, "X-Chunk-CRC32"));
    const char * body = req.body.data();
    const uint64_t len = (uint64_t) req.body.size();

    std::unique_lock<std::mutex> lk_io(sp->mtx_io, std::try_to_lock);
    if (!lk_io.owns_lock()) {
        up_throw(409, "chunk_in_progress", "a chunk of this upload is being written right now; send one chunk at a time");
    }
    if (!crc_text.empty()) {
        if (!up_is_hex(crc_text, 8)) {
            admin_http_error e(400, "invalid_request_error", "X-Chunk-CRC32 must be 8 hex digits");
            e.param = "X-Chunk-CRC32";
            throw e;
        }
        if (up_crc_text(up_crc32_update(0, (const unsigned char *) body, (size_t) len)) != crc_text) {
            up_throw(400, "crc_mismatch", "the chunk was damaged on the way (X-Chunk-CRC32 does not match): send it again");
        }
    }

    uint64_t current = 0;
    uint64_t size = 0;
    bool took_over = false;
    std::string name;
    {
        std::lock_guard<std::mutex> lk(sp->mtx);
        if (sp->state != UP_RECEIVING) {
            up_throw(sp->state == UP_CHECKING || sp->state == UP_PUBLISHING || sp->state == UP_READY ? 409 : 410,
                     sp->state == UP_CHECKING || sp->state == UP_PUBLISHING || sp->state == UP_READY ? "upload_busy" : "upload_gone",
                     string_format("this upload is %s", up_state_name(sp->state)), { {"state", up_state_name(sp->state)} });
        }
        if (file_index >= sp->files.size()) {
            admin_http_error e(400, "invalid_request_error", "file must be the index of a file of the upload");
            e.param = "file";
            throw e;
        }
        up_file & f = sp->files[(size_t) file_index];
        if (f.dropped) {
            up_throw(409, "file_dropped", "this file was dropped from the upload");
        }
        if (f.error.is_object()) {
            json extra = { {"file", f.index}, {"reason", up_json_str(f.error, "code")} };
            up_throw(422, up_json_str(f.error, "code") == "not_gguf" ? "not_gguf" : "file_rejected",
                     "this file cannot be used: " + up_json_str(f.error, "message") + " (drop it from the upload or cancel)", extra);
        }
        if (!client.empty() && !sp->client.empty() && client != sp->client && sp->t_client > 0 && !verify) {
            const int64_t idle = admin_internal_now_ms() - sp->t_client;
            if (idle < UP_CLIENT_MS && !takeover) {
                up_throw(409, "other_client_active", "another browser tab or client is uploading this file right now; take over with takeover=1",
                         { {"idle_ms", idle} });
            }
            took_over = true;
        }
        current = f.offset;
        size    = f.size;
        name    = sp->name;
    }
    if (took_over) {
        admin_audit(&req, "upload.takeover", name, "ok", { {"id", sp->id} });
    }

    if (offset > current) {
        up_throw(409, "offset_mismatch", string_format("this file has %llu bytes so far: continue at that offset", (unsigned long long) current),
                 { {"offset", current} });
    }
    if (offset + len > size) {
        up_throw(400, "beyond_declared_size", string_format("the chunk ends at %llu, after the declared size %llu of the file",
                 (unsigned long long) (offset + len), (unsigned long long) size));
    }

    const std::string part = std::to_string(file_index) + ".part";
    int err = 0;
    const int fd = up_file_open(sp->dir, part, UP_OPEN_WRITE, err);
    if (fd < 0) {
        up_throw(500, "write_failed", "the staged file cannot be opened: " + up_errno_text(err), { {"offset", current} });
    }
    struct fd_closer {
        int fd;
        bool closed = false;
        ~fd_closer() {
            if (!closed) {
                int e = 0;
                up_close(fd, e);
            }
        }
    } closer{ fd };

    // the part of the chunk that is already stored must be the same bytes (a retry, or the overlap of a resume)
    const uint64_t overlap_end = std::min(current, offset + len);
    if (overlap_end > offset) {
        std::vector<char> stored((size_t) std::min<uint64_t>(overlap_end - offset, 4 * UP_MIB));
        uint64_t pos = offset;
        while (pos < overlap_end) {
            const size_t n = (size_t) std::min<uint64_t>(overlap_end - pos, stored.size());
            if (!up_pread_all(fd, pos, stored.data(), n, err)) {
                up_throw(500, "write_failed", "the staged file cannot be read: " + up_errno_text(err), { {"offset", current} });
            }
            if (memcmp(stored.data(), body + (pos - offset), n) != 0) {
                up_throw(409, "content_mismatch", "these bytes differ from the ones already received: this is not the same file",
                         { {"offset", pos} });
            }
            pos += n;
        }
    }
    if (verify) {
        std::lock_guard<std::mutex> lk(sp->mtx);
        const up_file & f = sp->files[(size_t) file_index];
        return admin_internal_json(200, {
            {"verified",       true},
            {"verified_bytes", overlap_end > offset ? overlap_end - offset : 0},
            {"offset",         f.offset},
            {"durable_offset", f.durable},
            {"file_complete",  f.complete},
            {"state",          up_state_name(sp->state)},
        });
    }

    // the tail beyond what is stored (none for a chunk that ends inside the stored bytes: a late retry, or a resume
    // overlap longer than the chunk)
    const uint64_t tail_off = std::max(current, offset);
    const uint64_t tail_len = offset + len > tail_off ? offset + len - tail_off : 0;
    uint64_t durable = 0;
    {
        std::lock_guard<std::mutex> lk(sp->mtx);
        durable = sp->files[(size_t) file_index].durable;
    }
    // any write, sync or close error: back to the durable offset, the incremental hash is dropped
    auto io_failed = [&](int e, const char * what) {
        int ignored = 0;
        up_truncate(fd, durable, ignored);
        {
            std::lock_guard<std::mutex> lk(sp->mtx);
            up_file & f = sp->files[(size_t) file_index];
            f.offset    = durable;
            f.sha_valid = false;
            if (f.durable < std::min<uint64_t>(f.size, UP_HEAD_CRC_BYTES)) {
                f.head_crc_valid = false;
            }
        }
        up_persist(sp);
        uint64_t total = 0;
        uint64_t free_bytes = 0;
        admin_internal_dir_space(up_ctx().models_dir, total, free_bytes);
        if (up_is_disk_full(e)) {
            SRV_WRN("admin: upload %s: %s failed (%s), paused at %llu bytes\n", sp->id.c_str(), what, up_errno_text(e).c_str(),
                    (unsigned long long) durable);
            up_throw(507, "disk_full", "the disk is full: free space, then resume the upload (or cancel it to free its space)",
                     { {"free_bytes", free_bytes}, {"offset", durable}, {"durable_offset", durable} });
        }
        SRV_ERR("admin: upload %s: %s failed: %s\n", sp->id.c_str(), what, up_errno_text(e).c_str());
        up_throw(500, "write_failed", std::string("the chunk could not be written: ") + up_errno_text(e), { {"offset", durable} });
    };
    if (tail_len > 0) {
        // a chunk that does not fit in the space a non-privileged writer may use is refused before it is written
        // (a router running as root must not fill the reserve of the file system)
        uint64_t total = 0;
        uint64_t free_bytes = 0;
        admin_internal_dir_space(up_ctx().models_dir, total, free_bytes);
        if (total > 0 && free_bytes < tail_len) {
            uint64_t now_durable = 0;
            {
                std::lock_guard<std::mutex> lk(sp->mtx);
                now_durable = sp->files[(size_t) file_index].durable;
            }
            up_throw(507, "disk_full", "the disk is full: free space, then resume the upload (or cancel it to free its space)",
                     { {"free_bytes", free_bytes}, {"offset", current}, {"durable_offset", now_durable} });
        }
        const int injected = up_test_write_failure(tail_off + tail_len, false);
        if (injected != 0) {
            io_failed(injected, "write");
        }
        if (!up_pwrite_all(fd, tail_off, body + (tail_off - offset), (size_t) tail_len, err)) {
            io_failed(err, "write");
        }
    }

    // accepted: hash and head CRC over the new bytes only (after the write succeeded)
    bool sync_now = false;
    bool file_complete = false;
    uint64_t new_offset = 0;
    {
        std::lock_guard<std::mutex> lk(sp->mtx);
        up_file & f = sp->files[(size_t) file_index];
        if (tail_len > 0) {
            if (f.sha_valid) {
                sha256_update(&f.sha, (const unsigned char *) body + (tail_off - offset), (size_t) tail_len);
            }
            if (f.head_crc_valid && tail_off < UP_HEAD_CRC_BYTES) {
                const uint64_t n = std::min<uint64_t>(tail_len, UP_HEAD_CRC_BYTES - tail_off);
                f.head_crc = up_crc32_update(f.head_crc, (const unsigned char *) body + (tail_off - offset), (size_t) n);
            }
            f.offset = tail_off + tail_len;
        }
        new_offset    = f.offset;
        file_complete = f.offset == f.size;
        const int64_t now = admin_internal_now_ms();
        sync_now = file_complete || f.offset - f.durable >= up_checkpoint() || (f.offset > f.durable && now - f.t_sync >= UP_CHECKPOINT_MS);
        if (tail_len > 0) {
            sp->idle_ms = 0;
            sp->last_activity = admin_internal_wall_ms();
        }
        if (!client.empty()) {
            sp->client = client;
        }
        sp->t_client = now;
    }
    if (sync_now) {
        const int injected = up_test_write_failure(new_offset, true);
        if (injected != 0) {
            io_failed(injected, "fdatasync");
        }
        if (!up_datasync(fd, err)) {
            io_failed(err, "fdatasync");
        }
        up_drop_cache(fd, durable, new_offset - durable);
    }
    up_maybe_scan_header(sp, (int) file_index, fd, new_offset, file_complete);
    json scan_error;
    {
        std::lock_guard<std::mutex> lk(sp->mtx);
        scan_error = sp->files[(size_t) file_index].error;
    }
    if (scan_error.is_object()) {
        // not a usable GGUF file (seen in its first bytes): its data is dropped at once, the other files stay
        int ignored = 0;
        up_truncate(fd, 0, ignored);
        {
            std::lock_guard<std::mutex> lk(sp->mtx);
            up_file & f = sp->files[(size_t) file_index];
            f.offset = f.durable = 0;
            f.complete = false;
            sha256_init(&f.sha);
            f.sha_valid = true;
            f.sha_hex.clear();
            f.head_crc = 0;
            f.head_crc_valid = true;
        }
        up_persist(sp);
        const std::string code = up_json_str(scan_error, "code");
        up_throw(422, code == "not_gguf" ? "not_gguf" : "file_rejected",
                 "this file cannot be used: " + up_json_str(scan_error, "message") + " (drop it from the upload or cancel)",
                 { {"file", (int) file_index}, {"reason", code} });
    }
    closer.closed = true;
    if (!up_close(fd, err)) {
        io_failed(err, "close");
    }

    bool all_complete = false;
    bool persist = false;
    json out;
    {
        std::lock_guard<std::mutex> lk(sp->mtx);
        up_file & f = sp->files[(size_t) file_index];
        if (sync_now) {
            f.durable = new_offset;
            f.t_sync  = admin_internal_now_ms();
            persist   = true;
        }
        if (file_complete && !f.complete) {
            f.complete = true;
            persist    = true;
            if (f.sha_valid) {
                sha256_t copy = f.sha;
                unsigned char digest[SHA256_DIGEST_SIZE];
                sha256_final(&copy, digest);
                f.sha_hex = up_hex(digest, sizeof(digest));
            }
        }
        all_complete = true;
        for (const auto & g : sp->files) {
            all_complete = all_complete && (g.dropped || (g.complete && !g.error.is_object()));
        }
        if (all_complete && sp->state == UP_RECEIVING) {
            sp->state    = UP_CHECKING;
            sp->substate = "queued";
            persist = true;
            int64_t delay = 0;
            {
                std::lock_guard<std::mutex> lk_t(g_test_mtx);
                delay = g_test.check_delay_ms;
            }
            sp->t_checks_after = admin_internal_now_ms() + delay;
        }
        out = {
            {"offset",         f.offset},
            {"durable_offset", f.durable},
            {"file_complete",  f.complete},
            {"state",          up_state_name(sp->state)},
            {"file",           f.index},
            {"received_bytes", up_received_bytes(*sp)},
            {"total_bytes",    up_total_bytes(*sp)},
        };
        out["file_error"] = f.error;
    }
    if (all_complete) {
        up_fsync_dir(sp->dir);
    }
    if (persist) {
        up_persist(sp);
    }
    if (all_complete) {
        up_wake_worker();
    }
    return admin_internal_json(200, out);
}

//
// GET / POST /admin/models/upload, update, cancel, publish
//

static json up_disk_json(uint64_t needed, const std::string & own_id, std::vector<std::string> & warnings) {
    up_context & u = up_ctx();
    uint64_t total = 0;
    uint64_t free_bytes = 0;
    admin_internal_dir_space(u.models_dir, total, free_bytes);
    uint64_t others = 0;
    std::vector<std::shared_ptr<up_session>> all;
    {
        std::lock_guard<std::mutex> lk(u.mtx);
        for (const auto & [id, s] : u.sessions) {
            if (id != own_id) {
                all.push_back(s);
            }
        }
    }
    for (const auto & s : all) {
        std::lock_guard<std::mutex> lk(s->mtx);
        if (!up_is_final(s->state)) {
            others += up_total_bytes(*s) - std::min(up_total_bytes(*s), up_received_bytes(*s));
        }
    }
    if (total > 0 && needed > total) {
        warnings.push_back("larger_than_disk");
    } else if (total > 0 && needed + others > free_bytes) {
        warnings.push_back("not_enough_space");
    }
    json shares = json::array();
#if !defined(_WIN32)
    struct stat a;
    if (!u.models_dir.empty() && ::stat(u.models_dir.c_str(), &a) == 0) {
        struct stat b;
        const fs::path state_dir = admin_internal_state_dir();
        if (!state_dir.empty() && ::stat(state_dir.c_str(), &b) == 0 && a.st_dev == b.st_dev) {
            shares.push_back("admin_state");
        }
    }
#endif
    if (!shares.empty()) {
        warnings.push_back("shares_disk_with_state");
    }
    return {
        {"free_bytes",          free_bytes},
        {"total_bytes",         total},
        {"needed_bytes",        needed},
        {"other_uploads_bytes", others},
        {"shares_disk_with",    shares},
    };
}

static void up_validate_files(const json & body, std::vector<up_file> & files) {
    if (!body.contains("files") || !body.at("files").is_array() || body.at("files").empty()) {
        admin_http_error e(400, "invalid_request_error", "files must be a non-empty list of {name, size}");
        e.param = "files";
        throw e;
    }
    const json & list = body.at("files");
    if (list.size() > UP_MAX_FILES) {
        admin_http_error e(400, "invalid_request_error", string_format("an upload holds at most %zu files", UP_MAX_FILES));
        e.param = "files";
        throw e;
    }
    for (size_t i = 0; i < list.size(); i++) {
        const json & jf = list.at(i);
        auto bad = [&](const std::string & message) {
            admin_http_error e(400, "invalid_request_error", string_format("files[%zu]: ", i) + message);
            e.param = "files";
            throw e;
        };
        if (!jf.is_object()) {
            bad("must be an object {name, size}");
        }
        up_file f;
        f.index = (int) i;
        f.name  = up_json_str(jf, "name");
        if (f.name.empty() || f.name.size() > 255) {
            bad("name must be the file name (1 to 255 bytes)");
        }
        for (char c : f.name) {
            if ((unsigned char) c < 0x20 || c == '/' || c == '\\') {
                bad("name must be a plain file name");
            }
        }
        const std::string lower = up_lower(f.name);
        for (const char * prefix : { "mtp-", "dspark-", "dflash-", "eagle3-" }) {
            if (string_starts_with(lower, prefix)) {
                admin_http_error e(400, "unsupported_file", string_format("files[%zu]: draft models (%s...) cannot be uploaded yet", i, prefix));
                e.param = "files";
                e.extra = { {"reason", "draft"}, {"file", (int) i} };
                throw e;
            }
        }
        if (!jf.contains("size") || !jf.at("size").is_number_integer()) {
            bad("size must be the size in bytes");
        }
        const int64_t size = up_json_i64(jf, "size", -1);
        if (size < 24 || size > (int64_t) (1ull << 62)) {
            bad("size must be at least 24 bytes (a GGUF header)");
        }
        f.size          = (uint64_t) size;
        f.last_modified = up_json_i64(jf, "last_modified", 0);
        const std::string sha = up_lower(up_json_str(jf, "sha256"));
        if (!sha.empty() && !up_is_hex(sha, 64)) {
            bad("sha256 must be 64 hex digits (as shown on a Hugging Face file page)");
        }
        f.sha_expected = sha;
        sha256_init(&f.sha);
        files.push_back(f);
    }
}

static std::string up_settings_mode(const json & body, const std::string & def) {
    if (!body.contains("settings_mode") || body.at("settings_mode").is_null()) {
        return def;
    }
    const std::string mode = up_json_str(body, "settings_mode");
    if (mode != "replace" && mode != "keep") {
        admin_http_error e(400, "invalid_request_error", "settings_mode must be \"replace\" or \"keep\"");
        e.param = "settings_mode";
        throw e;
    }
    return mode;
}

static bool up_body_bool(const json & body, const char * key, bool def) {
    if (!body.contains(key) || body.at(key).is_null()) {
        return def;
    }
    if (!body.at(key).is_boolean()) {
        admin_http_error e(400, "invalid_request_error", std::string(key) + " must be true or false");
        e.param = key;
        throw e;
    }
    return body.at(key).get<bool>();
}

// the settings a session applies at publish: validated like POST /admin/models/settings
static admin_kv up_resolve_settings(const std::string & name, const json & settings, const std::string & mode,
                                    std::vector<std::string> & warnings, json & locks, json & existing) {
    admin_kv saved;
    json overrides;
    const bool has_entry = admin_internal_entry(name, overrides, saved);
    existing = has_entry ? overrides : json(nullptr);
    const admin_kv * base = mode == "keep" && has_entry ? &saved : nullptr;
    json request = settings.is_null() ? json::object() : settings;
    if (!request.is_object()) {
        admin_http_error e(400, "invalid_request_error", "settings must be an object of {key: value | null}");
        e.param = "settings";
        throw e;
    }
    return admin_internal_validate_settings(name, request, base, warnings, locks);
}

static server_http_res_ptr up_handle_get(const server_http_req & req) {
    const std::string id = req.get_param("id");
    if (!id.empty()) {
        std::shared_ptr<up_session> sp = up_find_or_throw(id);
        std::lock_guard<std::mutex> lk(sp->mtx);
        return admin_internal_json(200, up_session_json(*sp, true));
    }
    up_context & u = up_ctx();
    bool was_available = false;
    {
        std::lock_guard<std::mutex> lk(u.mtx);
        was_available = u.available;
    }
    if (!was_available) {
        up_open_dirs(u, false); // a directory that appeared or became writable since
    }
    std::vector<std::shared_ptr<up_session>> all;
    bool available = false;
    std::string reason;
    std::string message;
    {
        std::lock_guard<std::mutex> lk(u.mtx);
        for (const auto & [sid, s] : u.sessions) {
            all.push_back(s);
        }
        available = u.available;
        reason    = u.reason;
        message   = u.message;
    }
    json sessions = json::array();
    uint64_t staging = 0;
    for (const auto & s : all) {
        std::lock_guard<std::mutex> lk(s->mtx);
        sessions.push_back(up_session_json(*s, false));
        if (!up_is_final(s->state)) {
            staging += up_received_bytes(*s);
        }
    }
    std::string why;
    const bool settings_ok = admin_internal_settings_writable(why);
    return admin_internal_json(200, {
        {"available",      available},
        {"reason",         reason.empty() ? json(nullptr) : json(reason)},
        {"message",        message.empty() ? json(nullptr) : json(message)},
        {"limits",         up_limits_json()},
        {"sessions",       sessions},
        {"staging_bytes",  staging},
        {"models_dir",     u.models_dir_text.empty() ? json(nullptr) : json(u.models_dir_text)},
        {"settings_writable", settings_ok},
        {"settings_error", settings_ok ? json(nullptr) : json(why)},
    });
}

static server_http_res_ptr up_handle_create(const server_http_req & req) {
    const json body = admin_internal_body_object(req);
    const bool dry_run = up_body_bool(body, "dry_run", false);
    if (!dry_run) {
        admin_audit_intent(req, "upload.create", up_json_str(body, "name")); // a refused create is recorded too
    }
    up_require_available();

    std::vector<up_file> files;
    up_validate_files(body, files);
    const std::string name = up_json_str(body, "name");
    // held until the session is in the table (or the request failed): the name and the slot are reserved at once
    std::unique_lock<std::mutex> lk_names(up_ctx().mtx_names);
    up_check_name(name, "");
    const std::string mode = up_settings_mode(body, "replace");
    const bool auto_publish = up_body_bool(body, "auto_publish", true);
    const json settings_req = body.contains("settings") ? body.at("settings") : json(nullptr);

    std::vector<std::string> warnings;
    json locks;
    json existing;
    const admin_kv settings = up_resolve_settings(name, settings_req, mode, warnings, locks, existing);
    std::string why;
    if (!admin_internal_settings_writable(why)) {
        warnings.push_back("state_not_writable"); // a placement cannot be saved: the publish stops at settings_not_saved
    }
    uint64_t needed = 0;
    for (const auto & f : files) {
        needed += f.size;
    }
    json disk;
    if (!dry_run && up_count_unfinished() >= UP_MAX_SESSIONS) {
        json list = json::array();
        up_context & u = up_ctx();
        std::vector<std::shared_ptr<up_session>> all;
        {
            std::lock_guard<std::mutex> lk(u.mtx);
            for (const auto & [sid, s] : u.sessions) {
                all.push_back(s);
            }
        }
        for (const auto & s : all) {
            std::lock_guard<std::mutex> lk(s->mtx);
            if (!up_is_final(s->state)) {
                list.push_back(up_session_json(*s, false));
            }
        }
        up_throw(409, "too_many_uploads", string_format("at most %d uploads can be open at once: finish or cancel one first", UP_MAX_SESSIONS),
                 { {"sessions", list} });
    }
    disk = up_disk_json(needed, "", warnings);

    json files_json = json::array();
    for (const auto & f : files) {
        files_json.push_back({ {"index", f.index}, {"name", f.name}, {"size", f.size} });
    }
    json warn_json = json::array();
    for (const auto & w : warnings) {
        warn_json.push_back(w);
    }
    if (dry_run) {
        json out = {
            {"upload", {
                {"id",            nullptr},
                {"name",          name},
                {"chunk_max",     UP_CHUNK_MAX},
                {"chunk_default", UP_CHUNK_DEFAULT},
            }},
        };
        out["upload"]["files"]    = files_json;
        out["upload"]["settings"] = up_kv_json(settings);
        out["disk"]     = disk;
        out["warnings"] = warn_json;
        out["locks"]    = locks;
        if (existing.is_object()) {
            out["existing_settings"] = existing;
        }
        return admin_internal_json(200, out);
    }

    // the session: folder, lock, record
    up_context & u = up_ctx();
    auto sp = std::make_shared<up_session>();
    sp->id      = up_new_id();
    sp->name    = name;
    sp->files   = files;
    sp->settings = settings;
    sp->settings_request = settings_req.is_null() ? json::object() : settings_req;
    sp->settings_mode = mode;
    sp->auto_publish  = auto_publish;
    sp->warnings      = warnings;
    sp->creator       = admin_internal_client_text(req);
    sp->creator_actor = admin_audit_actor(req);
    sp->created = sp->updated = sp->last_activity = admin_internal_wall_ms();
    sp->t_idle  = admin_internal_now_ms();
    int err = 0;
    {
        std::lock_guard<std::mutex> lk(u.mtx);
        if (!u.staging.valid()) {
            up_throw(409, "upload_unavailable", u.message, { {"reason", u.reason} });
        }
        if (!up_dir_open(u.staging, sp->id, true, 0770, sp->dir, err)) {
            up_throw(500, "write_failed", "cannot create the staging folder: " + up_errno_text(err));
        }
    }
    up_apply_owner_dir(sp->dir, u.owner, &sp->warnings);
    bool elsewhere = false;
    if (!up_lock_take(sp->dir, up_lock_owner_text(), sp->lock, elsewhere)) {
        std::lock_guard<std::mutex> lk(u.mtx);
        up_remove_tree_at(u.staging, sp->id);
        up_throw(500, "write_failed", "cannot lock the staging folder");
    }
    for (const auto & f : sp->files) {
        const int fd = up_file_open(sp->dir, std::to_string(f.index) + ".part", UP_OPEN_CREATE, err);
        if (fd < 0) {
            sp->lock.release();
            std::lock_guard<std::mutex> lk(u.mtx);
            up_remove_tree_at(u.staging, sp->id);
            up_throw(500, "write_failed", "cannot create a staged file: " + up_errno_text(err));
        }
        up_apply_owner_fd(fd, u.owner, false, &sp->warnings);
        up_close(fd, err);
    }
    up_fsync_dir(sp->dir);
    {
        std::lock_guard<std::mutex> lk(u.mtx);
        u.sessions[sp->id] = sp;
    }
    lk_names.unlock();
    up_persist(sp);
    admin_audit(&req, "upload.create", name, "ok", { {"id", sp->id}, {"bytes", needed}, {"files", (long long) files.size()} });

    json out;
    {
        std::lock_guard<std::mutex> lk(sp->mtx);
        out = { {"upload", up_session_json(*sp, true)} };
        if (!sp->record_persisted) {
            sp->warnings.push_back("record_not_persisted");
            warn_json.push_back("record_not_persisted");
        }
    }
    out["upload"]["expires_at"] = up_iso(admin_internal_wall_ms() + up_ttl_ms());
    out["disk"]     = disk;
    out["warnings"] = warn_json;
    out["locks"]    = locks;
    if (existing.is_object()) {
        out["existing_settings"] = existing;
    }
    return admin_internal_json(201, out);
}

static server_http_res_ptr up_handle_update(const server_http_req & req) {
    const json body = admin_internal_body_object(req);
    std::shared_ptr<up_session> sp = up_find_or_throw(up_json_str(body, "id"));
    std::string name;
    std::string mode;
    json settings_req;
    up_state state;
    {
        std::lock_guard<std::mutex> lk(sp->mtx);
        state = sp->state;
        name  = sp->name;
        mode  = sp->settings_mode;
        settings_req = sp->settings_request;
    }
    if (up_is_final(state)) {
        up_throw(410, "upload_gone", string_format("this upload is %s", up_state_name(state)), { {"state", up_state_name(state)} });
    }
    if (state == UP_CHECKING || state == UP_PUBLISHING) {
        up_throw(409, "upload_busy", string_format("this upload is %s: change it when it is ready", up_state_name(state)),
                 { {"state", up_state_name(state)} });
    }
    const bool name_given = body.contains("name") && !body.at("name").is_null();
    const std::string new_name = name_given ? up_json_str(body, "name") : name;
    std::unique_lock<std::mutex> lk_names(up_ctx().mtx_names, std::defer_lock);
    if (name_given && new_name != name) {
        lk_names.lock(); // until the new name is set: a create cannot take it meanwhile
        up_check_name(new_name, sp->id);
    }
    const std::string new_mode = up_settings_mode(body, mode);
    const bool settings_given = body.contains("settings");
    const json new_settings_req = settings_given ? (body.at("settings").is_null() ? json::object() : body.at("settings")) : settings_req;
    std::vector<std::string> warnings;
    json locks;
    json existing;
    const admin_kv settings = up_resolve_settings(new_name, new_settings_req, new_mode, warnings, locks, existing);

    // expected SHA-256 per file and files to drop
    std::map<int, std::string> new_sha;
    if (body.contains("files") && body.at("files").is_array()) {
        for (size_t i = 0; i < body.at("files").size(); i++) {
            const json & jf = body.at("files").at(i);
            const int64_t index = up_json_i64(jf, "index", -1);
            const std::string sha = up_lower(up_json_str(jf, "sha256"));
            if (index < 0 || (!sha.empty() && !up_is_hex(sha, 64))) {
                admin_http_error e(400, "invalid_request_error", "files: each entry is {index, sha256} with 64 hex digits (\"\" removes it)");
                e.param = "files";
                throw e;
            }
            new_sha[(int) index] = sha;
        }
    }
    std::set<int> drop;
    if (body.contains("drop_files") && body.at("drop_files").is_array()) {
        for (size_t i = 0; i < body.at("drop_files").size(); i++) {
            const json & v = body.at("drop_files").at(i);
            if (!v.is_number_integer()) {
                admin_http_error e(400, "invalid_request_error", "drop_files must be a list of file indexes");
                e.param = "drop_files";
                throw e;
            }
            drop.insert((int) v.get<int64_t>());
        }
    }
    bool now_checking = false;
    {
        std::lock_guard<std::mutex> lk(sp->mtx);
        if (sp->state != state) {
            up_throw(409, "upload_busy", "the upload changed meanwhile, read it again", { {"state", up_state_name(sp->state)} });
        }
        for (const auto & [index, sha] : new_sha) {
            if (index >= (int) sp->files.size()) {
                admin_http_error e(400, "invalid_request_error", "files: no file with this index");
                e.param = "files";
                throw e;
            }
        }
        for (int index : drop) {
            if (index < 0 || index >= (int) sp->files.size()) {
                admin_http_error e(400, "invalid_request_error", "drop_files: no file with this index");
                e.param = "drop_files";
                throw e;
            }
        }
        bool drops_new = false;
        for (int index : drop) {
            drops_new = drops_new || !sp->files[(size_t) index].dropped;
        }
        if (drops_new && sp->staged) {
            // the checks passed with these files and arranged them under their final names: a dropped file would
            // still be published with the others
            up_throw(409, "upload_busy", "the files of this upload were checked and arranged for publishing together: "
                     "cancel the upload and upload again without that file", { {"state", up_state_name(sp->state)}, {"staged", true} });
        }
        size_t left = 0;
        for (const auto & f : sp->files) {
            left += (!f.dropped && !drop.count(f.index)) ? 1 : 0;
        }
        if (left == 0) {
            up_throw(400, "invalid_request_error", "an upload needs at least one file: cancel it instead");
        }
        sp->name          = new_name;
        sp->settings_mode = new_mode;
        sp->settings      = settings;
        sp->settings_request = new_settings_req;
        if (body.contains("auto_publish") && body.at("auto_publish").is_boolean()) {
            sp->auto_publish = body.at("auto_publish").get<bool>();
        }
        for (const auto & [index, sha] : new_sha) {
            sp->files[(size_t) index].sha_expected = sha;
        }
        for (int index : drop) {
            up_file & f = sp->files[(size_t) index];
            f.dropped = true;
            f.role.clear();
        }
        sp->idle_ms = 0;
        sp->last_activity = admin_internal_wall_ms();
        // a receiving session whose remaining files are all here now goes on to the checks
        if (sp->state == UP_RECEIVING) {
            bool all = true;
            for (const auto & f : sp->files) {
                all = all && (f.dropped || (f.complete && !f.error.is_object()));
            }
            if (all) {
                sp->state    = UP_CHECKING;
                sp->substate = "queued";
                now_checking = true;
            }
        }
        if (sp->state == UP_READY && (sp->blocked_by == "name_taken" || sp->blocked_by == "settings_invalid" ||
                                      sp->blocked_by == "sha256_mismatch")) {
            // re-judged at the next publish
        }
    }
    for (int index : drop) {
        int err = 0;
        up_unlink_at(sp->dir, std::to_string(index) + ".part", err); // frees its space at once
    }
    up_persist(sp);
    if (now_checking) {
        up_wake_worker();
    }
    json out;
    {
        std::lock_guard<std::mutex> lk(sp->mtx);
        out = { {"upload", up_session_json(*sp, true)} };
    }
    json warn_json = json::array();
    for (const auto & w : warnings) {
        warn_json.push_back(w);
    }
    out["warnings"] = warn_json;
    out["locks"]    = locks;
    if (existing.is_object()) {
        out["existing_settings"] = existing;
    }
    return admin_internal_json(200, out);
}

static server_http_res_ptr up_handle_cancel(const server_http_req & req) {
    const json body = admin_internal_body_object(req);
    admin_audit_intent(req, "upload.cancel", up_json_str(body, "id"));
    std::shared_ptr<up_session> sp = up_find_or_throw(up_json_str(body, "id"));
    uint64_t freed = 0;
    {
        std::lock_guard<std::mutex> lk(sp->mtx);
        if (up_is_final(sp->state)) {
            json out = { {"cancelled", sp->state == UP_CANCELLED}, {"freed_bytes", 0} };
            out["upload"] = up_session_json(*sp, true);
            return admin_internal_json(200, out);
        }
        if (sp->state == UP_PUBLISHING) {
            up_throw(409, "upload_busy", "this upload is being published right now", { {"state", "publishing"} });
        }
        if (sp->state == UP_CHECKING) {
            sp->cancel_requested = true; // the worker stops between its steps
        }
        freed = up_received_bytes(*sp);
    }
    up_finish(sp, UP_CANCELLED, json(nullptr), &req, "upload.cancel");
    json out = { {"cancelled", true}, {"freed_bytes", freed} };
    {
        std::lock_guard<std::mutex> lk(sp->mtx);
        out["upload"] = up_session_json(*sp, true);
    }
    return admin_internal_json(200, out);
}

static server_http_res_ptr up_handle_publish(const server_http_req & req) {
    const json body = admin_internal_body_object(req);
    std::shared_ptr<up_session> sp = up_find_or_throw(up_json_str(body, "id"));
    const bool force_sha        = up_body_bool(body, "ignore_sha256_mismatch", false);
    const bool without_settings = up_body_bool(body, "without_settings", false);
    int status = 202;
    {
        std::lock_guard<std::mutex> lk(sp->mtx);
        switch (sp->state) {
            case UP_PUBLISHED:
                status = 200;
                break;
            case UP_PUBLISHING:
            case UP_CHECKING:
                break; // idempotent
            case UP_READY:
                sp->force_sha        = sp->force_sha || force_sha;
                sp->without_settings = sp->without_settings || without_settings;
                if (sp->blocked_by == "check_failed" || sp->blocked_by == "sha256_mismatch") {
                    sp->state        = UP_CHECKING; // Retry: the checks run again (the hash is kept)
                    sp->substate     = "queued";
                    sp->retry_checks = true;
                } else {
                    sp->state = UP_PUBLISHING;
                }
                sp->blocked_by.clear();
                sp->blocked   = nullptr;
                sp->auto_publish = true;
                sp->idle_ms   = 0;
                break;
            case UP_RECEIVING:
                up_throw(409, "upload_busy", "this upload is not complete yet", { {"state", "receiving"} });
            default:
                up_throw(410, "upload_gone", string_format("this upload is %s", up_state_name(sp->state)), { {"state", up_state_name(sp->state)} });
        }
    }
    up_persist(sp);
    up_wake_worker();
    json out;
    {
        std::lock_guard<std::mutex> lk(sp->mtx);
        out = { {"upload", up_session_json(*sp, true)} };
    }
    return admin_internal_json(status, out);
}

//
// the validate helper, router side: one process at a time (the worker is the only caller)
//

struct up_helper_out {
    bool        answered = false; // a result line came back (its verdicts are definitive)
    json        result;
    std::string failure;          // not answered: spawn_failed | timeout | no_result | stopped
    std::string message;
};

static up_helper_out up_run_helper(const std::vector<std::string> & args, const std::string & label) {
    up_helper_out out;
    std::vector<std::string> cmd = admin_internal_self_command();
    cmd.insert(cmd.end(), args.begin(), args.end());
    std::vector<std::string> env = admin_internal_helper_env("validate");
    env.push_back("GGML_NO_BACKTRACE=1"); // an abort prints its message and exits, no debugger is attached
    const std::string test = up_test_validate_take();
    if (test == "hang") {
        env.push_back("PROVISION_ADMIN_HELPER_TEST=hang");
    } else if (test == "exit") {
        env.push_back("PROVISION_ADMIN_HELPER_TEST=exit");
    } else if (test == "spawn") {
        cmd[0] += ".missing-binary";
    }
    std::shared_ptr<admin_job> job = admin_job_start(label, { cmd }, env, up_helper_ms());
    up_context & u = up_ctx();
    while (!admin_job_wait(job, 200)) {
        if (u.stop.load()) {
            out.failure = "stopped";
            out.message = "the server is shutting down";
            return out;
        }
    }
    bool spawned = false;
    bool timed_out = false;
    int exit_code = 0;
    std::string output;
    {
        std::lock_guard<std::mutex> lk(job->mtx);
        spawned   = job->spawned;
        timed_out = job->timed_out;
        exit_code = job->exit_code;
        output    = job->output;
    }
    if (!spawned) {
        out.failure = "spawn_failed";
        out.message = "the check helper could not be started";
        return out;
    }
    const json result = admin_result_line(output);
    if (result.is_object() && up_json_bool(result, "ok", false)) {
        out.answered = true;
        out.result   = result;
        return out;
    }
    const std::vector<std::string> tail = admin_output_tail(output, 6);
    std::string text;
    for (const auto & line : tail) {
        text += (text.empty() ? "" : " | ") + line;
    }
    if (timed_out) {
        out.failure = "timeout";
        out.message = string_format("the check did not finish within %d s", up_helper_ms() / 1000);
    } else if (result.is_object()) {
        out.failure = "check_error";
        out.message = up_json_str(result.contains("error") ? result.at("error") : json::object(), "message");
    } else {
        out.failure = "no_result";
        out.message = string_format("the check helper exited with status %d without an answer%s", exit_code,
                                    text.empty() ? "" : (": " + up_cut(text, 400)).c_str());
    }
    return out;
}

// codes of a file that only make that file unusable (the user drops it or cancels); every other definitive
// verdict about the model rejects the session. io_error (the file could not be read) is no verdict at all: the
// check counts as failed and runs again (Retry)
static bool up_is_file_level(const std::string & code) {
    return code == "not_gguf" || code == "adapter" || code == "imatrix" || code == "projector_bad_metadata" ||
           code == "projector_without_modality" || code == "header_too_large" || code == "malformed";
}

static std::string up_part_path(const up_session & s, int index) {
    return admin_internal_path_to_utf8(s.dir.path / (std::to_string(index) + ".part"));
}

// classification of a complete set of headers (base design 3.7 phase 1): exactly one model (one file or a whole
// split set), at most one projector; the final names (3.9). false + code/message: definitive.
static bool up_classify(const up_session & s, json & plan, std::string & code, std::string & message) {
    std::vector<const up_file *> models;
    std::vector<const up_file *> projs;
    for (const auto & f : s.files) {
        if (f.dropped || f.error.is_object()) {
            continue;
        }
        (f.role == "projector" ? projs : models).push_back(&f);
    }
    if (models.empty()) {
        code    = projs.empty() ? "no_model" : "projector_without_model";
        message = projs.empty() ? "the upload holds no model file" : "a projector (mmproj) needs its model in the same upload";
        return false;
    }
    if (projs.size() > 1) {
        code    = "two_projectors";
        message = "an upload can hold one projector (mmproj) only";
        return false;
    }
    const long long count = models[0]->head.split_count;
    if (models.size() == 1) {
        if (count > 1) {
            code    = "split_incomplete";
            message = string_format("this model is split into %lld files, the upload holds one of them", count);
            return false;
        }
    } else {
        std::set<long long> seen;
        for (const up_file * m : models) {
            if (m->head.split_count <= 1) {
                code    = "two_models";
                message = "the upload holds more than one model: upload them one at a time";
                return false;
            }
            if (m->head.split_count != count || m->head.split_no < 0 || m->head.split_no >= count || seen.count(m->head.split_no)) {
                code    = "split_mismatch";
                message = "the files are not one split set (split.no / split.count disagree)";
                return false;
            }
            seen.insert(m->head.split_no);
        }
        if ((long long) models.size() != count) {
            code    = "split_incomplete";
            message = string_format("this model is split into %lld files, the upload holds %zu of them", count, models.size());
            return false;
        }
    }
    const std::string id = s.name;
    const bool folder = models.size() > 1 || !projs.empty();
    json files = json::array();
    for (const up_file * m : models) {
        std::string final_name = id + ".gguf";
        if (models.size() > 1) {
            final_name = string_format("%s-%05lld-of-%05lld.gguf", id.c_str(), m->head.split_no + 1, count);
        }
        files.push_back({ {"index", m->index}, {"final_name", final_name}, {"role", "model"},
                          {"split_no", models.size() > 1 ? json(m->head.split_no) : json(nullptr)} });
    }
    for (const up_file * p : projs) {
        files.push_back({ {"index", p->index}, {"final_name", "mmproj-" + id + ".gguf"}, {"role", "projector"}, {"split_no", nullptr} });
    }
    plan = {
        {"layout", folder ? "folder" : "file"},
        {"target", folder ? id : id + ".gguf"},
        {"id",     id},
    };
    plan["files"] = files;
    return true;
}

// the first shard (or the single file) of a plan, as a name inside the layout
static std::string up_plan_model_name(const json & plan, std::string & mmproj) {
    std::string first;
    mmproj.clear();
    if (!plan.is_object() || !plan.contains("files")) {
        return first;
    }
    const json & files = plan.at("files");
    for (size_t i = 0; i < files.size(); i++) {
        const json & f = files.at(i);
        const std::string role = up_json_str(f, "role");
        if (role == "projector") {
            mmproj = up_json_str(f, "final_name");
        } else if (first.empty() || up_json_i64(f, "split_no", 0) == 0) {
            if (first.empty() || up_json_i64(f, "split_no", -1) == 0) {
                first = up_json_str(f, "final_name");
            }
        }
    }
    return first;
}

static void up_set_ready(const std::shared_ptr<up_session> & sp, const std::string & blocked_by, const json & details) {
    {
        std::lock_guard<std::mutex> lk(sp->mtx);
        if (up_is_final(sp->state) || sp->cancel_requested) {
            return; // cancelled meanwhile
        }
        sp->state      = UP_READY;
        sp->substate.clear();
        sp->blocked_by = blocked_by;
        sp->blocked    = details;
        sp->idle_ms    = 0; // the expiry counts again from here
        sp->t_idle     = admin_internal_now_ms();
    }
    up_persist(sp);
    SRV_INF("admin: upload %s is ready (%s)\n", sp->id.c_str(), blocked_by.c_str());
}

static void up_reject(const std::shared_ptr<up_session> & sp, const std::string & code, const std::string & message, int file_index) {
    json error = { {"code", code}, {"message", message} };
    error["file"] = file_index >= 0 ? json(file_index) : json(nullptr);
    SRV_WRN("admin: upload %s rejected (%s): %s\n", sp->id.c_str(), code.c_str(), message.c_str());
    up_finish(sp, UP_REJECTED, error, nullptr, "upload.rejected");
}

// applies one helper verdict to a file (header phase); true when the session was rejected
static bool up_apply_header_verdict(const std::shared_ptr<up_session> & sp, int index, const up_helper_out & out) {
    std::string reject_code;
    std::string reject_message;
    {
        std::lock_guard<std::mutex> lk(sp->mtx);
        up_file & f = sp->files[(size_t) index];
        if (!out.answered) {
            f.head_state  = UP_HEAD_FAILED;
            f.head_result = { {"verdict", "failed"}, {"code", out.failure}, {"message", out.message} };
            return false;
        }
        json jf = json::object();
        if (out.result.contains("files") && out.result.at("files").is_array() && !out.result.at("files").empty()) {
            jf = out.result.at("files").at(0);
        }
        f.head_result = jf;
        const std::string verdict = up_json_str(jf, "verdict");
        const std::string code    = up_json_str(jf, "code");
        const std::string kind    = up_json_str(jf, "kind");
        if (!kind.empty()) {
            f.role = kind;
        }
        if (jf.contains("facts") && jf.at("facts").is_object() && f.role == "model") {
            sp->facts = jf.at("facts");
        }
        if (verdict == "ok") {
            f.head_state = UP_HEAD_OK;
        } else if (verdict == "warning") {
            f.head_state = UP_HEAD_WARNING;
        } else if (verdict == "reject" && code == "io_error") {
            f.head_state = UP_HEAD_FAILED; // not about the content: checked again
        } else if (verdict == "reject" && up_is_file_level(code)) {
            f.head_state = UP_HEAD_ERROR;
            f.error = { {"code", code}, {"message", up_json_str(jf, "message")} };
        } else if (verdict == "reject") {
            f.head_state   = UP_HEAD_ERROR;
            reject_code    = code.empty() ? "rejected" : code;
            reject_message = up_json_str(jf, "message");
        } else {
            f.head_state = UP_HEAD_FAILED;
        }
    }
    if (!reject_code.empty()) {
        up_reject(sp, reject_code, reject_message, index);
        return true;
    }
    return false;
}

// when every header is known: the plan, or a definitive rejection (wrong set of files) early, before the bulk data
static void up_classify_if_ready(const std::shared_ptr<up_session> & sp) {
    json plan;
    std::string code;
    std::string message;
    bool ready = true;
    bool ok = false;
    {
        std::lock_guard<std::mutex> lk(sp->mtx);
        if (up_is_final(sp->state)) {
            return;
        }
        for (const auto & f : sp->files) {
            if (f.dropped) {
                continue;
            }
            // a file that cannot be used waits for the user (drop it or cancel): no verdict about the set before
            ready = ready && !f.error.is_object() && (f.head_state == UP_HEAD_OK || f.head_state == UP_HEAD_WARNING);
        }
        if (!ready) {
            return;
        }
        ok = up_classify(*sp, plan, code, message);
        if (ok && sp->staged) {
            // the files already carry their staged names (under staged_id): up_stage renames them for the current
            // name and updates the plan, it needs the names they have now
            return;
        }
        if (ok) {
            sp->plan = plan;
            for (size_t i = 0; i < plan.at("files").size(); i++) {
                const json & pf = plan.at("files").at(i);
                const int64_t index = up_json_i64(pf, "index", -1);
                if (index >= 0 && index < (int64_t) sp->files.size()) {
                    sp->files[(size_t) index].final_name = up_json_str(pf, "final_name");
                }
            }
        }
    }
    if (!ok) {
        up_reject(sp, code, message, -1);
        return;
    }
    up_persist(sp);
}

// one queued header check, if any; true when it did some work
static bool up_work_header_checks() {
    up_context & u = up_ctx();
    std::vector<std::shared_ptr<up_session>> all;
    {
        std::lock_guard<std::mutex> lk(u.mtx);
        for (const auto & [id, s] : u.sessions) {
            all.push_back(s);
        }
    }
    for (const auto & sp : all) {
        int index = -1;
        std::string path;
        {
            std::lock_guard<std::mutex> lk(sp->mtx);
            if (sp->state != UP_RECEIVING && sp->state != UP_CHECKING) {
                continue;
            }
            for (const auto & f : sp->files) {
                if (!f.dropped && f.head_state == UP_HEAD_QUEUED && !sp->staged) {
                    index = f.index;
                    break;
                }
            }
            if (index < 0) {
                continue;
            }
            path = up_part_path(*sp, index);
        }
        const up_helper_out out = up_run_helper({ "--validate-phase", "header", "--validate-file", path }, "upload header check");
        if (out.failure == "stopped") {
            return false;
        }
        if (!up_apply_header_verdict(sp, index, out)) {
            up_classify_if_ready(sp);
        }
        up_persist(sp);
        return true;
    }
    return false;
}

// SHA-256 of a staged file from disk, with progress (after a restart or an I/O error the incremental state is gone)
static bool up_hash_file(const std::shared_ptr<up_session> & sp, int index, const std::string & name_in_dir, const up_dir & dir,
                         std::string & hex, std::string & error) {
    int err = 0;
    const int fd = up_file_open(dir, name_in_dir, UP_OPEN_READ, err);
    if (fd < 0) {
        error = "cannot open the staged file: " + up_errno_text(err);
        return false;
    }
#if defined(__linux__) && defined(POSIX_FADV_SEQUENTIAL)
    ::posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
    uint64_t size = 0;
    up_fsize(fd, size);
    sha256_t sha;
    sha256_init(&sha);
    std::vector<char> buf(4 * UP_MIB);
    uint64_t pos = 0;
    bool ok = true;
    while (pos < size) {
        if (up_ctx().stop.load()) {
            ok = false;
            error = "stopped";
            break;
        }
        {
            std::lock_guard<std::mutex> lk(sp->mtx);
            if (sp->cancel_requested) {
                ok = false;
                error = "cancelled";
                break;
            }
        }
        const size_t n = (size_t) std::min<uint64_t>(buf.size(), size - pos);
        if (!up_pread_all(fd, pos, buf.data(), n, err)) {
            ok = false;
            error = "cannot read the staged file: " + up_errno_text(err);
            break;
        }
        sha256_update(&sha, (const unsigned char *) buf.data(), n);
        up_drop_cache(fd, pos, n);
        pos += n;
        std::lock_guard<std::mutex> lk(sp->mtx);
        sp->hash_done += n;
    }
    up_close(fd, err);
    if (!ok) {
        return false;
    }
    unsigned char digest[SHA256_DIGEST_SIZE];
    sha256_final(&sha, digest);
    hex = up_hex(digest, sizeof(digest));
    (void) index;
    return true;
}

// a first staging that did not finish (a failed move that could not be undone, or a crash between the moves) left
// some files in out/ under the names it recorded before it began (final_name, folder staged_id): they are moved
// back to <i>.part, so the next staging starts from a clean state and a recovery never re-creates them empty.
// Caller: the session is not staged and no other thread works on it (worker, or recovery before the worker runs)
static void up_stage_repair(const std::shared_ptr<up_session> & sp) {
    std::vector<std::pair<int, std::string>> files; // index, final name
    std::vector<std::string> folders;               // where a folder layout may have been staged to
    {
        std::lock_guard<std::mutex> lk(sp->mtx);
        if (sp->staged || !sp->dir.valid()) {
            return;
        }
        for (const auto & f : sp->files) {
            // a final name read back from a record is one plain file name (up_file_from_record)
            if (!f.dropped && !f.final_name.empty()) {
                files.push_back({ f.index, f.final_name });
            }
        }
        for (const std::string & c : { sp->staged_id, up_json_str(sp->plan, "id"), sp->name }) {
            std::string reason;
            if (!c.empty() && up_name_valid(c, reason) && std::find(folders.begin(), folders.end(), c) == folders.end()) {
                folders.push_back(c);
            }
        }
    }
    if (files.empty()) {
        return;
    }
    up_dir out;
    int err = 0;
    if (!up_dir_open(sp->dir, UP_OUT_DIR, false, 0, out, err)) {
        return; // nothing was moved
    }
    int n_moved = 0;
    for (const auto & [index, final_name] : files) {
        const std::string part = std::to_string(index) + ".part";
        up_stat st;
        if (up_stat_at(sp->dir, part, st)) {
            continue; // still in place
        }
        bool moved = false;
        if (up_stat_at(out, final_name, st) && st.is_reg && !st.is_link) {
            moved = up_rename_at(out, final_name, sp->dir, part, err);
        }
        for (const auto & folder : folders) {
            if (moved) {
                break;
            }
            up_dir sub;
            if (up_dir_open(out, folder, false, 0, sub, err) && up_stat_at(sub, final_name, st) && st.is_reg && !st.is_link) {
                moved = up_rename_at(sub, final_name, sp->dir, part, err);
            }
        }
        n_moved += moved ? 1 : 0;
    }
    for (const auto & folder : folders) {
        up_rmdir_at(out, folder, err); // only when empty
    }
    if (n_moved > 0) {
        up_fsync_dir(out);
        up_fsync_dir(sp->dir);
        SRV_INF("admin: upload %s: %d file(s) of an interrupted staging were moved back\n", sp->id.c_str(), n_moved);
    }
}

// renames the parts into out/ under their final names (or renames an earlier staging to a new id). A failed move
// is undone (the files go back to <i>.part or to their earlier names), so a Retry or a rename starts again from a
// clean state; a first staging records its names before anything moves (up_stage_repair after a crash)
static bool up_stage(const std::shared_ptr<up_session> & sp, std::string & error) {
    up_context & u = up_ctx();
    json plan;
    bool staged = false;
    std::string staged_id;
    std::string id;
    size_t n_files = 0;
    {
        std::lock_guard<std::mutex> lk(sp->mtx);
        plan      = sp->plan;
        staged    = sp->staged;
        staged_id = sp->staged_id;
        id        = sp->name;
        n_files   = sp->files.size();
    }
    if (!plan.is_object() || !plan.contains("files") || !plan.at("files").is_array() || plan.at("files").empty()) {
        error = "no plan";
        return false;
    }
    const bool folder = up_json_str(plan, "layout") == "folder";
    // the names for `id`
    std::map<int, std::string> names;
    long long count = 0;
    for (size_t i = 0; i < plan.at("files").size(); i++) {
        const json & pf = plan.at("files").at(i);
        count += up_json_str(pf, "role") == "model" ? 1 : 0;
    }
    for (size_t i = 0; i < plan.at("files").size(); i++) {
        const json & pf = plan.at("files").at(i);
        const int64_t index = up_json_i64(pf, "index", -1);
        if (index < 0 || index >= (int64_t) n_files) {
            error = "the plan names a file the upload does not have";
            return false;
        }
        const std::string role = up_json_str(pf, "role");
        std::string name = id + ".gguf";
        if (role == "projector") {
            name = "mmproj-" + id + ".gguf";
        } else if (count > 1) {
            name = string_format("%s-%05lld-of-%05lld.gguf", id.c_str(), (long long) up_json_i64(pf, "split_no", 0) + 1, count);
        }
        names[(int) index] = name;
    }
    if (!folder && names.size() != 1) {
        error = "a single-file layout with more than one file";
        return false;
    }
    if (staged && staged_id == id) {
        return true;
    }
    std::map<int, std::string> old_names;
    if (!staged) {
        up_stage_repair(sp);
        // the names and the folder of this staging, recorded before the first move
        {
            std::lock_guard<std::mutex> lk(sp->mtx);
            for (const auto & [index, name] : names) {
                sp->files[(size_t) index].final_name = name;
            }
            sp->staged_id = id;
        }
        up_persist(sp);
    } else {
        std::lock_guard<std::mutex> lk(sp->mtx);
        for (const auto & [index, name] : names) {
            old_names[index] = sp->files[(size_t) index].final_name;
        }
    }
    int err = 0;
    up_dir out;
    if (!up_dir_open(sp->dir, UP_OUT_DIR, true, 0770, out, err)) {
        error = "cannot create the staging folder: " + up_errno_text(err);
        return false;
    }
    std::vector<std::string> warnings;
    up_apply_owner_dir(out, u.owner, &warnings);

    // the folder the files go to: out/<id>/ for a folder layout, out/ for a single file
    up_dir model_dir;
    bool folder_renamed = false;
    if (folder) {
        if (staged) {
            if (!up_rename_at(out, staged_id, out, id, err)) {
                error = "cannot rename the staged folder: " + up_errno_text(err);
                return false;
            }
            folder_renamed = true;
        }
        if (!up_dir_open(out, id, true, 0770, model_dir, err)) {
            if (folder_renamed) {
                int e2 = 0;
                up_rename_at(out, id, out, staged_id, e2);
            }
            error = "cannot create the staged model folder: " + up_errno_text(err);
            return false;
        }
        up_apply_owner_dir(model_dir, u.owner, &warnings);
    }
    const up_dir & dest = folder ? model_dir : out;

    std::vector<int> moved; // indexes, in order
    int position = 0;
    for (const auto & [index, name] : names) {
        const std::string from = staged ? old_names[index] : std::to_string(index) + ".part";
        bool ok = true;
        if (!(staged && from == name)) {
            const int injected = up_test_stage_failure(position);
            ok = injected == 0 && up_rename_at(staged ? dest : sp->dir, from, dest, name, err);
            if (injected != 0) {
                err = injected;
            }
        }
        position++;
        if (!ok) {
            // undo: every file back where it was, so the next attempt finds the state it started from
            bool undone = true;
            for (const int mi : moved) {
                int e2 = 0;
                const std::string back = staged ? old_names[mi] : std::to_string(mi) + ".part";
                undone = up_rename_at(dest, names[mi], staged ? dest : sp->dir, back, e2) && undone;
            }
            int e2 = 0;
            if (folder) {
                model_dir.reset();
                if (folder_renamed) {
                    undone = up_rename_at(out, id, out, staged_id, e2) && undone;
                } else {
                    up_rmdir_at(out, id, e2); // only when empty
                }
            }
            up_fsync_dir(out);
            up_fsync_dir(sp->dir);
            error = std::string(staged ? "cannot rename a staged file: " : "cannot move a staged file into place: ") + up_errno_text(err) +
                    (undone ? " (nothing was changed)" : " (it could not be undone completely: cancel the upload if a retry fails again)");
            return false;
        }
        moved.push_back(index);
    }
    for (const auto & [index, name] : names) {
        const int fd = up_file_open(dest, name, UP_OPEN_WRITE, err);
        if (fd >= 0) {
            up_apply_owner_fd(fd, u.owner, false, &warnings);
            up_datasync(fd, err);
            up_close(fd, err);
        }
    }
    if (folder) {
        up_fsync_dir(model_dir);
    }
    up_fsync_dir(out);
    up_fsync_dir(sp->dir);
    {
        std::lock_guard<std::mutex> lk(sp->mtx);
        for (const auto & [index, name] : names) {
            sp->files[(size_t) index].final_name = name;
        }
        for (size_t i = 0; i < sp->plan.at("files").size(); i++) {
            json & pf = sp->plan.at("files").at(i);
            const int index = (int) up_json_i64(pf, "index", -1);
            if (names.count(index)) {
                pf["final_name"] = names[index];
            }
        }
        sp->plan["target"] = folder ? id : id + ".gguf";
        sp->plan["id"]     = id;
        sp->staged    = true;
        sp->staged_id = id;
        for (const auto & w : warnings) {
            if (std::find(sp->warnings.begin(), sp->warnings.end(), w) == sp->warnings.end()) {
                sp->warnings.push_back(w);
            }
        }
    }
    up_persist(sp);
    return true;
}

// the staged path of a file of the plan ("" = not staged)
static std::string up_staged_path(const up_session & s, const std::string & final_name) {
    const bool folder = up_json_str(s.plan, "layout") == "folder";
    fs::path p = s.dir.path / UP_OUT_DIR;
    if (folder) {
        p /= admin_internal_path_from_utf8(s.staged_id);
    }
    return admin_internal_path_to_utf8(p / admin_internal_path_from_utf8(final_name));
}

// the checks of a complete upload (worker); returns after one step so a shutdown or a cancel is noticed
static bool up_still_checking(const std::shared_ptr<up_session> & sp) {
    std::lock_guard<std::mutex> lk(sp->mtx);
    return sp->state == UP_CHECKING && !sp->cancel_requested;
}

static void up_work_checks(const std::shared_ptr<up_session> & sp) {
    // 1. SHA-256: incremental while receiving; read again from disk where that state is unsure
    std::vector<int> to_hash;
    {
        std::lock_guard<std::mutex> lk(sp->mtx);
        sp->hash_done  = 0;
        sp->hash_total = 0;
        for (const auto & f : sp->files) {
            if (!f.dropped && f.sha_hex.empty()) {
                to_hash.push_back(f.index);
                sp->hash_total += f.size;
            }
        }
        if (!to_hash.empty()) {
            sp->substate = "hashing";
            sp->t_hash_start = admin_internal_now_ms();
        }
    }
    for (int index : to_hash) {
        std::string hex;
        std::string error;
        bool staged = false;
        bool folder = false;
        std::string final_name;
        std::string staged_id;
        {
            std::lock_guard<std::mutex> lk(sp->mtx);
            staged     = sp->staged;
            folder     = up_json_str(sp->plan, "layout") == "folder";
            final_name = sp->files[(size_t) index].final_name;
            staged_id  = sp->staged_id;
        }
        bool ok = false;
        if (staged) {
            up_dir out;
            up_dir model_dir;
            int err = 0;
            if (!up_dir_open(sp->dir, UP_OUT_DIR, false, 0, out, err)) {
                error = "the staged files are missing";
            } else if (!folder) {
                ok = up_hash_file(sp, index, final_name, out, hex, error);
            } else if (up_dir_open(out, staged_id, false, 0, model_dir, err)) {
                ok = up_hash_file(sp, index, final_name, model_dir, hex, error);
            } else {
                error = "the staged model folder is missing";
            }
        } else {
            ok = up_hash_file(sp, index, std::to_string(index) + ".part", sp->dir, hex, error);
        }
        if (!ok) {
            if (error != "stopped" && error != "cancelled") {
                up_set_ready(sp, "check_failed", { {"code", "hash_failed"}, {"message", error} });
            }
            return;
        }
        std::lock_guard<std::mutex> lk(sp->mtx);
        sp->files[(size_t) index].sha_hex = hex;
    }
    if (!up_still_checking(sp)) {
        return;
    }
    // an expected SHA-256 that does not match keeps the data: fix the value, publish anyway, or cancel
    {
        json details;
        {
            std::lock_guard<std::mutex> lk(sp->mtx);
            for (const auto & f : sp->files) {
                if (!f.dropped && !f.sha_expected.empty() && f.sha_expected != f.sha_hex && !sp->force_sha) {
                    details = {
                        {"code",     "sha256_mismatch"},
                        {"file",     f.index},
                        {"expected", f.sha_expected},
                        {"actual",   f.sha_hex},
                        {"message",  "the SHA-256 of the received file is not the expected one: check the expected value, "
                                     "publish anyway, or cancel"},
                    };
                    break;
                }
            }
        }
        if (details.is_object()) {
            up_set_ready(sp, "sha256_mismatch", details);
            return;
        }
    }

    // 2. classification: every header checked (again after a restart or a helper failure), then the final names
    std::vector<int> pending;
    {
        std::lock_guard<std::mutex> lk(sp->mtx);
        sp->substate = "classifying";
        for (const auto & f : sp->files) {
            if (!f.dropped && !f.error.is_object() && f.head_state != UP_HEAD_OK && f.head_state != UP_HEAD_WARNING) {
                pending.push_back(f.index);
            }
        }
    }
    for (int index : pending) {
        std::string path;
        {
            std::lock_guard<std::mutex> lk(sp->mtx);
            path = sp->staged ? up_staged_path(*sp, sp->files[(size_t) index].final_name) : up_part_path(*sp, index);
            sp->files[(size_t) index].head_state = UP_HEAD_QUEUED;
        }
        const up_helper_out out = up_run_helper({ "--validate-phase", "header", "--validate-file", path }, "upload header check");
        if (out.failure == "stopped" || !up_still_checking(sp)) {
            return;
        }
        if (up_apply_header_verdict(sp, index, out)) {
            return; // rejected
        }
    }
    bool failed = false;
    bool file_error = false;
    std::string failed_message;
    json staged_error; // a staged file refused now: {code, message, file}
    {
        std::lock_guard<std::mutex> lk(sp->mtx);
        for (auto & f : sp->files) {
            if (f.dropped) {
                continue;
            }
            if (f.error.is_object() && sp->staged) {
                // the layout is final (the files are under their final names in out/), so the file cannot be dropped
                // any more: it was changed on disk while the router was down, or this build checks it more strictly.
                // No verdict that ends the upload: it waits in ready (Retry, or cancel)
                if (!staged_error.is_object()) {
                    staged_error = { {"code", up_json_str(f.error, "code")}, {"file", f.index},
                                     {"message", "file " + std::to_string(f.index) + " (" + f.name + "): " + up_json_str(f.error, "message")} };
                }
                f.head_state  = UP_HEAD_FAILED;
                f.head_result = { {"verdict", "failed"}, {"code", up_json_str(f.error, "code")}, {"message", up_json_str(f.error, "message")} };
                f.error       = nullptr;
            } else if (f.error.is_object()) {
                file_error = true;
            } else if (f.head_state == UP_HEAD_FAILED) {
                failed = true;
                failed_message = up_json_str(f.head_result, "message");
            }
        }
        if (file_error) {
            // a file that cannot be used: back to receiving, so it can be dropped (the other files stay)
            sp->state = UP_RECEIVING;
            sp->substate.clear();
        }
    }
    if (file_error) {
        up_persist(sp);
        return;
    }
    if (staged_error.is_object()) {
        up_set_ready(sp, "check_failed", staged_error);
        return;
    }
    if (failed) {
        up_set_ready(sp, "check_failed", { {"code", "check_failed"}, {"message", failed_message} });
        return;
    }
    up_classify_if_ready(sp);
    if (!up_still_checking(sp)) {
        return;
    }
    std::string error;
    if (!up_stage(sp, error)) {
        up_set_ready(sp, "check_failed", { {"code", "staging_failed"}, {"message", error} });
        return;
    }

    // 3. the load checks on the final names: the loader's own checks without reading the weights, and the
    //    projector capabilities exactly as the router reads them on every reload
    std::string model_path;
    std::string mmproj_path;
    {
        std::lock_guard<std::mutex> lk(sp->mtx);
        sp->substate = "load_check";
        std::string mmproj_name;
        const std::string first = up_plan_model_name(sp->plan, mmproj_name);
        model_path  = up_staged_path(*sp, first);
        mmproj_path = mmproj_name.empty() ? std::string() : up_staged_path(*sp, mmproj_name);
    }
#if defined(_WIN32)
    {
        // llama.cpp builds the paths of the shards of a split model in a buffer of PATH_MAX (260) characters, so a
        // split model loads only from a shorter path; the staged copy is about 55 characters longer than the
        // published one. Never a verdict about the files: the upload waits for a shorter name (Rename, then Retry)
        long long n_models = 0;
        {
            std::lock_guard<std::mutex> lk(sp->mtx);
            const json & pf = sp->plan.is_object() && sp->plan.contains("files") ? sp->plan.at("files") : json::array();
            for (size_t i = 0; i < pf.size(); i++) {
                n_models += up_json_str(pf.at(i), "role") == "model" ? 1 : 0;
            }
        }
        if (n_models > 1) {
            std::wstring w = admin_internal_path_from_utf8(model_path).wstring();
            if (w.rfind(L"\\\\?\\UNC\\", 0) == 0) {
                w = L"\\\\" + w.substr(8);
            } else if (w.rfind(L"\\\\?\\", 0) == 0) {
                w = w.substr(4);
            }
            const std::string plain = admin_internal_path_to_utf8(fs::path(w));
            if (plain.size() >= 260) {
                std::string name;
                {
                    std::lock_guard<std::mutex> lk(sp->mtx);
                    name = sp->name;
                }
                // <staging>\<id>\<id>-00001-of-0000N.gguf: two characters less for every character less of the name
                const long long max_name = std::max<long long>(0, (259 - (long long) plain.size() + 2 * (long long) name.size()) / 2);
                up_set_ready(sp, "check_failed", { {"code", "path_too_long"}, {"max_name_length", max_name},
                    {"message", string_format("on Windows llama.cpp loads a split model only from a path of at most 259 "
                        "characters, and the upload is checked in %s first: rename it to at most %lld characters, then "
                        "Retry (nothing is uploaded again)", UP_STAGING, max_name)} });
                return;
            }
            model_path = plain; // within MAX_PATH: the plain form, which every API reads
        }
    }
#endif
    std::vector<std::string> args = { "--validate-phase", "full", "--validate-file", model_path };
    if (!mmproj_path.empty()) {
        args.push_back("--validate-mmproj");
        args.push_back(mmproj_path);
    }
    const up_helper_out out = up_run_helper(args, "upload load check");
    if (out.failure == "stopped" || !up_still_checking(sp)) {
        return;
    }
    if (!out.answered) {
        up_set_ready(sp, "check_failed", { {"code", out.failure}, {"message", out.message} });
        return;
    }
    const json files = out.result.contains("files") ? out.result.at("files") : json::array();
    for (size_t i = 0; i < files.size(); i++) {
        const json & jf = files.at(i);
        if (up_json_str(jf, "verdict") == "reject") {
            up_reject(sp, up_json_str(jf, "code").empty() ? "load_failed" : up_json_str(jf, "code"), up_json_str(jf, "message"), -1);
            return;
        }
        std::lock_guard<std::mutex> lk(sp->mtx);
        if (up_json_str(jf, "kind") == "model" && jf.contains("facts") && jf.at("facts").is_object()) {
            const json previous = sp->facts;
            sp->facts = jf.at("facts");
            if (previous.is_object() && previous.contains("vision")) {
                sp->facts["vision"] = previous.at("vision");
                sp->facts["audio"]  = previous.at("audio");
            }
        } else if (up_json_str(jf, "kind") == "projector" && jf.contains("facts") && jf.at("facts").is_object()) {
            if (!sp->facts.is_object()) {
                sp->facts = json::object();
            }
            sp->facts["vision"] = up_json_bool(jf.at("facts"), "vision", false);
            sp->facts["audio"]  = up_json_bool(jf.at("facts"), "audio", false);
        }
    }
    bool publish = false;
    {
        std::lock_guard<std::mutex> lk(sp->mtx);
        if (sp->state != UP_CHECKING || sp->cancel_requested) {
            return;
        }
        publish = sp->auto_publish && !sp->recovered_without_record;
        if (publish) {
            sp->state = UP_PUBLISHING;
            sp->substate.clear();
        }
    }
    if (!publish) {
        up_set_ready(sp, "manual", { {"code", "manual"}, {"message", "the checks passed: publish when ready"} });
        return;
    }
    up_persist(sp);
    up_wake_worker();
}

// publish (base design 3.8 with the critique fixes), under the apply mutex
static void up_work_publish(const std::shared_ptr<up_session> & sp) {
    up_context & u = up_ctx();
    server_models_routes * routes = admin_internal_routes();
    if (routes == nullptr) {
        return;
    }
    std::string id;
    std::string mode;
    json settings_request;
    bool without_settings = false;
    {
        std::lock_guard<std::mutex> lk(sp->mtx);
        id               = sp->name;
        mode             = sp->settings_mode;
        settings_request = sp->settings_request;
        without_settings = sp->without_settings;
    }
    std::lock_guard<std::mutex> lk_apply(admin_internal_apply_mutex());

    // 1. the name again (a model may have appeared meanwhile)
    try {
        up_check_name(id, sp->id);
    } catch (const admin_http_error & e) {
        json details = { {"code", e.type}, {"message", e.what()} };
        if (e.extra.is_object()) {
            for (const auto & [k, v] : e.extra.items()) {
                details[k] = v;
            }
        }
        up_set_ready(sp, "name_taken", details);
        return;
    }
    // 2. staging on the same file system as the models dir (a rename)
    try {
        up_require_available();
    } catch (const admin_http_error & e) {
        up_set_ready(sp, "publish_error", { {"code", "upload_unavailable"}, {"message", e.what()} });
        return;
    }
    // 3. the settings again, against the devices and locks of now
    admin_kv settings;
    if (!without_settings) {
        try {
            std::vector<std::string> warnings;
            json locks;
            json existing;
            settings = up_resolve_settings(id, settings_request, mode, warnings, locks, existing);
        } catch (const admin_http_error & e) {
            json details = { {"code", e.type}, {"message", e.what()} };
            details["param"] = e.param.empty() ? json(nullptr) : json(e.param);
            up_set_ready(sp, "settings_invalid", details);
            return;
        }
    }
    // 4. restage under the current id (a rename after name_taken)
    std::string error;
    if (!up_stage(sp, error)) {
        up_set_ready(sp, "publish_error", { {"code", "staging_failed"}, {"message", error} });
        return;
    }
    // 5. the per-model entry BEFORE the files become visible: autoload could start the model with defaults otherwise.
    //    Written even when "keep" leaves the values as they were: the entry starts again as pending, so the stage-2
    //    trial (and the revert after repeated failures) covers the first start of the new files
    json previous;
    bool entry_written = false;
    if (!without_settings) {
        admin_kv saved;
        json overrides;
        const bool has_entry = admin_internal_entry(id, overrides, saved);
        if (!settings.empty() || (mode == "replace" && has_entry)) {
            if (!admin_internal_put_upload_entry(id, settings, previous, error)) {
                up_set_ready(sp, "settings_not_saved", { {"code", "settings_not_saved"},
                    {"message", "the placement could not be saved: " + error + " (publish without it, or fix the state directory)"} });
                return;
            }
            entry_written = true;
        }
    }
    // 6. one rename that never replaces anything: a set is published as one folder
    json plan;
    std::string staged_id;
    {
        std::lock_guard<std::mutex> lk(sp->mtx);
        plan      = sp->plan;
        staged_id = sp->staged_id;
    }
    const bool folder = up_json_str(plan, "layout") == "folder";
    const std::string target = folder ? id : id + ".gguf";
    int err = 0;
    up_dir out;
    up_rename_result rr = UP_RENAME_FAILED;
    std::string method;
    bool other_fs = false;
    if (up_dir_open(sp->dir, UP_OUT_DIR, false, 0, out, err)) {
        std::lock_guard<std::mutex> lk(u.mtx);
        // the staged files and the models dir on one file system right now (something mounted over the staging
        // folder or the models dir since the upload began makes the rename a copy, which is refused)
        up_stat a;
        up_stat b;
        other_fs = up_dir_stat(out, a) && up_dir_stat(u.root, b) && a.dev != b.dev;
        if (!other_fs) {
            rr = up_rename_noreplace(out, target, u.root, target, folder, true, err, method);
        } else {
            err = EXDEV;
        }
    }
    if (rr != UP_RENAME_OK) {
        if (entry_written) {
            std::string restore_error;
            if (!admin_internal_restore_entry(id, previous, restore_error)) {
                SRV_ERR("admin: upload %s: the previous settings of %s could not be restored: %s\n", sp->id.c_str(), id.c_str(), restore_error.c_str());
            }
        }
        if (rr == UP_RENAME_EXISTS) {
            up_set_ready(sp, "name_taken", { {"code", "name_taken"}, {"by", "file"}, {"other", target},
                {"message", "a file or folder named " + target + " appeared in the models directory meanwhile: rename the upload"} });
        } else if (other_fs) {
            up_set_ready(sp, "publish_error", { {"code", "staging_other_filesystem"},
                {"message", std::string(UP_STAGING) + " is on another file system than the models directory now (a mount?): "
                            "publishing would copy the files instead of moving them"} });
        } else {
            up_set_ready(sp, "publish_error", { {"code", "publish_failed"}, {"errno", err},
                {"message", "moving the files into the models directory failed: " + up_errno_text(err)} });
        }
        return;
    }
    {
        std::lock_guard<std::mutex> lk(u.mtx);
        up_fsync_dir(u.root);
    }
    // 7. the model list; a failure keeps the files and the entry (the next rescan attaches the model) - a
    //    deliberate difference from a settings change, which rolls back
    std::string reload_error;
    try {
        routes->models.load_models();
    } catch (const std::exception & e) {
        reload_error = e.what();
        SRV_ERR("admin: upload %s is published, but reloading the model list failed: %s\n", sp->id.c_str(), e.what());
    }
    json result_files = json::array();
    json details = json::object();
    uint64_t bytes = 0;
    {
        std::lock_guard<std::mutex> lk(sp->mtx);
        json shas = json::array();
        for (const auto & f : sp->files) {
            if (f.dropped) {
                continue;
            }
            const std::string rel = folder ? id + "/" + f.final_name : f.final_name;
            result_files.push_back({ {"name", rel}, {"role", f.role}, {"size", f.size}, {"sha256", f.sha_hex} });
            shas.push_back(f.sha_hex);
            bytes += f.size;
        }
        sp->result = {
            {"model",        id},
            {"layout",       folder ? "folder" : "file"},
            {"target",       target},
            {"rename",       method},
            {"reload_error", reload_error.empty() ? json(nullptr) : json(reload_error)},
            {"settings_saved", entry_written},
        };
        sp->result["files"] = result_files;
        sp->state   = UP_PUBLISHED;
        sp->substate.clear();
        sp->blocked_by.clear();
        sp->blocked = nullptr;
        sp->t_final = admin_internal_now_ms();
        sp->last_activity = admin_internal_wall_ms();
        details["id"]     = sp->id;
        details["bytes"]  = bytes;
        details["layout"] = folder ? "folder" : "file";
        details["sha256"] = shas;
        details["arch"]   = up_json_str(sp->facts, "arch");
    }
    json creator;
    {
        std::lock_guard<std::mutex> lk(sp->mtx);
        creator = sp->creator_actor;
        if (!sp->without_settings && !sp->settings.empty()) {
            details["settings"] = up_kv_json(sp->settings); // the placement chosen at upload (whitelisted keys)
        }
    }
    up_persist(sp);
    if (creator.is_object()) {
        admin_audit_for(creator, "upload.published", id, "ok", details);
    } else {
        admin_audit(nullptr, "upload.published", id, "ok", details); // a session rebuilt without its record
    }
    SRV_INF("admin: upload %s published as %s (%s)\n", sp->id.c_str(), id.c_str(), folder ? "folder" : "file");
    // the facts the GPU card and the fit check use
    const std::optional<server_model_meta> meta = routes->models.get_meta(id);
    if (meta.has_value()) {
        std::string model_path;
        std::string mmproj_path;
        meta->preset.get_option("LLAMA_ARG_MODEL", model_path);
        meta->preset.get_option("LLAMA_ARG_MMPROJ", mmproj_path);
        admin_internal_facts_enqueue(model_path, mmproj_path);
    }
    {
        std::lock_guard<std::mutex> lk(u.mtx);
        u.cleanup.push_back(sp->id);
    }
}

// removes the staging folder of a finished session (and leftovers: trash-*, orphans)
static void up_work_cleanup() {
    up_context & u = up_ctx();
    std::vector<std::string> names;
    {
        std::lock_guard<std::mutex> lk(u.mtx);
        names.swap(u.cleanup);
    }
    for (const auto & name : names) {
        std::shared_ptr<up_session> sp = up_find(name);
        if (sp) {
            std::lock_guard<std::mutex> lk_io(sp->mtx_io);      // no chunk is being written into it
            std::lock_guard<std::mutex> lk_rec(sp->mtx_record); // and no hint
            std::lock_guard<std::mutex> lk(sp->mtx);
            if (!up_is_final(sp->state)) {
                continue; // not finished (a stale entry of the list)
            }
            sp->lock.release();
            sp->dir.reset();
        }
        bool ok = true;
        {
            std::lock_guard<std::mutex> lk(u.mtx);
            if (u.staging.valid()) {
                ok = up_remove_tree_at(u.staging, name);
            }
        }
        // an abandoned folder is recorded when its removal is done: once when it worked, once when it failed (the
        // sweep retries it every minute) and once more when a retry removed it
        const auto it_swept = u.swept.find(name);
        json swept = it_swept != u.swept.end() ? it_swept->second : json();
        if (it_swept != u.swept.end()) {
            u.swept.erase(it_swept);
        }
        const bool reported = u.cleanup_failed.count(name) > 0;
        if (ok) {
            if (reported) {
                u.cleanup_failed.erase(name);
                SRV_INF("admin: %s/%s is removed now\n", UP_STAGING, name.c_str());
            }
            if (swept.is_object()) {
                admin_audit(nullptr, "upload.swept", name, "ok", swept);
            }
        } else if (!reported) {
            u.cleanup_failed.insert(name);
            SRV_WRN("admin: could not remove all of %s/%s, the sweep retries (reported again once it is gone)\n", UP_STAGING,
                    name.c_str());
            if (swept.is_object()) {
                admin_audit(nullptr, "upload.swept", name, "failed", swept);
            }
        }
    }
}

static std::shared_ptr<up_session> up_session_from_json(const json & j, bool from_hint);

// the staging hint of a session folder `name`: true when it is valid (what up_recover would rebuild a session from);
// mtime_s: when it was written last (its router rewrites it at least every sweep while it runs)
static bool up_read_hint(const up_dir & d, const std::string & name, json & j, int64_t & mtime_s) {
    j = nullptr;
    mtime_s = 0;
    up_stat st;
    if (!up_stat_at(d, UP_HINT_FILE, st) || !st.is_reg || st.is_link || st.size >= 1 * UP_MIB) {
        return false;
    }
    mtime_s = st.mtime;
    int err = 0;
    std::string text;
    const int fd = up_file_open(d, UP_HINT_FILE, UP_OPEN_READ, err);
    if (fd < 0) {
        return false;
    }
    uint64_t size = 0;
    if (up_fsize(fd, size) && size > 0 && size < 1 * UP_MIB) {
        text.resize((size_t) size);
        if (!up_pread_all(fd, 0, &text[0], (size_t) size, err)) {
            text.clear();
        }
    }
    up_close(fd, err);
    if (text.empty()) {
        return false;
    }
    j = json::parse_no_throw(text);
    if (!j.is_object() || j.is_discarded()) {
        j = nullptr;
        return false;
    }
    const std::shared_ptr<up_session> sp = up_session_from_json(j, true);
    return sp && sp->id == name;
}

// expiry (counted while the router runs), retention of finished records, leftovers in the staging folder
static void up_work_sweep() {
    up_context & u = up_ctx();
    const int64_t now = admin_internal_now_ms();
    const int64_t ttl = up_ttl_ms();
    const int64_t keep = up_keep_final_ms();
    std::vector<std::shared_ptr<up_session>> all;
    {
        std::lock_guard<std::mutex> lk(u.mtx);
        for (const auto & [id, s] : u.sessions) {
            all.push_back(s);
        }
        u.t_sweep   = now;
        u.sweep_due = false;
    }
    std::vector<std::string> forget;
    for (const auto & sp : all) {
        bool expire = false;
        {
            std::lock_guard<std::mutex> lk(sp->mtx);
            if (sp->state == UP_RECEIVING || sp->state == UP_READY) {
                const int64_t delta = std::min<int64_t>(std::max<int64_t>(0, now - sp->t_idle), 5 * 60 * 1000);
                sp->idle_ms += delta;
                expire = sp->idle_ms >= ttl;
            }
            sp->t_idle = now;
            if (up_is_final(sp->state) && sp->t_final > 0 && now - sp->t_final >= keep) {
                forget.push_back(sp->id);
            }
        }
        if (expire) {
            SRV_INF("admin: upload %s expired after %lld s without progress\n", sp->id.c_str(), (long long) (ttl / 1000));
            up_finish(sp, UP_EXPIRED, { {"code", "expired"}, {"message", "no progress for too long"} }, nullptr, "upload.expired");
        } else if (!up_is_final(sp->state)) {
            up_persist(sp); // the idle time survives a restart
        }
    }
    for (const auto & id : forget) {
        std::lock_guard<std::mutex> lk(u.mtx);
        u.sessions.erase(id);
        up_remove_record(id);
    }
    // leftovers: trash-*, and session folders nobody owns (no session here, its lock free) after a while. A folder
    // with a valid hint of another router (several routers may share the models dir) is that router's session: its
    // TTL counts only while it runs, so it is kept until that TTL could have run out, plus a margin
    std::vector<up_dirent> entries;
    {
        std::lock_guard<std::mutex> lk(u.mtx);
        if (u.staging.valid()) {
            entries = up_list(u.staging);
        }
    }
    const int64_t wall_ms = admin_internal_wall_ms();
    const std::string me = up_owner_id_now();
    for (const auto & e : entries) {
        if (!e.st.is_dir || e.st.is_link) {
            continue;
        }
        if (string_starts_with(e.name, "trash-")) {
            std::lock_guard<std::mutex> lk(u.mtx);
            u.cleanup.push_back(e.name);
            continue;
        }
        if (!up_is_hex(e.name, 32) || up_find(e.name)) {
            continue;
        }
        if (wall_ms / 1000 - e.st.mtime < up_orphan_ms() / 1000) {
            continue;
        }
        up_dir d;
        int err = 0;
        {
            std::lock_guard<std::mutex> lk(u.mtx);
            if (!up_dir_open(u.staging, e.name, false, 0, d, err)) {
                continue;
            }
        }
        json hint;
        int64_t hint_mtime = 0;
        const bool has_hint = up_read_hint(d, e.name, hint, hint_mtime);
        const std::string owner = has_hint ? up_json_str(hint, "owner") : std::string();
        const bool foreign = has_hint && owner != me;
        if (foreign) {
            const int64_t idle    = std::max<int64_t>(0, up_json_i64(hint, "idle_ms", 0));
            const int64_t keep_ms = std::max<int64_t>(0, ttl - idle) + up_foreign_margin_ms();
            if (wall_ms - hint_mtime * 1000 < keep_ms) {
                continue; // not even its lock is touched
            }
        }
        bool elsewhere = false;
        int open_err = 0;
        up_lock probe;
        if (!up_lock_take(d, up_lock_owner_text(), probe, elsewhere, &open_err)) {
            if (!elsewhere) {
                static std::set<std::string> warned; // worker thread only
                if (warned.insert(e.name).second) {
                    SRV_WRN("admin: the lock of the upload folder %s/%s cannot be checked (%s), it is left alone\n", UP_STAGING,
                            e.name.c_str(), up_errno_text(open_err).c_str());
                }
            }
            continue; // another router's live session, or one that cannot be checked
        }
        probe.release();
        d.reset();
        if (u.cleanup_failed.count(e.name) == 0) { // a retry of a folder that could not be removed is not logged again
            if (foreign) {
                SRV_INF("admin: removing the upload folder %s/%s of another router (%s): it made no progress for longer than its expiry\n",
                        UP_STAGING, e.name.c_str(), owner.empty() ? "unknown" : owner.c_str());
            } else {
                SRV_INF("admin: removing the abandoned upload folder %s/%s\n", UP_STAGING, e.name.c_str());
            }
        }
        // recorded by up_work_cleanup() once the removal is done (this worker thread, right after the sweep)
        u.swept[e.name] = {
            {"folder",  std::string(UP_STAGING) + "/" + e.name}, // relative to models_dir
            {"foreign", foreign},
            {"name",    has_hint ? json(up_json_str(hint, "name")) : json(nullptr)},
        };
        std::lock_guard<std::mutex> lk(u.mtx);
        u.cleanup.push_back(e.name);
    }
}

static void up_worker_main() {
    up_context & u = up_ctx();
    while (!u.stop.load()) {
        bool did = false;
        try {
            up_work_cleanup();
            did = up_work_header_checks();
            if (!did) {
                std::vector<std::shared_ptr<up_session>> all;
                {
                    std::lock_guard<std::mutex> lk(u.mtx);
                    for (const auto & [id, s] : u.sessions) {
                        all.push_back(s);
                    }
                }
                for (const auto & sp : all) {
                    up_state state;
                    bool cancel = false;
                    int64_t after = 0;
                    {
                        std::lock_guard<std::mutex> lk(sp->mtx);
                        state  = sp->state;
                        cancel = sp->cancel_requested;
                        after  = sp->t_checks_after;
                    }
                    if (cancel || up_is_final(state)) {
                        continue;
                    }
                    if (state == UP_CHECKING && admin_internal_now_ms() >= after) {
                        up_work_checks(sp);
                        did = true;
                        // a pass ends in another state (or stops for a shutdown or a cancel); one that left the
                        // session in checking would run again at once, forever: it waits, and says so
                        std::lock_guard<std::mutex> lk(sp->mtx);
                        if (sp->state == UP_CHECKING && !sp->cancel_requested && !u.stop.load()) {
                            sp->t_checks_after = admin_internal_now_ms() + 5000;
                            SRV_WRN("admin: upload %s: the checks ended without a result, they run again in 5 s\n", sp->id.c_str());
                        }
                        break;
                    }
                    if (state == UP_PUBLISHING) {
                        up_work_publish(sp);
                        did = true;
                        break;
                    }
                }
            }
            bool sweep = false;
            {
                std::lock_guard<std::mutex> lk(u.mtx);
                sweep = u.sweep_due || admin_internal_now_ms() - u.t_sweep >= up_sweep_ms();
            }
            if (sweep) {
                up_work_sweep();
                up_work_cleanup();
            }
        } catch (const std::exception & e) {
            // an exception that leaves this thread would end the router (std::terminate)
            SRV_ERR("admin: the upload worker failed: %s\n", e.what());
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        if (did) {
            continue;
        }
        std::unique_lock<std::mutex> lk(u.mtx);
        u.cv.wait_for(lk, std::chrono::milliseconds(500), [&]() { return u.stop.load() || u.wake; });
        u.wake = false;
    }
}

//
// POST /admin/models/delete: a models_dir model (or only its projector)
//

struct up_owned_file {
    std::string rel;   // relative to models_dir: "x.gguf" or "folder/x.gguf"
    std::string name;  // the file name
    std::string role;  // model | shard | projector | draft
    uint64_t    bytes = 0;
};

// the files the preset of a models_dir model references, checked to lie inside the canonical models_dir at depth 1
// (a top-level file) or 2 (inside a first-level folder), none of them a symlink; throws 409 not_deletable.
// Only what the models_dir scanner gave this model is its own (common/preset.cpp load_from_models_dir): the model
// file (and its shards) under the id, and for a folder model the projector and draft inside that folder. A projector
// or draft that comes from the router command line or the [*] section of the preset file is shared by every model:
// it is never deleted, `shared` lists it
static std::vector<up_owned_file> up_owned_files(const server_model_meta & meta, bool projector_only, std::string & folder,
                                                 std::vector<std::string> & shared) {
    up_context & u = up_ctx();
    folder.clear();
    shared.clear();
    auto refuse = [](const std::string & reason, const std::string & message) {
        admin_http_error e(409, "not_deletable", message);
        e.extra = { {"reason", reason} };
        throw e;
    };
    if (meta.source != SERVER_MODEL_SOURCE_MODELS_DIR) {
        if (meta.source == SERVER_MODEL_SOURCE_CACHE) {
            refuse("cache", "this model is in the Hugging Face cache: delete it with DELETE /models (the Delete button of the model list)");
        }
        refuse("preset", "this model is defined (or changed) in the preset file models.ini: remove its section there");
    }
    if (u.models_dir.empty()) {
        refuse("no_models_dir", "this server has no --models-dir");
    }
    std::string model_path;
    std::string mmproj_path;
    std::string draft_path;
    meta.preset.get_option("LLAMA_ARG_MODEL", model_path);
    meta.preset.get_option("LLAMA_ARG_MMPROJ", mmproj_path);
    meta.preset.get_option("LLAMA_ARG_SPEC_DRAFT_MODEL", draft_path);
    if (model_path.empty()) {
        refuse("no_file", "this model has no model file");
    }
    // where the scanner put this model: <models_dir>/<id>.gguf (a top-level file) or <models_dir>/<id>/ (a folder)
    std::string own_folder; // "" = top-level file
    {
        std::error_code ec;
        const fs::path p = fs::absolute(admin_internal_path_from_utf8(model_path), ec);
        const fs::path parent = fs::weakly_canonical(p.parent_path(), ec);
        std::string id_of_file = admin_internal_path_to_utf8(p.filename());
        string_replace_all(id_of_file, ".gguf", "");
        const std::string not_own = "the model file of '" + meta.name + "' is not the one the models directory gives it (it is "
                                    "set in the preset file or on the router command line): it is not deleted from here";
        if (!ec && parent == u.models_dir) {
            if (id_of_file != meta.name) {
                refuse("preset", not_own);
            }
        } else if (!ec && parent.parent_path() == u.models_dir) {
            if (admin_internal_path_to_utf8(parent.filename()) != meta.name) {
                refuse("preset", not_own);
            }
            own_folder = meta.name;
        } else {
            refuse("outside_models_dir", "a file of this model is not directly in the models directory or one of its folders: " + model_path);
        }
    }
    // a companion is this model's own only inside its own folder
    auto own_companion = [&](const std::string & path) {
        if (path.empty() || own_folder.empty()) {
            return false;
        }
        std::error_code ec;
        const fs::path p = fs::absolute(admin_internal_path_from_utf8(path), ec);
        const fs::path parent = fs::weakly_canonical(p.parent_path(), ec);
        return !ec && parent.parent_path() == u.models_dir && admin_internal_path_to_utf8(parent.filename()) == own_folder;
    };
    const bool own_mmproj = own_companion(mmproj_path);
    const bool own_draft  = own_companion(draft_path);
    if (!mmproj_path.empty() && !own_mmproj) {
        shared.push_back(mmproj_path);
    }
    if (!draft_path.empty() && !own_draft && !projector_only) {
        shared.push_back(draft_path);
    }
    if (projector_only && !own_mmproj) {
        refuse("no_projector", mmproj_path.empty() ? "this model has no projector (mmproj)" :
               "the projector of this model is set for every model (on the router command line or in the [*] section of the "
               "preset file): it is not removed from here");
    }

    std::vector<std::pair<std::string, std::string>> wanted; // path, role
    if (!projector_only) {
        wanted.push_back({ model_path, "model" });
        // the other shards, named like the loader names them (<prefix>-%05d-of-%05d.gguf)
        const std::string file = admin_internal_path_to_utf8(admin_internal_path_from_utf8(model_path).filename());
        const size_t pos = file.rfind("-00001-of-");
        if (pos != std::string::npos && file.size() == pos + 20 && string_ends_with(file, ".gguf")) {
            uint64_t count = 0;
            if (up_parse_u64(file.substr(pos + 10, 5), count) && count > 1 && count <= 65535) {
                const fs::path dir = admin_internal_path_from_utf8(model_path).parent_path();
                const std::string prefix = file.substr(0, pos);
                for (uint64_t i = 2; i <= count; i++) {
                    const std::string shard = string_format("%s-%05llu-of-%05llu.gguf", prefix.c_str(), (unsigned long long) i, (unsigned long long) count);
                    wanted.push_back({ admin_internal_path_to_utf8(dir / admin_internal_path_from_utf8(shard)), "shard" });
                }
            }
        }
        if (own_draft) {
            wanted.push_back({ draft_path, "draft" });
        }
    }
    if (own_mmproj) {
        wanted.push_back({ mmproj_path, "projector" });
    }

    std::vector<up_owned_file> out;
    for (const auto & [path, role] : wanted) {
        std::error_code ec;
        const fs::path p = fs::absolute(admin_internal_path_from_utf8(path), ec);
        const fs::path parent = fs::weakly_canonical(p.parent_path(), ec);
        if (ec) {
            refuse("outside_models_dir", "a file of this model is not inside the models directory: " + path);
        }
        std::string rel;
        std::string sub;
        if (parent == u.models_dir) {
            rel = admin_internal_path_to_utf8(p.filename());
        } else if (parent.parent_path() == u.models_dir) {
            sub = admin_internal_path_to_utf8(parent.filename());
            rel = sub + "/" + admin_internal_path_to_utf8(p.filename());
        } else {
            refuse("outside_models_dir", "a file of this model is not directly in the models directory or one of its folders: " + path);
        }
        if (!sub.empty()) {
            if (!folder.empty() && folder != sub) {
                refuse("outside_models_dir", "the files of this model are in more than one folder");
            }
            folder = sub;
        }
        up_owned_file f;
        f.rel  = rel;
        f.name = admin_internal_path_to_utf8(p.filename());
        f.role = role;
        out.push_back(f);
    }
    // no symlinks, only regular files (lstat through the directory descriptors)
    up_dir root;
    int err = 0;
    {
        std::lock_guard<std::mutex> lk(u.mtx);
        if (!up_dir_open_root(u.models_dir, root, err)) {
            refuse("no_models_dir", "the models directory cannot be opened: " + up_errno_text(err));
        }
    }
    up_dir sub;
    if (!folder.empty()) {
        up_stat st;
        if (!up_stat_at(root, folder, st) || st.is_link || !st.is_dir) {
            refuse("symlink", "the folder of this model is a symlink or not a folder: it is not deleted from here");
        }
        if (!up_dir_open(root, folder, false, 0, sub, err)) {
            refuse("symlink", "the folder of this model cannot be opened: " + up_errno_text(err));
        }
    }
    std::vector<up_owned_file> present;
    for (auto & f : out) {
        const up_dir & d = folder.empty() || f.rel.find('/') == std::string::npos ? root : sub;
        up_stat st;
        if (!up_stat_at(d, f.name, st)) {
            if (f.role == "shard" || f.role == "draft") {
                continue; // a shard that is missing anyway
            }
            refuse("missing", "a file of this model is missing: " + f.rel);
        }
        if (st.is_link || !st.is_reg) {
            refuse("symlink", "a file of this model is a symlink or not a regular file: " + f.rel + " (it is not deleted from here)");
        }
        f.bytes = st.size;
        present.push_back(f);
    }
    return present;
}

static server_http_res_ptr up_handle_delete(const server_http_req & req) {
    const json body = admin_internal_body_object(req);
    const std::string requested = up_json_str(body, "model");
    if (requested.empty()) {
        admin_http_error e(400, "invalid_request_error", "model must be a model id");
        e.param = "model";
        throw e;
    }
    const bool dry_run        = up_body_bool(body, "dry_run", false);
    const bool stop           = up_body_bool(body, "stop", false);
    const bool remove_settings = up_body_bool(body, "remove_settings", true);
    const bool projector_only = up_body_bool(body, "projector_only", false);
    if (!dry_run) {
        admin_audit_intent(req, projector_only ? "model.remove_projector" : "model.delete", requested);
    }
    server_models_routes * routes = admin_internal_routes();
    up_context & u = up_ctx();

    auto describe = [&](const server_model_meta & meta, std::vector<up_owned_file> & files, std::string & folder, json & out) {
        std::vector<std::string> shared;
        files = up_owned_files(meta, projector_only, folder, shared);
        json jfiles = json::array();
        uint64_t total = 0;
        std::set<std::string> owned;
        for (const auto & f : files) {
            jfiles.push_back({ {"path", f.rel}, {"bytes", f.bytes}, {"role", f.role} });
            total += f.bytes;
            owned.insert(f.name);
        }
        // the other entries of the folder stay: "kept, not part of this model"
        json kept = json::array();
        size_t n_kept = 0; // all of them; the list holds the first 100
        uint64_t kept_bytes = 0;
        bool other_model = false;
        bool folder_removed = false;
        if (!folder.empty() && !projector_only) {
            up_dir root;
            up_dir sub;
            int err = 0;
            if (up_dir_open_root(u.models_dir, root, err) && up_dir_open(root, folder, false, 0, sub, err)) {
                for (const auto & e : up_list(sub)) {
                    if (owned.count(e.name)) {
                        continue;
                    }
                    const uint64_t bytes = e.st.is_dir ? 0 : e.st.size; // folders are kept whole, their size is not needed
                    n_kept++;
                    if (kept.size() < 100) {
                        kept.push_back({ {"path", folder + "/" + e.name}, {"bytes", e.st.is_dir ? json(nullptr) : json(bytes)},
                                         {"kind", e.st.is_dir ? "dir" : (e.st.is_link ? "symlink" : "file")} });
                    }
                    if (!e.st.is_dir) {
                        kept_bytes += bytes;
                    }
                    other_model = other_model || (e.st.is_reg && string_ends_with(e.name, ".gguf") && e.name.find("mmproj") == std::string::npos);
                }
                folder_removed = up_list(sub).size() == owned.size();
            }
        }
        // a projector or draft every model uses (router command line, [*] of the preset file): it stays
        for (const auto & path : shared) {
            std::error_code ec;
            const fs::path p = fs::absolute(admin_internal_path_from_utf8(path), ec);
            const fs::path parent = fs::weakly_canonical(p.parent_path(), ec);
            std::string shown = path;
            if (!ec && parent == u.models_dir) {
                shown = admin_internal_path_to_utf8(p.filename());
            } else if (!ec && parent.parent_path() == u.models_dir) {
                shown = admin_internal_path_to_utf8(parent.filename()) + "/" + admin_internal_path_to_utf8(p.filename());
            }
            const uintmax_t size = fs::file_size(p, ec);
            n_kept++;
            if (kept.size() < 100) {
                kept.push_back({ {"path", shown}, {"bytes", ec ? json(nullptr) : json((uint64_t) size)}, {"kind", "file"}, {"shared", true} });
            }
            if (!ec) {
                kept_bytes += (uint64_t) size;
            }
        }
        bool pinned = false;
        bool has_settings = false;
        admin_internal_model_flags(meta.name, pinned, has_settings);
        json warnings = json::array();
        if (other_model) {
            warnings.push_back("folder_keeps_other_model"); // after the delete the folder may list another file under this id
        }
        if (meta.is_running()) {
            warnings.push_back("running");
        }
        out = {
            {"model",          meta.name},
            {"source",         server_model_source_to_string(meta.source)},
            {"projector_only", projector_only},
            {"total_bytes",    total},
            {"n_files",        files.size()},
            {"n_kept",         n_kept}, // all of them, the list below holds at most 100
            {"kept_truncated", n_kept > kept.size()},
            {"kept_bytes",     kept_bytes},
            {"running",        meta.is_running()},
            {"status",         server_model_status_to_string(meta.status)},
            {"pinned",         pinned},
            {"has_settings",   has_settings},
            {"folder",         folder.empty() ? json(nullptr) : json(folder)},
            {"folder_removed", folder_removed},
        };
        out["files"]    = jfiles;
        out["kept"]     = kept;
        out["warnings"] = warnings;
    };

    std::optional<server_model_meta> meta = routes->models.get_meta(requested);
    if (!meta.has_value() || meta->status == SERVER_MODEL_STATUS_DOWNLOADING || meta->status == SERVER_MODEL_STATUS_DOWNLOADED) {
        // never 404 (upstream rewrites 404 bodies)
        admin_http_error e(409, "not_deletable", "model '" + requested + "' is not in the model list");
        e.extra = { {"reason", "not_found"} };
        throw e;
    }
    std::vector<up_owned_file> files;
    std::string folder;
    json out;
    describe(*meta, files, folder, out);
    if (dry_run) {
        out["dry_run"] = true;
        return admin_internal_json(200, out);
    }

    std::lock_guard<std::mutex> lk_apply(admin_internal_apply_mutex());
    meta = routes->models.get_meta(meta->name);
    if (!meta.has_value()) {
        admin_http_error e(409, "not_deletable", "model '" + requested + "' is not in the model list any more");
        e.extra = { {"reason", "not_found"} };
        throw e;
    }
    describe(*meta, files, folder, out);
    const std::string id = meta->name;
    if (meta->is_running()) {
        if (!stop) {
            admin_http_error e(409, "model_running", "model '" + id + "' is running: send \"stop\": true to stop it first");
            throw e;
        }
        admin_internal_stop_model(id); // 504 stop_timeout: nothing was deleted
    }

    // move the files into <staging>/trash-<rand>/: they leave the scan at once; deleted afterwards
    up_require_available();
    const std::string trash = "trash-" + up_new_id().substr(0, 16);
    up_dir trash_dir;
    up_dir root;
    up_dir sub;
    int err = 0;
    {
        std::lock_guard<std::mutex> lk(u.mtx);
        if (!up_dir_open(u.staging, trash, true, 0770, trash_dir, err) || !up_dir_open_root(u.models_dir, root, err)) {
            up_throw(500, "server_error", "cannot create the trash folder: " + up_errno_text(err));
        }
    }
    // the owner of models_dir, like the rest of staging: another router on the same models dir (or the host operator)
    // can remove what a router that died before its cleanup left behind
    up_apply_owner_dir(trash_dir, u.owner, nullptr);
    if (!folder.empty() && !up_dir_open(root, folder, false, 0, sub, err)) {
        up_throw(500, "server_error", "cannot open the folder of the model: " + up_errno_text(err));
    }
    std::vector<std::pair<const up_owned_file *, std::string>> moved;
    uint64_t freed = 0;
    for (size_t i = 0; i < files.size(); i++) {
        const up_owned_file & f = files[i];
        const up_dir & from = f.rel.find('/') == std::string::npos ? root : sub;
        const std::string tname = std::to_string(i) + "-" + f.name;
        if (!up_rename_at(from, f.name, trash_dir, tname, err)) {
            // undo what moved so far
            for (const auto & [mf, mt] : moved) {
                int e2 = 0;
                up_rename_at(trash_dir, mt, mf->rel.find('/') == std::string::npos ? root : sub, mf->name, e2);
            }
            {
                std::lock_guard<std::mutex> lk(u.mtx);
                u.cleanup.push_back(trash);
            }
            if (err == EXDEV || err == EBUSY) {
                admin_http_error e(409, "not_deletable", "a file of this model is on another file system or busy (a mount point?): " + f.rel);
                e.extra = { {"reason", "other_filesystem"} };
                throw e;
            }
            up_throw(500, "server_error", "cannot move " + f.rel + " out of the models directory: " + up_errno_text(err));
        }
        moved.push_back({ &f, tname });
        freed += f.bytes;
    }
    bool folder_removed = false;
    if (!folder.empty() && !projector_only && up_list(sub).empty()) {
        sub.reset();
        folder_removed = up_rmdir_at(root, folder, err);
    }
    up_fsync_dir(root);

    // autoload may have started it in between: the deadline-bounded stop again, then the model list
    std::string reload_error;
    json stop_error = nullptr;
    try {
        admin_internal_stop_model(id);
    } catch (const admin_http_error & e) {
        stop_error = e.what();
    }
    if (stop_error.is_null()) {
        try {
            routes->models.load_models();
        } catch (const std::exception & e) {
            reload_error = e.what();
        }
    } else {
        reload_error = "the model list was not reloaded: " + stop_error.get<std::string>();
    }
    std::vector<std::string> paths;
    for (const auto & f : files) {
        paths.push_back(admin_internal_path_to_utf8(u.models_dir / admin_internal_path_from_utf8(f.rel)));
    }
    json forgot = admin_internal_forget_model(id, paths, remove_settings && !projector_only);
    {
        std::lock_guard<std::mutex> lk(u.mtx);
        u.cleanup.push_back(trash); // deleted by the worker (on Linux the space comes back once no child maps the file)
    }
    up_wake_worker();
    json audit_paths = json::array();
    for (const auto & f : files) {
        if (audit_paths.size() < 16) {
            audit_paths.push_back(f.rel); // relative to models_dir
        }
    }
    json details = {
        {"bytes",            freed},
        {"files",            (long long) files.size()},
        {"paths",            audit_paths},
        {"projector_only",   projector_only},
        {"stopped",          meta->is_running()},
        {"settings_removed", forgot.value("settings_removed", false)},
        {"pin_removed",      forgot.value("pin_removed", false)},
        {"folder_removed",   folder_removed},
    };
    if (!reload_error.empty()) {
        details["reload_failed"] = true;
    }
    admin_audit(&req, projector_only ? "model.remove_projector" : "model.delete", id, "ok", details);
    SRV_INF("admin: %s %s (%llu bytes)\n", projector_only ? "removed the projector of" : "deleted", id.c_str(), (unsigned long long) freed);

    json res = {
        {"model",            id},
        {"freed_bytes",      freed},
        {"folder_removed",   folder_removed},
        {"settings_removed", forgot.value("settings_removed", false)},
        {"pin_removed",      forgot.value("pin_removed", false)},
        {"trash_left",       true},
        {"reload_error",     reload_error.empty() ? json(nullptr) : json(reload_error)},
    };
    res[projector_only ? "projector_removed" : "deleted"] = true;
    res["files"] = out.at("files");
    res["kept"]  = out.at("kept");
    return admin_internal_json(200, res);
}

//
// startup: models_dir, staging, recovery of the sessions of an earlier run (base design 3.5 with the critique fixes)
//

static bool up_file_from_record(const json & jf, up_file & f) {
    if (!jf.is_object()) {
        return false;
    }
    f.index = (int) up_json_i64(jf, "index", -1);
    f.name  = up_json_str(jf, "name");
    const int64_t size = up_json_i64(jf, "size", -1);
    if (f.index < 0 || f.name.empty() || size < 24) {
        return false;
    }
    f.size          = (uint64_t) size;
    f.last_modified = up_json_i64(jf, "last_modified", 0);
    f.sha_expected  = up_lower(up_json_str(jf, "sha256_expected"));
    if (!f.sha_expected.empty() && !up_is_hex(f.sha_expected, 64)) {
        f.sha_expected.clear();
    }
    const int64_t durable = up_json_i64(jf, "durable_offset", 0);
    f.durable  = durable < 0 ? 0 : std::min<uint64_t>((uint64_t) durable, f.size);
    f.complete = up_json_bool(jf, "complete", false);
    f.dropped  = up_json_bool(jf, "dropped", false);
    f.sha_hex  = up_lower(up_json_str(jf, "sha256"));
    if (!f.sha_hex.empty() && !up_is_hex(f.sha_hex, 64)) {
        f.sha_hex.clear();
    }
    f.role       = up_json_str(jf, "role");
    f.final_name = up_json_str(jf, "final_name");
    std::string reason;
    if (!f.final_name.empty() && (f.final_name.find('/') != std::string::npos || f.final_name.find('\\') != std::string::npos ||
                                  f.final_name.find("..") != std::string::npos)) {
        return false;
    }
    if (jf.contains("error") && jf.at("error").is_object()) {
        f.error = jf.at("error");
    }
    // written only once the first min(size, 1 MiB) bytes were durable (up_record_json)
    const std::string crc = up_lower(up_json_str(jf, "head_crc32"));
    f.head_crc_valid = up_is_hex(crc, 8);
    f.head_crc       = f.head_crc_valid ? (uint32_t) std::strtoul(crc.c_str(), nullptr, 16) : 0;
    return true;
}

// the resume identity of a recovered file that keeps `keep` bytes: the CRC32 of its first min(keep, 1 MiB) bytes,
// from the record when it holds the whole first MiB, else read again from disk (at most 1 MiB)
static void up_recover_head_crc(const up_dir & dir, const std::string & part, up_file & f, uint64_t keep) {
    const uint64_t head = std::min<uint64_t>(f.size, UP_HEAD_CRC_BYTES);
    if (keep >= head && f.head_crc_valid) {
        return;
    }
    f.head_crc       = 0;
    f.head_crc_valid = true;
    const uint64_t n = std::min<uint64_t>(keep, UP_HEAD_CRC_BYTES);
    if (n == 0) {
        return;
    }
    int err = 0;
    const int fd = up_file_open(dir, part, UP_OPEN_READ, err);
    std::vector<char> buf((size_t) n);
    if (fd < 0 || !up_pread_all(fd, 0, buf.data(), buf.size(), err)) {
        f.head_crc_valid = false; // unknown: the client checks the identity with a verify=1 probe instead
    } else {
        f.head_crc = up_crc32_update(0, (const unsigned char *) buf.data(), buf.size());
    }
    if (fd >= 0) {
        up_close(fd, err);
    }
}

// a session as the record (or, without one, the staging hint) describes it; every field read back is checked
static std::shared_ptr<up_session> up_session_from_json(const json & j, bool from_hint) {
    if (!j.is_object() || up_json_i64(j, "version", 0) != UP_RECORD_VERSION) {
        return nullptr;
    }
    auto sp = std::make_shared<up_session>();
    sp->id   = up_lower(up_json_str(j, "id"));
    sp->name = up_json_str(j, "name");
    std::string reason;
    if (!up_is_hex(sp->id, 32) || !up_name_valid(sp->name, reason)) {
        return nullptr;
    }
    if (!j.contains("files") || !j.at("files").is_array() || j.at("files").empty() || j.at("files").size() > UP_MAX_FILES) {
        return nullptr;
    }
    for (size_t i = 0; i < j.at("files").size(); i++) {
        up_file f;
        if (!up_file_from_record(j.at("files").at(i), f) || f.index != (int) i) {
            return nullptr;
        }
        sha256_init(&f.sha);
        sp->files.push_back(f);
    }
    if (from_hint) {
        // a hint is only a hint: no settings, no automatic publish, the admin confirms
        sp->state = UP_RECEIVING;
        sp->auto_publish = false;
        sp->settings_mode = "keep";
        sp->settings_request = json::object();
        sp->recovered_without_record = true;
        for (auto & f : sp->files) {
            f.complete = false;
            f.sha_hex.clear();
            f.role.clear();
            f.final_name.clear();
            f.error = nullptr;
        }
        return sp;
    }
    if (!up_state_from_name(up_json_str(j, "state"), sp->state)) {
        return nullptr;
    }
    sp->blocked_by    = up_json_str(j, "blocked_by");
    sp->blocked       = j.contains("blocked") ? j.at("blocked") : json(nullptr);
    sp->error         = j.contains("error") ? j.at("error") : json(nullptr);
    sp->settings_mode = up_json_str(j, "settings_mode") == "keep" ? "keep" : "replace";
    sp->auto_publish  = up_json_bool(j, "auto_publish", true);
    sp->creator       = up_json_str(j, "creator");
    sp->creator_actor = j.contains("creator_actor") && j.at("creator_actor").is_object() ? j.at("creator_actor") : json(nullptr);
    sp->created       = up_json_i64(j, "created", 0);
    sp->updated       = up_json_i64(j, "updated", 0);
    sp->last_activity = up_json_i64(j, "last_activity", 0);
    sp->idle_ms       = std::max<int64_t>(0, up_json_i64(j, "idle_ms", 0));
    sp->recovered_without_record = up_json_bool(j, "recovered_without_record", false);
    sp->staged        = up_json_bool(j, "staged", false);
    sp->staged_id     = up_json_str(j, "staged_id");
    if (sp->staged && !up_name_valid(sp->staged_id, reason)) {
        return nullptr;
    }
    sp->settings_request = j.contains("settings_request") && j.at("settings_request").is_object() ? j.at("settings_request") : json::object();
    // for display; the publish validates settings_request again against the devices and locks of then
    if (j.contains("settings") && j.at("settings").is_object()) {
        for (const auto & [key, value] : j.at("settings").items()) {
            if (value.is_string() && key.size() <= 64 && value.get<std::string>().size() <= 256) {
                sp->settings[key] = value.get<std::string>();
            }
        }
    }
    sp->plan    = j.contains("plan") && j.at("plan").is_object() ? j.at("plan") : json(nullptr);
    sp->facts   = j.contains("facts") && j.at("facts").is_object() ? j.at("facts") : json(nullptr);
    sp->result  = j.contains("result") && j.at("result").is_object() ? j.at("result") : json(nullptr);
    if (j.contains("warnings") && j.at("warnings").is_array()) {
        for (size_t i = 0; i < j.at("warnings").size(); i++) {
            if (j.at("warnings").at(i).is_string()) {
                sp->warnings.push_back(j.at("warnings").at(i).get<std::string>());
            }
        }
    }
    return sp;
}

static void up_recover() {
    up_context & u = up_ctx();
    const int64_t now  = admin_internal_now_ms();
    const int64_t wall = admin_internal_wall_ms();
    std::set<std::string> known;

    // 1. the records in the protected state directory
    std::vector<fs::path> records;
    if (!u.records_dir.empty()) {
        std::error_code ec;
        for (fs::directory_iterator it(u.records_dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string fname = admin_internal_path_to_utf8(it->path().filename());
            if (string_ends_with(fname, ".json") && fname.size() == 37 && it->is_regular_file(ec)) {
                records.push_back(it->path());
            }
        }
    }
    for (const auto & path : records) {
        std::string text;
        if (!admin_internal_read_file(path, text, 4 * UP_MIB)) {
            continue;
        }
        const json j = json::parse_no_throw(text);
        std::shared_ptr<up_session> sp = j.is_discarded() ? nullptr : up_session_from_json(j, false);
        if (!sp || admin_internal_path_to_utf8(path.filename()) != sp->id + ".json") {
            SRV_WRN("admin: ignoring the unreadable upload record %s\n", admin_internal_path_to_utf8(path).c_str());
            continue;
        }
        known.insert(sp->id);
        sp->record_persisted = true;
        sp->t_idle = now;
        if (up_is_final(sp->state)) {
            const int64_t age = sp->updated > 0 ? wall - sp->updated : 0;
            if (age >= up_keep_final_ms()) {
                std::error_code ec;
                fs::remove(path, ec);
                std::lock_guard<std::mutex> lk(u.mtx);
                u.cleanup.push_back(sp->id); // its staging folder too, if anything is left
                continue;
            }
            sp->t_final = now - std::max<int64_t>(0, age);
            std::lock_guard<std::mutex> lk(u.mtx);
            u.sessions[sp->id] = sp;
            u.cleanup.push_back(sp->id);
            continue;
        }
        int err = 0;
        {
            std::lock_guard<std::mutex> lk(u.mtx);
            if (!u.staging.valid() || !up_dir_open(u.staging, sp->id, false, 0, sp->dir, err)) {
                sp->dir.reset();
            }
        }
        if (!sp->dir.valid()) {
            // the staged data is gone; a publish that finished before the stop left its target behind
            bool published = false;
            if (sp->state == UP_PUBLISHING && sp->plan.is_object()) {
                up_stat st;
                published = up_stat_at(u.root, up_json_str(sp->plan, "target"), st);
            }
            sp->state   = published ? UP_PUBLISHED : UP_REJECTED;
            sp->error   = published ? json(nullptr) : json({ {"code", "staging_missing"}, {"message", "the staged files of this upload are gone"} });
            sp->t_final = now;
            std::lock_guard<std::mutex> lk(u.mtx);
            u.sessions[sp->id] = sp;
            continue;
        }
        bool elsewhere = false;
        if (!up_lock_take(sp->dir, up_lock_owner_text(), sp->lock, elsewhere)) {
            SRV_WRN("admin: upload %s is held by another process (%s), it is left alone\n", sp->id.c_str(),
                    elsewhere ? "its lock is taken" : "its lock cannot be taken");
            continue;
        }
        // files that an interrupted first staging moved to out/ go back to <i>.part (never re-created empty)
        up_stage_repair(sp);
        // the data on disk: never more than the durable offset of the record (bytes after it may be garbage)
        bool all_complete = true;
        for (auto & f : sp->files) {
            if (f.dropped) {
                continue;
            }
            up_stat st;
            if (sp->staged) {
                // already in out/ under the final names: complete by definition (the record says so)
                f.offset = f.durable = f.size;
                f.complete = true;
                f.sha_valid = false;
                continue;
            }
            const std::string part = std::to_string(f.index) + ".part";
            uint64_t on_disk = 0;
            if (up_stat_at(sp->dir, part, st) && st.is_reg && !st.is_link) {
                on_disk = st.size;
            }
            const uint64_t keep = std::min(f.durable, on_disk);
            const int fd = up_file_open(sp->dir, part, UP_OPEN_CREATE, err);
            if (fd >= 0) {
                if (on_disk != keep) {
                    up_truncate(fd, keep, err);
                    up_datasync(fd, err);
                }
                up_close(fd, err);
            }
            f.offset  = keep;
            f.durable = keep;
            f.complete = keep == f.size;
            f.sha_valid = keep == 0;
            if (!f.complete || f.sha_hex.empty()) {
                f.sha_hex.clear();
            }
            up_recover_head_crc(sp->dir, part, f, keep);
            f.head_state = UP_HEAD_NONE;
            f.head_need  = 8;
            all_complete = all_complete && (f.complete && !f.error.is_object());
        }
        sp->recovered = true;
        switch (sp->state) {
            case UP_RECEIVING:
                if (all_complete) {
                    sp->state    = UP_CHECKING;
                    sp->substate = "queued";
                }
                break;
            case UP_CHECKING:
                sp->substate = "queued";
                break;
            case UP_PUBLISHING: {
                // finished before the stop? the target exists and the staged copy is gone (or is the same inode)
                const std::string target = up_json_str(sp->plan, "target");
                up_stat t;
                up_stat s;
                up_dir out;
                const bool has_target = !target.empty() && up_stat_at(u.root, target, t);
                const bool has_out = up_dir_open(sp->dir, UP_OUT_DIR, false, 0, out, err) && up_stat_at(out, target, s);
                if (has_target && (!has_out || (t.dev == s.dev && t.ino == s.ino && t.ino != 0))) {
                    if (has_out) {
                        up_unlink_at(out, target, err); // a crash between link() and unlink()
                    }
                    sp->state   = UP_PUBLISHED;
                    sp->t_final = now;
                    sp->result  = { {"model", sp->name}, {"target", target}, {"recovered", true} };
                    std::lock_guard<std::mutex> lk(u.mtx);
                    u.cleanup.push_back(sp->id);
                } else if (has_target) {
                    sp->state      = UP_READY;
                    sp->blocked_by = "name_taken";
                    sp->blocked    = { {"code", "name_taken"}, {"by", "file"}, {"other", target},
                                       {"message", target + " exists in the models directory: rename the upload"} };
                } else {
                    // never publish straight from a record: the checks run again first
                    sp->state    = UP_CHECKING;
                    sp->substate = "queued";
                }
                break;
            }
            default:
                break;
        }
        {
            std::lock_guard<std::mutex> lk(u.mtx);
            u.sessions[sp->id] = sp;
        }
        up_persist(sp);
        admin_audit(nullptr, "upload.resumed_after_restart", sp->name, "ok", { {"id", sp->id}, {"state", up_state_name(sp->state)} });
        SRV_INF("admin: upload %s (%s) continues after the restart: %s, %llu of %llu bytes\n", sp->id.c_str(), sp->name.c_str(),
                up_state_name(sp->state), (unsigned long long) up_received_bytes(*sp), (unsigned long long) up_total_bytes(*sp));
    }

    // 2. staging folders without a record (the state directory was not writable): rebuilt from their hint, without
    //    settings and without an automatic publish; leftovers and trash are removed by the worker
    std::vector<up_dirent> entries;
    {
        std::lock_guard<std::mutex> lk(u.mtx);
        if (u.staging.valid()) {
            entries = up_list(u.staging);
        }
    }
    for (const auto & e : entries) {
        if (!e.st.is_dir || e.st.is_link) {
            continue;
        }
        if (string_starts_with(e.name, "trash-")) {
            std::lock_guard<std::mutex> lk(u.mtx);
            u.cleanup.push_back(e.name);
            continue;
        }
        if (!up_is_hex(e.name, 32) || known.count(e.name)) {
            continue;
        }
        up_dir d;
        int err = 0;
        {
            std::lock_guard<std::mutex> lk(u.mtx);
            if (!up_dir_open(u.staging, e.name, false, 0, d, err)) {
                continue;
            }
        }
        json j;
        int64_t hint_mtime = 0;
        if (!up_read_hint(d, e.name, j, hint_mtime)) {
            continue; // an orphan: the sweep removes it once it is old enough
        }
        // only this router's own hints (or one written before hints named their router): a session of another
        // router on the same models dir is never taken over, even while that router is down
        const std::string owner = up_json_str(j, "owner");
        if (!owner.empty() && owner != up_owner_id_now()) {
            SRV_INF("admin: upload folder %s/%s belongs to another router (%s), it is left alone\n", UP_STAGING, e.name.c_str(), owner.c_str());
            continue;
        }
        up_lock lock;
        bool elsewhere = false;
        if (!up_lock_take(d, up_lock_owner_text(), lock, elsewhere)) {
            continue; // another router's live session
        }
        std::shared_ptr<up_session> sp = up_session_from_json(j, true);
        if (!sp || sp->id != e.name) {
            lock.release();
            continue;
        }
        sp->idle_ms = std::max<int64_t>(0, up_json_i64(j, "idle_ms", 0)); // the expiry goes on where it was
        sp->dir  = std::move(d);
        sp->lock = lock;
        lock = up_lock();
        for (auto & f : sp->files) {
            up_stat st;
            uint64_t on_disk = 0;
            const std::string part = std::to_string(f.index) + ".part";
            if (up_stat_at(sp->dir, part, st) && st.is_reg && !st.is_link) {
                on_disk = st.size;
            }
            const uint64_t keep = std::min(f.durable, on_disk);
            const int pfd = up_file_open(sp->dir, part, UP_OPEN_CREATE, err);
            if (pfd >= 0) {
                if (on_disk != keep) {
                    up_truncate(pfd, keep, err);
                }
                up_close(pfd, err);
            }
            f.offset = f.durable = keep;
            f.complete  = keep == f.size;
            f.sha_valid = keep == 0;
            up_recover_head_crc(sp->dir, part, f, keep); // a hint has no CRC: read from disk
        }
        sp->recovered = true;
        sp->created = sp->updated = sp->last_activity = wall;
        sp->t_idle = now;
        sp->warnings.push_back("recovered_without_record");
        {
            std::lock_guard<std::mutex> lk(u.mtx);
            u.sessions[sp->id] = sp;
        }
        up_persist(sp);
        SRV_WRN("admin: upload %s (%s) was rebuilt from its staging folder only: no settings, publish it by hand\n",
                sp->id.c_str(), sp->name.c_str());
    }
}

void admin_upload_init(const common_params & params) {
    up_context & u = up_ctx();
    u.stop.store(false);
    u.models_dir_text = params.models_dir;
    u.models_dir.clear();
    if (!params.models_dir.empty()) {
        std::error_code ec;
        fs::path p = fs::canonical(admin_internal_path_from_utf8(params.models_dir), ec);
        if (ec) {
            p = fs::absolute(admin_internal_path_from_utf8(params.models_dir), ec);
        }
        u.models_dir = p;
    }
    // <state>/uploads is created with the first record (up_persist), not before: the state directory of a
    // server that never uploads keeps only its own files
    u.records_dir = admin_internal_state_dir() / UP_RECORD_DIR;
    u.records_writable = admin_internal_dir_writable(admin_internal_state_dir());
    up_owner_init(u);
#if !defined(_WIN32)
    struct stat sb;
    if (!u.models_dir.empty() && ::stat(u.models_dir.c_str(), &sb) == 0) {
        u.owner.chown       = ::geteuid() == 0 && sb.st_uid != 0; // root in Docker: like the entrypoint, to the owner of the mount
        u.owner.uid         = (unsigned) sb.st_uid;
        u.owner.gid         = (unsigned) sb.st_gid;
        u.owner.setgid_dirs = (sb.st_mode & S_ISGID) != 0;
    }
#endif
    up_open_dirs(u, false);
    if (u.available) {
        SRV_INF("admin: uploads go to %s (staging in %s)\n", u.models_dir_text.c_str(), UP_STAGING);
    } else {
        SRV_INF("admin: uploads are not available: %s\n", u.message.c_str());
    }
    try {
        up_recover();
    } catch (const std::exception & e) {
        SRV_ERR("admin: recovering the uploads of an earlier run failed: %s\n", e.what());
    }
    u.ready = true;
    if (!u.worker_started) {
        u.worker = std::thread(up_worker_main);
        u.worker_started = true;
    }
}

void admin_upload_register(server_http_context & ctx_http, std::vector<std::string> & paths) {
    static const char * const path_upload  = "/admin/models/upload";
    static const char * const path_chunk   = "/admin/models/upload/chunk";
    static const char * const path_update  = "/admin/models/upload/update";
    static const char * const path_publish = "/admin/models/upload/publish";
    static const char * const path_cancel  = "/admin/models/upload/cancel";
    static const char * const path_delete  = "/admin/models/delete";
    ctx_http.get (path_upload,  admin_internal_wrap(up_handle_get));
    ctx_http.post(path_upload,  admin_internal_wrap(up_handle_create));
    ctx_http.post(path_chunk,   admin_internal_wrap(up_handle_chunk));
    ctx_http.post(path_update,  admin_internal_wrap(up_handle_update));
    ctx_http.post(path_publish, admin_internal_wrap(up_handle_publish));
    ctx_http.post(path_cancel,  admin_internal_wrap(up_handle_cancel));
    ctx_http.post(path_delete,  admin_internal_wrap(up_handle_delete));
    // the cheap refusals of a chunk before its body is read (key, allow-list, session busy, writers, size)
    ctx_http.set_pre_body_handler(path_chunk, up_pre_body);
    for (const char * p : { path_upload, path_chunk, path_update, path_publish, path_cancel, path_delete }) {
        paths.push_back(p);
    }
}

json admin_upload_status_json() {
    up_context & u = up_ctx();
    std::lock_guard<std::mutex> lk(u.mtx);
    return {
        {"available", u.available},
        {"reason",    u.reason.empty() ? json(nullptr) : json(u.reason)},
        {"message",   u.message.empty() ? json(nullptr) : json(u.message)},
    };
}

json admin_upload_storage_json() {
    up_context & u = up_ctx();
    std::vector<std::shared_ptr<up_session>> all;
    {
        std::lock_guard<std::mutex> lk(u.mtx);
        for (const auto & [id, s] : u.sessions) {
            all.push_back(s);
        }
        u.sweep_due = true; // GET /admin/storage also runs the sweep (in the worker)
    }
    up_wake_worker();
    json uploads = json::array();
    uint64_t total = 0;
    for (const auto & s : all) {
        std::lock_guard<std::mutex> lk(s->mtx);
        if (up_is_final(s->state)) {
            continue;
        }
        json j = up_session_json(*s, false);
        j["bytes"] = up_received_bytes(*s);
        total += up_received_bytes(*s);
        uploads.push_back(j);
    }
    uint64_t trash = 0;
    {
        std::lock_guard<std::mutex> lk(u.mtx);
        if (u.staging.valid()) {
            for (const auto & e : up_list(u.staging)) {
                if (e.st.is_dir && !e.st.is_link && string_starts_with(e.name, "trash-")) {
                    up_dir d;
                    int err = 0;
                    if (up_dir_open(u.staging, e.name, false, 0, d, err)) {
                        trash += up_tree_bytes(d);
                    }
                }
            }
        }
    }
    return {
        {"uploads", uploads},
        {"upload_staging", {
            {"path",        u.models_dir.empty() ? json(nullptr) : json(admin_internal_path_to_utf8(u.models_dir / UP_STAGING))},
            {"bytes",       total},
            {"trash_bytes", trash},
        }},
    };
}

void admin_upload_tick() {
    up_context & u = up_ctx();
    bool due = false;
    {
        std::lock_guard<std::mutex> lk(u.mtx);
        if (!u.ready) {
            return;
        }
        due = admin_internal_now_ms() - u.t_sweep >= up_sweep_ms();
        if (due) {
            u.sweep_due = true;
            u.wake = true;
        }
    }
    if (due) {
        u.cv.notify_all();
    }
}

void admin_upload_stop() {
    up_context & u = up_ctx();
    u.stop.store(true);
    u.cv.notify_all();
    if (u.worker_started && u.worker.joinable()) {
        try {
            u.worker.join();
        } catch (const std::exception &) {
        }
    }
    u.worker_started = false;
    std::vector<std::shared_ptr<up_session>> all;
    {
        std::lock_guard<std::mutex> lk(u.mtx);
        for (const auto & [id, s] : u.sessions) {
            all.push_back(s);
        }
    }
    for (const auto & sp : all) {
        std::lock_guard<std::mutex> lk(sp->mtx);
        sp->lock.release();
    }
}

//
// PROVISION_ADMIN_HELPER=validate (a short-lived process of this binary; no GPU backend is loaded)
//
//   --validate-phase header --validate-file <path>      the early check of one file (may be a partial upload)
//   --validate-phase full   --validate-file <first shard> [--validate-mmproj <path>]
//
// Prints one PROVISION_ADMIN_RESULT line {"ok": true, "phase": ..., "files": [{path, kind, verdict, code, message,
// facts}]}. verdict: ok | warning | reject (definitive). An abort inside a llama.cpp / ggml call prints a
// definitive reject for the step it was in (ggml abort callback) before the process ends.
//

static std::mutex  g_helper_mtx;
static std::string g_helper_step_path;
static std::string g_helper_step_kind;
static std::string g_helper_phase;
static std::deque<std::string> g_helper_errors;

static void up_helper_log(enum ggml_log_level level, const char * text, void *) {
    if (text == nullptr) {
        return;
    }
    fputs(text, stderr);
    if (level == GGML_LOG_LEVEL_ERROR || level == GGML_LOG_LEVEL_WARN) {
        std::lock_guard<std::mutex> lk(g_helper_mtx);
        std::string line = text;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
            line.pop_back();
        }
        if (!line.empty()) {
            g_helper_errors.push_back(up_cut(line, 400));
            while (g_helper_errors.size() > 12) {
                g_helper_errors.pop_front();
            }
        }
    }
}

static std::string up_helper_errors_text() {
    std::lock_guard<std::mutex> lk(g_helper_mtx);
    std::string out;
    for (const auto & line : g_helper_errors) {
        if (line.find("error") != std::string::npos || line.find("failed") != std::string::npos || line.find("unknown") != std::string::npos ||
            line.find("invalid") != std::string::npos || line.find("not within") != std::string::npos) {
            out += (out.empty() ? "" : "; ") + line;
        }
    }
    return up_cut(out, 1200);
}

static void up_helper_clear_errors() {
    std::lock_guard<std::mutex> lk(g_helper_mtx);
    g_helper_errors.clear();
}

static void up_helper_abort(const char * message) {
    std::string path;
    std::string kind;
    std::string phase;
    {
        std::lock_guard<std::mutex> lk(g_helper_mtx);
        path  = g_helper_step_path;
        kind  = g_helper_step_kind;
        phase = g_helper_phase;
    }
    json file = {
        {"path",    path},
        {"kind",    kind.empty() ? json(nullptr) : json(kind)},
        {"verdict", "reject"},
        {"code",    kind == "projector" ? "projector_bad_metadata" : "malformed"},
        {"message", std::string("llama.cpp stopped on this file: ") + (message != nullptr ? message : "abort")},
    };
    json files = json::array();
    files.push_back(file);
    json result = { {"ok", true}, {"phase", phase}, {"aborted", true} };
    result["files"] = files;
    fprintf(stderr, "%s\n", message != nullptr ? message : "abort");
    admin_print_result(result);
}

static void up_helper_step(const std::string & path, const std::string & kind) {
    std::lock_guard<std::mutex> lk(g_helper_mtx);
    g_helper_step_path = path;
    g_helper_step_kind = kind;
}

static void up_helper_limit_memory() {
#if defined(__linux__)
    struct rlimit rl;
    rl.rlim_cur = (rlim_t) UP_HELPER_AS;
    rl.rlim_max = (rlim_t) UP_HELPER_AS;
    setrlimit(RLIMIT_AS, &rl);
    // the first one the OOM killer picks, never a running model or the router
    FILE * f = std::fopen("/proc/self/oom_score_adj", "w");
    if (f != nullptr) {
        std::fputs("1000", f);
        std::fclose(f);
    }
#elif defined(_WIN32)
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        info.ProcessMemoryLimit = (SIZE_T) UP_HELPER_AS;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info)) ||
            !AssignProcessToJobObject(job, GetCurrentProcess())) {
            CloseHandle(job);
        }
        // the handle stays open for the life of the process (the job ends with it)
    }
#elif !defined(_WIN32)
    struct rlimit rl;
    rl.rlim_cur = (rlim_t) UP_HELPER_AS;
    rl.rlim_max = (rlim_t) UP_HELPER_AS;
    setrlimit(RLIMIT_AS, &rl);
#endif
}

static void up_set_tensor_noop(struct ggml_tensor *, void *) {
}

static json up_helper_facts(const up_gguf_info & g) {
    json f = {
        {"arch",        g.arch},
        {"name",        g.name.empty() ? json(nullptr) : json(g.name)},
        {"n_params",    g.n_params},
        {"n_ctx_train", g.ctx_train >= 0 ? json(g.ctx_train) : json(nullptr)},
        {"n_layer",     g.n_layer >= 0 ? json(g.n_layer) : json(nullptr)},
        {"file_type",   g.file_type >= 0 ? json(g.file_type) : json(nullptr)},
        {"n_tensors",   g.n_tensors},
    };
    if (g.split_count > 1) {
        f["split"] = { {"no", g.split_no}, {"count", g.split_count} };
    }
    return f;
}

// the checks of one projector; empty code = usable
static void up_helper_check_projector(const std::string & path, const up_gguf_info & g, json & out) {
    std::string why;
    if (!up_clip_types_ok(g, why)) {
        out["verdict"] = "reject";
        out["code"]    = "projector_bad_metadata";
        out["message"] = why;
        return;
    }
    up_helper_step(path, "projector");
    const mtmd_caps caps = mtmd_get_cap_from_file(path.c_str());
    out["facts"] = { {"vision", caps.inp_vision}, {"audio", caps.inp_audio}, {"arch", g.arch} };
    if (!caps.inp_vision && !caps.inp_audio) {
        out["verdict"] = "reject";
        out["code"]    = "projector_without_modality";
        out["message"] = "the projector reports neither vision nor audio input (or this build cannot read it)";
        return;
    }
    out["verdict"] = "ok";
}

static json up_helper_header(const std::string & path) {
    json out = { {"path", path}, {"kind", nullptr} };
    const up_gguf_info g = up_gguf_scan_path(path);
    if (g.status != UP_SCAN_OK) {
        out["verdict"] = "reject";
        out["code"]    = g.code == "unsupported_gguf_version" ? "not_gguf" : (g.code.empty() ? "malformed" : g.code);
        out["message"] = g.message;
        return out;
    }
    const std::string type = up_lower(g.type);
    if (type == "adapter") {
        out["kind"] = "adapter";
        out["verdict"] = "reject";
        out["code"]    = "adapter";
        out["message"] = "this is an adapter (LoRA / control vector), not a model";
        return out;
    }
    if (type == "imatrix") {
        out["kind"] = "imatrix";
        out["verdict"] = "reject";
        out["code"]    = "imatrix";
        out["message"] = "this is an importance matrix, not a model";
        return out;
    }
    if (g.arch == "clip") {
        out["kind"] = "projector";
        up_helper_check_projector(path, g, out);
        return out;
    }
    out["kind"]  = "model";
    out["facts"] = up_helper_facts(g);
    if (g.arch.empty() && g.split_count > 1 && g.split_no > 0) {
        // a later shard of a split set holds only the split keys (gguf-split): checked with the first shard
        out["verdict"] = "ok";
        return out;
    }
    if (g.arch.empty()) {
        out["verdict"] = "reject";
        out["code"]    = "unknown_architecture";
        out["message"] = "the file has no general.architecture";
        return out;
    }
    // the loader on the metadata alone: unknown architecture, hyperparameters, vocabulary (no file behind it,
    // no tensor bounds: those are checked when the upload is complete)
    up_helper_step(path, "model");
    gguf_init_params params = { /*no_alloc =*/ true, /*ctx =*/ nullptr };
    gguf_context * meta = gguf_init_from_file(path.c_str(), params);
    if (meta == nullptr) {
        out["verdict"] = "reject";
        out["code"]    = "malformed";
        out["message"] = "the GGUF metadata cannot be read: " + up_helper_errors_text();
        return out;
    }
    llama_model_params mparams = llama_model_default_params();
    static ggml_backend_dev_t no_devices[1] = { nullptr };
    mparams.devices    = no_devices; // no GPU backend is touched
    mparams.vocab_only = true;
    mparams.n_gpu_layers = 0;
    up_helper_clear_errors();
    llama_model * model = llama_model_init_from_user(meta, up_set_tensor_noop, nullptr, mparams);
    if (model == nullptr) {
        const std::string errors = up_helper_errors_text();
        if (errors.find("unknown model architecture") != std::string::npos) {
            out["verdict"] = "reject";
            out["code"]    = "unknown_architecture";
            out["message"] = "architecture '" + g.arch + "' is not supported by this build of the server (update the server or the image)";
        } else if (errors.find("CLIP cannot be used as main model") != std::string::npos) {
            out["verdict"] = "reject";
            out["code"]    = "clip_as_model";
            out["message"] = "a projector cannot be used as the model";
        } else {
            // some architectures may need the tensors: confirmed (or not) when the upload is complete
            out["verdict"] = "warning";
            out["code"]    = "load_check_pending";
            out["message"] = "the metadata alone did not load (" + up_cut(errors, 400) + "); checked again when the upload is complete";
        }
    } else {
        out["verdict"] = "ok";
        llama_model_free(model);
    }
    gguf_free(meta);
    return out;
}

static json up_helper_full_model(const std::string & path) {
    json out = { {"path", path}, {"kind", "model"} };
    const up_gguf_info g = up_gguf_scan_path(path);
    if (g.status != UP_SCAN_OK) {
        out["verdict"] = "reject";
        out["code"]    = g.code.empty() ? "malformed" : g.code;
        out["message"] = g.message;
        return out;
    }
    // every shard within the header limit before the loader parses it
    if (g.split_count > 1) {
        const std::string file = admin_internal_path_to_utf8(admin_internal_path_from_utf8(path).filename());
        const size_t pos = file.rfind("-00001-of-");
        if (pos != std::string::npos) {
            const fs::path dir = admin_internal_path_from_utf8(path).parent_path();
            for (long long i = 2; i <= g.split_count; i++) {
                const std::string shard = string_format("%s-%05lld-of-%05lld.gguf", file.substr(0, pos).c_str(), i, g.split_count);
                const up_gguf_info gs = up_gguf_scan_path(admin_internal_path_to_utf8(dir / admin_internal_path_from_utf8(shard)));
                if (gs.status != UP_SCAN_OK) {
                    out["verdict"] = "reject";
                    out["code"]    = gs.code == "io_error" ? "split_incomplete" : (gs.code.empty() ? "malformed" : gs.code);
                    out["message"] = shard + ": " + gs.message;
                    return out;
                }
            }
        }
    }
    out["facts"] = up_helper_facts(g);
    up_helper_step(path, "model");
    llama_model_params mparams = llama_model_default_params();
    static ggml_backend_dev_t no_devices[1] = { nullptr };
    mparams.devices      = no_devices;
    mparams.vocab_only   = true;
    mparams.n_gpu_layers = 0;
    mparams.load_mode    = LLAMA_LOAD_MODE_NONE;
    up_helper_clear_errors();
    llama_model * model = llama_model_load_from_file(path.c_str(), mparams);
    if (model == nullptr) {
        const std::string errors = up_helper_errors_text();
        std::string code = "load_failed";
        if (errors.find("unknown model architecture") != std::string::npos) {
            code = "unknown_architecture";
        } else if (errors.find("not within the file bounds") != std::string::npos) {
            code = "truncated";
        } else if (errors.find("split") != std::string::npos) {
            code = "split_mismatch";
        } else if (errors.find("CLIP cannot be used") != std::string::npos) {
            code = "clip_as_model";
        }
        out["verdict"] = "reject";
        out["code"]    = code;
        out["message"] = "llama.cpp cannot load this model: " + (errors.empty() ? std::string("see the server log") : errors);
        return out;
    }
    out["facts"]["n_vocab"] = llama_vocab_n_tokens(llama_model_get_vocab(model));
    char desc[256] = {};
    llama_model_desc(model, desc, sizeof(desc));
    if (desc[0] != 0) {
        out["facts"]["desc"] = std::string(desc);
    }
    llama_model_free(model);
    out["verdict"] = "ok";
    return out;
}

int admin_upload_helper_validate(int argc, char ** argv) {
    std::setlocale(LC_NUMERIC, "C");
    up_helper_limit_memory();
    const std::string test = admin_internal_getenv("PROVISION_ADMIN_HELPER_TEST");
    if (test == "hang") {
        std::this_thread::sleep_for(std::chrono::hours(1));
        return 3;
    }
    if (test == "exit") {
        fprintf(stderr, "validate helper: simulated failure\n");
        return 3;
    }
    std::string phase;
    std::string file;
    std::string mmproj;
    for (int i = 1; i + 1 < argc; i++) {
        const std::string a = argv[i];
        if (a == "--validate-phase") {
            phase = argv[++i];
        } else if (a == "--validate-file") {
            file = argv[++i];
        } else if (a == "--validate-mmproj") {
            mmproj = argv[++i];
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_helper_mtx);
        g_helper_phase = phase;
    }
    llama_log_set(up_helper_log, nullptr);
    ggml_set_abort_callback(up_helper_abort);
    json result = { {"ok", true}, {"phase", phase} };
    json files = json::array();
    try {
        if (file.empty() || (phase != "header" && phase != "full")) {
            admin_print_result({ {"ok", false}, {"error", { {"code", "usage"}, {"message", "--validate-phase header|full --validate-file <path>"} }} });
            return 2;
        }
        if (phase == "header") {
            files.push_back(up_helper_header(file));
        } else {
            files.push_back(up_helper_full_model(file));
            if (!mmproj.empty()) {
                json p = { {"path", mmproj}, {"kind", "projector"} };
                const up_gguf_info g = up_gguf_scan_path(mmproj);
                if (g.status != UP_SCAN_OK) {
                    p["verdict"] = "reject";
                    p["code"]    = "projector_bad_metadata";
                    p["message"] = g.message;
                } else {
                    up_helper_check_projector(mmproj, g, p);
                }
                files.push_back(p);
            }
        }
    } catch (const std::bad_alloc &) {
        // the memory limit: not the file's fault for certain, the router keeps the data and offers a retry
        admin_print_result({ {"ok", false}, {"error", { {"code", "out_of_memory"}, {"message", "the check ran out of memory"} }} });
        return 1;
    } catch (const std::exception & e) {
        admin_print_result({ {"ok", false}, {"error", { {"code", "check_error"}, {"message", e.what()} }} });
        return 1;
    }
    result["files"] = files;
    admin_print_result(result);
    return 0;
}
