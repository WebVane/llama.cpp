# LicenseLib v2.1 — Client Integration Design

Status: draft.
Scope: client-side library, embedded into `provision_ocr_server` (C++20). Server contract is fixed by [`docs/openapi.json`](docs/openapi.json) — this document derives the client design from it.

---

## 1. Model summary (v2.1)

| Mode | When | Server contact | Local checks |
|---|---|---|---|
| **Trial** | dev/eval | never | HMAC-signed request counter (existing `Trial.cpp`, reused as-is) |
| **Online** | regular sale | **once** during activation | Ed25519-signed `.lic` + fingerprint match + (optional) request counter + expiry |
| **Offline** | air-gapped sale | never | Ed25519-signed `.lic` + (optional) request counter + expiry — **no fingerprint** |
| **Token** | integrator-signed grants (optional) | never | Ed25519-signed short-lived tokens with separate keyring |

Hard invariants (from openapi.json `info.description` and `License.description`):

* **After activation the client NEVER contacts the server again.** Anti-revocation is purely server-side (slot cap on next activation). A working install with a valid `.lic` keeps running even if its slot is revoked admin-side until the `.lic` expires.
* **The `.lic` is signed Ed25519 over the canonical JSON (RFC 8785 / JCS) of all fields EXCEPT `signature`.** The client must canonicalize byte-identical to the server, otherwise verification fails.
* **Online vs Offline is distinguished by presence of the `fingerprint` field.** Online `.lic` carries it and the client must re-verify locally; offline `.lic` omits the field entirely (it is `absent`, not `null`).

---

## 2. Public API

Single facade, header `license/LicenseLib.h`. No `iostream`, no Drogon types — keeps the integration point thin.

```cpp
namespace visio::license {

enum class Mode  { Trial, Offline, Online, Token };
enum class Deny  { None, NotInitialised, NoLicense, BadSignature, UnknownKid,
                   FingerprintMismatch, Expired, NotYetValid,
                   RequestLimitReached, TokenInvalid, ClockRollback,
                   TamperedState, ProductMismatch, ClientMismatch };

struct Config {
    std::string product;          // "OCR" — baked at CI, must match .lic.product
    std::string client_id;        // UUID baked at CI, must match .lic.clientId
    std::string activation_url;   // e.g. "https://license.provlabs.tech"
    std::filesystem::path state_dir;     // /var/lib/provision-llm  (writable)
    std::filesystem::path lic_path;      // state_dir / "license.lic"
    std::filesystem::path counter_path;  // state_dir / "counter.dat"
    std::optional<int> trial_limit;      // forwarded to legacy Trial when Mode::Trial
    bool allow_clock_skew_sec = 60;      // tolerated drift on expiresAt/issuedAt
};

struct InitResult { bool ok; Mode mode; std::string detail; };

struct Decision {
    bool   allow;
    Deny   reason = Deny::None;
    int    used = 0;        // counter snapshot, when applicable
    int    limit = 0;       // 0 == unlimited
    std::chrono::system_clock::time_point expires_at{};
};

class LicenseLib {
public:
    static LicenseLib& instance();

    // Resolve mode (priority: env override → lic file → trial fallback),
    // load .lic if present, verify signature, fingerprint, expiry, kid.
    // Idempotent.  No network IO unless explicitly requested via activate().
    InitResult init(const Config&);

    // First-run online flow.  Calls POST /api/v1/activate, persists .lic
    // atomically, then re-runs check().  Idempotent on the wire via
    // stable Idempotency-Key derived from fingerprint+activationCode.
    InitResult activate(std::string_view activation_code,
                        std::chrono::seconds timeout = std::chrono::seconds{15});

    // Hot path: called per inference request.  Consumes one slot of the
    // request counter when a limit is set.  O(1) amortised on the steady
    // path; one file write per consumed request (atomic rename).
    Decision  authorize_request();

    // Read-only snapshot for /health, /status, logs.  No counter mutation.
    Decision  check_license();

    // Inspector — for diagnostics and the "Trial used X/Y" banner.
    Mode      mode()   const;
    std::optional<License> current() const;   // POD view of the loaded .lic
};

} // namespace visio::license
```

### Call-site integration

Two integration points in `src/server/Routes.cpp`:

* In `main.cpp` after `AppConfig::load()`: `LicenseLib::instance().init(cfg)`. Fatal on `!ok` unless `Mode::Trial` fallback succeeded.
* In the request pre-handling advice (where `Trial::consume()` is called today):
  ```cpp
  auto d = LicenseLib::instance().authorize_request();
  if (!d.allow) { respond 403 with reason; return; }
  ```
  Existing `Trial.cpp` path stays as the implementation of `Mode::Trial` inside LicenseLib — call sites switch to the new facade once and stop branching on `PROVISION_TRIAL`.

---

## 3. Module decomposition

```
src/license/
├── DESIGN.md           ← this file
├── docs/openapi.json   ← server contract, source of truth
├── LicenseLib.{h,cpp}  ← public facade, mode dispatch, decision merge
├── Config.{h,cpp}      ← Config POD + env/file overrides
├── crypto/
│   ├── Ed25519.{h,cpp} ← libsodium verify wrapper (kid → pubkey lookup)
│   ├── Sha256.{h,cpp}  ← OpenSSL EVP wrapper (already linked for Trial)
│   └── Jcs.{h,cpp}     ← RFC 8785 canonical JSON serializer (custom; see §5)
├── fingerprint/
│   └── Fingerprint.{h,cpp}  ← CPU_ID + Hostname + clientId + product → SHA-256 hex
├── model/
│   ├── License.{h,cpp}  ← .lic POD, JSON ↔ struct, signature round-trip
│   └── Token.{h,cpp}    ← optional Token mode payload
├── validator/
│   └── LicenseValidator.{h,cpp}  ← signature, fingerprint, expiry, kid, product, clientId
├── keyring/
│   ├── Keyring.{h,cpp}        ← kid → Ed25519 pubkey, accept-if-any
│   └── trusted_keys.cmake.in  ← CI-substituted compile-time payload
├── activation/
│   ├── ActivationClient.{h,cpp}  ← libcurl POST /api/v1/activate
│   └── Idempotency.{h,cpp}       ← UUID derive + cached response state
├── counter/
│   └── RequestCounter.{h,cpp}  ← HMAC-signed counter (HMAC-SHA256, anti-tamper, anti-rollback)
└── Trial.{h,cpp}        ← KEPT — wired up as Mode::Trial backend
```

Each module is testable in isolation (GTest, mocked filesystem + HTTP). `LicenseLib` itself owns only state-machine + persistence glue.

### Module-by-module responsibilities

**`crypto/Ed25519`** — libsodium `crypto_sign_verify_detached`. Public API: `bool verify(payload, signature_b64url, pubkey_bytes)`. Pubkey lookup via `Keyring`. No signing on the client.

**`crypto/Sha256`** — already used by `Trial.cpp`. Same OpenSSL EVP wrapper, exposed for `Fingerprint`.

**`crypto/Jcs`** — see §5. The piece that must be bit-identical with the server's serializer.

**`fingerprint/Fingerprint`** — collects CPU_ID + Hostname + clientId + product, concatenates per spec (see §6), hashes SHA-256, returns hex string. Cacheable per-process; recompute on hostname change (rare, logged).

**`model/License`** — POD reflecting `License` schema from openapi.json (§4 below). Round-trips via nlohmann::json. Holds the **raw original bytes** of the unsigned canonical form too — verification re-runs JCS on parsed fields and asserts equality (defence against canonicalization drift, and a paranoia knob during early integration).

**`validator/LicenseValidator`** — orchestrates: load → JSON parse → JCS canonicalize → Ed25519 verify against `Keyring[kid]` → assert `product`, `clientId`, `issuedAt <= now+skew <= expiresAt`, fingerprint match if present. Returns `Decision` minus the counter step.

**`keyring/Keyring`** — `unordered_map<std::string, std::array<uint8_t,32>>` populated at startup from a compile-time constant table emitted by CMake from CI-provisioned pubkeys. Lookup is constant-time per kid. `accept-if-any` semantics: a `.lic` is valid as long as `kid` matches any trusted entry; key rotation = ship a new build with both old+new kids in the table.

**`activation/ActivationClient`** — libcurl, JSON request body, `Idempotency-Key` header (UUID v5 derived from `clientId + product + fingerprint + activationCode` so retries from the same machine always present the same key per openapi.json contract). One blocking call, configurable timeout, retries only on 5xx and connect-timeout (max 3, exponential backoff). 200 → parse, validate, persist. 401/403/409 → bubble distinct errors to the operator.

**`activation/Idempotency`** — derives the deterministic UUID v5 (namespace = product-specific seed baked at CI). Persists the last (Idempotency-Key, response) pair so a client crash mid-activation can retry safely.

**`counter/RequestCounter`** — generalisation of `Trial.cpp`. HMAC-SHA256 over `{used, last_seen_unix, limit, lic_hash}`. `lic_hash = SHA-256(JCS canonical .lic)` ties the counter to the specific `.lic` — replacing the `.lic` with another (e.g. a fresh activation) resets the counter cleanly; rolling back to an older `.lic` invalidates the counter and fails closed. Same anti-rollback (compare `last_seen_unix` to `now`) and anti-tamper (HMAC mismatch → exhausted) semantics as `Trial`.

**`Trial`** — KEPT. Wrapped behind `Mode::Trial` inside LicenseLib so existing build flag `-DPROVISION_TRIAL=ON` continues to compile and run untouched.

---

## 4. `.lic` schema

Mirrors `components.schemas.License` from openapi.json exactly:

| Field | Type | Required | Notes |
|---|---|---|---|
| `licenseId` | string (UUID) | yes | DB primary key on the server, opaque to the client |
| `clientId` | string (UUID) | yes | must equal `Config.client_id` |
| `product` | string | yes | must equal `Config.product` (e.g. `"OCR"`) |
| `tariff` | string | yes | informational (`"BUSINESS"`, `"ENTERPRISE"`, …) |
| `activationCode` | string | yes | identifier; in offline-`.lic` distinguishes purchases |
| `fingerprint` | string \| absent | **online only** | SHA-256 hex; presence = online, absence = offline. **Distinguish ABSENT from `null`** — openapi.json uses `absent`, the JCS serializer must not emit a `null` here |
| `maxServers` | integer | yes | informational on the client (slot cap is enforced server-side at activation) |
| `limits` | object | yes | sub-fields below |
| `limits.requestLimit` | integer \| `null` | no | when present > 0, the local counter enforces it; `null` / absent = unlimited |
| `limits.gpu` | boolean | no | gate for GPU code paths (advisory for now) |
| `issuedAt` | string (RFC 3339) | yes | must be ≤ `now + skew` |
| `expiresAt` | string (RFC 3339) | yes | hard fail when `now > expiresAt + skew` |
| `kid` | string | yes | trusted-keyring lookup |
| `signature` | string (base64url) | yes | Ed25519 over JCS(payload − signature) |

The on-disk format is the raw JSON returned by `/api/v1/activate` (or shipped via offline issuance) — UTF-8, no BOM, no surrounding whitespace required. Persisted atomically via temp-file + `rename(2)`.

---

## 5. Canonical JSON (RFC 8785 / JCS) — exact rules

Must match the server byte-for-byte. The single hardest interop point. All rules below apply to the client signature *verification* step (not signing — client never signs):

1. **Encoding**: UTF-8, no BOM. Output is `std::string` of bytes.
2. **Object members**: sort keys by **UTF-16 code-unit order** (NB: not UTF-8 byte order; matters for non-ASCII keys — our schema is ASCII-only so the two coincide, but the serializer must implement the strict JCS rule). Sorted recursively at every depth.
3. **Member separator**: `:` with **no surrounding whitespace**. Element separator: `,` with no surrounding whitespace.
4. **No insignificant whitespace** anywhere — no spaces, no newlines, no tabs.
5. **Strings**: UTF-8 → JSON-string encoding per RFC 8785 §3.2.2:
   * Escape only: `"`→`\"`, `\\`→`\\\\`, U+0008→`\b`, U+0009→`\t`, U+000A→`\n`, U+000C→`\f`, U+000D→`\r`, other C0 / DEL → `\u00XX` with **lowercase** hex.
   * U+2028 / U+2029: NOT escaped (JCS deviates from JSON.stringify here — these stay literal).
   * No `\uXXXX` for characters that have a short escape (no `` when `\b` is available).
   * Surrogate pairs: only valid pairs, never lone surrogates.
6. **Numbers**: ECMAScript `Number.prototype.toString()` (per JCS §3.2.2.3). Integers in safe range (≤ 2⁵³−1) — emit without decimal point. We deliberately use only integer fields (`maxServers`, `limits.requestLimit`) so the float-formatting trap (`5.0` vs `5`, `1e21` switchover) does not bite. The serializer must still reject floats to fail loud if the server schema ever adds one.
7. **Booleans / null**: literal `true`, `false`, `null` — never quoted, no surrounding space.
8. **Arrays**: preserve insertion order, no sort. We have no array fields in `License` itself but `Token` may use them.
9. **Datetime fields** (`issuedAt`, `expiresAt`): treated as **opaque strings**. The server emits a fixed RFC 3339 form (`YYYY-MM-DDTHH:MM:SS.sssZ` or `YYYY-MM-DDTHH:MM:SSZ`); the client passes those bytes through unchanged. The client parses them only for the expiry check — but the bytes used during JCS for signature verification are the original ones from the wire.
10. **Absent vs null**: an absent field (e.g. `fingerprint` in an offline `.lic`) **must not be emitted at all** during canonicalization. `null` is a distinct value and is emitted as the literal `null`. The schema only has one nullable field (`limits.requestLimit`) — explicitly differentiate `{ "limits": { "requestLimit": null } }` from `{ "limits": {} }` and document the convention server-side. Our parser path: round-trip via nlohmann::json with `null` preserved (`nullptr_t` value) and absent fields not added.

**Verification algorithm** (client side):

```
input:    raw_json_bytes (the .lic on disk or from /activate response)
1. parse  J = json::parse(raw_json_bytes)             // tolerates any input whitespace
2. take   sig  = J["signature"]; payload = J minus "signature"
3. emit   canonical_bytes = JCS(payload)              // deterministic, see rules above
4. lookup pk = Keyring[J["kid"]];  fail if absent
5. verify Ed25519_verify(sig_b64url_decode(sig), canonical_bytes, pk)
6. assert product == Config.product, clientId == Config.client_id
7. parse  issuedAt, expiresAt; check time window
8. if J has "fingerprint":
      compute local_fp = SHA256(CPU_ID || Hostname || clientId || product) hex
      assert fingerprint == local_fp
   else: offline mode, fingerprint check skipped
```

Failure at any step → `Decision{allow=false, reason=…}` with the most specific `Deny` value (so operators get actionable errors, not just "bad license").

**Test corpus**: ship server-generated `.lic` golden samples in `tests/license/golden/` and assert byte-identical JCS output during CI. This is the single highest-value test — canonicalization drift between server and client is the v2.1 footgun.

---

## 6. Fingerprint formula

From openapi.json `ActivateRequest.fingerprint.description`:

> `SHA-256 hex = SHA-256(CPU_ID ‖ Hostname ‖ clientId ‖ product)`

Concrete client implementation:

* `‖` = byte concatenation (no separator, no delimiter). Document this in the test corpus; one shared test fixture with the four inputs and the expected hex must be cross-checked against the server's reference impl before shipping.
* **CPU_ID** sources, in priority order:
  1. Linux: `/sys/devices/system/cpu/cpu0/topology/physical_package_id` + `/sys/class/dmi/id/product_uuid` (concatenated, read once at init). Need root or DMI accessibility; in Docker `/sys/class/dmi` is exposed by default but `product_uuid` requires `--privileged` or `--cap-add SYS_ADMIN` — fallback below.
  2. Linux fallback: `cpuid` `EAX=0` family/model/stepping + `/proc/cpuinfo` `serial` if present.
  3. Linux ultimate fallback: MAC address of the lowest-indexed non-loopback NIC. Documented as the weakest tier — fingerprint becomes NIC-bound rather than CPU-bound.
  4. Windows: `IOCTL_STORAGE_QUERY_PROPERTY` on the system drive (SMART serial), fall back to `Win32_ComputerSystemProduct.UUID` via WMI/DMI.
* **Hostname**: `gethostname(2)` on POSIX, `GetComputerNameExW(ComputerNamePhysicalDnsHostname)` on Windows. **No FQDN, no domain suffix** — the server's reference must match.
* **clientId / product**: byte-exact strings from `Config`. UUID form for clientId (lowercase, 36 chars with dashes — per UUID canonical form used by openapi.json).
* Output: lowercase hex of SHA-256, 64 chars, no separators.

Caching: compute once at `init()`, store in process memory. **Do not** persist (writing the fingerprint to disk creates an attack surface for swap-in cloning attacks; recompute on each cold start instead — cheap).

**Hostname stability (operational requirement)**:

* The fingerprint embeds hostname → if hostname changes between activation and a later run, all `.lic` validations fail.
* **Docker default behaviour is to assign a random hostname per `docker run` invocation** unless `--hostname=<name>` (or compose `hostname:`) is set. This is the most common foot-gun.
* **Required deployment guidance** (must appear in the operator runbook before any online-mode customer ships):
  1. Run with `--hostname=<stable-host>` matching the host's `/etc/hostname`, OR
  2. Bind-mount `/etc/hostname` and `/etc/hosts` from the host into the container, OR
  3. Use `network_mode: host` in compose (inherits host hostname).
* On Kubernetes: pod hostname is the pod name by default — for licensed deployments require a `StatefulSet` (stable name) or set `hostname:` explicitly on the pod spec.
* At init time, the client logs `[license] fingerprint inputs: cpu_id=… host=… → fp=…` (truncated CPU_ID) so operators can diagnose "license suddenly invalid after restart" quickly.

---

## 7. Modes and decision dispatch

`LicenseLib::init()` resolves mode by this precedence (first match wins):

1. **`PROVISION_LICENSE_MODE` env override** — explicit `trial` / `offline` / `online` / `token`. Useful for dev and emergency fallback. Refuses to enable Online without a present `.lic`.
2. **`.lic` file at `Config.lic_path`** — parsed, validated, mode = Online if `fingerprint` present else Offline.
3. **Token bundle** at `Config.state_dir / "tokens.json"` — Mode::Token.
4. **Build-time `PROVISION_TRIAL=ON`** → Mode::Trial via the existing `Trial.cpp`.
5. **None of the above** → `init()` returns `{ok=false, mode=Trial, detail="no license"}` and the server refuses to serve.

Per-request dispatch (`authorize_request()`):

| Mode | Allow when |
|---|---|
| Trial | `Trial::consume()` returns true |
| Offline | signature still valid (re-checked once per N requests cheaply via cached LicenseValidator state) AND `now ≤ expiresAt + skew` AND `counter.consume()` (only if `requestLimit` set) |
| Online | Offline rules + fingerprint still matches (rechecked once per process restart — hostname mid-process changes are out of scope) |
| Token | active token signature valid + token unexpired |

Per-request cost on the steady path: one HMAC-SHA256 over the counter struct + one `rename(2)` (only when `requestLimit` is set). No Ed25519 verify per request — that runs once at init and once after activation, the result is cached for the process lifetime.

---

## 8. Online activation flow

```
1. operator provides activation code (env PROVISION_LICENSE_ACTIVATION_CODE or admin CLI input)
2. LicenseLib::activate(code) computes fingerprint
3. derives Idempotency-Key = UUIDv5(NS_ACTIVATION, clientId + product + fp + code)
4. POST {activation_url}/api/v1/activate
       headers: Content-Type: application/json,
                Idempotency-Key: <uuid>
       body:    {"activationCode": "...", "fingerprint": "..."}
5. on 200:  validate response.lic via LicenseValidator
            write lic_path atomically (temp + rename)
            reset RequestCounter (new lic_hash)
            return InitResult{ok=true, mode=Online}
6. on 401:  bad code → return {ok=false, detail="invalid activation code"}
7. on 403:  slot exhausted → distinct error, operator opens cabinet to revoke another binding
8. on 409:  same Idempotency-Key with different body → log + abort (programming error)
9. on 5xx / network: retry with exponential backoff up to 3 times, then fail
```

Idempotency cache: `state_dir/activation.dat` stores `(key, last_response_status, last_response_body_hash)`. If a previous attempt got to step 5 but crashed before persisting `.lic`, the next run re-sends the same key and the server returns the cached response per its idempotency contract — no slot is consumed twice.

---

## 9. Storage

All under `Config.state_dir`:

| File | Mode | Purpose |
|---|---|---|
| `license.lic` | 0640 | Ed25519-signed license JSON (online or offline) |
| `counter.dat` | 0600 | HMAC-signed request counter (per `.lic`) |
| `activation.dat` | 0600 | Idempotency cache for the activation endpoint |
| `tokens.json` | 0640 | optional Token-mode bundle (signed by integrator key) |
| `trial.dat` | 0600 | existing Trial counter (Mode::Trial only) |

Default `state_dir`:

* Linux (Docker / bare metal): `/var/lib/provision-llm/` — same as Trial today. Owned by the service user.
* Windows: `%PROGRAMDATA%\ProvisionLLM\` — same as Trial.

**Docker requirement**: this directory must be a **named volume or bind mount**, not the container's ephemeral layer:

```yaml
volumes:
  - provision-llm-state:/var/lib/provision-llm
```

Without persistence, every container restart loses the `.lic` → re-activation required → eventually hits `maxServers` and the customer can't run. Document this as a hard requirement in the deployment guide; consider an init-time check `is_writable(state_dir) AND has_persistence_marker()` that prints a loud warning on missing volume mount.

---

## 10. Dependencies and build integration

| Need | Choice | Rationale |
|---|---|---|
| Ed25519 verify | **libsodium** (`crypto_sign_verify_detached`) | Audited, single function, no curve plumbing. apt: `libsodium-dev`. |
| SHA-256 | **OpenSSL libcrypto** (already linked for `Trial`) | No new dep — same EVP wrapper as today. |
| HMAC-SHA256 (counter) | **OpenSSL libcrypto** | Already used in `Trial.cpp`. |
| JSON parse | **nlohmann/json** (already in project) | Round-trip with `null` preservation. |
| Canonical JSON (JCS) | **custom serializer** in `crypto/Jcs.cpp` | nlohmann's `dump()` is NOT JCS — sort order, escape rules, number format all differ. Write our own walker over the parsed `json` tree. |
| HTTP (activation) | **libcurl** | Already a transitive dep via Drogon; explicit `find_package(CURL)` to make it direct. |
| UUID v5 | small in-tree implementation | One function, deterministic, no point pulling boost::uuid. |

CMakeLists.txt additions (new option, default ON for release builds):

```cmake
option(PROVISION_LICENSE "Build full LicenseLib (online/offline/token modes)" ON)

if(PROVISION_LICENSE)
    find_package(unofficial-sodium CONFIG QUIET)   # vcpkg name
    if(NOT TARGET unofficial-sodium::sodium)
        find_library(SODIUM_LIBRARY sodium REQUIRED)
        find_path(SODIUM_INCLUDE_DIR sodium.h REQUIRED)
    endif()
    find_package(CURL REQUIRED)
    # OpenSSL::Crypto already linked when PROVISION_TRIAL=ON; ensure unconditionally
    # under PROVISION_LICENSE.
    find_package(OpenSSL REQUIRED)

    target_sources(provision_ocr_server PRIVATE
        src/license/LicenseLib.cpp
        src/license/Config.cpp
        src/license/crypto/Ed25519.cpp
        src/license/crypto/Sha256.cpp
        src/license/crypto/Jcs.cpp
        src/license/fingerprint/Fingerprint.cpp
        src/license/model/License.cpp
        src/license/model/Token.cpp
        src/license/validator/LicenseValidator.cpp
        src/license/keyring/Keyring.cpp
        src/license/activation/ActivationClient.cpp
        src/license/activation/Idempotency.cpp
        src/license/counter/RequestCounter.cpp)
    target_link_libraries(provision_ocr_server PRIVATE
        OpenSSL::Crypto CURL::libcurl
        $<IF:$<TARGET_EXISTS:unofficial-sodium::sodium>,unofficial-sodium::sodium,${SODIUM_LIBRARY}>)
    target_compile_definitions(provision_ocr_server PRIVATE
        PROVISION_LICENSE=1
        PROVISION_LICENSE_CLIENT_ID="${PROVISION_LICENSE_CLIENT_ID}"
        PROVISION_LICENSE_PRODUCT="${PROVISION_LICENSE_PRODUCT}"
        PROVISION_LICENSE_ACTIVATION_URL="${PROVISION_LICENSE_ACTIVATION_URL}")
    # Keyring: CMake reads a CI-provisioned JSON file with trusted pubkeys
    # and configures src/license/keyring/trusted_keys.cmake.in into
    # generated trusted_keys.h.  Each entry: { kid: "...", pubkey: "<32-byte hex>" }.
    configure_file(
        src/license/keyring/trusted_keys.cmake.in
        ${CMAKE_BINARY_DIR}/gen/license/trusted_keys.h @ONLY)
    target_include_directories(provision_ocr_server PRIVATE ${CMAKE_BINARY_DIR}/gen)
endif()
```

`PROVISION_TRIAL` and `PROVISION_LICENSE` are **composable**, not exclusive: a trial build that also carries the full LicenseLib code path means a single binary can support both eval (trial counter, no `.lic`) and paid (`.lic` present) flows — `LicenseLib::init` picks the right mode at runtime per §7.

CI-injected build args (Docker):

```
--build-arg PROVISION_LICENSE_CLIENT_ID=<uuid-from-customer-tenant>
--build-arg PROVISION_LICENSE_PRODUCT=OCR
--build-arg PROVISION_LICENSE_ACTIVATION_URL=https://license.provlabs.tech
--build-arg PROVISION_LICENSE_KEYRING_JSON=/run/secrets/keyring.json   # mounted at build time
```

The keyring JSON file is not committed to git — provisioned via GitLab CI/CD variable (file type), passed to Docker build via `--secret`, consumed once at configure-time, baked into `trusted_keys.h`, gone from final image. Old kids stay in the keyring until end-of-life of all `.lic` files signed by them.

---

## 11. Failure modes and reporting

Every `Decision{allow=false}` carries a specific `Deny` value. The route layer maps these to HTTP responses and operator-visible messages:

| `Deny` | HTTP | Operator message |
|---|---|---|
| `NoLicense` | 403 | "No license installed. Run activation or mount a valid offline .lic." |
| `BadSignature` | 403 | ".lic signature invalid — file corrupted or untrusted source." |
| `UnknownKid` | 403 | ".lic signed by an unknown key id — your build does not trust the server that signed it. Upgrade required." |
| `FingerprintMismatch` | 403 | "Online .lic was issued for a different machine. Reactivate or run with stable hostname." |
| `Expired` | 403 | "License expired on YYYY-MM-DD. Renew via the cabinet." |
| `NotYetValid` | 403 | "License not yet valid (issuedAt in the future). Check system clock." |
| `RequestLimitReached` | 403 | "Request limit reached (used/limit). Upgrade tariff." |
| `ClockRollback` | 403 | "System clock moved backwards relative to last-seen state. Refusing to serve." |
| `TamperedState` | 403 | "Local counter file tampered with. Refusing to serve." |
| `ProductMismatch` / `ClientMismatch` | 403 | "License belongs to a different product/tenant. Wrong .lic file." |
| `TokenInvalid` | 403 | "Integrator token expired or signature mismatch." |
| `NotInitialised` | 503 | "License subsystem failed to initialise. See logs." |

All failure events are logged at WARN with sufficient detail (kid, expiresAt, last-seen, etc.) for support to diagnose without access to the host. The actual `.lic` content (signature, activationCode) is logged truncated.

`/health` reports `{ "license": { "mode": "online", "used": 117, "limit": 5000, "expires_at": "..." } }` so dashboards can alarm before customer-visible failure (e.g. 90% of `requestLimit`).

---

## 12. Threat model (brief)

* **Casual file tampering** (edit `.lic` JSON to remove `expiresAt`): Ed25519 signature breaks → deny.
* **Counter file rollback**: HMAC + `last_seen` timestamp → deny.
* **`.lic` file copy across machines** (online mode): fingerprint mismatch → deny.
* **`.lic` file copy across machines** (offline mode): allowed by design — offline `.lic` is reusable, mitigated by the slot cap on the **purchase side** and short expiry windows; not a client-side problem.
* **Clock manipulation forward** (skip expiry check): wins until `last_seen` catches up, then anti-rollback fires on the next clock correction. Acceptable for a v2.1 trust model.
* **Binary patching to skip the check**: out of scope. We are not building anti-RE; the `.lic` system protects honest customers and creates an audit trail, not an unbreakable lock.
* **Stolen activation code reuse**: server-side concern (slot cap, rate limit, code revocation). Client-side: nothing to do.
* **Keyring exfiltration**: keys are public — keyring is no secret. The private signing key never leaves the server.

---

## 13. Open questions for the server team

1. **JCS reference impl byte-equality**: do they have an existing JCS test corpus we can pin? Suggest a shared `golden/*.json` + `golden/*.canonical` directory in a small cross-repo artefact.
2. **Float fields in `License`**: confirm none will be added — if `requestLimit` ever becomes a float (e.g. fractional QPS), client must implement ECMA-262 number serialisation.
3. **`fingerprint` field absence vs `null`** in offline `.lic`: confirm the canonicalization treats absent and not-emitted identically (i.e. server NEVER emits `"fingerprint": null` in offline). Pin this in a golden.
4. **`limits.requestLimit` value when unlimited**: `null`, `0`, or absent? Client should accept all three but document the canonical choice.
5. **Datetime format stability**: confirm fixed format (e.g. always `…Z`, never `+00:00`; always millisecond precision or always second). Pin in a golden.
6. **Keyring file format** for the CI hook: confirm shape `{ "keys": [{"kid": "...", "pubkey": "<base64url>"}] }`.

These questions block the first byte-identical interop test — closing them turns LicenseLib from "plausible" to "shippable".
