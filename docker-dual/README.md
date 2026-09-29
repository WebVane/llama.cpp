# llama.cpp dual-model image (chat + embeddings)

A small Dockerfile + docker-compose that builds `llama-server` from this
checkout and runs it in **router mode**, serving two models on a single
endpoint:

| Model id (used as `"model": ...` in requests) | Purpose    | Default GGUF                                    |
|-----------------------------------------------|------------|-------------------------------------------------|
| `qwen2.5-chat`                                | chat       | `Qwen2.5-7B-Instruct-Q5_K_M.gguf` (~5.4 GB)     |
| `gte-qwen2-embed`                             | embeddings | `gte-Qwen2-1.5B-instruct-Q8_0.gguf` (~1.7 GB)   |

Models are **not** baked into the image &mdash; the runtime expects them in
`/models/` (volume mount). The router uses
[`--models-preset`](../tools/server/README.md#using-multiple-models) plus the
`models.ini` shipped in this directory.

## Files

```
docker-dual/
├── Dockerfile          # multi-stage CUDA build, target: llama-server
├── entrypoint.sh       # launches llama-server in router mode
├── models.ini          # router preset (chat + embeddings, per-model GPU)
├── docker-compose.yml  # one-shot up
└── README.md
```

## 1. Requirements

- NVIDIA GPU (recent drivers). The shipped `models.ini` assumes **two** GPUs
  (chat on `CUDA1`, embeddings on `CUDA0`); see section 6 for single-GPU hosts.
- [NVIDIA Container Toolkit](https://docs.nvidia.com/datacenter/cloud-native/container-toolkit/install-guide.html)
- Docker 24+ with `compose` plugin
- VRAM:
  - `MODELS_MAX=1` (default) &mdash; ≈ 8 GB; models are swapped between
    requests, the first request after a switch is 1&ndash;5 s slower while
    the new model loads.
  - `MODELS_MAX=2` &mdash; ≈ 12 GB; both stay resident.
  - two GPUs &mdash; put each model on its own card with `device = CUDAn`
    (see section 6); then only the per-model size matters.

## 2. Download the models

From the repo root (so the files land in `./models/` next to `docker-dual/`):

```bash
mkdir -p models

# chat
huggingface-cli download bartowski/Qwen2.5-7B-Instruct-GGUF \
    Qwen2.5-7B-Instruct-Q5_K_M.gguf --local-dir ./models

# embeddings
huggingface-cli download second-state/gte-Qwen2-1.5B-instruct-GGUF \
    gte-Qwen2-1.5B-instruct-Q8_0.gguf --local-dir ./models
```

If you prefer different files, update the `model = ...` paths in `models.ini`
before building.

## 3. Build

The build context is the **repository root** (we need `COPY . /src`). The
`Dockerfile` lives in this subdirectory.

### a) docker compose (recommended)

```bash
cd docker-dual
docker compose build
docker compose up -d
```

### b) plain docker

```bash
# from the repository root
docker build -f docker-dual/Dockerfile -t llama-dual .
docker run -d --name llama-dual --gpus all \
    -p 8080:8080 \
    -v "$(pwd)/models:/models:ro" \
    llama-dual
```

To target specific GPU compute capabilities instead of `all-major`:

```bash
docker build -f docker-dual/Dockerfile \
    --build-arg CUDA_ARCH="86;89" \
    -t llama-dual .
```

### c) trial build (N requests, then activation; self-contained)

Pass `--build-arg TRIAL=1` to compile the license subsystem (shared with
`provision_ocr_server`, see `src/license/README.md`) into the image with the
trial fallback: the server serves `TRIAL_LIMIT` **inference** requests (web
UI and API alike), then refuses with HTTP 403 until the product is activated
with a code from the License Server (product `LLM`).

The trial image is **self-contained** — the two GGUF weights are downloaded
from HuggingFace at build time and baked into `/models`, so no model mount
is needed. The trusted keyring is passed as a BuildKit secret so the image
can verify the `.lic` it receives on activation:

```bash
DOCKER_BUILDKIT=1 docker build -f docker-dual/Dockerfile \
    --build-arg TRIAL=1 \
    --build-arg TRIAL_LIMIT=50 \
    --secret id=license_keyring,src=./keyring.json \
    -t llama-dual:trial .

# Run it — note: do NOT mount /models, the weights are already inside.
# The license state volume and a STABLE hostname are required: online
# licenses are bound to (machine-id, hostname), a random container hostname
# would invalidate the license on every restart.
docker run -d --name llama-dual-trial --gpus all \
    --hostname llama-dual \
    -p 8080:8080 \
    -v llama-license-state:/var/lib/provision-llm \
    llama-dual:trial
```

- **Activation**: web UI → Settings → License (or the banner on the chat
  screen) → enter the code; or
  `curl -X POST localhost:8080/license/activate -d '{"activationCode":"XXXX-XXXX-XXXX"}'`;
  or start the container with `-e PROVISION_LICENSE_ACTIVATION_CODE=...`
  (idempotent — the persisted `.lic` wins on later boots). `GET /license/status`
  shows mode / used / limit without consuming anything.
- Counted: `/chat/completions`, `/completions`, `/v1/responses`,
  `/v1/messages`, `/embeddings`, `/rerank`, transcriptions, `/infill` — and
  only a delivered 2xx result; a 400/503 never burns a slot.
- **Not** counted: `/health`, `/props`, `/models`, `/metrics`,
  `/tokenize`, `/detokenize`, `/license/*`, web UI assets. The Docker
  `HEALTHCHECK` therefore never consumes the trial.
- License state — the signed `.lic`, the HMAC-signed request counter
  (tamper- and clock-rollback-resistant) and the persisted machine-id —
  lives in `/var/lib/provision-llm` (`PROVISION_LICENSE_STATE_DIR`); the
  `-v llama-license-state:...` mount persists it across container
  recreation. Deleting the volume restarts the trial budget but also drops
  the license, so a customer would have to activate again (and each
  activation of a new machine identity consumes one of the license's server
  slots).
- Build args: `LICENSE_PRODUCT` (default `LLM`), `LICENSE_ACTIVATION_URL`
  (default `https://license.scan-dev.com`); runtime overrides:
  `PROVISION_LICENSE_PRODUCT`, `PROVISION_LICENSE_ACTIVATION_URL`,
  `PROVISION_LICENSE_CPU_ID_OVERRIDE` / `PROVISION_LICENSE_HOSTNAME_OVERRIDE`
  (pin the machine identity explicitly).
- In router mode (the default) the request is counted once in the router
  process; the spawned child instances do not double-count.
- The weights come from `bartowski/Qwen2.5-7B-Instruct-GGUF` and
  `second-state/gte-Qwen2-1.5B-instruct-GGUF`. Override
  `--build-arg TRIAL_CHAT_MODEL_URL=...` / `TRIAL_EMBED_MODEL_URL=...` to pull
  from an internal mirror instead (the build needs network access to the
  source). The baked weights add ~7 GB to the image.

In CI the trial image is built by the `build_llama_dual_trial` job and
pushed as a single rolling `:trial` tag (only the latest version is kept;
the previous `:trial` image is reused as the build cache). The runner needs
network egress to the weight source.

## 4. Try it

```bash
# chat
curl -sS http://localhost:8080/v1/chat/completions \
    -H 'Content-Type: application/json' \
    -d '{
        "model": "qwen2.5-chat",
        "messages": [
            {"role": "user", "content": "Say hi in one word."}
        ]
    }' | jq .

# embeddings
curl -sS http://localhost:8080/v1/embeddings \
    -H 'Content-Type: application/json' \
    -d '{
        "model": "gte-qwen2-embed",
        "input": "the quick brown fox"
    }' | jq '.data[0].embedding | length'

# list loaded / available models
curl -sS http://localhost:8080/v1/models | jq .
```

## 5. Knobs

All are environment variables on the container (override via
`docker run -e` or the `environment:` block in `docker-compose.yml`).

| Env             | Default              | Effect                                            |
|-----------------|----------------------|---------------------------------------------------|
| `PORT`          | `8080`               | HTTP port (inside the container)                  |
| `MODELS_MAX`    | `1`                  | Max simultaneously-loaded models. `2` keeps both. |
| `MODELS_CONFIG` | `/config/models.ini` | Path to the router preset file                    |
| `MODEL_DEVICES` | *(empty)*            | Per-model GPU override, `<section>=<dev>,...` (section 6) |
| `CUDA_DEVICE_ORDER` | `PCI_BUS_ID`     | Makes `CUDAn` follow `nvidia-smi` order; leave as is |
| `CUDA_ARCH`     | `all-major` *(build-arg)* | `CMAKE_CUDA_ARCHITECTURES`                   |

Switching from `--models-max 1` to `2` keeps both the chat and the embedding
model resident, eliminating swap latency on the first request after a model
change at the cost of extra VRAM (~12 GB total here):

```bash
# compose
MODELS_MAX=2 docker compose up -d

# plain docker
docker run -d --name llama-dual --gpus all \
    -p 8080:8080 \
    -v "$(pwd)/models:/models:ro" \
    -e MODELS_MAX=2 \
    llama-dual
```

You can also mount a custom `models.ini` over the baked-in one without
rebuilding the image:

```bash
docker run -d --name llama-dual --gpus all \
    -p 8080:8080 \
    -v "$(pwd)/models:/models:ro" \
    -v "$(pwd)/my-models.ini:/config/models.ini:ro" \
    llama-dual
```

## 6. Multi-GPU: which model runs on which GPU

In router mode every model is a separate `llama-server` child process, so
each one can be pinned to its own GPU. Placement is decided in two steps:

1. **Which host GPUs the container sees** &mdash; `device_ids` in
   `docker-compose.yml` (or `--gpus '"device=0,1"'` with plain `docker run`).
   Inside the container the exposed GPUs are renumbered `CUDA0`, `CUDA1`, ...
   in PCI-bus order (the image sets `CUDA_DEVICE_ORDER=PCI_BUS_ID`, so the
   order matches `nvidia-smi -L` among the exposed GPUs). With
   `device_ids: ["1"]` only host GPU 1 is visible and it becomes `CUDA0`.

2. **Which of those each model uses** &mdash; either per section in
   `models.ini`:

   ```ini
   [qwen2.5-chat]
   model  = /models/Qwen2.5-7B-Instruct-Q5_K_M.gguf
   device = CUDA1

   [gte-qwen2-embed]
   model  = /models/gte-Qwen2-1.5B-instruct-Q8_0.gguf
   device = CUDA0
   ```

   or from `docker-compose.yml` / the shell via `MODEL_DEVICES`, which is
   applied by `entrypoint.sh` on top of the preset and wins over the file:

   ```bash
   MODEL_DEVICES="qwen2.5-chat=CUDA1,gte-qwen2-embed=CUDA0" docker compose up -d
   ```

   Accepted values are whatever `--device` accepts: `CUDA0`, `CUDA1`,
   `none` (CPU only), or several GPUs to split one model
   (`CUDA0,CUDA1` in the ini; in `MODEL_DEVICES` use a semicolon inside the
   value, `big=CUDA0;CUDA1`, because the comma separates entries).
   `*=CUDA0` applies to every section (an entry naming a section still wins),
   e.g. to run everything on one GPU.

Notes:

- `MODELS_MAX` must be `2` (the compose default) for both models to be
  resident at the same time; with `1` they still swap even on two GPUs.
- A child process only touches the GPUs listed in its `device`, so a GPU
  that no model is assigned to stays untouched even if it is exposed.
- List what the container actually sees:

  ```bash
  docker compose run --rm llama-dual --list-devices
  ```

- `--main-gpu` / `--split-mode` / `--tensor-split` (`main-gpu`, `split-mode`,
  `tensor-split` keys) work per section as well for finer multi-GPU splits.
- Single GPU: the shipped preset pins chat to `CUDA1`, which does not exist on a
  one-GPU host (the child fails with `invalid device: CUDA1`). Start with
  `MODEL_DEVICES="*=CUDA0"` or edit the `device` keys. The TRIAL image drops
  the pins at build time, so it runs on any GPU count.
- `MODEL_DEVICES` is applied once at startup by writing a patched copy to
  `/tmp/models.ini`; the router reads that copy, so `GET /v1/models?reload=1`
  will not see later edits to the mounted `models.ini` -- restart the container
  (or leave `MODEL_DEVICES` empty and keep `device` in the file).
- The Web UI pre-selects the model tagged `default` (`tags = chat, default`
  here). If the server runs with `--no-models-autoload`, that model must also
  have `load-on-startup = true`, otherwise the first message fails with
  `model is not loaded`.

## 7. Anatomy of `models.ini`

```ini
version = 1

[*]                       ; defaults inherited by every model
ctx-size     = 8192
n-gpu-layers = 99

[qwen2.5-chat]            ; section name == value of "model" in requests
model    = /models/Qwen2.5-7B-Instruct-Q5_K_M.gguf
jinja    = true
device   = CUDA1          ; GPU for this model (CUDA0, CUDA1, none, ...)
load-on-startup = true    ; preload at startup (first request is instant)
tags     = chat, default  ; "default" = pre-selected in the Web UI

[gte-qwen2-embed]
model      = /models/gte-Qwen2-1.5B-instruct-Q8_0.gguf
embeddings = true
pooling    = last
device     = CUDA0        ; embeddings live on the other GPU
```

Keys are long CLI argument names without `--` (short forms like `c` /
`ngl` and `LLAMA_ARG_*` env-var names also work). See
[`tools/server/README.md`](../tools/server/README.md#model-presets) for the
full preset reference, including the preset-only `load-on-startup` and
`stop-timeout` keys.

## 8. Troubleshooting

- **`models preset not found at /config/models.ini`** &mdash; the file was not
  baked in or the bind mount points at a missing path. Rebuild or check the
  `-v` you passed.
- **`HEALTHCHECK` keeps failing for several minutes** &mdash; that's expected
  on the first launch with `MODELS_MAX=2`; both models are being loaded.
  Either wait, or check progress with `docker logs -f llama-dual`.
- **`CUDA error: no kernel image is available...`** &mdash; the image was
  built for archs your GPU doesn't match. Rebuild with
  `--build-arg CUDA_ARCH="<your sm>"` (e.g. `86` for RTX 30xx, `89` for
  40xx, `90` for H100).
- **`invalid device: CUDA1`** when a model loads &mdash; the container sees
  fewer GPUs than `models.ini` / `MODEL_DEVICES` refer to. Check
  `device_ids` in `docker-compose.yml` (or `--gpus`) and compare with
  `docker compose run --rm llama-dual --list-devices`; remember the
  exposed GPUs are renumbered from `CUDA0` inside the container.
  On a single-GPU host start with `MODEL_DEVICES="*=CUDA0"`.
- **Both models end up on the same GPU** &mdash; `MODEL_DEVICES` overrides
  `models.ini`; make sure the shell/compose variable is either unset or
  lists both sections. Also `MODELS_MAX=1` swaps models regardless of
  how many GPUs there are.
