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
├── docker-compose.yml  # one-shot up: server + nginx front door
├── nginx/
│   └── default.conf.template  # nginx config: transparent proxy, /admin allow-list
└── README.md
```

## 1. Requirements

- NVIDIA GPU (recent drivers). The shipped `models.ini` assumes **two** GPUs
  (chat on `CUDA1`, embeddings on `CUDA0`); see section 6 for single-GPU hosts.
  The image is built for NVIDIA (CUDA) only. For AMD or Intel GPUs, or a
  server without Docker, build `provision-server` from source and run it on
  the host with the files in [`deploy/linux/`](../deploy/linux/), see
  [`docs/provision-admin.md`](../docs/provision-admin.md#13-deployment-on-a-linux-host).
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
`Dockerfile` lives in this subdirectory. The repository's `.dockerignore` keeps
`docker-dual/models/`, `docker-dual/models-rw/` and every `.provision-upload/`
folder out of the build context, so models stored next to the compose file
(downloaded, uploaded or dropped in) are not sent to `docker build`; `.gitignore`
keeps `docker-dual/models-rw/` and `.provision-upload/` out of git as well.

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
# models-rw: where models added at run time (POST /models downloads) are kept.
# Without this mount they stay inside the container and the log warns that a
# recreate loses them (section 8).
mkdir -p models-rw/hf models-rw/local
docker run -d --name llama-dual --gpus all \
    -p 8080:8080 \
    -v "$(pwd)/models:/models:ro" \
    -v "$(pwd)/models-rw:/models-rw" \
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

In CI the licensed trial image is built by the `build_llama_dual_release`
job and pushed as a single rolling `:release` tag (only the latest version
is kept; the previous `:release` image is reused as the build cache). It
bakes the public k3 key from `src/license/docs/vectors/keyring.json`, which
matches the test License Server (`license.scan-dev.com`); a different
License Server needs its own public key there. The unlicensed image comes
from `build_llama_dual` as `:dev`. The runner needs network egress to the
weight source.

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
| `MODELS_MAX`    | `1` (compose: `3`)   | Max simultaneously-loaded models. `2` keeps both. |
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

- `MODELS_MAX` must be at least `2` (the compose default is `3`, see
  section 8) for both models to be resident at the same time; with `1` they
  still swap even on two GPUs.
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
  (or leave `MODEL_DEVICES` empty and keep `device` in the file). A
  `models.ini` saved with Windows line endings (CRLF) is patched as well.
- The admin panel (section 8) can set the GPU of every model without editing
  `models.ini` (Admin -> Models -> Settings). `MODEL_DEVICES` wins over it and
  the panel then shows that model's GPU as locked. The panel tells GPUs apart
  by PCI bus id, so it notices when `CUDA0` is another card after a change of
  `device_ids`.
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
offline      = true       ; model processes never go to the network

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
load-on-startup = true    ; preloaded too (needs MODELS_MAX >= 2)
```

Keys are long CLI argument names without `--` (short forms like `c` /
`ngl` and `LLAMA_ARG_*` env-var names also work). See
[`tools/server/README.md`](../tools/server/README.md#model-presets) for the
full preset reference, including the preset-only `load-on-startup` and
`stop-timeout` keys.

Two things to know about the shipped file:

- `offline = true` in `[*]` makes every model process load from the files on
  disk only. It does not affect downloads started with `POST /models` (the
  download worker gets no preset) or models given by a local path. A section
  that should fetch its own `hf-repo` on first load needs `offline = false`.
- Both models have `load-on-startup = true`. The router refuses to start when
  more models are preloaded than `MODELS_MAX` allows, so with a lower limit
  (the image default is `1`) `entrypoint.sh` keeps the key for the first
  sections only, logs a warning and starts the server from a patched copy
  (`/tmp/models.startup.ini`): the chat model is preloaded, the embedding
  model loads on first use.

## 8. Admin panel and the nginx front door (docker compose)

Full description, API reference and upgrade checklist:
[`docs/provision-admin.md`](../docs/provision-admin.md). The short version:

- `docker-compose.yml` now starts two containers. **nginx** is the only one
  with a published port (`${PORT:-8096}`); it forwards every request to
  `llama-dual` unchanged (streaming included). `llama-dual` itself is no
  longer published. Copy `nginx/default.conf.template` along with the compose
  file: without it `docker compose up` stops with "bind source path does not
  exist".
- The compose file enables the admin layer of the server (`PROVISION_ADMIN=1`):
  an `/admin` API and an **Admin** page in the web UI for the model limit, the
  GPU for newly added models, pinned models, storage and a download check,
  per-model settings (GPU, context size, parallel requests, GPU layers, flash
  attention, KV cache precision, micro-batch, role) with a fit check done by
  llama.cpp itself, GPU memory cards, the reason and output tail of a model
  that failed to start, a diagnostics panel, the upload of GGUF files from the
  browser and the deletion of models in `./models-rw/local`. Loading,
  unloading, downloading and deleting downloaded models keep using the public
  model routes, exactly as before.
- The live GPU memory comes from `nvidia-smi`, which the NVIDIA Container
  Toolkit mounts into the container because the compose file sets
  `NVIDIA_DRIVER_CAPABILITIES: "compute,utility"`. Keep `utility` in it.
- `/admin` is closed for every address until you allow some. The value is a
  list of nginx directives, each ending with a semicolon:

  ```bash
  # check first: a typo stops nginx, and with it the whole API
  ADMIN_ALLOW_RULES="allow 10.64.0.11;" docker compose run --rm --no-deps nginx nginx -t
  ADMIN_ALLOW_RULES="allow 10.64.0.11;" docker compose up -d
  ```

  Requests made on the Docker host itself (`curl http://localhost:8096/...`)
  do not come from `127.0.0.1`: nginx sees the gateway address of the compose
  network. Read the comments in `nginx/default.conf.template` before allowing
  that address.
- Changing settings also needs the admin key (`X-Admin-Key`). Set
  `PROVISION_ADMIN_KEY`, or read the generated one:

  ```bash
  docker compose exec llama-dual cat /var/lib/provision-llm-admin/admin.key
  ```

- `./models-rw` (compose: `MODELS_RW_DIR`) is the one place for every model
  added at run time, so this single mount keeps all of them across a recreate:
  downloaded models in `./models-rw/hf`, uploaded models in
  `./models-rw/local`, where a `.gguf` file dropped in by hand appears after a
  rescan as well. Create both before the first start
  (`mkdir -p models-rw/hf models-rw/local`), so that you own them: what Docker
  or the container creates can end up owned by root.
- Without the `/models-rw` mount (plain `docker run`, an older compose file)
  the panel still works: the entrypoint creates `/models-rw/hf` and
  `/models-rw/local` inside the container, but those models are lost when the
  container is recreated (`docker compose down`/`up`, an image update; a plain
  restart keeps them). The container log says so at start-up
  (`[entrypoint] WARNING: /models-rw is not mounted from the host`), the Admin
  page shows the warning on every tab (`models_storage_ephemeral`) and
  **Diagnostics** lists "Model storage" as a warning. Before such a container
  is recreated, copy the models out, then start it with the mount:

  ```bash
  mkdir -p models-rw
  docker compose cp llama-dual:/models-rw/. ./models-rw/
  docker compose up -d
  ```

  A copy made with `sudo` belongs to root; give it back to yourself before the
  start (`sudo chown -R "$(id -u):$(id -g)" models-rw`), so that you can still
  drop files into `local/`.

- Models uploaded in the panel (**Add model** -> **Upload GGUF**: one file, a
  split set, with or without its projector; it continues after a dropped
  connection, a reload or a restart) land in `./models-rw/local` as well, and
  only after the server has checked them. Unfinished uploads live in
  `./models-rw/local/.provision-upload/`: leave that folder alone, the server
  cleans it up. The server gives uploaded files the owner and group of
  `./models-rw/local` (it runs as root in the container) and makes them
  group-writable.
  The panel shows the upload as unavailable only when the server has no
  writable models directory (the entrypoint could not create
  `/models-rw/local`, for example on a read-only mount; the tab names the
  reason). The shipped nginx front door needs no change for uploads; a proxy of
  your own in front of it may need a larger body limit for `/admin/`, see
  [`docs/provision-admin.md`](../docs/provision-admin.md#169-behind-a-reverse-proxy).
- Models in `./models-rw/local` (uploaded or dropped in) can be deleted in the
  panel (**Delete files**): only the files the model uses, other files in its
  folder stay. In the same dialog, **Remove only the projector** drops only the
  projector of a model. Downloaded models keep their **Delete**.
  Settings (`admin-state.json`), per-model settings (`admin-models.json`) and
  the generated key live in the `llama-admin-state` volume.
- The **Activity** tab shows what was changed through the admin API, refused
  admin requests and some effects of the public model routes (the log is
  `admin-audit*.jsonl` in the same volume). In this stack the server sees every
  request coming from nginx, so it records nginx as the address and the real
  one, from `X-Forwarded-For`, as unverified. To record it as verified, give
  nginx a fixed address and trust it, see
  [`docs/provision-admin.md`](../docs/provision-admin.md#182-who-addresses-and-proxies).
- Before every image update note the image digest (the CI tags are rolling,
  `docker compose pull` leaves no older image behind) and back up the admin
  state; `LLAMA_DUAL_IMAGE=<repo>@sha256:...` is the way back:

  ```bash
  docker image inspect --format '{{index .RepoDigests 0}}' \
      "$(docker inspect --format '{{.Image}}' llama-dual)" | tee image-before-update.txt
  docker compose exec -T llama-dual tar czf - -C /var/lib/provision-llm-admin . \
      > admin-state-$(date +%F).tgz
  ```

  To drop all per-model settings: remove them per model in the panel
  (**Remove custom settings**), or remove the file while the server is
  stopped (a running server writes it again):

  ```bash
  docker compose stop llama-dual
  docker compose run --rm --no-deps -T --entrypoint sh llama-dual -c \
      'rm -f /var/lib/provision-llm-admin/admin-models.json*'
  docker compose start llama-dual
  ```

  Details, and how to restore a backup:
  [`docs/provision-admin.md`](../docs/provision-admin.md#14-upgrading-an-existing-docker-host).

| Compose variable | Default | Effect |
|------------------|---------|--------|
| `ADMIN_ALLOW_RULES` | *(empty)* | nginx `allow` rules for `/admin`; empty = denied for everyone |
| `PROVISION_ADMIN` | `1` | `0` switches the admin layer off (`/admin` answers 404) |
| `PROVISION_ADMIN_KEY` | *(empty)* | Admin key; empty = generated on first start |
| `PROVISION_ADMIN_DEFAULT_DEVICE` | `CUDA0` | GPU for models without a `device` of their own (downloaded / dropped in) until a value is saved in the panel; `MODEL_DEVICES="*=..."` wins |
| `PROVISION_ADMIN_PINNED` | `qwen2.5-chat,gte-qwen2-embed` | Models that are never unloaded to make room, until a list is saved in the panel |
| `PROVISION_ADMIN_GPU_PROBE` | `auto` | Source of the live GPU memory (`auto` = `nvidia-smi` here, `off` = memory as measured at start-up) |
| `MODELS_MAX` | `3` | Both shipped models plus one slot for an added model |
| `MODELS_RW_DIR` | `./models-rw` | Host directory mounted at `/models-rw`, the one root for every model added at run time: downloads (`hf/`), uploads and dropped-in models (`local/`). Without the mount they live inside the container and the server warns that a recreate loses them |
| `NGINX_IMAGE` | `nginx:1.27-alpine` | Image of the front door (use a mirror if Docker Hub is not reachable) |
| `HF_TOKEN`, `MODEL_ENDPOINT` | *(empty)* | Hugging Face token / mirror endpoint for downloads |

The admin layer is enabled by the compose file, not by the image: a plain
`docker run` of the image, or an older compose file, behaves as before.

## 9. Troubleshooting

- **The published port does not answer and `docker compose ps` shows nginx
  restarting** &mdash; the nginx configuration is invalid, almost always a
  missing semicolon in `ADMIN_ALLOW_RULES`. `docker compose logs nginx` names
  the line.
- **Port 8096 answers "Welcome to nginx!" and `404` for every API route**
  &mdash; nginx runs with its stock configuration: the template was not
  rendered (an older compose file that mounts a directory, with
  `nginx/default.conf.template` missing). Use the shipped compose file and
  copy the template next to it.
- **`502 Bad Gateway`** &mdash; nginx is up but `llama-dual` is down or still
  starting; see `docker compose logs llama-dual`.
- **`/admin/...` answers 403** &mdash; the client address is not in
  `ADMIN_ALLOW_RULES`. `docker compose logs nginx` shows the address nginx
  saw (`access forbidden by rule, client: ...`).
- **The Upload GGUF tab of "Add model" shows a reason instead of the drop
  zone** &mdash; the server has no writable models directory: the entrypoint
  could not create `/models-rw/local` (a read-only mount? the container log
  says), or the directory is not writable for the server. The tab names the
  reason; fix it and run `docker compose up -d` again.
- **The Admin page warns "Model storage is not persistent"; the container log
  says `/models-rw is not mounted from the host`** &mdash; the container runs
  without the `/models-rw` mount, so downloaded and uploaded models live inside
  it and are lost when it is recreated. Copy them out and add the mount, see
  section 8 (`docker compose cp llama-dual:/models-rw/. ./models-rw/`, then
  `docker compose up -d`).
- **An upload stops with "The disk is full"** &mdash; the filesystem of
  `./models-rw` is full (it often also holds the admin state volume and the
  Docker images). Free space, then press **Resume**; or **Cancel**, which frees
  what was received.
- **The admin panel shows no live GPU memory** &mdash; `nvidia-smi` is
  missing inside the container: `NVIDIA_DRIVER_CAPABILITIES` must contain
  `utility` (check with `docker compose exec llama-dual sh -c 'echo
  $NVIDIA_DRIVER_CAPABILITIES; command -v nvidia-smi'`). Admin -> Settings ->
  Diagnostics shows what the server found.
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
