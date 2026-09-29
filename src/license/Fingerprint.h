// Per-host fingerprint: SHA-256(CPU_ID ‖ Hostname ‖ clientId ‖ product).
//
// All four components are concatenated byte-by-byte with **no
// separator** — same convention as the license server's reference
// impl (see src/license/docs/openapi.json schemas.ActivateRequest.
// fingerprint.description and DESIGN.md §6).
//
// On Linux / Docker the primary CPU_ID source is **/etc/machine-id**
// (systemd-d_bus convention; present in every modern distro and
// always populated in container images during build).  Fallback chain:
//
//   1. /etc/machine-id                         — canonical, 32 hex chars
//   2. /var/lib/dbus/machine-id                — legacy systemd location
//   3. /sys/class/dmi/id/product_uuid          — DMI hardware UUID
//                                                (root or SYS_ADMIN in Docker)
//   4. MAC address of the first non-loopback NIC, hex-lowercase
//   5. Persisted per-install id at <state_dir>/machine-id — a
//      cryptorandom UUID generated on first use and stored in the
//      same persistent volume as license.lic / counter.dat.  Treated
//      as STABLE; lets Docker Desktop / WSL2 / VM hosts (where no
//      hardware machine-id is exposed) activate online without an
//      explicit PROVISION_LICENSE_CPU_ID_OVERRIDE.  The chain still tries
//      every hardware source above first — this last link only fires
//      when nothing else is stable.  Copying the state directory
//      between machines preserves the binding (that's intentional and
//      documented in the runbook).
//
// Docker note: by default the image's machine-id is fixed at image-build
// time, so multiple containers from the same image share a fingerprint.
// For per-host stability operators must bind-mount /etc/machine-id from
// the host (documented as a deployment requirement), pass an explicit
// override via the environment (PROVISION_LICENSE_CPU_ID_OVERRIDE), OR rely
// on the persisted-state-id fallback (which writes to the named volume,
// so the fingerprint sticks to the volume even across container
// restarts and image upgrades).
//
// Hostname source: gethostname(2) on POSIX, GetComputerNameExW
// (ComputerNamePhysicalDnsHostname) on Windows.  No FQDN — the server's
// reference impl uses bare hostname.
//
// On Windows the implementation falls back to a stub that reads only
// hostname + machine GUID from the registry, sufficient for the dev
// build to compile and run unit tests.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace visio::license {

// Where read_cpu_id() picked the value from.  The facade uses this to
// decide whether to fail-loud at startup: MAC-fallback in particular is
// volatile across reboots (NIC re-enumeration changes ordering) and
// will silently break the license unless the operator pinned a stable
// override.
enum class CpuIdSource {
    Unknown,           // probe failed entirely; do not run online mode
    Override,          // PROVISION_LICENSE_CPU_ID_OVERRIDE env (stable by construction)
    MachineIdEtc,      // /etc/machine-id (systemd) — STABLE
    MachineIdDbus,     // /var/lib/dbus/machine-id (legacy) — STABLE
    DmiProductUuid,    // /sys/class/dmi/id/product_uuid — STABLE per host
                       // (shared between containers on the same host)
    MacFallback,       // first non-loopback NIC's MAC — UNSTABLE
    WindowsMachineGuid,// HKLM\SOFTWARE\Microsoft\Cryptography\MachineGuid — STABLE
    PersistedStateId,  // <state_dir>/machine-id, generated on first use.
                       // Tied to the persistent volume (license.lic,
                       // counter.dat) — STABLE while the volume sticks.
};

// Friendly label for logs and the deny-detail field.
const char* cpu_id_source_label(CpuIdSource s) noexcept;

// True for sources expected to survive a reboot AND a NIC change.  The
// facade fails closed on init when this is false unless the caller has
// explicitly accepted the risk via Config.require_stable_cpu_id=false
// (e.g. for non-production smoke tests).
bool cpu_id_source_stable(CpuIdSource s) noexcept;

struct FingerprintSources {
    std::string cpu_id;          // /etc/machine-id (or fallback) on Linux
    CpuIdSource cpu_id_source = CpuIdSource::Unknown;
    std::string hostname;        // gethostname()
    std::string client_id;       // baked at CI
    std::string product;         // baked at CI (e.g. "OCR")
};

class Fingerprint {
public:
    // Pre-baked from an existing hex string — for tests, or when the
    // value has already been computed elsewhere (e.g. cached from init).
    explicit Fingerprint(std::string hex) : hex_(std::move(hex)) {}

    // Pure constructor: hash exactly what's provided, no I/O.  Used by
    // tests and by from_system() after the I/O is done.
    static Fingerprint from_sources(const FingerprintSources& src);

    // Collect CPU_ID + Hostname from the running system, combine with the
    // provided clientId/product, return the resulting fingerprint along
    // with the source values that went into it (so callers can log them
    // verbatim for debugging "fingerprint changed after restart" issues).
    // Honours these env overrides (test/ops escape hatches):
    //   PROVISION_LICENSE_CPU_ID_OVERRIDE    — bypass file-system probe
    //   PROVISION_LICENSE_HOSTNAME_OVERRIDE  — bypass gethostname()
    //
    // `state_dir` is the writable persistent caller-state directory
    // (Config::state_dir = PROVISION_LICENSE_STATE_DIR or platform default).
    // It's used ONLY as the location of the persisted-state-id
    // fallback (<state_dir>/machine-id) when the hardware chain ends
    // at the unstable MAC-fallback step.  Passing an empty path skips
    // the persisted fallback entirely (legacy behaviour — keeps the
    // chain exactly as it was before the state-id link was added).
    static Fingerprint from_system(std::string client_id,
                                   std::string product,
                                   std::filesystem::path state_dir = {},
                                   FingerprintSources* sources_out = nullptr);

    const std::string& hex() const { return hex_; }

private:
    std::string hex_;
};

// Low-level probe (exposed for tests + diagnostics).  Returns the first
// available CPU_ID source per the priority chain above, or std::nullopt
// if none could be read.  Read once at startup; do not poll.
std::optional<std::string> read_cpu_id();

// Same as read_cpu_id() but also reports which source was used.  The
// facade prefers this form because the source label drives the
// stability check at init.
//
// `state_dir` (optional, default empty) enables the persisted-state-id
// fallback: if the hardware chain lands on MAC-fallback (unstable) and
// state_dir is writable, the function generates / reads
// <state_dir>/machine-id and returns it with source=PersistedStateId.
// Passing an empty path keeps the legacy chain (override → machine-id
// → dbus → dmi/win-guid → mac → nullopt).
struct CpuIdProbe {
    std::string value;
    CpuIdSource source = CpuIdSource::Unknown;
};
std::optional<CpuIdProbe> read_cpu_id_with_source(
    const std::filesystem::path& state_dir = {});

// gethostname() (POSIX) / GetComputerNameExW (Win); std::nullopt on
// failure.
std::optional<std::string> read_hostname();

}  // namespace visio::license
