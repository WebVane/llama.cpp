#include "license/RequestCounter.h"

#include "license/DurableWrite.h"
#include "license/Jcs.h"

#include <nlohmann/json.hpp>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <array>
#include <limits>
#include <optional>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>

namespace visio::license {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

constexpr std::string_view kHmacKey =
#ifdef PROVISION_LICENSE_COUNTER_KEY
    PROVISION_LICENSE_COUNTER_KEY
#else
    "provision-llm-counter/v2/9d7f3a51b8e0c2f4"
#endif
    ;

// Anti-rollback tolerance on the wall clock.  Picks up clock jitter
// without opening the door to a meaningful budget reset.
constexpr int64_t kRollbackSkewSec = 60;

std::string hex_lower(const uint8_t* data, std::size_t len) {
    std::ostringstream o;
    o << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < len; ++i) {
        o << std::setw(2) << static_cast<unsigned>(data[i]);
    }
    return o.str();
}

std::string hmac_sha256_hex(std::string_view msg) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> buf{};
    unsigned int out_len = 0;
    HMAC(EVP_sha256(),
         kHmacKey.data(), static_cast<int>(kHmacKey.size()),
         reinterpret_cast<const unsigned char*>(msg.data()), msg.size(),
         buf.data(), &out_len);
    return hex_lower(buf.data(), out_len);
}

// Canonical JSON of the protected fields.  We reuse the JCS serializer
// already verified by the interop conformance tests — the keys are
// ASCII-only so the canonical form is stable across implementations.
// NB: window_start_unix joined the protected set; the format change is
// a hard break with v1 counter.dat files (they fail the HMAC and load
// returns Tampered → fail-closed → operator either deletes counter.dat
// or re-activates, which resets cleanly).
std::string canonical_protected(int64_t used,
                                int64_t last_seen_unix,
                                int64_t window_start_unix,
                                int64_t limit,
                                const std::string& lic_hash) {
    json j;
    j["used"]              = used;
    j["last_seen_unix"]    = last_seen_unix;
    j["window_start_unix"] = window_start_unix;
    j["limit"]             = limit;
    j["lic_hash"]          = lic_hash;
    return jcs_canonicalize(j);
}


// ---- per-licence ledger -------------------------------------------------
//
// {"entries": {"<lic_hash>": {"used":…, "last_seen_unix":…,
//                             "window_start_unix":…, "limit":…}, …},
//  "hmac": "<hex>"}
//
// The HMAC covers the entries under a "kind" tag of their own, so the body
// of a counter.dat can never be passed off as a ledger or the other way
// round even though both use the same key.
constexpr std::size_t kLedgerMaxEntries = 16;

std::string canonical_ledger(const json& entries) {
    json j;
    j["kind"]    = "counter-ledger/v1";
    j["entries"] = entries;
    return jcs_canonicalize(j);
}

// Verified entries, or nullopt when the file is missing, unreadable,
// malformed or fails its HMAC — an untrustworthy ledger is no ledger.
std::optional<json> read_ledger(const fs::path& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return std::nullopt;
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream oss;
    oss << in.rdbuf();
    json j;
    try {
        j = json::parse(oss.str());
    } catch (...) {
        return std::nullopt;
    }
    if (!j.is_object() || !j.contains("entries") || !j["entries"].is_object() ||
        !j.contains("hmac") || !j["hmac"].is_string()) {
        return std::nullopt;
    }
    if (hmac_sha256_hex(canonical_ledger(j["entries"])) != j["hmac"].get<std::string>()) {
        return std::nullopt;
    }
    return j["entries"];
}

bool write_ledger(const fs::path& path, const json& entries) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    json j;
    j["entries"] = entries;
    j["hmac"]    = hmac_sha256_hex(canonical_ledger(entries));
    const std::string body = j.dump();
    std::random_device rd;
    const auto tmp = path.parent_path() /
                     (path.filename().string() + ".tmp." +
                      std::to_string(rd()) + "." + std::to_string(rd()));
    // every byte on disk (write, fsync, size, close checked) before the rename: a full disk leaves the
    // previous ledger in place instead of an empty file
    if (!durable_write_file(tmp, body)) return false;
    return durable_replace(tmp, path);
}

bool ledger_entry_valid(const json& e) {
    return e.is_object() &&
           e.contains("used")              && e["used"].is_number_integer() &&
           e.contains("last_seen_unix")    && e["last_seen_unix"].is_number_integer() &&
           e.contains("window_start_unix") && e["window_start_unix"].is_number_integer() &&
           e.contains("limit")             && e["limit"].is_number_integer();
}

}  // namespace

RequestCounter::RequestCounter(fs::path path,
                               std::string lic_hash,
                               int64_t limit,
                               bool hard_limit)
    : path_(std::move(path)),
      lic_hash_(std::move(lic_hash)),
      limit_(limit),
      hard_limit_(hard_limit) {}

bool RequestCounter::advance_window_if_needed(int64_t now_unix) {
    // No window enforcement when no quota is set.  Saves us the disk
    // write on the unlimited path even across day-boundary requests.
    if (limit_ <= 0) return false;
    // Trial / hard-limit mode: `limit` is a lifetime cap.  Don't roll
    // the 30-day window forward — used stays at whatever it grew to,
    // and once it hits `limit` consume() refuses forever.
    if (hard_limit_) return false;
    if (window_start_unix_ <= 0) {
        // Never anchored (e.g. fresh counter file just created by load()
        // with a v2-format that somehow has window_start_unix=0).
        // Anchor it now so the first 30-day cycle starts here.
        window_start_unix_ = now_unix;
        return false;
    }
    bool advanced = false;
    while (now_unix >= window_start_unix_ + kCounterWindowSeconds) {
        window_start_unix_ += kCounterWindowSeconds;
        used_ = 0;
        advanced = true;
    }
    return advanced;
}

CounterLoad RequestCounter::load(std::chrono::system_clock::time_point now) {
    const int64_t now_unix = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();

    std::error_code ec;
    if (!fs::exists(path_, ec)) {
        // No prior state.  In-memory state is "used=0, last_seen=now,
        // window_start=now, lic_hash=ours".  The file is created on
        // first consume() (or reset() if the activation flow runs).
        used_ = 0;
        last_seen_unix_ = now_unix;
        window_start_unix_ = now_unix;
        last_load_ = CounterLoad::FreshState;
        fail_closed_ = false;
        return last_load_;
    }

    std::ifstream in(path_, std::ios::binary);
    if (!in) {
        last_load_ = CounterLoad::IoError;
        fail_closed_ = true;
        return last_load_;
    }
    std::ostringstream oss;
    oss << in.rdbuf();
    const std::string raw = oss.str();

    json j;
    try {
        j = json::parse(raw);
    } catch (...) {
        last_load_ = CounterLoad::Tampered;
        fail_closed_ = true;
        return last_load_;
    }
    if (!j.is_object() ||
        !j.contains("used")              || !j["used"].is_number_integer() ||
        !j.contains("last_seen_unix")    || !j["last_seen_unix"].is_number_integer() ||
        !j.contains("window_start_unix") || !j["window_start_unix"].is_number_integer() ||
        !j.contains("limit")             || !j["limit"].is_number_integer() ||
        !j.contains("lic_hash")          || !j["lic_hash"].is_string() ||
        !j.contains("hmac")              || !j["hmac"].is_string()) {
        last_load_ = CounterLoad::Tampered;
        fail_closed_ = true;
        return last_load_;
    }

    const int64_t f_used      = j["used"].get<int64_t>();
    const int64_t f_last_seen = j["last_seen_unix"].get<int64_t>();
    const int64_t f_window    = j["window_start_unix"].get<int64_t>();
    const int64_t f_limit     = j["limit"].get<int64_t>();
    const std::string f_lic   = j["lic_hash"].get<std::string>();
    const std::string f_hmac  = j["hmac"].get<std::string>();

    const std::string canon =
        canonical_protected(f_used, f_last_seen, f_window, f_limit, f_lic);
    if (hmac_sha256_hex(canon) != f_hmac) {
        last_load_ = CounterLoad::Tampered;
        fail_closed_ = true;
        return last_load_;
    }

    if (f_last_seen > now_unix + kRollbackSkewSec) {
        last_load_ = CounterLoad::Rollback;
        fail_closed_ = true;
        return last_load_;
    }

    if (f_lic != lic_hash_) {
        // Counter file belongs to a different .lic.  Could be a
        // legitimate activation (in which case the activation flow
        // will call reset() right after), or an attempt to roll back
        // to an older .lic to reuse its budget.  load() fails closed;
        // only an explicit reset() unblocks.
        // Pre-populate in-memory state with the file's used/last_seen/
        // window so a diagnostic snapshot() still reports something
        // useful.
        used_ = f_used;
        last_seen_unix_ = f_last_seen;
        window_start_unix_ = f_window;
        last_load_ = CounterLoad::LicHashMismatch;
        fail_closed_ = true;
        return last_load_;
    }

    used_ = f_used;
    last_seen_unix_ = f_last_seen;
    window_start_unix_ = f_window;
    // We keep the constructor's `limit_` as the authoritative cap; if
    // the file held a stale limit, we ignore it.  The protected HMAC
    // still covers the file's `limit` so tampering with that field is
    // a separate Tampered case detected above.
    last_load_ = CounterLoad::Ok;
    fail_closed_ = false;

    // Roll the window forward if `now` has crossed one or more 30-day
    // boundaries since the file was last written.  Persist immediately
    // so the next load() observes the reset budget even if the process
    // dies before the first consume().
    if (advance_window_if_needed(now_unix)) {
        (void)write_atomic();   // best-effort; IO failure surfaces on consume()
    }
    return last_load_;
}

bool RequestCounter::reset(std::chrono::system_clock::time_point now) {
    const int64_t now_unix = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    used_ = 0;
    last_seen_unix_ = now_unix;
    // Activation anchor: the 30-day counter window starts NOW.  This is
    // exactly what the operator runbook promises — "30 days from
    // activation, then reset".
    window_start_unix_ = now_unix;
    last_load_ = CounterLoad::Ok;
    fail_closed_ = false;
    return write_atomic();
}

bool RequestCounter::consume(std::chrono::system_clock::time_point now, int64_t n) {
    if (fail_closed_) return false;
    if (n <= 0) return true;

    // Anti-rollback check on every call: if the clock moved backwards
    // since the last persisted last_seen, reject.  We're permissive on
    // the kRollbackSkewSec window — NTP jitter shouldn't lock us out.
    const int64_t now_unix = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    if (last_seen_unix_ > now_unix + kRollbackSkewSec) {
        fail_closed_ = true;
        last_load_ = CounterLoad::Rollback;
        return false;
    }

    // 30-day window: advance + zero `used` if we've crossed one (or
    // more) boundaries since the last consume / reset.
    advance_window_if_needed(now_unix);

    // All-or-nothing: a batch that does not fit is refused as a whole so
    // the caller never runs part of the work it cannot deliver.
    if (limit_ > 0 && used_ + n > limit_) return false;

    used_ += n;
    last_seen_unix_ = std::max(now_unix, last_seen_unix_);
    // Persist on every consume only when a limit is enforced.  Unlimited
    // path doesn't touch disk per-request — saves ~one fsync of latency
    // we don't otherwise need.
    if (limit_ > 0) {
        if (!write_atomic()) {
            // Roll back the in-memory bump so the user isn't silently
            // charged slots we failed to persist.
            used_ -= n;
            return false;
        }
    }
    return true;
}

bool RequestCounter::refund(std::chrono::system_clock::time_point now, int64_t n) {
    if (fail_closed_) return false;
    if (n <= 0) return true;
    const int64_t now_unix = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    // A refund never re-opens a window that has since rolled over: the
    // reserved slots belonged to the previous budget and are simply gone.
    if (advance_window_if_needed(now_unix)) return true;
    const int64_t before = used_;
    used_ = std::max<int64_t>(0, used_ - n);
    last_seen_unix_ = std::max(now_unix, last_seen_unix_);
    if (limit_ > 0 && used_ != before) {
        if (!write_atomic()) {
            used_ = before;
            return false;
        }
    }
    return true;
}

bool RequestCounter::refresh(std::chrono::system_clock::time_point now) {
    if (fail_closed_) return false;
    const int64_t now_unix = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    if (!advance_window_if_needed(now_unix)) return false;
    last_seen_unix_ = std::max(now_unix, last_seen_unix_);
    (void)write_atomic();   // best-effort; an IO failure surfaces on consume()
    return true;
}

CounterSnapshot RequestCounter::snapshot() const {
    return CounterSnapshot{
        used_, limit_, last_seen_unix_, window_start_unix_, lic_hash_,
    };
}

bool RequestCounter::write_atomic() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(path_.parent_path(), ec);  // best-effort

    const std::string canon =
        canonical_protected(used_, last_seen_unix_, window_start_unix_,
                            limit_, lic_hash_);
    json j;
    j["used"]              = used_;
    j["last_seen_unix"]    = last_seen_unix_;
    j["window_start_unix"] = window_start_unix_;
    j["limit"]             = limit_;
    j["lic_hash"]          = lic_hash_;
    j["hmac"]              = hmac_sha256_hex(canon);
    const std::string body = j.dump();

    // PID + random suffix avoids tmp-file collisions on concurrent writers.
    std::random_device rd;
    const auto tmp = path_.parent_path() /
                     (path_.filename().string() + ".tmp." +
                      std::to_string(rd()) + "." + std::to_string(rd()));

    // Every byte on disk (write, fsync, size, close checked) BEFORE the rename: on a full disk the write
    // fails here and the previous counter.dat stays as it was. A short or empty file renamed into place
    // would read as Tampered at the next start and lock a licensed install for good.
    if (!durable_write_file(tmp, body)) return false;
    if (!durable_replace(tmp, path_)) return false;
    ledger_upsert();
    return true;
}

fs::path RequestCounter::ledger_path() const {
    return path_.parent_path() / (path_.filename().string() + ".ledger");
}

void RequestCounter::ledger_upsert() {
    if (!ledger_cache_) {
        // First write through this instance: take the verified on-disk
        // entries once; from here on the in-memory copy is authoritative
        // for this process and every write only serialises it.
        std::map<std::string, LedgerEntry> m;
        if (const auto entries = read_ledger(ledger_path())) {
            for (auto it = entries->begin(); it != entries->end(); ++it) {
                if (!ledger_entry_valid(it.value())) continue;
                const json& e = it.value();
                m[it.key()] = LedgerEntry{
                    e["used"].get<int64_t>(),
                    e["last_seen_unix"].get<int64_t>(),
                    e["window_start_unix"].get<int64_t>(),
                    e["limit"].get<int64_t>(),
                };
            }
        }
        ledger_cache_ = std::move(m);
    }
    auto& m = *ledger_cache_;
    m[lic_hash_] = LedgerEntry{used_, last_seen_unix_, window_start_unix_, limit_};
    // Bounded: a machine does not see many licences, but nothing should
    // grow without limit on a customer's disk.  Evict the licence seen
    // longest ago — never the one being written now.
    while (m.size() > kLedgerMaxEntries) {
        std::string oldest;
        int64_t oldest_seen = std::numeric_limits<int64_t>::max();
        for (const auto& [hash, e] : m) {
            if (hash == lic_hash_) continue;
            if (e.last_seen_unix < oldest_seen) { oldest_seen = e.last_seen_unix; oldest = hash; }
        }
        if (oldest.empty()) break;
        m.erase(oldest);
    }
    json entries = json::object();
    for (const auto& [hash, e] : m) {
        json je;
        je["used"]              = e.used;
        je["last_seen_unix"]    = e.last_seen_unix;
        je["window_start_unix"] = e.window_start_unix;
        je["limit"]             = e.limit;
        entries[hash] = je;
    }
    (void)write_ledger(ledger_path(), entries);
}

bool RequestCounter::restore_from_ledger(std::chrono::system_clock::time_point now) {
    const int64_t now_unix = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    const auto entries = read_ledger(ledger_path());
    if (!entries || !entries->contains(lic_hash_)) return false;
    const json& e = (*entries)[lic_hash_];
    if (!ledger_entry_valid(e)) return false;
    // A tally from the future is a rolled-back clock; restoring it would only
    // hide that from load()/consume(), whose job it is to refuse.
    if (e["last_seen_unix"].get<int64_t>() > now_unix + kRollbackSkewSec) return false;

    used_              = e["used"].get<int64_t>();
    last_seen_unix_    = e["last_seen_unix"].get<int64_t>();
    window_start_unix_ = e["window_start_unix"].get<int64_t>();
    advance_window_if_needed(now_unix);
    last_load_   = CounterLoad::Ok;
    fail_closed_ = false;
    ledger_cache_.reset();   // re-read the verified ledger on the next write
    if (!write_atomic()) {
        // Could not persist the restored state: stay closed rather than count
        // requests we cannot record.
        fail_closed_ = true;
        last_load_   = CounterLoad::IoError;
        return false;
    }
    return true;
}

}  // namespace visio::license
