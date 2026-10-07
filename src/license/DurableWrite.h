#pragma once

// Durable small-file writes for the license state (counter.dat, its ledger, the installed .lic, machine-id).
//
// The earlier pattern (std::ofstream, good() checked BEFORE the buffered bytes reached the file, no fsync,
// then rename) could put an EMPTY or short file in place when the disk was full: the write error only
// shows at close / write-back. load() then saw invalid JSON and reported Tampered, so a licensed install on
// a full disk (e.g. filled by an upload on the same file system) failed closed for good.
//
// durable_write_file() checks every step (each write, fsync, the size of the open file, close) and removes
// the temporary file on any failure; durable_replace() is only called after it succeeded, so a short file is
// never renamed over a good one, and a rename that fails leaves the good one in place.

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#include <sys/types.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace visio::license {

// Test seam (tests/test-license.cpp only): after `skip` more durable writes succeed, the next `count` ones
// stop after `after_bytes` bytes and fail, as they would on a full disk.
struct DurableWriteFault {
    std::atomic<int>    skip{0};
    std::atomic<int>    count{0};
    std::atomic<size_t> after_bytes{0};
    std::atomic<int>    rename_fail{0}; // durable_replace_test_fail()
};

inline DurableWriteFault & durable_write_fault() {
    static DurableWriteFault fault;
    return fault;
}

inline void durable_write_test_fail(int skip, int count, size_t after_bytes) {
    DurableWriteFault & f = durable_write_fault();
    f.after_bytes.store(after_bytes);
    f.skip.store(skip);
    f.count.store(count);
}

// writes `data` into `tmp` (created or truncated) and makes it durable; false: tmp is removed again
inline bool durable_write_file(const std::filesystem::path & tmp, std::string_view data, bool owner_only = false) {
    size_t limit = data.size();
    bool   fault = false;
    {
        DurableWriteFault & f = durable_write_fault();
        if (f.count.load() > 0) {
            if (f.skip.load() > 0) {
                f.skip.fetch_sub(1);
            } else {
                f.count.fetch_sub(1);
                limit = (std::min)(limit, f.after_bytes.load()); // parenthesised: windows.h may define min
                fault = true;
            }
        }
    }
    bool ok = true;
#if defined(_WIN32)
    (void) owner_only; // the ACL of the state directory applies
    int fd = -1;
    if (_wsopen_s(&fd, tmp.c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY | _O_NOINHERIT, _SH_DENYWR,
                  _S_IREAD | _S_IWRITE) != 0 || fd < 0) {
        return false;
    }
    const char * p = data.data();
    size_t left = limit;
    while (ok && left > 0) {
        const int w = _write(fd, p, (unsigned int) (std::min)(left, (size_t) (1u << 30)));
        if (w <= 0) {
            ok = false;
            break;
        }
        p    += w;
        left -= (size_t) w;
    }
    ok = ok && !fault;
    ok = ok && _commit(fd) == 0;
    struct _stat64 st;
    ok = ok && _fstat64(fd, &st) == 0 && (uint64_t) st.st_size == (uint64_t) data.size();
    ok = (_close(fd) == 0) && ok;
#else
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, owner_only ? 0600 : 0666);
    if (fd < 0) {
        return false;
    }
    if (owner_only) {
        (void) ::fchmod(fd, 0600); // an existing temp file keeps no wider mode
    }
    const char * p = data.data();
    size_t left = limit;
    while (ok && left > 0) {
        const ssize_t w = ::write(fd, p, left);
        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w <= 0) {
            ok = false;
            break;
        }
        p    += w;
        left -= (size_t) w;
    }
    ok = ok && !fault;
    ok = ok && ::fsync(fd) == 0;
    struct stat st;
    ok = ok && ::fstat(fd, &st) == 0 && (uint64_t) st.st_size == (uint64_t) data.size();
    ok = (::close(fd) == 0) && ok;
#endif
    if (!ok) {
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
    }
    return ok;
}

// Test seam (tests/test-license.cpp only): the next `count` calls of durable_replace() fail as a rename that
// returns ENOSPC / EIO would (a full btrfs, a quota, a failing disk).
inline void durable_replace_test_fail(int count) {
    durable_write_fault().rename_fail.store(count);
}

// moves a temp file written by durable_write_file() into place, then makes the rename durable (POSIX:
// fsync of the directory); false: `path` is unchanged (the previous good file stays), tmp is removed.
// The target is never removed first: with a failing second rename that lost the good file AND the new one
// (a reset request counter, a lost activation, a new machine id).
inline bool durable_replace(const std::filesystem::path & tmp, const std::filesystem::path & path) {
    std::error_code ec;
    {
        DurableWriteFault & f = durable_write_fault();
        if (f.rename_fail.load() > 0) {
            f.rename_fail.fetch_sub(1);
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
#if defined(_WIN32)
    // std::filesystem::rename replaces an existing file (MoveFileExW with MOVEFILE_REPLACE_EXISTING); a target that
    // another process holds open for a moment (a scanner, an indexer) gives a sharing violation: retried
    for (int attempt = 0; attempt < 20; attempt++) {
        std::filesystem::rename(tmp, path, ec);
        if (!ec) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
#else
    // rename() replaces atomically: when it fails, `path` is still the previous file
    std::filesystem::rename(tmp, path, ec);
#endif
    if (ec) {
        std::error_code ec2;
        std::filesystem::remove(tmp, ec2);
        return false;
    }
#if !defined(_WIN32)
    const std::filesystem::path dir = path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
    const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        (void) ::fsync(dfd);
        ::close(dfd);
    }
#endif
    return true;
}

}  // namespace visio::license
