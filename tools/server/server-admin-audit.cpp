// Provision admin layer, stage 3 milestone M3 (fork-owned, see server-admin-audit.h and docs/provision-admin.md):
// the audit store (three append-only JSON-lines files with rotation, an in-memory ring when they cannot be written),
// the merge windows and caps of refusals, and GET /admin/audit.

#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX // std::min / std::max below, before any header includes windows.h
#endif

#include "server-admin-audit.h"
#include "server-admin-internal.h"

#include "server-common.h"
#include "server-http.h"

#include "common.h"
#include "log.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#include <sys/types.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

//
// constants (fixed: nothing here is tuned per host)
//

static const uint64_t AUD_MAX_BYTES        = 4ull << 20; // a file is rotated to .1 when it would grow past this
static const size_t   AUD_LINE_MAX         = 4096;       // one record, with its newline
static const size_t   AUD_RING_MAX         = 1000;       // records kept in memory while a file cannot be written
static const int64_t  AUD_MERGE_MS         = 60 * 1000;  // refusals of one client + path + status are merged this long
static const int      AUD_GROUPS_PER_MIN   = 60;         // new refusal records per merge window, then one summary
static const size_t   AUD_GROUPS_MAX       = 256;        // open merge windows at once
static const size_t   AUD_PEERS_MAX        = 256;        // distinct clients counted in one summary
static const size_t   AUD_SAMPLE_MAX       = 8;          // client addresses listed in one summary
static const size_t   AUD_ITEMS_MAX        = 16;         // paths / model ids listed in one summary
static const int      AUD_OBSERVED_PER_MIN = 30;         // observed records per merge window, then one summary
static const int64_t  AUD_RETRY_MS         = 60 * 1000;  // a file that could not be written is tried again after this
static const size_t   AUD_TARGET_MAX       = 256;
static const int      AUD_LIMIT_DEFAULT    = 100;
static const int      AUD_LIMIT_MAX        = 500;
static const uint64_t AUD_READ_MAX         = 64ull << 20; // read at most the last 64 MiB of one file (rotation failing for long)

// changes + server records; refusals; observed effects of the public API. The last two can be caused without the
// admin key, so each has a file (and a ring) of its own: they can never push a change record out
enum { AUD_MAIN = 0, AUD_REFUSALS = 1, AUD_OBSERVED = 2, AUD_N_STORES = 3 };
static const char * const AUD_FILE_NAMES[AUD_N_STORES] = { "admin-audit.jsonl", "admin-audit-refusals.jsonl",
                                                            "admin-audit-observed.jsonl" };
static const char * const AUD_STORE_NAMES[AUD_N_STORES] = { "changes", "refusals", "observed" };

//
// state
//

struct aud_line {
    uint64_t    seq = 0;
    std::string text; // one JSON object, without the newline
};

struct aud_store {
    fs::path    path;               // <state>/<name>
    bool        persisted = false;  // records are appended to the file
    uint64_t    size      = 0;      // bytes of the current file, as last seen
    std::deque<aud_line> ring;      // records that are not on disk, oldest first
    uint64_t    lost      = 0;      // records dropped from the full ring in this episode
    std::string error;              // why the file is not written ("" = persisted)
    int64_t     error_wall = 0;     // wall ms: since when
    int64_t     t_retry    = 0;     // steady ms of the next attempt
    bool        rotate_warned = false;
    bool        check_tail = true;  // the file may end without a newline (at startup, after a failed write)
};

// a merge window: refusals of one client + path + status (or the probe of one path, or the overflow of the caps)
struct aud_group {
    int         store = AUD_REFUSALS;
    uint64_t    group = 0;          // seq of its first line: the id of the record
    int64_t     t_open     = 0;     // steady ms
    int64_t     first_wall = 0;
    int64_t     last_wall  = 0;
    uint64_t    count   = 0;
    uint64_t    written = 0;        // count in the latest line written
    bool        multi   = false;    // several clients: they are counted and sampled
    std::set<std::string>    peers;
    bool                     peers_capped = false;
    std::vector<std::string> sample;
    std::set<std::string>    items; // overflow: the paths / model ids
    bool                     items_capped = false;
    std::string kind;
    std::string action;
    std::string target;
    std::string result;
    json        actor;
    json        details;            // the fixed part
};

struct aud_test_hooks {
    uint64_t max_bytes  = 0; // PROVISION_ADMIN_TEST_AUDIT_MAX_BYTES
    uint64_t fail_at    = 0; // PROVISION_ADMIN_TEST_AUDIT_FAIL=<n>[,<count>]: appends n .. n+count-1 fail
    uint64_t fail_count = 0;
    int64_t  retry_ms   = -1; // PROVISION_ADMIN_TEST_AUDIT_RETRY_MS
    int64_t  merge_ms   = -1; // PROVISION_ADMIN_TEST_AUDIT_MERGE_MS
    size_t   line_max   = 0;  // PROVISION_ADMIN_TEST_AUDIT_LINE_MAX
};

struct aud_context {
    std::mutex mtx; // leaf: nothing else is locked or called while it is held
    bool       ready = false;
    aud_store  stores[AUD_N_STORES];
    uint64_t   seq       = 0;
    uint64_t   max_bytes = AUD_MAX_BYTES;
    size_t     line_max  = AUD_LINE_MAX;
    int64_t    merge_ms  = AUD_MERGE_MS;
    int64_t    retry_ms  = AUD_RETRY_MS;
    std::vector<std::string> secrets;
    std::map<std::string, aud_group> groups; // open merge windows
    int64_t    rate_bucket = -1; // merge window of the latest refusal record opened
    int        rate_opened = 0;
    int64_t    obs_bucket  = -1;
    int        obs_count   = 0;
    uint64_t   n_appends   = 0;  // append attempts (test hook)
    aud_test_hooks test;
};

// never destroyed: detached threads may still record while the process exits
static aud_context & aud_ctx() {
    static aud_context * c = new aud_context();
    return *c;
}

static aud_test_hooks g_aud_test; // written once by admin_audit_read_env() before the init

//
// small helpers
//

static bool aud_parse_u64(const std::string & s, uint64_t & out) {
    if (s.empty() || s.size() > 19) {
        return false;
    }
    uint64_t v = 0;
    for (char ch : s) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        v = v * 10 + (uint64_t) (ch - '0');
    }
    out = v;
    return true;
}

// printable ASCII only (header values and ids can hold any byte), cut to max bytes
static std::string aud_printable(const std::string & s, size_t max_len) {
    std::string out;
    out.reserve(std::min(s.size(), max_len));
    for (char ch : s) {
        if (out.size() >= max_len) {
            out += "...";
            break;
        }
        const unsigned char u = (unsigned char) ch;
        out += u >= 0x20 && u < 0x7f ? ch : '?';
    }
    return out;
}

// kind / action / result: a fixed alphabet, so the line head can be read without a JSON parser
static std::string aud_token(const std::string & s) {
    std::string out;
    for (char ch : s) {
        if (out.size() >= 64) {
            break;
        }
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                        ch == '.' || ch == '_' || ch == '-';
        out += ok ? ch : '_';
    }
    return out.empty() ? std::string("unknown") : out;
}

static std::string aud_errno_text(int err) {
#if defined(_WIN32)
    char buf[256] = {};
    strerror_s(buf, sizeof(buf), err);
    return std::string(buf);
#else
    const char * s = strerror(err); // under the store mutex
    return std::string(s != nullptr ? s : "unknown error");
#endif
}

// s with each secret replaced
static std::string aud_redact_text(std::string s, const std::vector<std::string> & secrets) {
    for (const auto & secret : secrets) {
        size_t pos = 0;
        while ((pos = s.find(secret, pos)) != std::string::npos) {
            s.replace(pos, secret.size(), "[redacted]");
            pos += 10;
        }
    }
    return s;
}

// every string (values and keys) of j with each secret replaced
static void aud_redact(json & j, const std::vector<std::string> & secrets) {
    auto clean = [&](const std::string & s) {
        return aud_redact_text(s, secrets);
    };
    if (j.is_string()) {
        j = clean(j.get<std::string>());
    } else if (j.is_array()) {
        for (auto & v : j) {
            aud_redact(v, secrets);
        }
    } else if (j.is_object()) {
        json out = json::object();
        for (auto it = j.begin(); it != j.end(); ++it) {
            json v = it.value();
            aud_redact(v, secrets);
            out[clean(it.key())] = v;
        }
        j = out;
    }
}

// the line of one record: a fixed head (read by aud_scan_head without a JSON parser), then the rest of the object
static std::string aud_build_line(const aud_context & c, uint64_t seq, const std::string & kind, const std::string & action,
                                  const std::string & result, uint64_t group, json rest) {
    aud_redact(rest, c.secrets);
    std::string head = "{\"seq\":" + std::to_string(seq) + ",\"kind\":\"" + aud_token(kind) + "\",\"action\":\"" +
                       aud_token(action) + "\",\"result\":\"" + aud_token(result) + "\"";
    if (group != 0) {
        head += ",\"group\":" + std::to_string(group);
    }
    for (int attempt = 0; attempt < 3; attempt++) {
        const std::string body = safe_json_to_str(rest); // invalid UTF-8 is replaced, never thrown
        std::string line = head;
        if (body.size() > 2) {
            line += "," + body.substr(1);
        } else {
            line += "}";
        }
        if (line.size() + 1 <= c.line_max) {
            return line;
        }
        // too long: the details are cut first, then everything that is not the head
        if (attempt == 0 && rest.contains("details")) {
            const std::string d = safe_json_to_str(rest.at("details"));
            rest["details"] = { {"truncated", true}, {"text", d.substr(0, std::min<size_t>(1024, c.line_max / 4))} };
        } else {
            json small = { {"truncated", true} };
            for (const char * key : { "ts", "time" }) {
                if (rest.contains(key)) {
                    small[key] = rest.at(key);
                }
            }
            rest = small;
        }
    }
    return head + ",\"truncated\":true}";
}

// the head of a line: {"seq":N,"kind":"K","action":"A","result":"R"[,"group":G], ... (a line written by hand or
// cut by a crash falls back to the JSON parser, and is skipped when that fails too)
struct aud_head {
    uint64_t    seq   = 0;
    uint64_t    group = 0;
    std::string kind;
    std::string action;
    std::string result;
};

static bool aud_scan_head(const char * p, size_t n, aud_head & h) {
    size_t i = 0;
    auto lit = [&](const char * s) {
        const size_t len = strlen(s);
        if (i + len > n || memcmp(p + i, s, len) != 0) {
            return false;
        }
        i += len;
        return true;
    };
    auto num = [&](uint64_t & out) {
        const size_t start = i;
        uint64_t v = 0;
        while (i < n && p[i] >= '0' && p[i] <= '9' && i - start < 19) {
            v = v * 10 + (uint64_t) (p[i] - '0');
            i++;
        }
        out = v;
        return i > start;
    };
    auto str = [&](std::string & out) {
        const size_t start = i;
        while (i < n && p[i] != '"' && p[i] != '\\') {
            i++;
        }
        if (i >= n || p[i] != '"') {
            return false;
        }
        out.assign(p + start, i - start);
        i++;
        return true;
    };
    if (!lit("{\"seq\":") || !num(h.seq) || !lit(",\"kind\":\"") || !str(h.kind) || !lit(",\"action\":\"") ||
        !str(h.action) || !lit(",\"result\":\"") || !str(h.result)) {
        return false;
    }
    h.group = 0;
    if (lit(",\"group\":") && !num(h.group)) {
        return false;
    }
    return h.seq > 0;
}

static bool aud_head_of(const char * p, size_t n, aud_head & h) {
    if (aud_scan_head(p, n, h)) {
        return true;
    }
    const json j = json::parse_no_throw(std::string(p, n));
    if (j.is_discarded() || !j.is_object() || !j.contains("seq") || !j.at("seq").is_number_integer() ||
        j.at("seq").get<long long>() <= 0) {
        return false;
    }
    auto s = [&](const char * key) {
        return j.contains(key) && j.at(key).is_string() ? j.at(key).get<std::string>() : std::string();
    };
    h.seq    = j.at("seq").get<uint64_t>();
    h.group  = j.contains("group") && j.at("group").is_number_integer() && j.at("group").get<long long>() > 0
             ? j.at("group").get<uint64_t>() : 0;
    h.kind   = s("kind");
    h.action = s("action");
    h.result = s("result");
    return h.seq > 0;
}

// at most the last `max` bytes of a file (a partial first line is dropped by the reader)
static bool aud_read_tail(const fs::path & path, uint64_t max, std::string & out, bool & cut) {
    out.clear();
    cut = false;
    std::error_code ec;
    const uintmax_t size = fs::file_size(path, ec);
    if (ec) {
        return false;
    }
#if defined(_WIN32)
    FILE * f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
#else
    FILE * f = fopen(path.c_str(), "rb");
#endif
    if (f == nullptr) {
        return false;
    }
    uint64_t start = 0;
    if ((uint64_t) size > max) {
        start = (uint64_t) size - max;
        cut   = true;
    }
#if defined(_WIN32)
    const bool seek_ok = _fseeki64(f, (long long) start, SEEK_SET) == 0;
#else
    const bool seek_ok = fseeko(f, (off_t) start, SEEK_SET) == 0;
#endif
    if (!seek_ok) {
        fclose(f);
        return false;
    }
    out.resize((size_t) ((uint64_t) size - start));
    const size_t n = out.empty() ? 0 : fread(&out[0], 1, out.size(), f);
    out.resize(n);
    fclose(f);
    if (cut) {
        const size_t nl = out.find('\n');
        out.erase(0, nl == std::string::npos ? out.size() : nl + 1);
    }
    return true;
}

// calls fn(ptr, len) for every non-empty line
template <typename F>
static void aud_for_lines(const std::string & text, F fn) {
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) {
            nl = text.size();
        }
        size_t end = nl;
        if (end > pos && text[end - 1] == '\r') {
            end--;
        }
        if (end > pos) {
            fn(text.data() + pos, end - pos);
        }
        pos = nl + 1;
    }
}

// the highest seq in a file (the last lines; all of it when they cannot be read)
static uint64_t aud_last_seq(const fs::path & path) {
    uint64_t best = 0;
    for (uint64_t window : { (uint64_t) 64 << 10, AUD_READ_MAX }) {
        std::string text;
        bool cut = false;
        if (!aud_read_tail(path, window, text, cut)) {
            return 0;
        }
        aud_for_lines(text, [&](const char * p, size_t n) {
            aud_head h;
            if (aud_head_of(p, n, h)) {
                best = std::max(best, h.seq);
            }
        });
        if (best > 0 || !cut) {
            break;
        }
    }
    return best;
}

//
// writing
//

static fs::path aud_rotated_path(const fs::path & path) {
    fs::path p = path;
    p += ".1";
    return p;
}

// file primitives: 0 = ok, else an errno value
static int aud_fd_open(const fs::path & path, int & fd) {
#if defined(_WIN32)
    fd = -1;
    const errno_t rc = _wsopen_s(&fd, path.c_str(), _O_WRONLY | _O_APPEND | _O_CREAT | _O_BINARY | _O_NOINHERIT,
                                 _SH_DENYNO, _S_IREAD | _S_IWRITE);
    return rc != 0 || fd < 0 ? (rc != 0 ? (int) rc : EIO) : 0;
#else
    // never through a symlink; private (also when an older copy was created otherwise)
    fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        return errno;
    }
    struct stat st;
    if (::fstat(fd, &st) == 0 && (st.st_mode & 077) != 0 && st.st_uid == ::geteuid()) {
        (void) ::fchmod(fd, 0600);
    }
    return 0;
#endif
}

static uint64_t aud_fd_size(int fd, uint64_t def) {
#if defined(_WIN32)
    struct _stat64 st;
    return _fstat64(fd, &st) == 0 ? (uint64_t) st.st_size : def;
#else
    struct stat st;
    return ::fstat(fd, &st) == 0 ? (uint64_t) st.st_size : def;
#endif
}

static int aud_fd_write(int fd, const std::string & data) {
    size_t done = 0;
    while (done < data.size()) {
#if defined(_WIN32)
        const int w = _write(fd, data.data() + done, (unsigned int) std::min<size_t>(data.size() - done, 1u << 20));
#else
        const ssize_t w = ::write(fd, data.data() + done, data.size() - done);
        if (w < 0 && errno == EINTR) {
            continue;
        }
#endif
        if (w <= 0) {
            return w < 0 ? errno : EIO;
        }
        done += (size_t) w;
    }
    return 0;
}

static int aud_fd_sync(int fd) {
#if defined(_WIN32)
    return _commit(fd) == 0 ? 0 : errno;
#else
    return ::fsync(fd) == 0 ? 0 : errno;
#endif
}

static bool aud_fd_truncate(int fd, uint64_t size) {
#if defined(_WIN32)
    return _chsize_s(fd, (long long) size) == 0;
#else
    return ::ftruncate(fd, (off_t) size) == 0;
#endif
}

// false when the last byte of a non-empty file is not a newline (a line cut by a crash); true when it cannot be
// read (the append tells whether the file can be used at all)
static bool aud_ends_with_newline(const fs::path & path, uint64_t size) {
    if (size == 0) {
        return true;
    }
#if defined(_WIN32)
    FILE * f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
#else
    FILE * f = fopen(path.c_str(), "rb");
#endif
    if (f == nullptr) {
        return true;
    }
#if defined(_WIN32)
    const bool seek_ok = _fseeki64(f, (long long) size - 1, SEEK_SET) == 0;
#else
    const bool seek_ok = fseeko(f, (off_t) size - 1, SEEK_SET) == 0;
#endif
    int ch = '\n';
    if (seek_ok) {
        ch = fgetc(f);
    }
    fclose(f);
    return ch == EOF || ch == '\n';
}

static int aud_fd_close(int fd) {
#if defined(_WIN32)
    return _close(fd) == 0 ? 0 : errno;
#else
    return ::close(fd) == 0 ? 0 : errno;
#endif
}

// lines appended and made durable (data: one or more lines, each with its newline): open (create, private), rotate
// first when they would grow the file past the size limit, append, fsync, close. A failed write or fsync truncates
// the file back to its old length when it can (the reader skips a cut line anyway). A file that may end without a
// newline (a line cut by a crash, or a failed write whose truncate failed too) gets one first, so that the cut
// line stays on a line of its own. maybe_written: false = nothing of data can be in the file
static bool aud_append_data(aud_context & c, aud_store & s, const std::string & data, std::string & error,
                            bool * maybe_written = nullptr) {
    const std::string name = admin_internal_path_to_utf8(s.path.filename());
    if (maybe_written != nullptr) {
        *maybe_written = false;
    }
    c.n_appends++;
    if (c.test.fail_at > 0 && c.n_appends >= c.test.fail_at && c.n_appends < c.test.fail_at + std::max<uint64_t>(1, c.test.fail_count)) {
        error = "cannot write " + name + ": simulated write error (test hook)";
        return false;
    }
    int fd = -1;
    int err = aud_fd_open(s.path, fd);
    if (err != 0) {
        error = "cannot open " + name + ": " + aud_errno_text(err);
        return false;
    }
    uint64_t old_size = aud_fd_size(fd, s.size);
    if (old_size > 0 && old_size + data.size() > c.max_bytes) {
        // the current file becomes .1 (an older .1 is replaced), the records start a new file
        aud_fd_close(fd);
        std::error_code ec;
        fs::rename(s.path, aud_rotated_path(s.path), ec);
        if (!ec) {
            admin_internal_fsync_dir(s.path.parent_path());
            s.rotate_warned = false;
        } else if (!s.rotate_warned) {
            s.rotate_warned = true;
            SRV_WRN("admin: audit: cannot rotate %s (%s), it keeps growing until the rotation works\n", name.c_str(),
                    ec.message().c_str());
        }
        err = aud_fd_open(s.path, fd);
        if (err != 0) {
            error = "cannot open " + name + ": " + aud_errno_text(err);
            return false;
        }
        old_size = aud_fd_size(fd, ec ? old_size : 0);
    }
    const bool newline_first = s.check_tail && !aud_ends_with_newline(s.path, old_size);
    if (maybe_written != nullptr) {
        *maybe_written = true;
    }
    err = aud_fd_write(fd, newline_first ? "\n" + data : data);
    if (err == 0) {
        err = aud_fd_sync(fd);
    }
    if (err != 0 && aud_fd_truncate(fd, old_size) && maybe_written != nullptr) {
        *maybe_written = false;
    }
    const int err_close = aud_fd_close(fd);
    if (err == 0) {
        err = err_close; // the data is in the file, but whether it is durable is unknown
    }
    if (err != 0) {
        s.check_tail = true;
        error = "cannot write " + name + ": " + aud_errno_text(err);
        return false;
    }
    if (old_size == 0) {
        admin_internal_fsync_dir(s.path.parent_path()); // a new file: its name must survive a power loss too
    }
    s.check_tail = false;
    s.size = old_size + data.size() + (newline_first ? 1 : 0);
    return true;
}

static bool aud_append(aud_context & c, aud_store & s, const std::string & text, std::string & error) {
    return aud_append_data(c, s, text + "\n", error);
}

static void aud_ring_push(aud_store & s, uint64_t seq, const std::string & text) {
    s.ring.push_back({ seq, text });
    while (s.ring.size() > AUD_RING_MAX) {
        s.ring.pop_front();
        s.lost++;
    }
}

static void aud_fail(aud_context & c, int store, const std::string & error) {
    aud_store & s = c.stores[store];
    const bool first = s.persisted || s.error.empty();
    s.persisted = false;
    s.error     = error;
    if (s.error_wall == 0) {
        s.error_wall = admin_internal_wall_ms();
    }
    s.t_retry = admin_internal_now_ms() + c.retry_ms;
    if (first) {
        SRV_ERR("admin: audit: %s - the %s records are kept in memory only (the last %zu) until writing works again "
                "(tried every %lld s)\n", error.c_str(), AUD_STORE_NAMES[store], AUD_RING_MAX, (long long) (c.retry_ms / 1000));
    }
}

// a file that could not be written: try again when it is due. The ring goes first, in order, then a record
// audit.persisted_again of this store (it says what was lost; when the ring was empty it is the proof that the file
// can be written again). They are written in batches of at most the rotation size, one write and one fsync each,
// so that the store mutex is never held for a thousand fsyncs. The persisted_again record exists only once it is in
// the file: when its write fails it is dropped and its seq is given out again (unless part of it may be in the
// file). Caller holds c.mtx
static void aud_retry(aud_context & c, int store) {
    aud_store & s = c.stores[store];
    if (s.persisted || s.path.empty()) {
        return;
    }
    const int64_t now = admin_internal_now_ms();
    if (now < s.t_retry) {
        return;
    }
    s.t_retry = now + c.retry_ms; // the next attempt, unless this one works
    const int64_t wall = admin_internal_wall_ms();
    const json rest = {
        {"ts",      wall},
        {"time",    admin_internal_iso_time(wall)},
        {"target",  AUD_FILE_NAMES[store]},
        {"actor",   { {"type", "server"}, {"label", "server"} }},
        {"details", { {"store", AUD_STORE_NAMES[store]},
                      {"not_persisted_since", s.error_wall > 0 ? json(admin_internal_iso_time(s.error_wall)) : json(nullptr)},
                      {"lost", s.lost} }},
    };
    const uint64_t seq_again = c.seq + 1; // taken only once the record is in the file
    const std::string again = aud_build_line(c, seq_again, "server", "audit.persisted_again", "ok", 0, rest) + "\n";
    size_t done = 0; // ring lines written (removed from the ring at the end)
    bool again_written = false;
    std::string error;
    while (!again_written) {
        std::string data;
        size_t n = 0;
        while (done + n < s.ring.size()) {
            const std::string & text = s.ring[done + n].text;
            if (n > 0 && data.size() + text.size() + 1 > c.max_bytes) {
                break;
            }
            data += text;
            data += '\n';
            n++;
        }
        const bool last = done + n == s.ring.size() && (n == 0 || data.size() + again.size() <= c.max_bytes);
        if (last) {
            data += again;
        }
        bool maybe_written = false;
        if (!aud_append_data(c, s, data, error, &maybe_written)) {
            if (last && maybe_written) {
                c.seq = std::max(c.seq, seq_again); // part of it may be in the file: its seq is never given out again
            }
            break;
        }
        done += n;
        again_written = last;
    }
    s.ring.erase(s.ring.begin(), s.ring.begin() + (std::ptrdiff_t) done);
    if (!again_written) {
        s.error = error;
        return;
    }
    c.seq = std::max(c.seq, seq_again);
    SRV_INF("admin: audit: the %s records are written to %s again (%llu records were lost meanwhile)\n", AUD_STORE_NAMES[store],
            admin_internal_path_to_utf8(s.path.filename()).c_str(), (unsigned long long) s.lost);
    s.persisted  = true;
    s.error.clear();
    s.error_wall = 0;
    s.lost       = 0;
}

// one record into a store: the file when it can be written, else the ring; returns its seq. Caller holds c.mtx
static uint64_t aud_emit(aud_context & c, int store, const std::string & kind, const std::string & action,
                         const std::string & result, uint64_t group, const json & rest) {
    aud_retry(c, store);
    const uint64_t seq = ++c.seq;
    const std::string text = aud_build_line(c, seq, kind, action, result, group, rest);
    aud_store & s = c.stores[store];
    if (s.persisted) {
        std::string error;
        if (aud_append(c, s, text, error)) {
            return seq;
        }
        aud_fail(c, store, error);
    }
    aud_ring_push(s, seq, text);
    return seq;
}

//
// merge windows
//

static void aud_group_peer(aud_group & g, const std::string & client) {
    if (client.empty()) {
        return;
    }
    if (g.peers.count(client)) {
        return;
    }
    if (g.peers.size() >= AUD_PEERS_MAX) {
        g.peers_capped = true;
        return;
    }
    g.peers.insert(client);
    if (g.sample.size() < AUD_SAMPLE_MAX) {
        g.sample.push_back(client);
    }
}

static void aud_group_item(aud_group & g, const std::string & item) {
    if (item.empty() || g.items.count(item)) {
        return;
    }
    if (g.items.size() >= AUD_ITEMS_MAX) {
        g.items_capped = true;
        return;
    }
    g.items.insert(item);
}

// the record of a window as it stands now
static json aud_group_rest(const aud_group & g) {
    json details = g.details.is_object() ? g.details : json::object();
    details["count"] = g.count;
    details["first"] = admin_internal_iso_time(g.first_wall);
    details["last"]  = admin_internal_iso_time(g.last_wall);
    if (g.multi) {
        details["clients"]        = g.peers.size();
        details["clients_capped"] = g.peers_capped;
        details["sample"]         = g.sample;
    }
    if (!g.items.empty()) {
        details["items"]        = g.items;
        details["items_capped"] = g.items_capped;
    }
    return {
        {"ts",      g.first_wall},
        {"time",    admin_internal_iso_time(g.first_wall)},
        {"target",  g.target},
        {"actor",   g.actor},
        {"details", details},
        {"count",   g.count},
    };
}

// writes the final count of every window that ended (all: every window); caller holds c.mtx
static void aud_close_groups(aud_context & c, bool all) {
    const int64_t now = admin_internal_now_ms();
    for (auto it = c.groups.begin(); it != c.groups.end(); ) {
        aud_group & g = it->second;
        if (all || now - g.t_open >= c.merge_ms) {
            if (g.count > g.written) {
                aud_emit(c, g.store, g.kind, g.action, g.result, g.group, aud_group_rest(g));
                g.written = g.count;
            }
            it = c.groups.erase(it);
        } else {
            ++it;
        }
    }
}

// opens a window: its first line is written at once (a crash loses at most the count of the window)
static aud_group & aud_open_group(aud_context & c, const std::string & key, aud_group g) {
    const int64_t wall = admin_internal_wall_ms();
    g.t_open     = admin_internal_now_ms();
    g.first_wall = wall;
    g.last_wall  = wall;
    g.count      = 1;
    g.group      = aud_emit(c, g.store, g.kind, g.action, g.result, 0, aud_group_rest(g));
    g.written    = 1;
    aud_group & out = c.groups[key];
    out = std::move(g);
    return out;
}

//
// API of the store
//

void admin_audit_read_env() {
    aud_test_hooks t;
    uint64_t n = 0;
    if (aud_parse_u64(admin_internal_getenv("PROVISION_ADMIN_TEST_AUDIT_MAX_BYTES"), n) && n > 0) {
        t.max_bytes = std::max<uint64_t>(n, 256);
    }
    const std::string fail = admin_internal_getenv("PROVISION_ADMIN_TEST_AUDIT_FAIL");
    if (!fail.empty()) {
        const std::vector<std::string> parts = string_split<std::string>(fail, ',');
        if (!parts.empty() && aud_parse_u64(parts[0], n) && n > 0) {
            t.fail_at    = n;
            t.fail_count = 1;
        }
        if (parts.size() > 1 && aud_parse_u64(parts[1], n)) {
            t.fail_count = n;
        }
    }
    if (aud_parse_u64(admin_internal_getenv("PROVISION_ADMIN_TEST_AUDIT_RETRY_MS"), n)) {
        t.retry_ms = (int64_t) n;
    }
    if (aud_parse_u64(admin_internal_getenv("PROVISION_ADMIN_TEST_AUDIT_MERGE_MS"), n) && n > 0) {
        t.merge_ms = (int64_t) n;
    }
    if (aud_parse_u64(admin_internal_getenv("PROVISION_ADMIN_TEST_AUDIT_LINE_MAX"), n) && n > 0) {
        t.line_max = (size_t) std::min<uint64_t>(std::max<uint64_t>(n, 512), AUD_LINE_MAX);
    }
    g_aud_test = t;
}

void admin_audit_store_init(const fs::path & state_dir, bool state_dir_ok, const std::string & version) {
    aud_context & c = aud_ctx();
    std::lock_guard<std::mutex> lk(c.mtx);
    // a clean slate (the CLI can start the server more than once per process)
    for (auto & s : c.stores) {
        s = aud_store();
    }
    c.groups.clear();
    c.secrets.clear();
    c.seq         = 0;
    c.rate_bucket = -1;
    c.rate_opened = 0;
    c.obs_bucket  = -1;
    c.obs_count   = 0;
    c.n_appends   = 0;
    c.test        = g_aud_test;
    c.max_bytes   = c.test.max_bytes > 0 ? c.test.max_bytes : AUD_MAX_BYTES;
    c.line_max    = c.test.line_max > 0 ? c.test.line_max : AUD_LINE_MAX;
    c.merge_ms    = c.test.merge_ms > 0 ? c.test.merge_ms : AUD_MERGE_MS;
    c.retry_ms    = c.test.retry_ms >= 0 ? c.test.retry_ms : AUD_RETRY_MS;

    const bool writable = state_dir_ok && admin_internal_dir_writable(state_dir);
    for (int i = 0; i < AUD_N_STORES; i++) {
        aud_store & s = c.stores[i];
        s.path = state_dir / AUD_FILE_NAMES[i];
        std::error_code ec;
        const uintmax_t size = fs::file_size(s.path, ec);
        s.size = ec ? 0 : (uint64_t) size;
        // records continue the numbering of the files (the cursor of GET /admin/audit)
        c.seq = std::max(c.seq, aud_last_seq(s.path));
        c.seq = std::max(c.seq, aud_last_seq(aud_rotated_path(s.path)));
        s.persisted = writable;
        if (!writable) {
            s.error      = state_dir_ok ? "the admin state directory cannot be written" : "the admin state directory does not exist";
            s.error_wall = admin_internal_wall_ms();
            s.t_retry    = admin_internal_now_ms() + c.retry_ms;
        }
    }
    c.ready = true;
    if (!writable) {
        SRV_ERR("admin: audit: %s - the audit log is kept in memory only (the last %zu records), set "
                "PROVISION_ADMIN_STATE_DIR to a writable directory\n", c.stores[AUD_MAIN].error.c_str(), AUD_RING_MAX);
    }
    const int64_t wall = admin_internal_wall_ms();
    json rest = {
        {"ts",      wall},
        {"time",    admin_internal_iso_time(wall)},
        {"target",  ""},
        {"actor",   { {"type", "server"}, {"label", "server"} }},
        {"details", { {"version", version}, {"persisted", writable}, {"max_bytes", c.max_bytes} }},
    };
    aud_emit(c, AUD_MAIN, "server", "audit.started", "ok", 0, rest);
}

void admin_audit_add_secret(const std::string & value) {
    if (value.size() < 8) {
        return; // too short to tell apart from ordinary text (nothing records the key header anyway)
    }
    aud_context & c = aud_ctx();
    std::lock_guard<std::mutex> lk(c.mtx);
    if (std::find(c.secrets.begin(), c.secrets.end(), value) == c.secrets.end()) {
        c.secrets.push_back(value);
    }
}

std::string admin_audit_redact_text(const std::string & text) {
    aud_context & c = aud_ctx();
    std::lock_guard<std::mutex> lk(c.mtx);
    return aud_redact_text(text, c.secrets);
}

json admin_audit_redact_json(const json & value) {
    aud_context & c = aud_ctx();
    json out = value;
    std::lock_guard<std::mutex> lk(c.mtx);
    aud_redact(out, c.secrets);
    return out;
}

void admin_audit_store_add(const std::string & kind, const json & actor, const std::string & action,
                           const std::string & target, const std::string & result, const json & details) {
    aud_context & c = aud_ctx();
    try {
        std::lock_guard<std::mutex> lk(c.mtx);
        if (!c.ready) {
            return;
        }
        aud_close_groups(c, false);
        const int64_t wall = admin_internal_wall_ms();
        const std::string tgt = aud_printable(aud_redact_text(target, c.secrets), AUD_TARGET_MAX); // redacted before the cut
        const int store = kind == "observed" ? AUD_OBSERVED : AUD_MAIN;
        if (kind == "observed") {
            // anyone who can reach the public API can cause these: capped like the refusals
            const int64_t bucket = admin_internal_now_ms() / c.merge_ms;
            if (bucket != c.obs_bucket) {
                c.obs_bucket = bucket;
                c.obs_count  = 0;
            }
            if (c.obs_count >= AUD_OBSERVED_PER_MIN) {
                const std::string key = "more|observed|" + std::to_string(bucket);
                auto it = c.groups.find(key);
                if (it != c.groups.end()) {
                    it->second.count++;
                    it->second.last_wall = wall;
                    aud_group_item(it->second, aud_token(action) + " " + tgt);
                    return;
                }
                aud_group g;
                g.store   = AUD_OBSERVED;
                g.kind    = "observed";
                g.action  = "observed.more";
                g.target  = "";
                g.result  = "ok";
                g.actor   = actor;
                g.details = { {"note", "more effects of the public API than the audit records one by one"} };
                aud_group_item(g, aud_token(action) + " " + tgt);
                aud_open_group(c, key, std::move(g));
                return;
            }
            c.obs_count++;
        }
        json rest = {
            {"ts",      wall},
            {"time",    admin_internal_iso_time(wall)},
            {"target",  tgt},
            {"actor",   actor},
            {"details", details.is_object() ? details : json::object()},
        };
        aud_emit(c, store, kind, action, result, 0, rest);
    } catch (const std::exception & e) {
        SRV_ERR("admin: audit: recording %s failed: %s\n", action.c_str(), e.what());
    }
}

void admin_audit_store_refusal(const json & actor, const std::string & client, const std::string & path, int status,
                               const std::string & code, const std::string & reason, bool probe) {
    aud_context & c = aud_ctx();
    try {
        std::lock_guard<std::mutex> lk(c.mtx);
        if (!c.ready) {
            return;
        }
        aud_close_groups(c, false);
        const int64_t wall = admin_internal_wall_ms();
        const std::string p   = aud_printable(path, AUD_TARGET_MAX);
        const std::string who = aud_printable(client, 64);
        const std::string tail = p + "|" + std::to_string(status) + "|" + aud_token(reason);
        std::string key = probe ? "probe|" + tail : "client|" + who + "|" + tail;
        auto it = c.groups.find(key);
        if (it != c.groups.end()) {
            it->second.count++;
            it->second.last_wall = wall;
            if (it->second.multi) {
                aud_group_peer(it->second, who);
            }
            return;
        }
        const int64_t bucket = admin_internal_now_ms() / c.merge_ms;
        if (bucket != c.rate_bucket) {
            c.rate_bucket = bucket;
            c.rate_opened = 0;
        }
        // the probe has one window per status and reason (it is merged over every client): never capped, so a
        // flood cannot hide that browsers of refused addresses are opening the page
        if (!probe && (c.rate_opened >= AUD_GROUPS_PER_MIN || c.groups.size() + 4 >= AUD_GROUPS_MAX)) {
            // over the caps: one summary per window ("N more from M clients"), so that a flood from many
            // addresses neither grows the table nor fills the file
            key = "more|refusal|" + std::to_string(bucket);
            it = c.groups.find(key);
            if (it != c.groups.end()) {
                it->second.count++;
                it->second.last_wall = wall;
                aud_group_peer(it->second, who);
                aud_group_item(it->second, p);
                return;
            }
            aud_group g;
            g.store   = AUD_REFUSALS;
            g.kind    = "refusal";
            g.action  = "access.refused_more";
            g.target  = "";
            g.result  = "refused";
            g.multi   = true;
            g.actor   = { {"type", "various"}, {"label", "several clients"} };
            g.details = { {"note", "more refused requests than the audit records one by one"} };
            aud_group_peer(g, who);
            aud_group_item(g, p);
            aud_open_group(c, key, std::move(g));
            return;
        }
        if (!probe) {
            c.rate_opened++;
        }
        aud_group g;
        g.store   = AUD_REFUSALS;
        g.kind    = "refusal";
        g.action  = "access.refused";
        g.target  = p;
        g.result  = "refused";
        g.multi   = probe;
        g.actor   = probe ? json({ {"type", "various"}, {"label", "browsers of refused addresses"} }) : actor;
        g.details = { {"status", status}, {"code", aud_token(code)}, {"reason", aud_token(reason)}, {"probe", probe} };
        if (probe) {
            aud_group_peer(g, who);
        }
        aud_open_group(c, key, std::move(g));
    } catch (const std::exception & e) {
        SRV_ERR("admin: audit: recording a refusal failed: %s\n", e.what());
    }
}

void admin_audit_tick() {
    aud_context & c = aud_ctx();
    try {
        std::lock_guard<std::mutex> lk(c.mtx);
        if (!c.ready) {
            return;
        }
        aud_close_groups(c, false);
        for (int i = 0; i < AUD_N_STORES; i++) {
            aud_retry(c, i); // when due (every minute while a file cannot be written)
        }
    } catch (const std::exception & e) {
        SRV_ERR("admin: audit: %s\n", e.what());
    }
}

void admin_audit_flush() {
    aud_context & c = aud_ctx();
    try {
        std::lock_guard<std::mutex> lk(c.mtx);
        if (c.ready) {
            aud_close_groups(c, true);
        }
    } catch (const std::exception & e) {
        SRV_ERR("admin: audit: %s\n", e.what());
    }
}

bool admin_audit_persisted() {
    aud_context & c = aud_ctx();
    std::lock_guard<std::mutex> lk(c.mtx);
    if (!c.ready) {
        return true;
    }
    for (const auto & s : c.stores) {
        if (!s.persisted) {
            return false;
        }
    }
    return true;
}

json admin_audit_status_json() {
    aud_context & c = aud_ctx();
    std::lock_guard<std::mutex> lk(c.mtx);
    bool persisted = true;
    std::string error;
    int64_t since = 0;
    size_t in_memory = 0;
    uint64_t lost = 0;
    for (const auto & s : c.stores) {
        if (!s.persisted) {
            persisted = false;
            if (error.empty()) {
                error = s.error;
            }
            if (s.error_wall > 0 && (since == 0 || s.error_wall < since)) {
                since = s.error_wall;
            }
        }
        in_memory += s.ring.size();
        lost      += s.lost;
    }
    return {
        {"persisted", persisted},
        {"error",     error.empty() ? json(nullptr) : json(error)},
        {"since",     since > 0 ? json(admin_internal_iso_time(since)) : json(nullptr)},
        {"in_memory", in_memory},
        {"lost",      lost},
        {"max_bytes", c.max_bytes},
        {"ring_max",  AUD_RING_MAX},
    };
}

json admin_audit_storage_json() {
    aud_context & c = aud_ctx();
    std::lock_guard<std::mutex> lk(c.mtx);
    json files = json::array();
    uint64_t total = 0;
    bool persisted = true;
    for (int i = 0; i < AUD_N_STORES; i++) {
        const aud_store & s = c.stores[i];
        persisted = persisted && s.persisted;
        for (const fs::path & p : { s.path, aud_rotated_path(s.path) }) {
            std::error_code ec;
            const uintmax_t size = fs::file_size(p, ec);
            if (ec) {
                continue;
            }
            files.push_back({ {"name", admin_internal_path_to_utf8(p.filename())}, {"bytes", (uint64_t) size}, {"store", AUD_STORE_NAMES[i]} });
            total += (uint64_t) size;
        }
    }
    return {
        {"persisted",   persisted},
        {"files",       files},
        {"total_bytes", total},
        {"max_bytes",   c.max_bytes}, // per file; each has one rotated copy (.1)
    };
}

void admin_audit_diagnostics(std::string & status, json & value, std::string & hint) {
    const json st = admin_audit_status_json();
    const json storage = admin_audit_storage_json();
    value = st;
    value["files"] = storage.at("files");
    if (st.at("persisted").get<bool>()) {
        status = "ok";
        hint   = "every record is written to the state directory (rotated at " +
                 std::to_string(st.at("max_bytes").get<uint64_t>() >> 20) + " MiB, one older copy kept)";
        return;
    }
    status = "warning";
    hint   = "the audit log is kept in memory only (the last " + std::to_string(AUD_RING_MAX) + " records) since " +
             (st.at("since").is_string() ? st.at("since").get<std::string>() : std::string("startup")) + ": " +
             (st.at("error").is_string() ? st.at("error").get<std::string>() : std::string("unknown error")) +
             "; it is written again by itself once the state directory can be written (tried every minute), "
             "records kept in memory meanwhile are lost on restart";
}

//
// GET /admin/audit
//

struct aud_record_ref {
    uint64_t    id      = 0;      // seq of the first line (the cursor)
    aud_head    head;
    std::string text;             // the latest version of the record
    bool        persisted = true;
    bool        open      = false; // a merge window still counting
    bool        updated   = false;
};

static std::vector<std::string> aud_split_filter(const std::string & s) {
    std::vector<std::string> out;
    for (const auto & part : string_split<std::string>(s, ',')) {
        std::string v;
        for (char ch : part) {
            if (ch != ' ' && ch != '*') {
                v += ch;
            }
        }
        while (!v.empty() && v.back() == '.') {
            v.pop_back();
        }
        if (!v.empty()) {
            out.push_back(v);
        }
    }
    return out;
}

static bool aud_match(const std::vector<std::string> & filter, const std::string & value, bool prefix) {
    if (filter.empty()) {
        return true;
    }
    for (const auto & f : filter) {
        if (value == f || (prefix && value.size() > f.size() && value.compare(0, f.size(), f) == 0 && value[f.size()] == '.')) {
            return true;
        }
    }
    return false;
}

// every record of every store (files and ring), the latest version of each merged record, and the open windows
static void aud_collect(std::vector<aud_record_ref> & out, std::vector<std::pair<uint64_t, std::string>> * raw) {
    aud_context & c = aud_ctx();
    std::string texts[AUD_N_STORES][2];
    std::deque<aud_line> rings[AUD_N_STORES];
    std::vector<aud_record_ref> open;
    {
        std::lock_guard<std::mutex> lk(c.mtx);
        if (!c.ready) {
            return;
        }
        aud_close_groups(c, false);
        for (int i = 0; i < AUD_N_STORES; i++) {
            bool cut = false;
            aud_read_tail(aud_rotated_path(c.stores[i].path), AUD_READ_MAX, texts[i][0], cut);
            aud_read_tail(c.stores[i].path, AUD_READ_MAX, texts[i][1], cut);
            rings[i] = c.stores[i].ring;
        }
        for (const auto & [key, g] : c.groups) {
            aud_record_ref r;
            r.id          = g.group;
            r.head.seq    = g.group;
            r.head.kind   = g.kind;
            r.head.action = g.action;
            r.head.result = g.result;
            r.text        = aud_build_line(c, g.group, g.kind, g.action, g.result, 0, aud_group_rest(g));
            r.open        = true;
            open.push_back(std::move(r));
        }
    }
    std::map<uint64_t, aud_record_ref> firsts;                   // id -> record
    struct update_ref {
        uint64_t    seq = 0;
        std::string text;
        bool        persisted = true;
    };
    std::map<uint64_t, update_ref> latest; // group -> its latest update
    std::set<uint64_t> seen;
    auto take = [&](const char * p, size_t n, bool persisted) {
        aud_head h;
        if (!aud_head_of(p, n, h) || !seen.insert(h.seq).second) {
            return; // unreadable, or a duplicate (a write whose error came after the data reached the disk)
        }
        if (raw != nullptr) {
            raw->push_back({ h.seq, std::string(p, n) });
        }
        if (h.group != 0) {
            auto it = latest.find(h.group);
            if (it == latest.end() || it->second.seq < h.seq) {
                update_ref u;
                u.seq       = h.seq;
                u.text.assign(p, n);
                u.persisted = persisted;
                latest[h.group] = std::move(u);
            }
            return;
        }
        aud_record_ref r;
        r.id        = h.seq;
        r.head      = h;
        r.text.assign(p, n);
        r.persisted = persisted;
        firsts[h.seq] = std::move(r);
    };
    for (int i = 0; i < AUD_N_STORES; i++) {
        aud_for_lines(texts[i][0], [&](const char * p, size_t n) { take(p, n, true); });
        aud_for_lines(texts[i][1], [&](const char * p, size_t n) { take(p, n, true); });
        for (const auto & l : rings[i]) {
            take(l.text.data(), l.text.size(), false);
        }
    }
    for (auto & [group, upd] : latest) {
        auto it = firsts.find(group);
        if (it != firsts.end()) {
            it->second.text      = upd.text;
            it->second.updated   = true;
            it->second.persisted = it->second.persisted && upd.persisted;
        } else {
            // its first line was rotated away: the update stands for the record
            aud_record_ref r;
            aud_head_of(upd.text.data(), upd.text.size(), r.head);
            r.id        = group;
            r.text      = upd.text;
            r.updated   = true;
            r.persisted = upd.persisted;
            firsts[group] = std::move(r);
        }
    }
    for (auto & r : open) {
        auto it = firsts.find(r.id);
        if (it != firsts.end()) {
            r.persisted = it->second.persisted;
            it->second = std::move(r);
        } else {
            firsts[r.id] = std::move(r);
        }
    }
    out.reserve(firsts.size());
    for (auto & [id, r] : firsts) {
        out.push_back(std::move(r));
    }
}

static server_http_res_ptr aud_handle_get(const server_http_req & req) {
    auto bad = [](const char * param, const std::string & message) {
        admin_http_error e(400, "invalid_request_error", message);
        e.param = param;
        return e;
    };
    uint64_t before = 0;
    const std::string before_text = req.get_param("before");
    if (!before_text.empty() && (!aud_parse_u64(before_text, before) || before == 0)) {
        throw bad("before", "before must be the next_before value of an earlier answer (a positive integer)");
    }
    int limit = AUD_LIMIT_DEFAULT;
    const std::string limit_text = req.get_param("limit");
    if (!limit_text.empty()) {
        uint64_t n = 0;
        if (!aud_parse_u64(limit_text, n) || n < 1 || n > (uint64_t) AUD_LIMIT_MAX) {
            throw bad("limit", string_format("limit must be between 1 and %d", AUD_LIMIT_MAX));
        }
        limit = (int) n;
    }
    const std::vector<std::string> f_kind   = aud_split_filter(req.get_param("kind"));
    const std::vector<std::string> f_action = aud_split_filter(req.get_param("action"));
    const std::vector<std::string> f_result = aud_split_filter(req.get_param("result"));
    for (const auto & k : f_kind) {
        if (k != "change" && k != "refusal" && k != "server" && k != "observed") {
            throw bad("kind", "kind must be change, refusal, server or observed (comma-separated)");
        }
    }
    const std::string format = req.get_param("format");
    if (!format.empty() && format != "json" && format != "jsonl") {
        throw bad("format", "format must be json or jsonl");
    }
    auto wanted = [&](const aud_head & h, uint64_t id) {
        return (before == 0 || id < before) && aud_match(f_kind, h.kind, false) && aud_match(f_action, h.action, true) &&
               aud_match(f_result, h.result, false);
    };

    if (format == "jsonl") {
        // the raw log, oldest first: every line of every file (and of the rings), merged records with their updates
        std::vector<aud_record_ref> records;
        std::vector<std::pair<uint64_t, std::string>> raw;
        aud_collect(records, &raw);
        std::sort(raw.begin(), raw.end(), [](const auto & a, const auto & b) { return a.first < b.first; });
        std::map<uint64_t, const std::string *> live; // first line of a window still counting -> its count so far
        for (const auto & r : records) {
            if (r.open) {
                live[r.id] = &r.text;
            }
        }
        std::string body;
        for (const auto & [seq, text] : raw) {
            aud_head h;
            if (aud_head_of(text.data(), text.size(), h) && wanted(h, h.group != 0 ? h.group : h.seq)) {
                const auto it = h.group == 0 ? live.find(h.seq) : live.end();
                body += it != live.end() ? *it->second : text;
                body += "\n";
            }
        }
        auto res = std::make_unique<server_http_res>();
        res->status       = 200;
        res->content_type = "application/x-ndjson; charset=utf-8";
        res->data         = std::move(body);
        res->headers["Cache-Control"]       = "no-store";
        res->headers["Content-Disposition"] = "attachment; filename=\"provision-admin-audit.jsonl\"";
        return res;
    }

    std::vector<aud_record_ref> records;
    aud_collect(records, nullptr);
    std::vector<const aud_record_ref *> match;
    for (const auto & r : records) {
        if (wanted(r.head, r.id)) {
            match.push_back(&r);
        }
    }
    std::sort(match.begin(), match.end(), [](const aud_record_ref * a, const aud_record_ref * b) { return a->id > b->id; });
    json entries = json::array();
    uint64_t last_id = 0;
    size_t n = 0;
    for (; n < match.size() && (int) n < limit; n++) {
        const aud_record_ref & r = *match[n];
        json j = json::parse_no_throw(r.text);
        if (j.is_discarded() || !j.is_object()) {
            j = { {"seq", r.id}, {"kind", r.head.kind}, {"action", r.head.action}, {"result", r.head.result}, {"unreadable", true} };
        }
        if (r.updated || r.open) {
            j["updated_seq"] = j.value("seq", (uint64_t) r.id);
            j.erase("group");
        }
        j["seq"]       = r.id;
        j["id"]        = std::to_string(r.id);
        j["persisted"] = r.persisted;
        j["open"]      = r.open;
        entries.push_back(j);
        last_id = r.id;
    }
    const bool more = n < match.size();
    const json st = admin_audit_status_json();
    json warnings = json::array();
    if (!st.at("persisted").get<bool>()) {
        warnings.push_back("audit_not_persisted");
    }
    return admin_internal_json(200, {
        {"entries",     entries},
        {"next_before", more ? json(std::to_string(last_id)) : json(nullptr)},
        {"matched",     match.size()},
        {"persisted",   st.at("persisted")},
        {"audit",       st},
        {"warnings",    warnings},
    });
}

void admin_audit_register(server_http_context & ctx_http, std::vector<std::string> & paths) {
    static const char * const path_audit = "/admin/audit";
    ctx_http.get(path_audit, admin_internal_wrap(aud_handle_get));
    paths.push_back(path_audit);
}
