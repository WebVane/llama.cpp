# Provision LLM Engine — Windows installer

Native Windows port of `docker-dual/`: a single installer that ships a
**universal** `llama-server` running in **router mode** with two models
baked into the install — chat (`qwen2.5-chat`) and embeddings
(`gte-qwen2-embed`) — on one HTTP endpoint with the embedded WebUI.

**Universal GPU support.** The binary bundles three ggml backends as
runtime-loaded DLLs and picks one automatically per machine:

| GPU on the machine | backend used | DLL |
|---|---|---|
| NVIDIA (e.g. GTX 1080 Ti) | **CUDA** | `ggml-cuda.dll` |
| AMD (e.g. RX 7800 XT) / Intel Arc | **Vulkan** | `ggml-vulkan.dll` |
| no GPU / unsupported | **CPU** (auto microarch) | `ggml-cpu-*.dll` |

`ggml-backend-reg` loads every backend it can; the CUDA backend simply
reports 0 devices on an AMD box (no NVIDIA driver) and Vulkan takes over.
CUDA is registered before Vulkan, so on NVIDIA hardware CUDA wins even
though the card is also visible via Vulkan.

No Docker, no Node.js at runtime. After install you get a Start-menu
shortcut that opens a console window running the server, plus a "WebUI"
shortcut that opens `http://127.0.0.1:8080/`.

**Built-in Swagger / OpenAPI.** The server itself serves interactive API
docs (this is a local patch to `tools/server/`, not upstream — see
"llama.cpp source patch" below):

- `http://127.0.0.1:8080/swagger` — Swagger UI, **"Try it out" works**
  (same-origin, no CORS setup, no extra process)
- `http://127.0.0.1:8080/openapi.json` — the OpenAPI 3.0.3 spec

Assets are vendored and embedded in the binary (offline, like the WebUI).

## Files in this folder

| file | role |
|---|---|
| `build-payload.ps1` | builds `llama-server` + collects all DLLs/models into `payload\` |
| `models.ini` | router preset (chat + embeddings), relative paths |
| `ProvisionLLM_launcher.bat` | starts the server in router mode (Windows analog of `entrypoint.sh`) |
| `Open-WebUI.url` | Start-menu shortcut target for the browser UI |
| `llama-dual.iss` | Inno Setup script — turns `payload\` into `ProvisionLLMEngine-Setup.exe` |
| `models\` | **you create this** — drop the two GGUF files here before building |

## Build machine prerequisites

Only needed on the machine that *builds* the installer (not on end-user PCs):

- Windows 10/11 x64
- Visual Studio 2022 + "Desktop development with C++"
- NVIDIA CUDA Toolkit **>= 12.4** (sets `%CUDA_PATH%`). 12.4 is the floor:
  the MSVC 14.4x STL hard-rejects older CUDA with `static_assert`
  "error STL1002". `winget install --id Nvidia.CUDA --version 12.4 --force`
- Vulkan SDK (sets `%VULKAN_SDK%`).
  `winget install KhronosGroup.VulkanSDK`
- CMake 3.24+ (on `PATH` or bundled with VS — the script auto-finds the
  VS-bundled copy)
- [Inno Setup 6](https://jrsoftware.org/isdl.php)
- An NVIDIA driver on the build box if you want to smoke-test CUDA

End-user PCs need **only a GPU driver**: a recent NVIDIA driver (R550+ for
the CUDA path, since the build uses CUDA 12.4) **or** any current AMD/Intel
driver (provides the Vulkan loader). CUDA Toolkit / VS / Vulkan SDK are
build-only — runtime DLLs are bundled; `nvcuda.dll` / `vulkan-1.dll` come
from the driver.

## Step 1 — get the models

These are bundled **inside** the installer, so download them into
`windows-installer\models\` first:

```powershell
cd windows-installer
mkdir models
# chat (~5.4 GB)
Invoke-WebRequest -OutFile .\models\Qwen2.5-7B-Instruct-Q5_K_M.gguf `
  'https://huggingface.co/bartowski/Qwen2.5-7B-Instruct-GGUF/resolve/main/Qwen2.5-7B-Instruct-Q5_K_M.gguf?download=true'
# embeddings (~1.7 GB)
Invoke-WebRequest -OutFile .\models\gte-Qwen2-1.5B-instruct-Q8_0.gguf `
  'https://huggingface.co/second-state/gte-Qwen2-1.5B-instruct-GGUF/resolve/main/gte-Qwen2-1.5B-instruct-Q8_0.gguf?download=true'
```

If you change the filenames, update the `model = ...` lines in `models.ini`
to match (the section names `qwen2.5-chat` / `gte-qwen2-embed` are what
clients send and should stay).

## Step 2 — build the payload

```powershell
cd windows-installer
pwsh -ExecutionPolicy Bypass -File .\build-payload.ps1
```

What it does (mirrors the working `docker-dual` build flags):

- CMake configure with `GGML_CUDA=ON`, `GGML_BACKEND_DL=ON`,
  `GGML_CPU_ALL_VARIANTS=ON`, `GGML_NATIVE=OFF`,
  `CMAKE_CUDA_ARCHITECTURES=61-real;75-virtual;80-virtual;86-real;89-real`
- Full build (every backend DLL, required by `GGML_BACKEND_DL`)
- Collects into `payload\`: `llama-server.exe`, all `ggml*/llama*.dll`,
  CUDA runtime DLLs (`cudart64_*`, `cublas64_*`, `cublasLt64_*`),
  `models.ini`, `ProvisionLLM_launcher.bat`, `Open-WebUI.url`, and `models\*.gguf`

`sm_61` is in the arch list so the **GTX 1080 Ti** gets native code, and
`GGML_CPU_ALL_VARIANTS` ships sse42/avx/avx2/... CPU backends so the install
runs on **old CPUs without AVX2** (the reason the Linux build kept crashing
with SIGILL until we added this).

Override the GPU set if you want a slimmer build:

```powershell
.\build-payload.ps1 -CudaArch "61-real;86-real" -Clean
```

### Trial build (N requests, then activation)

Pass `-Trial` to produce an evaluation build of `provision-server` with the
license subsystem: it serves a fixed number of **inference** requests, then
refuses with HTTP 403 until the product is activated with a code from the
License Server:

```powershell
.\build-payload.ps1 -Trial -LicenseKeyringJson .\keyring.json   # default: 50 requests
.\build-payload.ps1 -Trial -TrialLimit 100 -LicenseKeyringJson .\keyring.json
```

- `-LicenseKeyringJson` is the trusted Ed25519 keyring
  (`{ "keys": [ { "kid": "...", "pubkey": "<base64url>" } ] }`) that lets the
  build verify the `.lic` the License Server issues. Without it the trial
  still runs, but activation fails (`unknown kid`).
- Product / server defaults: `-LicenseProduct LLM`,
  `-LicenseActivationUrl https://license.scan-dev.com`; both can also be
  overridden at runtime with `PROVISION_LICENSE_PRODUCT` /
  `PROVISION_LICENSE_ACTIVATION_URL`.
- The limit applies to both the web UI and the API — they hit the same
  endpoints (`/chat/completions`, `/completions`, `/v1/responses`,
  `/v1/messages`, `/embeddings`, `/rerank`, transcriptions, `/infill`).
- Health checks (`/health`, `/v1/health`), metadata endpoints (`/props`,
  `/models`, `/metrics`, `/tokenize`, `/detokenize`, web UI assets) and the
  license endpoints (`GET /license/status`, `POST /license/activate`) are
  **not** counted; only a delivered 2xx result burns a slot.
- **Activation**: web UI → Settings → License (or the banner on the chat
  screen) → enter the code; or `POST /license/activate
  {"activationCode": "XXXX-XXXX-XXXX"}`; or set
  `PROVISION_LICENSE_ACTIVATION_CODE` in the environment before the first
  start. The signed `.lic` is stored under `%PROGRAMDATA%\ProvisionLLM` next
  to the HMAC-signed request counter; editing those files or rolling the
  system clock back locks requests out.
- In router mode (the default for this installer), the request is counted once
  in the router process; spawned child instances do not double-count.
- `-Trial` also turns on `LLAMA_BUILD_BORINGSSL` so libcrypto (SHA-256, HMAC,
  Ed25519) and TLS for the activation call are statically bundled — no extra
  DLL to ship and no system OpenSSL required.

The subsystem is compiled in via `-DLLAMA_SERVER_TRIAL=ON`
(`-DLLAMA_SERVER_TRIAL_LIMIT=N`, `-DLLAMA_SERVER_LICENSE_PRODUCT`,
`-DLLAMA_SERVER_LICENSE_ACTIVATION_URL`, `-DLLAMA_SERVER_LICENSE_KEYRING_JSON`;
see `tools/server/CMakeLists.txt`). A normal build (no `-Trial`) is
unrestricted.

## Step 3 — compile the installer

```powershell
& "C:\Users\<you>\AppData\Local\Programs\Inno Setup 6\ISCC.exe" .\llama-dual.iss
# or, for a machine-wide Inno Setup install:
& "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe" .\llama-dual.iss
```

Output goes to `windows-installer\Output\`. Because the bundled models push
the total past Windows' ~4.2 GB single-`.exe` limit, the script enables
**disk spanning** (`DiskSpanning=yes` in `llama-dual.iss`), so you get:

```
Output\
  ProvisionLLMEngine-Setup.exe        <- run this
  ProvisionLLMEngine-Setup-1.bin      <- data slices (~2.1 GB each)
  ProvisionLLMEngine-Setup-2.bin
  ProvisionLLMEngine-Setup-3.bin
  ...
```

**Distribute the whole `Output\` folder together.** All `.bin` slices must
sit next to `ProvisionLLMEngine-Setup.exe`; the `.exe` pulls the bundled models and
DLLs from the slices during install. The `.gguf` files are stored
uncompressed (already quantised) so compile is mostly I/O, not CPU.

## Step 4 — install & use (end-user machine)

End-user machine only needs a **recent NVIDIA driver** — CUDA Toolkit and
Visual Studio are *not* required (runtime DLLs are bundled; `nvcuda.dll`
comes from the driver).

1. Run `ProvisionLLMEngine-Setup.exe` (asks for admin — installs into Program Files).
   The installer warns if no NVIDIA driver is detected.
2. Start menu → **Provision LLM Engine** → a console window opens, server boots.
   First model load takes ~10–30 s on a 1080 Ti.
3. Start menu → **Provision LLM Engine WebUI** (or browse to
   `http://127.0.0.1:8080/`). Pick `qwen2.5-chat` in the model selector.

### API examples

```powershell
# chat
curl -s http://localhost:8080/v1/chat/completions `
  -H "Content-Type: application/json" `
  -d '{ \"model\": \"qwen2.5-chat\", \"messages\": [ { \"role\": \"user\", \"content\": \"hi\" } ] }'

# embeddings
curl -s http://localhost:8080/v1/embeddings `
  -H "Content-Type: application/json" `
  -d '{ \"model\": \"gte-qwen2-embed\", \"input\": \"the quick brown fox\" }'
```

## Knobs (edit `ProvisionLLM_launcher.bat`, then restart)

| var | default | effect |
|---|---|---|
| `PORT` | `8080` | HTTP port for API + WebUI |
| `HOST` | `127.0.0.1` | `0.0.0.0` exposes on LAN (also open Windows Firewall for `llama-server.exe`) |
| `MODELS_MAX` | `2` | `2` = both models resident (~9 GB VRAM, fits 1080 Ti); `1` = swap between requests |

After install `ProvisionLLM_launcher.bat` lives in the install folder
(`%ProgramFiles%\Provision LLM Engine\ProvisionLLM_launcher.bat`); edit it there.

## Notes / gotchas

- **`flash-attn = on`** is kept in `models.ini`: on Pascal it uses the
  generic tile kernel (works, saves VRAM, no tensor-core speedup).
- **The `default` model** the router lists is a phantom entry with no GGUF —
  don't request it, it hangs. Use `qwen2.5-chat` / `gte-qwen2-embed`.
- **CUDA version**: the build uses whatever CUDA Toolkit is installed on the
  build machine. Unlike the Linux image we did not pin 12.2 — pin it
  yourself (install the matching Toolkit) if a target PC has an old driver.
  Driver floor: CUDA 12.2→R535, 12.4→R550, 12.6→R560, 12.8→R570.
- Embeddings are not exposed in the WebUI (chat-only) — use the API.
- Uninstall via Start menu → *Uninstall Provision LLM Engine* or Apps & features;
  it removes the models too.

## llama.cpp source patch (branding + Swagger)

Unlike a pure wrapper, the rebrand and the built-in Swagger endpoint
**modify llama.cpp source** (new HTTP routes + WebUI source). This is a
**local patch**: re-apply it after every `git pull` of llama.cpp; per
llama.cpp's AI policy it is **not** submitted upstream.

Swagger / favicon assets are kept in the WebUI's `static/` dir so they are
reproducible build inputs (`npm run build` copies `static/` → `public/`;
the C++ build then xxd-embeds them). `tools/server/public/` is pure build
output — do not hand-edit it.

Touched files:

| file | change |
|---|---|
| `tools/server/webui/static/swagger.html` | new — Swagger UI bootstrap |
| `tools/server/webui/static/openapi.json` | new — OpenAPI 3.0.3 spec |
| `tools/server/webui/static/swagger-ui.css` / `swagger-ui-bundle.js` | new — vendored offline (jsdelivr) |
| `tools/server/webui/static/favicon.svg` | replaced with the brand logo |
| `tools/server/webui/scripts/post-build.sh` | stop deleting `favicon.svg` from the bundle |
| `tools/server/webui/src/lib/constants/ui.ts` | `APP_NAME` default → `Provision LLM Engine` |
| `tools/server/webui/src/.../SidebarNavigation.svelte` | inline brand logo SVG in the header |
| `tools/server/webui/src/.../ServerLoadingSplash.svelte`, `chat/[id]/+page.svelte` | "llama.cpp" → "Provision LLM Engine" |
| `tools/server/CMakeLists.txt` | +5 entries in `PUBLIC_ASSETS` (favicon + 4 swagger) |
| `tools/server/server-http.cpp` | +5 `#include`, +5 `GET` routes (`/favicon.svg`, `/swagger`, `/swagger-ui.css`, `/swagger-ui-bundle.js`, `/openapi.json`) under `LLAMA_BUILD_WEBUI` |

Build order matters: **`npm run build` in `tools/server/webui/` first**
(regenerates `public/` incl. the rebranded bundle + assets), **then** the
C++ build embeds them.

To regenerate the vendored Swagger UI assets:

```powershell
cd tools/server/webui/static
curl.exe -sL -o swagger-ui.css        https://cdn.jsdelivr.net/npm/swagger-ui-dist/swagger-ui.css
curl.exe -sL -o swagger-ui-bundle.js  https://cdn.jsdelivr.net/npm/swagger-ui-dist/swagger-ui-bundle.js
```

The spec is hand-maintained for the endpoints Provision LLM Engine actually
exposes; extend `openapi.json` if you add routes. The brand logo lives in
`logo.svg` (repo root) and `windows-installer/favicon.svg`; the launcher
shortcut icon `favicon.ico` is generated from the latter by
`windows-installer/tools/svg2ico/convert.js`.
