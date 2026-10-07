#include "license/Fingerprint.h"

#include "license/DurableWrite.h"
#include "license/Sha256.h"

#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <unistd.h>
#  include <sys/socket.h>
#  include <sys/ioctl.h>
#  include <net/if.h>
#  include <netinet/in.h>
#endif

namespace visio::license {
namespace {

std::string trim(std::string s) {
    auto issp = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && issp(static_cast<unsigned char>(s.back())))  s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && issp(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

std::optional<std::string> read_file_first_line(const std::string& path) {
    std::ifstream in(path);
    if (!in) return std::nullopt;
    std::string line;
    std::getline(in, line);
    line = trim(line);
    if (line.empty()) return std::nullopt;
    return line;
}

std::string hex_lower(const uint8_t* data, std::size_t len) {
    std::ostringstream o;
    o << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < len; ++i) {
        o << std::setw(2) << static_cast<unsigned>(data[i]);
    }
    return o.str();
}

#if !defined(_WIN32)
// MAC of the first non-loopback, non-zero NIC found in /sys/class/net.
// Hex-lowercase, no separators ("aabbccddeeff").  Returns nullopt if no
// usable NIC.  We avoid getifaddrs to keep glibc/Bionic deltas out of
// the picture — /sys/class/net is universally readable in Linux 3.x+.
std::optional<std::string> read_mac_fallback() {
    namespace fs = std::filesystem;
    fs::path base{"/sys/class/net"};
    std::error_code ec;
    if (!fs::is_directory(base, ec)) return std::nullopt;
    std::vector<std::string> ifs;
    for (auto& e : fs::directory_iterator(base, ec)) {
        const auto name = e.path().filename().string();
        if (name == "lo") continue;
        ifs.push_back(name);
    }
    std::sort(ifs.begin(), ifs.end());
    for (const auto& name : ifs) {
        auto line = read_file_first_line((base / name / "address").string());
        if (!line) continue;
        // "00:00:00:00:00:00" → empty / pseudo NIC; skip.
        bool any = false;
        for (char c : *line) if (c != '0' && c != ':') { any = true; break; }
        if (!any) continue;
        // Strip colons, lowercase.
        std::string out;
        out.reserve(12);
        for (char c : *line) {
            if (c == ':') continue;
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        if (out.size() == 12) return out;
    }
    return std::nullopt;
}
#endif

// Canonical hyphenated lowercase UUID (8-4-4-4-12, 36 chars) from 16
// random bytes.  Sets version 4 + RFC 4122 variant bits so the result
// is a well-formed UUIDv4 — useful when an operator greps the value
// looking for the format.
std::optional<std::string> generate_random_uuid_string() {
    std::array<uint8_t, 16> b{};
    if (RAND_bytes(b.data(), static_cast<int>(b.size())) != 1) {
        return std::nullopt;
    }
    b[6] = static_cast<uint8_t>((b[6] & 0x0F) | 0x40);  // version 4
    b[8] = static_cast<uint8_t>((b[8] & 0x3F) | 0x80);  // variant 10
    char buf[37];
    std::snprintf(buf, sizeof(buf),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  b[0], b[1], b[2], b[3],
                  b[4], b[5], b[6], b[7],
                  b[8], b[9],
                  b[10], b[11], b[12], b[13], b[14], b[15]);
    return std::string{buf, 36};
}

// Read or create the persisted per-install id at <state_dir>/machine-id.
//   * file exists and non-empty → return its (trimmed) contents
//   * file missing or empty AND state_dir is writable → generate a fresh
//     cryptorandom UUID, write atomically (temp + rename, chmod 0600 on
//     POSIX), return it
//   * any IO failure (state_dir doesn't exist and can't be created,
//     unwritable, OpenSSL RAND failed, rename failed) → nullopt
// Caller (read_cpu_id_with_source) treats nullopt as "persisted source
// unavailable — fall through to MAC".
std::optional<std::string> read_or_create_persisted_state_id(
    const std::filesystem::path& state_dir) {
    namespace fs = std::filesystem;
    if (state_dir.empty()) return std::nullopt;

    const auto path = state_dir / "machine-id";

    // Read existing value first.  We trust whatever the operator put
    // there, including hand-edited values — same convention as
    // /etc/machine-id (no format validation).
    if (auto v = read_file_first_line(path.string()); v && !v->empty()) {
        return v;
    }

    std::error_code ec;
    fs::create_directories(state_dir, ec);
    // create_directories returns ok=false on existing dir AND on
    // failure; check writability by trying to open the temp file
    // rather than relying on the ec.

    auto uuid = generate_random_uuid_string();
    if (!uuid) return std::nullopt;

    // Atomic write: temp + chmod + rename.  PID + random suffix avoids
    // concurrent-writer races (rare here, but cheap to keep).
    std::random_device rd;
    const auto tmp = state_dir / ("machine-id.tmp." +
                                  std::to_string(rd()) + "." +
                                  std::to_string(rd()));
    // Written durably (write, fsync, size, close checked) with mode 0600 (operator-only read/write; POSIX
    // only, on Windows the file inherits the ACLs of state_dir, typically already restrictive under
    // %ProgramData%\ProvisionLLM), with a trailing newline so `cat` output looks normal. A short machine-id
    // on a full disk would change the fingerprint and unbind the licence.
    if (!durable_write_file(tmp, *uuid + "\n", /*owner_only*/ true)) {
        return std::nullopt;
    }
    if (!durable_replace(tmp, path)) {
        return std::nullopt;
    }
    return uuid;
}

}  // namespace

std::optional<CpuIdProbe> read_cpu_id_with_source(
    const std::filesystem::path& state_dir) {
    // Env override — first, no I/O, used by tests + ops escape hatch.
    if (const char* env = std::getenv("PROVISION_LICENSE_CPU_ID_OVERRIDE");
        env && *env) {
        return CpuIdProbe{std::string{env}, CpuIdSource::Override};
    }

#if defined(_WIN32)
    // Windows: read MachineGuid from HKLM\SOFTWARE\Microsoft\Cryptography.
    // 36-char UUID, stable per OS install.  Mirrors machine-id semantics.
    HKEY key{};
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "SOFTWARE\\Microsoft\\Cryptography",
                      0, KEY_READ | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS) {
        char buf[64]{};
        DWORD type = 0;
        DWORD cb = sizeof(buf);
        const LONG rv = RegQueryValueExA(key, "MachineGuid", nullptr, &type,
                                         reinterpret_cast<LPBYTE>(buf), &cb);
        RegCloseKey(key);
        if (rv == ERROR_SUCCESS && type == REG_SZ && cb > 0) {
            std::string s{buf, cb > 0 ? cb - 1 : 0};
            s = trim(s);
            if (!s.empty()) return CpuIdProbe{s, CpuIdSource::WindowsMachineGuid};
        }
    }
    // Same fallback rationale as the Linux branch — when MachineGuid
    // can't be read (locked-down VDIs, restricted ACLs, some Windows
    // Server SKUs), drop to the persisted-state-id sitting on the same
    // %ProgramData% volume the rest of the license state lives on.
    if (auto v = read_or_create_persisted_state_id(state_dir); v) {
        return CpuIdProbe{*v, CpuIdSource::PersistedStateId};
    }
    return std::nullopt;
#else
    if (auto v = read_file_first_line("/etc/machine-id"); v) {
        return CpuIdProbe{*v, CpuIdSource::MachineIdEtc};
    }
    if (auto v = read_file_first_line("/var/lib/dbus/machine-id"); v) {
        return CpuIdProbe{*v, CpuIdSource::MachineIdDbus};
    }
    if (auto v = read_file_first_line("/sys/class/dmi/id/product_uuid"); v) {
        return CpuIdProbe{*v, CpuIdSource::DmiProductUuid};
    }
    // Persisted-state-id BEFORE MAC: when state_dir is writable, prefer
    // a sticky-to-volume UUID over a NIC-order-dependent MAC.  We don't
    // change the hardware chain ABOVE this point — that's the existing
    // priority order the operator runbook documents and the test
    // harness depends on.  Only the terminal branch (where today we'd
    // surface MAC + maybe refuse for instability) gets the new link.
    if (auto v = read_or_create_persisted_state_id(state_dir); v) {
        return CpuIdProbe{*v, CpuIdSource::PersistedStateId};
    }
    if (auto v = read_mac_fallback(); v) {
        return CpuIdProbe{*v, CpuIdSource::MacFallback};
    }
    return std::nullopt;
#endif
}

std::optional<std::string> read_cpu_id() {
    if (auto p = read_cpu_id_with_source()) return p->value;
    return std::nullopt;
}

const char* cpu_id_source_label(CpuIdSource s) noexcept {
    switch (s) {
        case CpuIdSource::Override:            return "override";
        case CpuIdSource::MachineIdEtc:        return "machine-id";
        case CpuIdSource::MachineIdDbus:       return "dbus-machine-id";
        case CpuIdSource::DmiProductUuid:      return "dmi-product-uuid";
        case CpuIdSource::MacFallback:         return "mac-fallback";
        case CpuIdSource::WindowsMachineGuid:  return "windows-machineguid";
        case CpuIdSource::PersistedStateId:    return "persisted-state-id";
        case CpuIdSource::Unknown:             break;
    }
    return "unknown";
}

bool cpu_id_source_stable(CpuIdSource s) noexcept {
    switch (s) {
        case CpuIdSource::Override:
        case CpuIdSource::MachineIdEtc:
        case CpuIdSource::MachineIdDbus:
        case CpuIdSource::DmiProductUuid:
        case CpuIdSource::WindowsMachineGuid:
        case CpuIdSource::PersistedStateId:
            // Persisted-state-id is stable for as long as the operator
            // keeps the state volume mounted — same precondition the
            // license.lic and counter.dat already require to survive
            // restarts.  If the volume is wiped, EVERYTHING goes (no
            // .lic, no counter, no fingerprint) and a fresh activation
            // is needed anyway.  So treating it as STABLE for the
            // stability gate is consistent with the rest of the design.
            return true;
        case CpuIdSource::MacFallback:
        case CpuIdSource::Unknown:
            return false;
    }
    return false;
}

std::optional<std::string> read_hostname() {
    if (const char* env = std::getenv("PROVISION_LICENSE_HOSTNAME_OVERRIDE");
        env && *env) {
        return std::string{env};
    }
#if defined(_WIN32)
    wchar_t buf[256]{};
    DWORD len = sizeof(buf) / sizeof(buf[0]);
    if (GetComputerNameExW(ComputerNamePhysicalDnsHostname, buf, &len) == 0) {
        return std::nullopt;
    }
    // UTF-16 → UTF-8 via WideCharToMultiByte.
    const int n = WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(len),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::nullopt;
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(len),
                        out.data(), n, nullptr, nullptr);
    return out;
#else
    char buf[256]{};
    if (gethostname(buf, sizeof(buf) - 1) != 0) return std::nullopt;
    std::string s{buf};
    s = trim(s);
    if (s.empty()) return std::nullopt;
    return s;
#endif
}

Fingerprint Fingerprint::from_sources(const FingerprintSources& src) {
    std::string concat;
    concat.reserve(src.cpu_id.size() + src.hostname.size() +
                   src.client_id.size() + src.product.size());
    concat += src.cpu_id;
    concat += src.hostname;
    concat += src.client_id;
    concat += src.product;
    const auto h = sha256(concat);
    return Fingerprint{hex_lower(h.data(), h.size())};
}

Fingerprint Fingerprint::from_system(std::string client_id,
                                     std::string product,
                                     std::filesystem::path state_dir,
                                     FingerprintSources* sources_out) {
    FingerprintSources src;
    if (auto p = read_cpu_id_with_source(state_dir)) {
        src.cpu_id = p->value;
        src.cpu_id_source = p->source;
    } else {
        src.cpu_id_source = CpuIdSource::Unknown;
    }
    src.hostname = read_hostname().value_or(std::string{});
    src.client_id = std::move(client_id);
    src.product   = std::move(product);
    if (sources_out) *sources_out = src;
    return from_sources(src);
}

}  // namespace visio::license
