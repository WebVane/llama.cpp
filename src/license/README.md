# LicenseLib runbook

> This module is shared with `provision_ocr_server`. In provision-server (this
> repository) it is built by `tools/server/CMakeLists.txt` (`LLAMA_SERVER_LICENSE`
> / `LLAMA_SERVER_TRIAL`), the product is `LLM`, the HTTP transport is
> cpp-httplib and logging goes through llama.cpp's `log.h`. Endpoints and
> the per-request gate live in `tools/server/server.cpp`.

This is the operations-side reference for the v2.1 license subsystem
that ships with `provision_ocr_server`. The end-to-end design (architecture,
schemas, threat model) lives in [`DESIGN.md`](DESIGN.md); the wire
contract is [`docs/openapi.json`](docs/openapi.json). This file answers
the practical questions: how do I configure it, deploy it, and debug it
when something goes wrong.

---

## 1. Modes

`LicenseLib` resolves the active mode at startup, in this priority:

1. **Environment override** — `PROVISION_LICENSE_MODE` = `trial` / `offline` / `online` / `token`. Useful for dev and emergency fallback.
2. **`.lic` file** at `${state_dir}/license.lic` — has `fingerprint` ⇒ **Online**; no `fingerprint` ⇒ **Offline**.
3. **Token bundle** at `${state_dir}/tokens.json` — **Token** (verification stub in C5; full impl pending).
4. **Build-time trial** (`-DLLAMA_SERVER_TRIAL=ON`) — falls back to **Trial** with the compile-time limit.
5. None of the above — the server starts **LOCKED** (`mode=none`): inference routes answer 403 with `{error, license:{mode, reason, used, limit}}`, while `/health`, `GET /license/status` and `POST /license/activate` stay reachable so the operator can activate without a restart (see `license_startup()` in `tools/server/server.cpp`).

| Mode | When to use | Server contact | Local enforcement |
|---|---|---|---|
| **Trial** | dev / eval | never | HMAC-signed request counter, hard cap (default 50) |
| **Online** | regular sale | once during activation | Ed25519 verify + fingerprint match + (optional) request counter + expiry |
| **Offline** | air-gapped sale | never | Ed25519 verify + (optional) request counter + expiry — **no fingerprint** |
| **Token** | integrator-signed grants | never | Pending |

---

## 2. Configuration

`Config::from_defaults()` builds the active config from compile defines plus environment overrides. In provision-server the defines come from the `LLAMA_SERVER_*` CMake cache entries in `tools/server/CMakeLists.txt` (`LLAMA_SERVER_LICENSE_PRODUCT` -> `PROVISION_LICENSE_PRODUCT`, `LLAMA_SERVER_LICENSE_ACTIVATION_URL` -> `PROVISION_LICENSE_ACTIVATION_URL`, `LLAMA_SERVER_TRIAL_LIMIT` -> `PROVISION_TRIAL_LIMIT`, `LLAMA_SERVER_LICENSE_KEYRING_JSON` -> baked keyring); `clientId` is runtime-only (env). **None of these need to be set for a Trial-only eval build.** Set them all for an Online deployment.

| Env var | Compile define | Default | Notes |
|---|---|---|---|
| `PROVISION_LICENSE_PRODUCT` | `PROVISION_LICENSE_PRODUCT` | `LLM` (this build; `OCR` in provision_ocr_server) | Must match `product` in any accepted `.lic` |
| `PROVISION_LICENSE_CLIENT_ID` | — (env only) | empty | UUID of the customer tenant. Must match `clientId` in any accepted `.lic` |
| `PROVISION_LICENSE_ACTIVATION_URL` | `PROVISION_LICENSE_ACTIVATION_URL` (`LLAMA_SERVER_LICENSE_ACTIVATION_URL`) | `https://license.scan-dev.com` | override at runtime if the License Server moves |
| `PROVISION_LICENSE_STATE_DIR` | — | `/var/lib/provision-llm` (Linux); `%PROGRAMDATA%\ProvisionLLM` (Windows) | Must be a **persistent, writable directory** — see §3 |
| `PROVISION_LICENSE_TRIAL_LIMIT` | `PROVISION_TRIAL_LIMIT` | `50` | Used only in Trial mode |
| `PROVISION_LICENSE_MODE` | — | unset | Force a mode for the current process |
| `PROVISION_LICENSE_CPU_ID_OVERRIDE` | — | unset | **Critical for Docker-Online deploys** — see §4 |
| `PROVISION_LICENSE_HOSTNAME_OVERRIDE` | — | unset | Same |
| `PROVISION_LICENSE_REQUIRE_STABLE_CPU_ID` | — | `1` | Set `0` to disable the loud-fail when CPU_ID resolves to MAC fallback (NOT for production) |

The trusted Ed25519 public keys are baked into the binary at build time via `configure_file` — see §5.

---

## 3. State directory

The license subsystem writes three files into `state_dir`:

| File | Purpose | Lifetime |
|---|---|---|
| `license.lic` | The signed license (atomically rewritten on each successful `activate()`) | Survives upgrades; rotates on reactivation |
| `counter.dat` | HMAC-signed request counter, bound to the loaded `.lic`'s SHA-256 hash | Survives upgrades. Re-activating a licence the machine has counted before keeps its tally; only a licence new to the machine starts at 0 |
| `counter.dat.ledger` | HMAC-signed tally of every licence this machine has counted (up to 16), mirrored on every counter write | Lets a licence get its real tally back after switching A→B→A, or after `counter.dat` alone is deleted or damaged. Deleting **both** files still starts the licence over — a counter the customer can erase cannot be authoritative on the client |
| `tokens.json` | Optional integrator-signed token bundle (Token mode) | Operator-managed |

(Trial builds additionally write `trial.dat` if the legacy module is kept linked — but the unified facade now uses `counter.dat` for trial mode too.)

**Hard requirement**: `state_dir` must be a **persistent, writable volume**. In Docker:

```yaml
volumes:
  - provision-llm-state:/var/lib/provision-llm
```

Without persistence:
- Every container restart loses the `.lic` ⇒ re-activation required ⇒ eventually hits `maxServers` ⇒ customer can't run.
- Counter resets on every restart ⇒ no enforcement of `requestLimit`.

The facade does NOT currently detect a missing volume mount — that check is on the deployment side.

---

## 4. The Docker / Kubernetes online-deploy gotchas (this section saves a support ticket per customer)

Online mode binds the license to a SHA-256 of `CPU_ID ‖ Hostname ‖ clientId ‖ product`. If any of those four inputs changes between activation and a subsequent restart, the local fingerprint no longer matches the one embedded in the `.lic` ⇒ every request is rejected with `FingerprintMismatch`.

### 4.1 Hostname (must be stable AND unique per instance)

Docker default behaviour: assign a random hostname on each `docker run`. **This will break online mode every single restart.**

Required deployment options (pick one):

* **Compose / `docker run`**: `--hostname=<stable-name>` (or `hostname:` in compose).
* **Kubernetes**: a `StatefulSet` (stable pod name) or `hostname:` set explicitly on the pod spec. A bare `Deployment` will not give you a stable hostname.
* **Bind-mount**: `-v /etc/hostname:/etc/hostname:ro -v /etc/hosts:/etc/hosts:ro` to inherit the host's hostname.

**Uniqueness matters too.** If two instances share the same hostname *and* the same machine-id (e.g. two containers on the same host, both using the host's `/etc/machine-id`), they share the same fingerprint and compete for the same activation slot — the first one to call `/activate` wins, the second sees `NoFreeSlot` (HTTP 403).

### 4.2 `CPU_ID` source

The facade probes, in priority order (Linux): `/etc/machine-id` → `/var/lib/dbus/machine-id` → `/sys/class/dmi/id/product_uuid` → **persisted-state-id** (`<state_dir>/machine-id`, see below) → MAC fallback. The chosen source is logged at startup:

```
[license] fingerprint: source=machine-id cpu_id=ab12cd34... host=visio-ocr-1 …
```

The source `mac-fallback` is treated as **unstable** and fails init unless you've explicitly accepted the risk (`PROVISION_LICENSE_REQUIRE_STABLE_CPU_ID=0`).

The `persisted-state-id` source is **STABLE** for as long as the operator keeps the state volume mounted — same precondition `license.lic` and `counter.dat` already require. The file is a cryptorandom UUIDv4 generated on first use, written atomically to `<state_dir>/machine-id` with `0600` perms (POSIX). It's auto-created when the hardware chain ends at MAC-fallback and `<state_dir>` is writable. **Copying the state directory between machines preserves the binding** — that's intentional and matches how `.lic` already carries machine identity through a volume copy.

Three safe configurations for Online mode under Docker:

* **A. Per-container machine-id**: a Dockerfile that writes a fresh, unique `/etc/machine-id` on first boot of each container. The hostname uniqueness from §4.1 is then automatic. (Most "official" Linux images already do this.)
* **B. Explicit override**: `-e PROVISION_LICENSE_CPU_ID_OVERRIDE=<UUID>` set at both **activation time** and **every subsequent run**. The override must be the same across reruns. This is the right path for evaluators on machines where `/etc/machine-id` isn't reliable.
* **C. Persisted-state-id** (default fallback): no environment configuration needed — the facade auto-generates `<state_dir>/machine-id` on first activation and reuses it on subsequent boots. Works out of the box on Docker Desktop / WSL2 / VM hosts where no stable hardware machine-id is exposed. Requirement: `state_dir` must be a persistent named volume (already required for `license.lic`).

DMI `product_uuid` is stable per-host but shared between containers on the same host — if you go that route, the hostname uniqueness (§4.1) is what stops two containers from colliding on the same fingerprint.

### 4.3 Recovery

If `state_dir/license.lic` is missing or corrupted on startup, the operator re-runs `activate()` — either by restarting with `PROVISION_LICENSE_ACTIVATION_CODE` set, or over HTTP via `POST /license/activate` (`{"activationCode": "..."}`, subject to the server `--api-key` if one is set, no restart needed) or from the web UI (Settings -> License). The Idempotency-Key is a pure function of `(clientId, product, fingerprint, activationCode)`, so the same machine submitting the same code always presents the same key, and the server returns the cached `.lic` without consuming a new slot.

---

## 5. Trusted keyring (CI provisioning)

`Keyring::baked()` returns the trusted Ed25519 public keys, sealed into the binary at configure-time. CI provides them via:

```cmake
cmake -DLLAMA_SERVER_LICENSE_KEYRING_JSON=/path/to/keyring.json ...
```

The file format mirrors `docs/vectors/keyring.json`:

```json
{ "keys": [ { "kid": "k-2026-q1", "pubkey": "<base64url 32B>" }, ... ] }
```

CMake reads the file at configure time, inlines its bytes as a raw-string literal into `src/license/keyring_baked.cpp.in`, and links the generated TU into `visio_core`. At runtime `Keyring::baked()` parses the embedded bytes once and returns the populated keyring.

**Rotation**: multiple `kid` entries are valid simultaneously (accept-if-any). Roll a new key by shipping a build with **both old and new kids** in the keyring, wait for all field `.lic`s signed with the old kid to expire, then ship a build with only the new kid. The license server (separate repo) picks a `kid` at sign time; the client trusts any that's in its baked keyring.

If `LLAMA_SERVER_LICENSE_KEYRING_JSON` is **not** set at configure time, the fallback `Keyring::baked()` returns an empty keyring — every `.lic` verification fails. This is the intentional "secure default" for dev / unsigned builds. **Production builds must set this; CI fails the build if a hash of the resulting binary lacks any trusted kid.**

Tests inject a separate keyring via `LicenseLib::set_keyring_for_test()` — the in-tree golden keyring at `src/license/docs/vectors/keyring.json` is **never** included in production builds.

---

## 6. Init diagnostics

`LicenseLib::init()` logs three things at INFO level on success:

```
[license] fingerprint: source=machine-id cpu_id=<8-char-prefix>... host=<hostname> client_id=<UUID> product=OCR fp=<64 hex>
[license] mode=online detail=loaded online license, limit=100000
```

On failure, the `detail` string in the InitResult is also logged at ERROR. Common patterns:

| Detail substring | Meaning | Operator action |
|---|---|---|
| `CPU_ID source 'mac-fallback' is not stable` | Probe fell through to MAC | Bind-mount `/etc/machine-id` or set `PROVISION_LICENSE_CPU_ID_OVERRIDE` |
| `online .lic fingerprint does not match local machine` | Either machine identity changed since activation, or `.lic` was copied from another host | Re-activate (idempotent); verify hostname/machine-id stability |
| `lic product='X' does not match build product='OCR'` | `.lic` was issued for a different product line | Wrong file; obtain the right `.lic` |
| `lic rejected (reason=bad_signature; ...)` | Either tampered file or keyring mismatch | Verify the `.lic` against `docs/openapi.json`'s `License` schema; ensure the build's keyring contains the signing kid |
| `mode resolution failed (no license sources found...)` | No `.lic`, no tokens, no Trial build | Place a `.lic` at `${state_dir}/license.lic` or rebuild with `-DLLAMA_SERVER_TRIAL=ON` |
| `activation failed (status=N; detail=...)` | `activate()` did not get a usable response | Map status N via `ActivationStatus` enum in `Activation.h` — 1=`InvalidCode`, 2=`NoFreeSlot`, 3=`IdempotencyConflict`, 4=`NetworkError`, 5=`HttpError`, 6=`BadResponse`, 7=`FingerprintMismatch` |

The per-request DenyReason on 403 responses is surfaced in the JSON body under `error` (machine-readable) and `message` (human-readable).

---

## 7. Per-request decision (hot path)

In provision-server both calls live in `license_gate()` (`tools/server/server.cpp`), which wraps every inference route (completions, chat, responses, messages, embeddings, rerank, infill, transcriptions); `/health`, `/props`, `/models`, tokenize, `/license/*` and the UI assets are not wrapped. The original OCR integration in `Routes.cpp` is described below for reference:

1. **Pre-handler advice — `LicenseLib::check_license()`**, read-only. Every path except `/health`, `/license/status`, `/license/activate` and `/docs*` is gated here: no license / expired / limit already reached → 403 with `{error, mode, used, limit, message}` and nothing is consumed.
2. **Result delivery — `LicenseLib::authorize_request()`**, the metered consume. The `/processing*` and `/get_words` handlers wrap their response callback in `metered()`, which calls `authorize_request()` only when a **2xx** result is about to be sent. A 415 (undecodable image), 400 (unknown template, interpreter error, empty PDF), 503 (worker queue full) or 500 is passed through unmetered — those never burn trial quota. Losing the race for the last slot between the gate and delivery turns the computed result into the same 403 body.

Cost on the hot path:

* Trial / Offline / Online + `requestLimit > 0`: one HMAC-SHA256 over the counter struct + one `rename(2)` to atomically persist (on the thread that delivers the result — a worker thread for OCR, the IO thread for text-layer PDFs).
* Trial / Offline / Online + `requestLimit == 0`: a few timestamp comparisons, no disk I/O.

Ed25519 verification and fingerprint match run **once** at `init()` and are cached for the process lifetime. Mid-process system-clock changes are detected (re-checked expiry on every call); mid-process hostname / CPU_ID changes are deliberately NOT — those would imply a host reconfiguration that should bounce the service.

---

## 8. Quick reference: dev / CI / prod build commands

**Dev (Trial, no signing keyring):**
```bash
cmake -DLLAMA_SERVER_TRIAL=ON -DLLAMA_SERVER_TRIAL_LIMIT=50 -DLLAMA_BUILD_BORINGSSL=ON ...
```

**Conformance test (no production keyring, the test injects the golden keyring):**
```bash
cmake -DLLAMA_SERVER_TRIAL=ON -DLLAMA_BUILD_TESTS=ON ... && cmake --build build --target test-license
ctest --test-dir build -R test-license --output-on-failure
```

**Production trial-with-activation build (what `docker-dual` `TRIAL=1` does):**
```bash
cmake \
  -DLLAMA_SERVER_TRIAL=ON -DLLAMA_SERVER_TRIAL_LIMIT=50 \
  -DLLAMA_SERVER_LICENSE_PRODUCT=LLM \
  -DLLAMA_SERVER_LICENSE_ACTIVATION_URL=https://license.scan-dev.com \
  -DLLAMA_SERVER_LICENSE_KEYRING_JSON=/run/secrets/license_keyring \
  ...
```

`-DLLAMA_SERVER_LICENSE=ON` without `LLAMA_SERVER_TRIAL` builds a licensed-only
server: no trial fallback, a `.lic` (or activation) is required before any
inference request is served. `clientId` is never baked; set
`PROVISION_LICENSE_CLIENT_ID` at runtime for strict tenant binding.

The keyring file is mounted only at configure time (e.g. via GitLab CI's "file" variable or `--secret` for BuildKit), and the secret never appears in the resulting image.
