<#
.SYNOPSIS
    Builds a universal llama-server (CUDA + Vulkan + CPU) from this checkout
    and assembles the Inno Setup payload: exe + all runtime DLLs + models.ini
    + launcher + models.

.DESCRIPTION
    Windows port of docker-dual's build stage, made universal:
      GGML_CUDA            -> NVIDIA GPU offload (used on NVIDIA cards)
      GGML_VULKAN          -> cross-vendor GPU offload (used on AMD/Intel,
                              also works on NVIDIA as fallback)
      GGML_BACKEND_DL      -> backends are DLLs, selected at runtime: the
                              installed binary runs CUDA on NVIDIA and
                              Vulkan on AMD from one build
      GGML_CPU_ALL_VARIANTS-> ships sse42/avx/avx2/... CPU backends so the
                              install runs on old CPUs (no AVX2 required)
      CMAKE_CUDA_ARCHITECTURES includes sm_61 for the GTX 1080 Ti.

    After this succeeds, compile windows-installer\llama-dual.iss with Inno
    Setup (ISCC.exe) to produce the distributable installer.

.PREREQUISITES
    - Visual Studio 2022 with "Desktop development with C++"
    - NVIDIA CUDA Toolkit >= 12.4 (sets %CUDA_PATH%); 12.4 is the floor
      because MSVC 14.4x STL hard-rejects older CUDA (error STL1002)
    - Vulkan SDK (sets %VULKAN_SDK%; provides headers + glslc shader
      compiler).  The Vulkan *loader* on target machines comes from the
      GPU driver -- nothing extra to bundle.
    - CMake 3.24+ (on PATH or bundled with Visual Studio)
    - GGUF models placed in windows-installer\models\ (see README.md)

.EXAMPLE
    pwsh -ExecutionPolicy Bypass -File .\build-payload.ps1

.EXAMPLE
    .\build-payload.ps1 -CudaArch "61-real;86-real" -Clean
#>
[CmdletBinding()]
param(
    # Semicolon list passed to CMAKE_CUDA_ARCHITECTURES.  Default matches
    # docker-dual: Pascal (1080 Ti) + Turing..Ada.  No Blackwell (needs 12.8).
    [string] $CudaArch = "61-real;75-virtual;80-virtual;86-real;89-real",

    [string] $Config   = "Release",

    # Wipe the build dir first (use after changing CUDA toolkit / flags).
    [switch] $Clean,

    # Build provision-server with the license subsystem and the trial
    # fallback: the server serves at most -TrialLimit inference requests
    # (web UI and API alike), then refuses with HTTP 403 until the product
    # is activated (Settings -> License in the web UI, or POST
    # /license/activate).  Health checks and metadata endpoints are exempt.
    # License state (.lic, signed request counter) lives under
    # %PROGRAMDATA%\ProvisionLLM.
    [switch] $Trial,

    # Number of inference requests the trial allows before activation is required.
    [int] $TrialLimit = 50,

    # Trusted Ed25519 keyring JSON baked into the trial build so it can
    # verify the .lic the License Server issues.  Without it the trial
    # works but activation fails ("unknown kid").
    [string] $LicenseKeyringJson = "",

    # Product name a .lic must carry (as registered on the License Server).
    [string] $LicenseProduct = "LLM",

    # License Server base URL used for online activation.
    [string] $LicenseActivationUrl = "https://license.scan-dev.com"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$ScriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot   = Resolve-Path (Join-Path $ScriptDir "..")
$BuildDir   = Join-Path $RepoRoot "build-win-cuda"
$PayloadDir = Join-Path $ScriptDir "payload"
$ModelsSrc  = Join-Path $ScriptDir "models"

function Write-Step($msg) { Write-Host "`n=== $msg ===" -ForegroundColor Cyan }
function Fail($msg) { Write-Host "ERROR: $msg" -ForegroundColor Red; exit 1 }

# --- sanity checks ---------------------------------------------------------
Write-Step "Checking prerequisites"

# cmake: prefer one on PATH, else fall back to the copy bundled with
# Visual Studio (VS ships cmake under Common7\IDE\...\CMake\CMake\bin).
$CMake = $null
$onPath = Get-Command cmake -ErrorAction SilentlyContinue
if ($onPath) {
    $CMake = $onPath.Source
} else {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vsRoot = & $vswhere -latest -property installationPath
        $cand = Join-Path $vsRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
        if (Test-Path $cand) { $CMake = $cand }
    }
}
if (-not $CMake) {
    Fail "cmake not found on PATH nor bundled with Visual Studio. Install CMake 3.24+."
}
Write-Host "  cmake     = $CMake"

# node/npm: the web UI (tools/ui) is built from source during CMake
# (LLAMA_BUILD_UI=ON below) -- the fork UI is customized, so the upstream
# prebuilt bucket cannot be used. Vite needs Node >= 20.
$NodeCmd = Get-Command node -ErrorAction SilentlyContinue
if (-not $NodeCmd) {
    Fail "Node.js not found on PATH. Install Node.js 22 LTS (the web UI is built from source)."
}
Write-Host "  node      = $($NodeCmd.Source) ($(& node --version))"

$cudaPath = $env:CUDA_PATH
if ([string]::IsNullOrWhiteSpace($cudaPath) -or -not (Test-Path $cudaPath)) {
    Fail "CUDA Toolkit not found (CUDA_PATH unset). Install the NVIDIA CUDA Toolkit >= 12.4."
}
Write-Host "  CUDA_PATH   = $cudaPath"
Write-Host "  CUDA_ARCH   = $CudaArch"

# Vulkan SDK: needed to compile GGML_VULKAN (headers + glslc). The machine
# env var is set by the installer but a pre-existing shell won't see it,
# so fall back to probing C:\VulkanSDK\<ver> if VULKAN_SDK is unset here.
$vkSdk = $env:VULKAN_SDK
if ([string]::IsNullOrWhiteSpace($vkSdk) -or -not (Test-Path $vkSdk)) {
    $vkSdk = [Environment]::GetEnvironmentVariable('VULKAN_SDK','Machine')
}
if ([string]::IsNullOrWhiteSpace($vkSdk) -or -not (Test-Path $vkSdk)) {
    $latest = Get-ChildItem 'C:\VulkanSDK' -Directory -ErrorAction SilentlyContinue |
              Sort-Object Name -Descending | Select-Object -First 1
    if ($latest) { $vkSdk = $latest.FullName }
}
if ([string]::IsNullOrWhiteSpace($vkSdk) -or -not (Test-Path "$vkSdk\Bin\glslc.exe")) {
    Fail "Vulkan SDK not found (VULKAN_SDK unset / no glslc). Install it: winget install KhronosGroup.VulkanSDK"
}
$env:VULKAN_SDK = $vkSdk
$env:Path = "$vkSdk\Bin;" + $env:Path
Write-Host "  VULKAN_SDK  = $vkSdk"

# --- configure + build -----------------------------------------------------
if ($Clean -and (Test-Path $BuildDir)) {
    Write-Step "Cleaning $BuildDir"
    Remove-Item -Recurse -Force $BuildDir
}

# Trial build: license subsystem + trial fallback, and bundle BoringSSL so
# the libcrypto it needs (SHA-256 / HMAC / Ed25519) and TLS for the
# https:// activation URL are always present (statically linked -- no extra
# DLL to ship) even on machines without OpenSSL.
$TrialArgs = @()
if ($Trial) {
    Write-Host "  TRIAL build: $TrialLimit requests, then activation (product $LicenseProduct)" -ForegroundColor Yellow
    $TrialArgs = @(
        "-DLLAMA_SERVER_TRIAL=ON",
        "-DLLAMA_SERVER_TRIAL_LIMIT=$TrialLimit",
        "-DLLAMA_SERVER_LICENSE_PRODUCT=$LicenseProduct",
        "-DLLAMA_SERVER_LICENSE_ACTIVATION_URL=$LicenseActivationUrl",
        "-DLLAMA_BUILD_BORINGSSL=ON"
    )
    if ($LicenseKeyringJson) {
        if (-not (Test-Path $LicenseKeyringJson)) { Fail "License keyring not found: $LicenseKeyringJson" }
        $TrialArgs += "-DLLAMA_SERVER_LICENSE_KEYRING_JSON=$((Resolve-Path $LicenseKeyringJson).Path)"
        Write-Host "  license keyring = $LicenseKeyringJson"
    } else {
        Write-Host "  WARNING: no -LicenseKeyringJson - the build cannot verify any .lic (activation will fail)" -ForegroundColor Yellow
    }
}

Write-Step "Configuring (CMake)"
& $CMake -B "$BuildDir" -S "$RepoRoot" `
    -DCMAKE_BUILD_TYPE="$Config" `
    -DGGML_NATIVE=OFF `
    -DGGML_CUDA=ON `
    -DGGML_VULKAN=ON `
    -DGGML_BACKEND_DL=ON `
    -DGGML_CPU_ALL_VARIANTS=ON `
    -DCMAKE_CUDA_ARCHITECTURES="$CudaArch" `
    -DLLAMA_CURL=OFF `
    -DLLAMA_BUILD_TESTS=OFF `
    -DLLAMA_BUILD_EXAMPLES=OFF `
    -DLLAMA_BUILD_UI=ON `
    -DLLAMA_USE_PREBUILT_UI=OFF `
    @TrialArgs
if ($LASTEXITCODE -ne 0) { Fail "CMake configure failed." }

# Full build (not --target llama-server): GGML_BACKEND_DL needs every
# backend DLL built, exactly like the docker-dual image.
Write-Step "Building (this takes a while -- CUDA kernels)"
& $CMake --build "$BuildDir" --config $Config -j
if ($LASTEXITCODE -ne 0) { Fail "Build failed." }

# --- locate build output ---------------------------------------------------
# VS multi-config -> build\bin\Release ; Ninja -> build\bin
Write-Step "Collecting payload"
$exe = Get-ChildItem -Path $BuildDir -Recurse -Filter "provision-server.exe" |
       Select-Object -First 1
if (-not $exe) { Fail "provision-server.exe not found under $BuildDir after build." }
$binDir = $exe.DirectoryName
Write-Host "  build output: $binDir"

if (Test-Path $PayloadDir) { Remove-Item -Recurse -Force $PayloadDir }
New-Item -ItemType Directory -Path $PayloadDir | Out-Null
New-Item -ItemType Directory -Path (Join-Path $PayloadDir "models") | Out-Null

# 1. provision-server.exe + everything ggml/provision built next to it (*.dll)
Copy-Item $exe.FullName $PayloadDir
Get-ChildItem -Path $binDir -Filter *.dll | ForEach-Object {
    Copy-Item $_.FullName $PayloadDir
}

# 2. CUDA runtime DLLs (the driver provides nvcuda.dll -- do NOT ship that).
$cudaBin = Join-Path $cudaPath "bin"
$cudaDlls = @("cudart64_*.dll", "cublas64_*.dll", "cublasLt64_*.dll")
foreach ($pat in $cudaDlls) {
    $hit = Get-ChildItem -Path $cudaBin -Filter $pat -ErrorAction SilentlyContinue
    if (-not $hit) { Write-Host "  WARN: no $pat in $cudaBin" -ForegroundColor Yellow }
    $hit | ForEach-Object { Copy-Item $_.FullName $PayloadDir }
}

# 3. config + launcher + webui shortcut + brand icon (shortcut icon)
Copy-Item (Join-Path $ScriptDir "models.ini")    $PayloadDir
Copy-Item (Join-Path $ScriptDir "ProvisionLLM_launcher.bat")  $PayloadDir
Copy-Item (Join-Path $ScriptDir "Open-WebUI.url") $PayloadDir
$icoSrc = Join-Path $ScriptDir "favicon.ico"
if (Test-Path $icoSrc) {
    Copy-Item $icoSrc $PayloadDir
} else {
    Write-Host "  WARN: favicon.ico missing -- shortcut icon will fall back" -ForegroundColor Yellow
}

# 4. models (bundled inside the install). Optional at build time -- if absent
#    the installer just won't contain them; warn loudly.
$expected = @(
    "Qwen2.5-7B-Instruct-Q5_K_M.gguf",
    "gte-Qwen2-1.5B-instruct-Q8_0.gguf"
)
$missing = @()
foreach ($m in $expected) {
    $src = Join-Path $ModelsSrc $m
    if (Test-Path $src) {
        Write-Host "  bundling model: $m"
        Copy-Item $src (Join-Path $PayloadDir "models")
    } else {
        $missing += $m
    }
}

# --- summary ---------------------------------------------------------------
Write-Step "Done"
$size = (Get-ChildItem -Recurse $PayloadDir | Measure-Object Length -Sum).Sum
Write-Host ("  payload: {0}  ({1:N1} GB)" -f $PayloadDir, ($size / 1GB))
if ($missing.Count -gt 0) {
    Write-Host "  WARNING: models NOT bundled (missing in $ModelsSrc):" -ForegroundColor Yellow
    $missing | ForEach-Object { Write-Host "    - $_" -ForegroundColor Yellow }
    Write-Host "  See README.md for download links, drop them in, rerun." -ForegroundColor Yellow
}
Write-Host "`nNext: compile the installer with Inno Setup:" -ForegroundColor Green
Write-Host '  & "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe" .\llama-dual.iss' -ForegroundColor Green
