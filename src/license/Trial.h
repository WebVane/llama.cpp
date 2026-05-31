// Trial-licence counter. Self-contained module: depends only on OpenSSL's
// libcrypto (HMAC-SHA256) and the C++ standard library (errors are written to
// stderr). Drop the two files (Trial.h + Trial.cpp) and the matching CMake
// snippet into any project — no project-specific includes required.
//
// Behaviour:
//   * Persistent N-request counter signed with HMAC-SHA256.  State file
//     lives at a per-OS canonical path (see Trial.cpp).
//   * On every successful consume() the counter is incremented and the
//     file is re-signed.  When the limit is reached, consume() returns
//     false — the caller is expected to short-circuit the request.
//   * Clock-rollback protection: a last-seen timestamp is stored in the
//     signed state; if the wall clock is earlier than last-seen, the
//     trial is treated as exhausted.
//   * Tamper detection: any modification of the file outside this module
//     invalidates the HMAC and marks the trial exhausted.
//
// Build:  enabled by -DVISIO_TRIAL=ON in CMakeLists.txt, which links
// OpenSSL::Crypto, compiles Trial.cpp into visio_server, and defines
// VISIO_TRIAL=1 so call-sites can be gated.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace visio::license {

struct TrialStatus {
    int used = 0;
    int limit = 0;
    bool exhausted = false;
    bool tampered = false;     // signature or clock check failed
    std::string state_path;    // resolved on-disk path
};

class Trial {
public:
    static Trial& instance();

    // Load (or create) the state file. Safe to call multiple times.
    // Returns false only on an unrecoverable IO error.
    bool init();

    // Atomically increment the counter and persist. Returns true if the
    // request is allowed, false if the limit is already reached or the
    // state file is tampered/rolled-back.
    bool consume();

    TrialStatus status();

    // The limit baked in at compile time. Override at build time via
    // -DVISIO_TRIAL_LIMIT=N.
    static constexpr int kLimit =
#ifdef VISIO_TRIAL_LIMIT
        VISIO_TRIAL_LIMIT
#else
        50
#endif
        ;

private:
    Trial() = default;
    Trial(const Trial&) = delete;
    Trial& operator=(const Trial&) = delete;
};

}  // namespace visio::license
