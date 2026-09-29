// Persistent per-license request counter with a 30-day rolling window.
//
// State: { used, last_seen_unix, window_start_unix, limit, lic_hash },
// signed with HMAC-SHA256 over a canonical JSON serialization of those
// five fields.
//
// `window_start_unix` anchors the current 30-day budget window.  On
// every consume():
//   * if now >= window_start + 30d, advance window_start by 30d (one or
//     more times so the new window straddles `now`) AND zero `used`.
//     That gives "30 days from activation, then reset, repeat" without
//     drifting if the operator happens to skip a window.
//   * otherwise the window is unchanged and consume() just increments
//     `used` (capped at limit).
// Anchor: reset() sets window_start_unix = now.  The activation flow calls
// reset() only for a licence this machine has never counted, so its first
// 30-day window starts at the moment of activation — what the operator
// runbook promises.  A licence seen before keeps its tally: either
// counter.dat still holds it, or restore_from_ledger() brings it back from
// the per-licence ledger (<counter path>.ledger).
//
// lic_hash = SHA-256 hex of the canonical bytes of the .lic the counter
// is bound to.  This is the single most important invariant: it makes
// rollback-to-an-older-.lic detectable.  Behaviour matrix:
//
//   * counter file absent ↔ FreshState
//        New install or first activation.  In-memory state has used=0
//        and the lic_hash the caller provided; the file is written on
//        first consume().
//
//   * file present, HMAC verifies, lic_hash matches ↔ Ok
//        Steady-state.  Returned counter resumes from the persisted
//        used/last_seen.
//
//   * file present, HMAC fails ↔ Tampered
//        Either casual edit ("set used=0") or an integrity bit-flip.
//        Fail closed.  We do NOT auto-reset, because doing so turns
//        tampering into a denial-of-restriction.
//
//   * file present, last_seen_unix > now (+ skew) ↔ Rollback
//        System clock moved backwards.  Fail closed until the clock
//        catches up — or the operator legitimately reactivates and
//        explicitly resets via reset().
//
//   * file present, HMAC verifies, lic_hash differs ↔ LicHashMismatch
//        The counter is bound to a different .lic than the one currently
//        loaded.  This happens on (a) legitimate activation = the
//        activation flow calls reset() with the new lic_hash, OR
//        (b) rollback-to-older-.lic = adversary trying to reuse budget.
//        load() always reports the mismatch and fails closed.  The
//        activation flow — which has just had the licence vouched for by
//        the licence server — then takes that licence's tally from the
//        ledger if it has one (restore_from_ledger), and only a licence
//        the machine has never counted gets reset() to a fresh budget.
//        init() does NOT consult the ledger here: without the server's
//        word, a mismatch is a swap back to another .lic (possibly one
//        the vendor revoked) and stays fail-closed.
//
// HMAC key is compile-time (override via -DPROVISION_LICENSE_COUNTER_KEY=...).
// Anti-casual-tampering, not cryptographic — a determined attacker with
// the binary can extract it.  The .lic signature, NOT this HMAC, is the
// cryptographic anchor of the system.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace visio::license {

enum class CounterLoad {
    Ok,
    FreshState,
    Tampered,
    Rollback,
    LicHashMismatch,
    IoError,
};

struct CounterSnapshot {
    int64_t used = 0;
    int64_t limit = 0;             // 0 → unlimited (consume() always allows)
    int64_t last_seen_unix = 0;
    int64_t window_start_unix = 0; // anchor of the current 30-day window
                                   // (set by reset(); advanced inside consume())
    std::string lic_hash;
};

// Duration of one budget window.  Hard-coded at 30 calendar days
// (30 * 86400 = 2592000 seconds) — matches the "30 days from activation,
// then reset" semantic documented in /docs.  Exposed as a constant so
// tests can verify against it without re-deriving the magic number.
inline constexpr int64_t kCounterWindowSeconds = 30 * 86400;

class RequestCounter {
public:
    // path: writable file location (e.g. /var/lib/provision-llm/counter.dat).
    // lic_hash: SHA-256 hex of the canonical .lic this counter binds to.
    // limit: max requests; 0 disables enforcement.
    // hard_limit: when true, the 30-day rolling window is disabled —
    //   `limit` becomes a HARD lifetime cap (used in trial builds where
    //   "50 requests total" is the intended semantics).  When false
    //   (production-licensed default), `used` resets to 0 every 30
    //   days from the activation anchor.
    RequestCounter(std::filesystem::path path,
                   std::string lic_hash,
                   int64_t limit,
                   bool hard_limit = false);

    // Load existing state.  Returns the classification (see header).
    // After Ok / FreshState the counter is usable via consume() / snapshot();
    // after Tampered / Rollback / LicHashMismatch consume() will refuse
    // until reset() is called.
    CounterLoad load(std::chrono::system_clock::time_point now);

    // Explicit reset — called by the activation flow after a successful
    // POST /activate, with the new .lic's hash + limit.  Overwrites the
    // file atomically with used=0, last_seen=now, given lic_hash+limit.
    // Returns false on IO failure.
    bool reset(std::chrono::system_clock::time_point now);

    // Restore THIS licence's tally from the per-licence ledger.
    //
    // counter.dat remembers one licence; the ledger (`<counter path>.ledger`,
    // same HMAC key, domain-separated) remembers every licence this machine
    // has counted, updated on every write.  Without it the budget could be
    // reset on demand: activating A -> B -> A rebound the counter to B and
    // back, and each switch started A over at used=0; deleting or damaging
    // counter.dat alone did the same.  Now the licence gets its real tally
    // back.
    //
    // Returns true when an entry for this lic_hash was found, verified and
    // persisted back into counter.dat.  Returns false — and leaves the state
    // untouched — when there is no ledger, the ledger fails its HMAC, it has
    // no entry for this licence, or the entry lies in the future (clock
    // rollback, which load()/consume() must keep catching).
    //
    // Deleting BOTH files still starts the licence over: a counter the
    // customer can erase cannot be made authoritative on the client.  The
    // durable fix for that is counting on the licence server.
    bool restore_from_ledger(std::chrono::system_clock::time_point now);

    // Try to consume `n` request slots at once (a batched request produces
    // n results).  All-or-nothing: returns true iff used + n <= limit, and
    // then persists the updated state atomically (temp + rename) in ONE
    // write when limit > 0; when limit == 0 the file is written only at
    // reset() / first call and otherwise left untouched (no throughput
    // penalty on the unlimited path).
    bool consume(std::chrono::system_clock::time_point now, int64_t n = 1);

    // Give `n` slots back — the caller reserved them with consume() before
    // running the request and the result was never delivered (handler
    // error, client gone).  Never goes below 0; persists like consume().
    bool refund(std::chrono::system_clock::time_point now, int64_t n = 1);

    // Roll the 30-day window forward if `now` has crossed it, without
    // consuming anything.  Lets a read-only status / pre-check observe a
    // reset budget instead of the stale `used` of the previous window
    // (otherwise an exhausted licence stayed locked until a restart).
    // Returns true when the window was advanced; persisted best-effort.
    bool refresh(std::chrono::system_clock::time_point now);

    // Read-only view; safe to call after load() or reset().
    CounterSnapshot snapshot() const;

    // Reason load() returned non-Ok; clears after reset() / successful
    // load().  Available so the facade can surface a specific DenyReason.
    CounterLoad last_load_status() const { return last_load_; }

private:
    // One licence's tally as kept in the ledger (see ledger_upsert).
    struct LedgerEntry {
        int64_t used = 0;
        int64_t last_seen_unix = 0;
        int64_t window_start_unix = 0;
        int64_t limit = 0;
    };

    bool write_atomic();
    // Record the current tally for lic_hash_ in the ledger.  Best effort:
    // counter.dat stays the primary record, so a ledger write failure never
    // turns a successful count into a failed one.  The ledger is read and
    // HMAC-verified from disk once per counter instance and kept in
    // ledger_cache_ afterwards, so a metered request costs one rewrite of
    // the ledger, not a read + verify + rewrite.
    void ledger_upsert();
    std::filesystem::path ledger_path() const;

    // Advance window_start_unix_ in 30-day strides until `now` falls
    // inside [window_start_unix_, window_start_unix_ + 30d).  Each
    // stride zeroes `used_`.  Returns true if the window was advanced
    // at least once (used reset to 0); false if `now` was already
    // inside the current window.
    bool advance_window_if_needed(int64_t now_unix);

    std::filesystem::path path_;
    std::string lic_hash_;
    int64_t limit_;
    int64_t used_ = 0;
    int64_t last_seen_unix_ = 0;
    int64_t window_start_unix_ = 0;
    CounterLoad last_load_ = CounterLoad::FreshState;
    bool fail_closed_ = true;   // until load() / reset() succeeds
    bool hard_limit_ = false;   // disable 30-day rolling reset
    std::optional<std::map<std::string, LedgerEntry>> ledger_cache_;
};

}  // namespace visio::license
