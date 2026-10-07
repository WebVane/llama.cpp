# Provision LLM Engine: admin panel

This document describes the admin layer of `provision-server` in router mode: what it
does, how it is switched on, how it is locked down, the per-model settings and the GPU
memory view, uploading and deleting model files, the activity log of admin actions, the
HTTP API, where the data lives, and how to run and upgrade it, both with the
`docker-dual/` Docker deployment and directly on a Linux host.

- [1. What it is](#1-what-it-is)
- [2. What stays public, by design](#2-what-stays-public-by-design)
- [3. Configuration](#3-configuration)
- [4. Locking it down](#4-locking-it-down)
- [5. Settings](#5-settings)
- [6. Per-model settings](#6-per-model-settings)
- [7. GPUs and memory](#7-gpus-and-memory)
- [8. Will it fit?](#8-will-it-fit)
- [9. When a model fails to start](#9-when-a-model-fails-to-start)
- [10. Diagnostics](#10-diagnostics)
- [11. Storage](#11-storage)
- [12. API reference](#12-api-reference)
- [13. Deployment on a Linux host](#13-deployment-on-a-linux-host)
- [14. Upgrading an existing Docker host](#14-upgrading-an-existing-docker-host)
- [15. Troubleshooting](#15-troubleshooting)
- [16. Uploading GGUF files](#16-uploading-gguf-files)
- [17. Deleting models of the models directory](#17-deleting-models-of-the-models-directory)
- [18. Activity log](#18-activity-log)

## 1. What it is

The admin layer adds an API under `/admin` and an **Admin** page to the web UI
(`#/admin`):

| Function | Where |
|---|---|
| See all models, load / unload them, download a model from Hugging Face, delete a downloaded model, rescan the model directories | Admin page, **Models** tab. Uses the server's *existing* model routes. |
| Check a download before starting it: which files, how big, is there enough disk space | `POST /admin/models/download/check` |
| Upload GGUF files from the browser: one model (a file or a split set, with or without its projector) or several at once; it continues after a dropped connection, a reload of the page or a restart of the server; this server build checks the files before the model appears, and the GPU and context are chosen before its first start | Models tab, **Add model** -> **Upload GGUF**, or drop the files onto the tab; `/admin/models/upload`, see [16](#16-uploading-gguf-files) |
| Delete a model of the models directory (uploaded or dropped in by hand): only the files it uses; or remove only its projector | Models tab, **Delete files**; `POST /admin/models/delete`, see [17](#17-deleting-models-of-the-models-directory) |
| Change the maximum number of loaded models without a restart | `POST /admin/settings` (`models_max`) |
| Choose the GPU for models that were added later (downloaded or dropped in) | `POST /admin/settings` (`default_device`) |
| Pin models so they are never unloaded to make room for another one | `POST /admin/settings` (`pinned`) |
| See where models are stored and how much space is left | `GET /admin/storage` |
| Change one model's GPU, context size, parallel requests, GPU layers, flash attention, KV cache precision, micro-batch and role, with **Save** or **Save and restart**; a setting that stops the model from starting is reverted automatically | Models tab, **Settings** on a model; `/admin/models/settings`, see [6](#6-per-model-settings) |
| Before saving: will the model fit on its GPU? llama.cpp itself checks the new settings without loading the weights | the Settings dialog; `POST /admin/models/settings/check`, see [8](#8-will-it-fit) |
| One card per GPU: total, used and free memory and the models on it | Models tab; `GET /admin/gpus`, see [7](#7-gpus-and-memory) |
| Why a model did not start: a reason and the last lines of its output | Models tab, failed row; `GET /admin/models/log`, see [9](#9-when-a-model-fails-to-start) |
| Export one model's settings to a file and import them (on this or another server) | the Settings dialog, see [6.8](#68-export-and-import) |
| What the server found on this machine (Docker or host, GPU backends, GPUs, memory source, state directory, proxy), with a hint for every problem | Settings tab, **Diagnostics**; `GET /admin/diagnostics`, see [10](#10-diagnostics) |
| What was done through the admin API, from which address, and which admin requests were refused; what the server did by itself; what it saw of the public model routes (finished downloads, removed models, failed starts) | **Activity** tab; `GET /admin/audit`, see [18](#18-activity-log) |

The layer exists only in the **router** process (the one started with `--models-preset`
/ `--models-dir`, i.e. without a model of its own) and only when it is enabled with
`PROVISION_ADMIN=1`. When it is not enabled the server behaves exactly as before and
every `/admin/...` path answers 404. The model child processes never see the admin
variables or the admin key.

Two fixes in the server itself apply with or without the admin layer: a projector file
(`mmproj`) whose metadata has values of the wrong type, dropped into the models
directory or downloaded, no longer stops the server at start-up or at a rescan (the
model is listed without image / audio input and the log names the file); and a download
with `POST /models` no longer takes an `@` in a file path of the repository for the user
part of the URL, which could send the request, with `HF_TOKEN`, to another host.

It works the same in the unlicensed (`:dev`) and the licensed (`:release`) image, and
in a build made from source. Admin requests are never counted against the license
budget and keep working when the license is used up or missing.

It works on any Linux server, in Docker or directly on the host, with any number and mix
of GPUs (NVIDIA, AMD, Intel, through CUDA, HIP/ROCm, Vulkan or SYCL) or with none. It
measures what it shows on the machine it runs on, at run time; nothing has to be
configured per machine for the GPU view or the fit check. The Docker image of
`docker-dual/` is built for NVIDIA GPUs only; for other GPUs build the server from
source and run it on the host ([13](#13-deployment-on-a-linux-host)). An image for AMD
or Intel GPUs is not part of this release.

## 2. What stays public, by design

The admin layer does **not** move, hide or protect any route that existed before. In
particular these keep working for every client that can reach the API, with no admin
key and from any address:

| Route | Effect |
|---|---|
| `GET /models`, `GET /v1/models` | list models |
| `GET /models?reload=1` | re-read `models.ini`, the model cache and the models directory |
| `POST /models/load`, `POST /models/unload` | load / unload a model (unload also cancels a running download) |
| `POST /models` | download a model from Hugging Face |
| `DELETE /models?model=...` | delete a downloaded model |
| `GET /models/sse` | live status feed |
| `GET /slots` | slot state (the restart confirmation reads how many slots are busy) |
| automatic loading on the first request to a model | |

Why: the chat UI's model selector and existing API clients depend on these routes, and
changing them would break both. The Admin page uses the very same routes for its Models
tab and says so on the page.

So the two locks described below protect **the settings** (model limit, GPU for new
models, pinned models, per-model settings), the storage, GPU and diagnostics
information, the model output tails, the download pre-flight check, the upload of GGUF
files and the deletion of files of the models directory (new `/admin` routes). They do
not stop a user of the API from loading, unloading or downloading models, or from
deleting downloaded ones (`DELETE /models` deletes only models of the model cache, as
before). If that matters in your environment, restrict who can reach the API at all
(network, or the server's `--api-key` / `LLAMA_API_KEY`, which then applies to `/admin`
as well). The activity log cannot say who used these routes either; it records only some
of their effects, as "public API (unknown)" ([18.4](#184-what-cannot-be-recorded)).

`GET /v1/models` shows the effective arguments of every model (`status.args`,
`status.preset`), so the values of per-model settings are visible there without a key.
They are device names and numbers; the whitelist ([6.1](#61-which-models-and-which-settings))
contains no paths and no secrets.

## 3. Configuration

### 3.1 Server environment variables

Read once when the router starts, then removed from the process environment (the model
processes never see them).

| Variable | Meaning |
|---|---|
| `PROVISION_ADMIN` | `1`, `true` or `on` enables the admin layer. Anything else, or unset: disabled. |
| `PROVISION_ADMIN_KEY` | The admin key. Optional. |
| `PROVISION_ADMIN_KEY_FILE` | Path of a file whose first line is the admin key. Optional. |
| `PROVISION_ADMIN_STATE_DIR` | Directory for saved settings, per-model settings and the generated key. Default `/var/lib/provision-llm-admin` (Linux), `%ProgramData%\ProvisionLLM\admin` (Windows). It must exist or be creatable by the server, be writable, and survive restarts and updates (a Docker volume, or a directory on the host). When it cannot be created the server log says so and names this variable. |
| `PROVISION_ADMIN_DEFAULT_DEVICE` | Start value of "GPU for new models", used until a value is saved from the panel. |
| `PROVISION_ADMIN_PINNED` | Comma-separated start value of the pinned models, used until a list is saved from the panel. |
| `PROVISION_ADMIN_GPU_PROBE` | Where the live GPU memory comes from, see [7.2](#72-live-memory). `auto` (default): for every GPU vendor the inventory found, its tool from `PATH` (`nvidia-smi`; `amd-smi`, else `rocm-smi`, both also from `$ROCM_PATH/bin` and `/opt/rocm/bin`). `off`: no vendor tool. `nvidia-smi:<path>`, `amd-smi:<path>`, `rocm-smi:<path>`, or just an absolute path (the tool is told by its file name): that tool at that path. `file:<path>`: recorded tool output for tests (the first line names the format). |
| `PROVISION_ADMIN_INVENTORY` | The GPU inventory at start-up, see [7.1](#71-which-gpus-the-server-has). `auto` (default): run it. `off`: skip it (GPU names only, no PCI ids, no start-up memory). `file:<path>`: a recorded result, for tests. |
| `PROVISION_ADMIN_ALLOW_FROM` | The server's own address allow-list for every `/admin` route, for hosts without nginx, see [4.3](#43-built-in-address-allow-list-hosts-without-nginx). IPv4 / IPv6 addresses and CIDR ranges, separated by commas or blanks, e.g. `10.64.0.0/24, 127.0.0.1, ::1`. Unset or empty (default): off, any address may use `/admin` (with the admin key). An entry that cannot be read locks `/admin` for everybody. |
| `PROVISION_ADMIN_TRUSTED_PROXIES` | Addresses / ranges of reverse proxies in front of the server. Only for a request from one of them is `X-Forwarded-For` used to find the client address, for the allow-list and for the activity log ([18.2](#182-who-addresses-and-proxies)); unset (default): `X-Forwarded-For` is ignored (the activity log keeps it as unverified). Every listed address can claim any client address, so list only the proxies themselves. Without `PROVISION_ADMIN_ALLOW_FROM` it changes only which address the activity log records, not who may use `/admin`. |

`MODEL_DEVICES` (the variable of the Docker entrypoint) is only read, never removed. It
is an extra lock source when it is present ([6.4](#64-locked-fields)); on a host without
the Docker entrypoint nothing applies it, so do not set it there.

Empty values count as "not set".

The server also starts short-lived helper processes of its own binary (the GPU
inventory, the fit check and the check of uploaded files). They are selected with
internal environment variables that the server sets itself; do not set any other
`PROVISION_ADMIN_*` variable.

Uploading and deleting model files have no setting of their own: they need only a
models directory (`--models-dir`) that the server can write to, see
[16.1](#161-what-can-be-uploaded). Nor does the activity log: it is written into the
admin state directory ([18.5](#185-storage-rotation-and-retention)).

### 3.2 `docker-dual/docker-compose.yml`

All of these can be set in the shell or in a `.env` file next to the compose file.

| Variable | Default | Meaning |
|---|---|---|
| `PORT` | `8096` | Host port. Published by the `nginx` service; the server itself is not published. |
| `ADMIN_ALLOW_RULES` | *(empty)* | nginx `allow` directives for `/admin`. Empty: `/admin` is denied for everyone. See [4.1](#41-address-allow-list-nginx). |
| `NGINX_IMAGE` | `nginx:1.27-alpine` | Image of the front door. Point it at a mirror if the host cannot pull from Docker Hub. |
| `PROVISION_ADMIN` | `1` | `0` switches the admin layer off. |
| `PROVISION_ADMIN_KEY` | *(empty)* | Empty: the server generates a key into the admin state volume. |
| `PROVISION_ADMIN_DEFAULT_DEVICE` | `CUDA0` | Set it to an empty value to leave new models on all visible GPUs. |
| `PROVISION_ADMIN_PINNED` | `qwen2.5-chat,gte-qwen2-embed` | Set it to an empty value to pin nothing. |
| `PROVISION_ADMIN_GPU_PROBE` | `auto` | See [3.1](#31-server-environment-variables). In the image, `auto` finds the `nvidia-smi` that the NVIDIA Container Toolkit mounts. |
| `MODELS_MAX` | `3` | The two shipped models plus one slot for a model added later. |
| `MODELS_RW_DIR` | `./models-rw` | Host directory for downloaded, uploaded and dropped-in models, mounted at `/models-rw`; without it the panel cannot upload. Create `hf/` and `local/` in it yourself before the first start, see [11. Storage](#11-storage). |
| `MODEL_DEVICES` | *(empty)* | Per-model GPU override applied by the entrypoint; it wins over `models.ini` and over the panel ([6.3](#63-where-a-value-comes-from)). |
| `HF_TOKEN` | *(empty)* | Hugging Face token for gated or private repositories. |
| `MODEL_ENDPOINT` | *(empty)* | Hugging Face endpoint, e.g. an internal mirror. Default `https://huggingface.co/`. |

Fixed in the compose file:

- `NVIDIA_DRIVER_CAPABILITIES: "compute,utility"`. `utility` makes the NVIDIA Container
  Toolkit mount `nvidia-smi` into the container, which is where the live GPU memory
  comes from. The CUDA base image has the same default; the compose file spells it out
  so that an edit elsewhere (for example in the `capabilities` list) cannot drop it.
- `GGML_LOG_LEVEL` is passed on but not read by this server build; it is kept so that
  an existing `.env` entry does no harm.

The admin layer is switched on **by the compose file**, not by the image: a new image
started with an old compose file behaves as before.

Example `.env`:

```
ADMIN_ALLOW_RULES="allow 10.64.0.11; allow 10.64.0.0/24;"
MODELS_RW_DIR=/srv/llm/models-rw
```

### 3.3 What the entrypoint does

`docker-dual/entrypoint.sh` does these things for the admin layer; none of them can
abort the start-up:

- When `/models-rw` is mounted it creates `/models-rw/hf` and `/models-rw/local` if they
  are missing (and gives them the owner of `/models-rw`), exports
  `LLAMA_CACHE=/models-rw/hf` (unless `LLAMA_CACHE` is already set) and passes
  `--models-dir /models-rw/local`, which is also where uploads go
  ([16](#16-uploading-gguf-files)). Without the mount nothing changes, and the panel
  shows the upload as unavailable.
- Empty `PROVISION_ADMIN_KEY`, `PROVISION_ADMIN_KEY_FILE` and `PROVISION_ADMIN_STATE_DIR`
  are removed from the environment.
- Startup-preload guard. The router refuses to start when more models are marked
  `load-on-startup` than may be loaded at once. The shipped `models.ini` preloads both
  models, so with a limit of 1 (still the image default, e.g. plain `docker run`) the
  entrypoint keeps the key only for the first sections in file order, logs a warning
  and starts the server with a patched copy (`/tmp/models.startup.ini`). The limit it
  uses is `MODELS_MAX`, lowered to the "Max loaded models" value saved from the panel
  if there is one.
- `MODEL_DEVICES` is applied to a `models.ini` with Windows line endings (CRLF) as well.
  Before, such a file was left unpatched and only "not in file" warnings were printed,
  while the panel would have shown the device as locked by `MODEL_DEVICES`.

### 3.4 `models.ini`

- `[*]` has `offline = true`: model processes never go to the network. A downloaded
  model is loaded from the files on disk, so loading does not depend on Hugging Face
  being reachable and cannot turn into a silent re-download when the upstream
  repository changes. Downloads are not affected (the download worker is started
  without any preset), and neither are models given by a local path. To update a
  downloaded model, delete it and download it again. A section of your own that relies
  on fetching its `hf-repo` on first load needs `offline = false`. Do not set
  `LLAMA_ARG_OFFLINE` in the container environment instead: the download worker would
  inherit it.
- `[gte-qwen2-embed]` has `load-on-startup = true`, like the chat model: both resident
  models take their GPU memory before anything else is loaded.
- The panel can override the per-model settings of the sections
  ([6](#6-per-model-settings)); the file itself is never written. Per-model settings
  saved in the panel win over the section, `MODEL_DEVICES` and the router command line
  win over the panel.

### 3.5 Outside Docker

The admin layer is part of the server binary, so it works without Docker too: set
`PROVISION_ADMIN=1` in the environment of the router. [13](#13-deployment-on-a-linux-host)
describes a complete Linux host installation (systemd unit, environment file, host nginx
site). Without nginx the admin key is the only lock unless you switch on the server's
own address allow-list ([4.3](#43-built-in-address-allow-list-hosts-without-nginx)):
keep the server bound to `127.0.0.1`, put a reverse proxy with an equivalent `/admin`
rule in front of it, or set `PROVISION_ADMIN_ALLOW_FROM`.

The state directory holds the admin key. A state directory that the server creates
itself is restricted to the server's account: mode `0700` on Linux; on Windows an
access list of its own (SYSTEM, Administrators and the account the server runs as)
instead of the one inherited from `%ProgramData%`, which every local user can read. A
directory, `admin.key`, `admin-state.json` or `admin-models.json` that **already
exists** is used with whatever permissions and owner it has; the server does not check
them. On Windows therefore let the server create the state directory, or point
`PROVISION_ADMIN_STATE_DIR` at a directory that only the service account can use.

## 4. Locking it down

Two independent locks:

1. **Address allow-list**: `/admin` is answered only for client addresses you name,
   everybody else gets `403` (`admin_forbidden`). Either in nginx
   ([4.1](#41-address-allow-list-nginx), the Docker deployment and the host nginx site):
   the request never reaches the server. Or, on a host without nginx, in the server
   itself ([4.3](#43-built-in-address-allow-list-hosts-without-nginx), off by default).
2. **Admin key**: every `/admin` endpoint except the status probe needs the
   `X-Admin-Key` header. It works the same with either allow-list or none.

### 4.1 Address allow-list (nginx)

`docker-compose.yml` runs an `nginx` container as the only published service. Its
configuration is rendered from `docker-dual/nginx/default.conf.template` when the
container starts. On a host without Docker, `deploy/linux/nginx-provision-llm.conf` does
the same with the host's own nginx ([13.5](#135-nginx)).

- `location = /admin` and `location ^~ /admin/`: the rules from `ADMIN_ALLOW_RULES`,
  then `deny all`.
- `location ^~ /cors-proxy`: the same rules (see "Keep in mind" below).
- everything else: forwarded unchanged.

`ADMIN_ALLOW_RULES` holds raw nginx directives, **each terminated by a semicolon**:

```bash
ADMIN_ALLOW_RULES="allow 10.64.0.11; allow 10.64.0.0/24;" docker compose up -d
```

When this is the only value that changed, only the nginx container is re-created; the
server and its loaded models are not touched.

A missing semicolon or a typo makes the configuration invalid. nginx then does not
start, and because it is the only published service the whole API is down. Check first:

```bash
ADMIN_ALLOW_RULES="allow 10.64.0.11;" docker compose run --rm --no-deps nginx nginx -t
```

**Which address counts.** The one nginx sees as the TCP peer. It is the first field of
every access-log line, and a refused request is logged with it:

```bash
docker compose logs --tail 20 nginx
# ... access forbidden by rule, client: 10.64.0.57, server: _, request: "GET /admin/status HTTP/1.1" ...
```

- Clients on the LAN normally arrive with their real address.
- Requests made **on the Docker host itself** (`curl http://localhost:8096/...`, an SSH
  tunnel that ends on the host, other containers that go through the published port)
  do **not** arrive from `127.0.0.1`. Docker forwards them, and nginx sees the gateway
  address of the compose network, for example `172.18.0.1`:

  ```bash
  docker inspect llama-dual-nginx \
      --format '{{range .NetworkSettings.Networks}}{{.Gateway}}{{end}}'
  ```

  Allowing that address means "anything that runs on the Docker host". On hosts where
  Docker's userland proxy also carries remote traffic (IPv6 clients of an IPv4-only
  network, rootless Docker, Docker Desktop) remote clients show up with the gateway
  address too, and allowing it would open `/admin` to everybody. Before you allow the
  gateway address, make a request from another machine and confirm that the log shows
  that machine's own address.
- The gateway address can change when the network is re-created (`docker compose down`).
  The commented-out `networks:` block at the end of `docker-compose.yml` pins the
  subnet if you need a stable value.
- `allow 127.0.0.1;` only matches processes inside the nginx container.
- With the host nginx site ([13.5](#135-nginx)) there is no Docker in between: requests
  made on the host come from `127.0.0.1` / `::1`, which that site allows by default.

**Spelling tricks do not get around the rule.** nginx matches locations against the
normalised path: percent-encoding decoded once, `.` and `..` segments resolved,
repeated slashes merged. So `/%61dmin/status`, `/admin%2Fstatus`, `//admin/status`,
`/x/../admin/status` and `http://other/admin/status` are all `/admin/status` for the
rule. The server routes on the path decoded once as well, so every request line that
the server would treat as an `/admin` route is one that nginx puts into the two
locations. The one decoding the server knows and nginx does not (`%uXXXX`) is rejected
by nginx with `400` before any location is tried. Paths that merely look similar
(`/administrator`, `/admin.json`, `/Admin/status`) are not admin routes on the server
either and are forwarded like any other unknown path.

Keep in mind:

- The server container must stay unpublished. Do not add `ports:` to `llama-dual`, and
  do not attach containers you do not trust to this compose network: they can reach
  `llama-dual:8080` directly.
- On a Linux host, processes on the Docker host itself can reach the server container
  directly as well, at its address on the compose network (port 8080), without passing
  nginx. For them only the admin key protects the settings: count everybody with an
  account on the Docker host as able to reach `/admin`. The same holds for the host
  installation, where the server listens on `127.0.0.1:8080`.
- The rule assumes that the server's routes start at `/`. Do not set `--api-prefix`
  (`LLAMA_ARG_API_PREFIX`): with a prefix the admin API moves to `<prefix>/admin/...`,
  which nginx forwards from every address.
- Keep the experimental server features switched off that make the server send
  requests or run commands for a client (all of them are off in this stack). Through
  them a client reaches the server's own `/admin` from inside, where the address rule
  does not apply:
  - `--ui-mcp-proxy` / `--agent`: the CORS proxy at `/cors-proxy` fetches any URL,
    `http://127.0.0.1:8080/admin/...` included. nginx gives `/cors-proxy` the same
    allow-list as `/admin`, so with the proxy switched on only those addresses can use
    it (switched off, the server answers `403` to everybody anyway).
  - `--tools`, `--agent`, MCP servers: built-in tools run commands and read files in the
    container, the admin key included.
  - Vertex AI mode (`AIP_MODE=PREDICTION`): its predict route calls other routes of the
    server by name, with the client's headers and query string. The `/admin` routes are
    kept out of its dispatch table, so `/admin` itself cannot be reached through it, but
    every other route can, `/cors-proxy` included, past the nginx rule for that path.
- Docker publishes ports with its own firewall rules, in front of `ufw`. A `ufw` rule
  does not restrict who can reach port 8096.
- nginx speaks plain HTTP, like the server did. The admin key travels in a request
  header: use the admin API from a trusted network, or add TLS in front.

What the proxy does to ordinary traffic: nothing visible. Request line, headers and
body are passed on as sent; `Host` is preserved and `X-Forwarded-For`,
`X-Forwarded-Proto` and `X-Real-IP` are added. Responses are not buffered or
compressed, so server-sent events (`/models/sse`, `"stream": true`) arrive as they are
produced. There is no request size limit of nginx's own (the server's limit of 100 MB
applies). A response that stays silent, such as an idle event feed, is kept open for
up to 24 hours. While the server is down or restarting clients get `502` from nginx
instead of "connection refused".

### 4.2 Admin key

Sent as `X-Admin-Key: <key>`. The server takes the key from, in this order:

1. `PROVISION_ADMIN_KEY`
2. the first line of the file named by `PROVISION_ADMIN_KEY_FILE`
3. `<state dir>/admin.key`

Leading and trailing blanks of the key are ignored. If none exists it generates a
random key (32 bytes, hex) into `<state dir>/admin.key` and logs the **path**, never
the value. Read it with:

```bash
docker compose exec llama-dual cat /var/lib/provision-llm-admin/admin.key   # Docker
sudo cat /var/lib/provision-llm-admin/admin.key                             # host
```

The web UI asks for the key when the Admin page is opened and keeps it for the browser
session (or on the device if "Remember on this device" is ticked).

To change the key: set `PROVISION_ADMIN_KEY` and run `docker compose up -d` (host:
edit the environment file and restart the service), or delete `admin.key` and restart
the server to get a new generated one.

A key passed in `PROVISION_ADMIN_KEY` is part of the container configuration and can
be read with `docker inspect`. If that matters, mount a file with the key into the
container and name it in `PROVISION_ADMIN_KEY_FILE` (add both to the `llama-dual`
service), or use the generated key.

A key file (`PROVISION_ADMIN_KEY_FILE` as well as `admin.key`) must be ASCII or UTF-8
text; a UTF-8 byte order mark is fine. A UTF-16 file (what Windows PowerShell 5.1
writes with `>` or `Out-File`) and a key with characters outside printable ASCII are
refused: the server log names the file and the reason (never the key), and the next
source in the list above is used. A refused `<state dir>/admin.key` is replaced by a
newly generated key. In PowerShell write the file with
`Set-Content -Encoding ascii -Path <file> -Value <key>`.

If no key can be obtained (nothing configured and the state directory is not
writable) the protected endpoints answer `503` (`admin_key_unavailable`).

If the server runs with `--api-key`, that check comes first and applies to `/admin`
like to any other route.

### 4.3 Built-in address allow-list (hosts without nginx)

The server itself can limit `/admin` to a list of client addresses. This is the option
for a plain Linux host without nginx or another reverse proxy in front of the server.
Where nginx is in front (the Docker deployment, the host site of
[13.5](#135-nginx)), keep the rule there ([4.1](#41-address-allow-list-nginx)): nginx
also locks `/cors-proxy`, and it sees the real client address.

**Off by default.** With `PROVISION_ADMIN_ALLOW_FROM` unset or empty any client address
may use `/admin`, exactly as before; most installations restrict `/admin` in their own
nginx. The admin key ([4.2](#42-admin-key)) works the same in every case: the
allow-list is checked first, then the key.

| Variable | Meaning |
|---|---|
| `PROVISION_ADMIN_ALLOW_FROM` | IPv4 and IPv6 addresses and CIDR ranges, separated by commas and / or blanks: `10.64.0.0/24, 127.0.0.1, ::1, fd00::/8`. Unset or empty: off. |
| `PROVISION_ADMIN_TRUSTED_PROXIES` | Addresses and ranges of reverse proxies whose `X-Forwarded-For` the server believes (see below), for the allow-list and for the activity log. Unset: `X-Forwarded-For` is ignored. |

Both are read once when the router starts (restart the server after a change) and are
removed from the environment like every `PROVISION_ADMIN_*` variable: the model
processes never see them.

What the allow-list does:

- It applies to **every** `/admin` route, the status probe `GET /admin/status` (which
  needs no key) included. No other route is affected.
- A request from an address that is not listed gets `403` with the type nginx uses, and
  the message names the address the server saw:

  ```json
  { "error": { "code": 403, "message": "The admin API is not available from this address; the server sees the request coming from 10.64.1.7", "type": "admin_forbidden" } }
  ```

  The server log has one warning per refused request (without the key or any other
  header): `admin: refused /admin/settings from 10.64.1.7: not on PROVISION_ADMIN_ALLOW_FROM`.
  The activity log records the refusals too, merged per address and path
  ([18.3](#183-refused-requests)). The web UI hides the Admin item
  ([4.4](#44-the-admin-item-in-the-web-ui)).
- Addresses are compared as numbers, not as text: `2001:DB8:0:0::1` is `2001:db8::1`, and
  an IPv4 client that a dual-stack listener (`--host ::`) reports as `::ffff:10.64.0.7`
  matches the IPv4 entries (`10.64.0.0/24`). An entry with host bits set
  (`10.64.0.5/24`) is read as its network (`10.64.0.0/24`), with a warning in the log and
  in Diagnostics: check that the network is what you meant (`10.64.0.5/2` would be
  `0.0.0.0/2`). `0.0.0.0/0` matches every IPv4 address, `::/0` every address.
- Not accepted: host names (`localhost`), IPv4 parts with leading zeros (`10.064.0.1`),
  brackets (`[::1]`), zone ids (`fe80::1%eth0`), prefix lengths outside 0-32 (IPv4) or
  0-128 (IPv6), and an IPv4-mapped address in IPv6 form with a prefix length below 96
  (`::ffff:10.0.0.0/8` would be `::/8`, which holds every IPv4 address; write
  `10.0.0.0/8`, or `::ffff:10.0.0.0/104`).
- **Fail closed.** An entry the server cannot read, in either variable, is logged as an
  error that names it (`admin: PROVISION_ADMIN_ALLOW_FROM: invalid entry 'localhost': ...`),
  and every `/admin` request answers `403` `admin_forbidden` ("The admin API is locked:
  the address allow-list of the server has an invalid entry ...") from every address
  until the value is fixed and the server restarted. The server does not silently drop
  a restriction you asked for.
- A request without a network address (a Unix socket, `--host /path/to.sock`) is refused
  while the allow-list is on.

**Which address counts.** The TCP peer of the request, as the server sees it. On a host,
requests made on the machine itself come from `127.0.0.1` / `::1` and LAN clients come
with their own address. In a container the server sees the Docker bridge gateway or the
nginx container instead, which is one reason why the Docker deployment keeps the rule
in nginx. Diagnostics ([10](#10-diagnostics), item "Admin address allow-list") shows the
address the server saw for that very request (`seen_from`) and the TCP peer (`peer`).

**Behind a reverse proxy: trusted proxies and `X-Forwarded-For`.** Behind a proxy every
request comes from the proxy's address. The client address is in `X-Forwarded-For`, but
any client can send that header itself, so the server ignores it unless the TCP peer is
listed in `PROVISION_ADMIN_TRUSTED_PROXIES`. For a request from a trusted proxy the
server reads `X-Forwarded-For` from the right: every proxy appends the address it got
the request from, so the entries on the right are written by your proxies and whatever
a client wrote itself is further left. The first entry from the right that is not a
trusted proxy is the client; entries left of it are never looked at. When every entry
is a trusted proxy, the leftmost one counts. When the header is missing, sent in more
than one header line (in any spelling of its name), or the entry that decides is not an
address, the request counts as coming from the proxy itself.

Example: nginx on the same host (it sends `X-Forwarded-For $proxy_add_x_forwarded_for`),
the server on `127.0.0.1`:

```bash
PROVISION_ADMIN_ALLOW_FROM=10.64.0.0/24
PROVISION_ADMIN_TRUSTED_PROXIES=127.0.0.1
```

| `X-Forwarded-For` of a request from 127.0.0.1 | Client | Result |
|---|---|---|
| `10.64.0.7` | `10.64.0.7` | allowed |
| `10.64.0.7, 203.0.113.9` (the client sent `10.64.0.7` itself, nginx appended its real address) | `203.0.113.9` | `403` |
| `10.64.0.7, 192.168.100.5` with `192.168.100.5` also a trusted proxy | `10.64.0.7` | allowed |
| none (a request made on the host straight to the server, past nginx) | `127.0.0.1` | `403` |
| `10.64.0.7`, sent by a process on the host itself straight to `127.0.0.1:8080`, past nginx | `10.64.0.7` | allowed |

- **A trusted proxy address can claim any client address.** The server cannot tell a
  request that a proxy forwards from one that something else at the proxy's address
  makes with an `X-Forwarded-For` of its own. With `127.0.0.1` trusted, as above, every
  account and process on the host (model processes, MCP servers and tools included)
  passes the built-in allow-list; for them only the admin key applies, as with nginx
  alone ([4.1](#41-address-allow-list-nginx)). In this combination the built-in list
  re-checks what nginx forwards and adds nothing against the host itself. List only the
  proxies' own addresses, never a client network: with `192.168.0.0/16` trusted every
  host of that network could claim `10.64.0.7`. The server warns at startup and in
  Diagnostics about a trusted range that holds more IPv4 addresses than a `/24`, or an
  IPv6 range shorter than `/64`.
- Do not put a trusted proxy on the allow-list too, unless everybody who can reach that
  proxy may use `/admin`: a request it forwards without a usable `X-Forwarded-For`
  counts as coming from the proxy and is then allowed. The server warns about such an
  overlap at startup and in Diagnostics.
- `PROVISION_ADMIN_TRUSTED_PROXIES` without `PROVISION_ADMIN_ALLOW_FROM` does not restrict
  anything: any address may use `/admin`. It then only decides which client address the
  activity log records as verified ([18.2](#182-who-addresses-and-proxies)). The server
  says so at startup and in Diagnostics ("PROVISION_ADMIN_TRUSTED_PROXIES without
  PROVISION_ADMIN_ALLOW_FROM only affects the client address the audit log records: any
  address may use /admin"). That is the way to get verified addresses in the log behind a
  proxy that keeps the access rule itself, such as the nginx of the Docker deployment.

Examples:

```bash
# host without nginx: the server listens on the LAN; /admin for the host itself and the admin network
PROVISION_LLM_HOST=0.0.0.0
PROVISION_ADMIN_ALLOW_FROM=127.0.0.1, ::1, 10.64.0.0/24

# one admin workstation, IPv4 and IPv6
PROVISION_ADMIN_ALLOW_FROM=10.64.0.11 fd00:64::11

# behind a load balancer at 10.0.5.2 that sets X-Forwarded-For
PROVISION_ADMIN_ALLOW_FROM=10.64.0.0/24
PROVISION_ADMIN_TRUSTED_PROXIES=10.0.5.2
```

Check it: from a listed address `curl -s http://<server>:8080/admin/status` answers
`200` `{"admin":true,...}`, from any other address `403` `admin_forbidden`. With the key,
`GET /admin/status` reports `"ip_allowlist": true`, and Diagnostics lists the entries,
the trusted proxies and the address the request was seen from.

Keep in mind:

- It filters addresses, it does not authenticate: everybody who can use an allowed
  address (every account on an allowed host, the host itself when `127.0.0.1` is listed
  or trusted as a proxy) still needs only the admin key. The key travels in plain HTTP; use the admin API from
  a trusted network.
- The CORS proxy (`--ui-mcp-proxy` / `--agent`) fetches any URL a client names, from this
  host: with it switched on, any client can reach `/admin` from an address of the host
  (`127.0.0.1`, or the host's own LAN address, which may be on the list) and choose the
  `X-Forwarded-For` of that request too. Keep it off with the built-in allow-list (the
  server warns at startup and in Diagnostics when it is on), or lock `/cors-proxy` in
  nginx ([4.1](#41-address-allow-list-nginx)). The other server features listed in 4.1
  that send requests or run commands for a client must stay off as well.
- `--api-key`, when set, is still checked first, unchanged.

### 4.4 The Admin item in the web UI

The web UI decides with the status probe `GET /admin/status`
([12.1](#121-get-adminstatus)), which needs no key, whether the sidebar shows **Admin**:

| The probe answers | Admin item | A direct visit to `#/admin` |
|---|---|---|
| `200`: the admin layer is on and the address is allowed | shown | the Admin page; it asks for the key when it has none |
| `403`: the address is refused by nginx (`ADMIN_ALLOW_RULES`, or the `geo` list of the host site) or by the server's own allow-list (`PROVISION_ADMIN_ALLOW_FROM`) | hidden | goes to the start page of the chat, without a message. Only a browser that holds an admin key (entered in this tab, kept for the browser session, or saved with "Remember on this device") gets the Admin page with the explanation "Admin API is not available from this address", in which the server's message names the address it saw |
| `404`: the admin layer is off, or the server is older than the admin panel | hidden | the Admin page says that the admin API is not available |
| no answer (the server is down or unreachable) | hidden | the Admin page says that the admin API is not available, with **Retry** |

- With no allow-list configured (the default of a host without nginx) the item is shown
  to everyone who can reach the server, as before; they still need the key.
- When an admin request answers `403` later in the session (the allow-list was changed,
  the client moved to another address, a VPN reconnected with another one), the item
  disappears at once, without a reload. An Admin page that is open at that moment
  switches to "Admin API is not available from this address" with the server's message,
  **Retry** and **Back to chat**; a tab that holds no admin key goes back to the chat.
- A `401` (no or a wrong key) does not hide the item: the Admin page asks for the key.
- Hiding the item is a convenience, not a lock. The locks are the address allow-list and
  the admin key ([4.1](#41-address-allow-list-nginx) to
  [4.3](#43-built-in-address-allow-list-hosts-without-nginx)).

## 5. Settings

Server-wide settings. Saved in `<state dir>/admin-state.json` and applied immediately,
without a restart. Every setting is optional in the file; a setting that was never
saved follows its start-up source.

| Setting | Saved value, else | else |
|---|---|---|
| `models_max` | `--models-max` (`MODELS_MAX`) | |
| `default_device` | `PROVISION_ADMIN_DEFAULT_DEVICE` | empty: feature off |
| `pinned` | `PROVISION_ADMIN_PINNED` | empty: nothing pinned |

Note the consequence: once a value has been saved from the panel, changing the
corresponding compose variable has no effect any more. Change it in the panel, or
remove the state file to hand control back to compose.

### 5.1 Max loaded models (`models_max`)

How many models may be loaded at the same time. When the limit is reached and another
model is needed, the server unloads the least recently used idle model that is not
pinned.

- Raising the value takes effect at once (waiting requests are woken up).
- Lowering it does not unload anything by itself. The next time a model has to be
  loaded, the server unloads idle models that are not pinned, least recently used
  first, until the new model fits under the new limit.
- Allowed range: `limits.models_max_min` .. `limits.models_max_max` of
  `GET /admin/settings` (1 .. 16).
- It must stay greater than the number of pinned models, and it cannot go below the
  number of models marked `load-on-startup` (the server would not start again).
- It is a count. The server does not unload by GPU memory: the loaded models must
  actually fit on your cards. The GPU cards ([7](#7-gpus-and-memory)) show what is
  used.

### 5.2 Pinned models (`pinned`)

A pinned model is never chosen when the server has to unload something to make room.
Pinning does not load a model (that is `load-on-startup` in `models.ini`) and does not
prevent an explicit unload.

At least one slot must always stay free of pins, otherwise no other model could ever
be loaded: with a limit of N models at most N-1 pins take effect.

- `POST /admin/settings` enforces the rule. A request with `pinned` is checked against
  that list as a whole; a request without it (for example only a new `models_max`) is
  checked against the pins in effect at that moment. A request that breaks the rule is
  rejected.
- A list that was not checked against the current limit, i.e. the start-up list from
  `PROVISION_ADMIN_PINNED` (or a saved list after `MODELS_MAX` was lowered), is cut by
  position: its first N-1 names are in effect, the rest is ignored (`pinned_ignored`
  in the settings). A name among the first N-1 that is not in the model list yet still
  takes its place, and that model is protected the moment it appears (download,
  rescan, preset change) without any admin request. Raising the limit brings more
  names of the list into effect.
- With the compose defaults (limit 3, both shipped models listed) both are pinned. If
  `MODELS_MAX` is lowered to 2, only the first name stays pinned; with 1 nothing is
  pinned and the two shipped models swap as they always did.

### 5.3 GPU for new models (`default_device`)

Applies to models that have **no `device` of their own**: models downloaded from the
panel (source `cache`) and files dropped into the models directory (source
`models_dir`). Sections of `models.ini` are not touched by this setting (they can get a
GPU of their own in the per-model settings, [6](#6-per-model-settings)).

Values: empty (not set: such models use all visible GPUs), `none` (CPU only), or one
or more device names separated by `,` or `;`, for example `CUDA0`, `ROCm0`, `Vulkan1`
or `CUDA0;CUDA1`. The names are the ones the server's GPU backends report
(`build_devices` in the settings, `provision-server --list-devices`). A name that this
server does not have is not applied: the panel and `GET /admin/settings` report
`default_device_not_present`, and such models fall back to all visible GPUs.

Changing the value rebuilds the model list. A model that is **running** and whose
arguments change because of it is unloaded and loads again on its next use.

Who decides the device of a model that has no per-model setting, highest first:

| Model comes from | 1 | 2 | 3 | 4 |
|---|---|---|---|---|
| a section of `models.ini` | `MODEL_DEVICES` entry for that section | `MODEL_DEVICES` `*=` entry | `device` in the section | `device` in `[*]` |
| a download or a dropped-in file | `MODEL_DEVICES` `*=` entry | `device` in `[*]` of `models.ini` | `default_device` from the panel | `PROVISION_ADMIN_DEFAULT_DEVICE` |

With a per-model setting see the full table in [6.3](#63-where-a-value-comes-from).

`MODEL_DEVICES` therefore keeps its documented meaning ("wins over `models.ini`") and
also wins over the panel. With a `*=` entry (typical for a single-GPU host:
`MODEL_DEVICES="*=CUDA0"`) the panel shows the setting as **locked** with that value,
and `POST /admin/settings` refuses to change it (`409`, `default_device_locked`). A
`device` key written by hand into `[*]` has the same effect on new models, but the
panel cannot know about it and does not show the lock.

## 6. Per-model settings

Admin page, **Models** tab, **Settings** on a model row. Every model can have its own
values for a short list of settings, whatever its source: a section of `models.ini`, a
downloaded model, a file in the models directory. They are saved in
`<state dir>/admin-models.json` ([11](#11-storage)), next to the server-wide settings,
and survive restarts, container re-creation and image updates. `models.ini` itself is
never written.

### 6.1 Which models and which settings

The keys are the names used in `models.ini` (and in the API and the export file):

| Key | Values | In the dialog | Notes |
|---|---|---|---|
| `device` | `none` (CPU only), or 1 to 4 GPU names of this server joined by `,` | GPU | Names as the server's backends report them (`CUDA0`, `ROCm0`, `Vulkan0`, `SYCL0`, ...). Compared without regard to case, stored in the server's spelling. |
| `ctx-size` | 256 .. 1048576 | Context size | The KV cache pool of the model, shared by its slots; see [6.2](#62-context-size-one-shared-pool-or-per-request). |
| `kv-unified-per-slot` | 256 .. 1048576 | (Context, mode "per request x slots") | Upper limit for one request; written together with `ctx-size`, see [6.2](#62-context-size-one-shared-pool-or-per-request). |
| `parallel` | `-1` (auto, 4 slots) or 1 .. 16 | Parallel requests | How many requests the model serves at the same time. |
| `n-gpu-layers` | `auto`, `all` or 0 .. 999 | GPU layers (advanced) | `auto` lets llama.cpp move layers to the CPU when GPU memory is short. |
| `flash-attn` | `on`, `off`, `auto` | Flash attention (advanced) | A quantized KV cache needs it (`on` or `auto`). |
| `cache-type-k`, `cache-type-v` | `f16`, `bf16`, `q8_0`, `q4_0`; both equal | KV cache precision (advanced, one control) | `q8_0` halves the KV memory with a small quality loss; on some GPUs prompt processing is slower. |
| `ubatch-size` | 32 .. 8192 | Micro-batch (advanced) | Compute memory and prompt speed. For embedding / reranking models with `mean`, `cls` or `rank` pooling it is also the longest input accepted; with `last` pooling it affects only speed and compute memory. |
| `embeddings` | `true`, `false` | Model role: Embeddings | |
| `pooling` | `none`, `mean`, `cls`, `last`, `rank` | Model role: Embeddings | |
| `reranking` | `true`, `false` | Model role: Reranking | |

The model role decides which API endpoints work for the model (chat completions for a
chat model, `/v1/embeddings` for an embedding model, `/v1/rerank` for a reranker).

Rules (a violation is refused with `400` and nothing is saved):

- `cache-type-k` and `cache-type-v` must be equal (the dialog shows one control);
- a quantized KV cache needs an effective `flash-attn` other than `off`;
- `parallel` is not 0; `device` is `none` alone or names without duplicates;
- every value is checked for type and range; unknown keys are refused.

Only warnings: `pooling` without embeddings or reranking, a context above what the
model was trained for, a `ubatch-size` above `ctx-size`.

The dialog does not store a value that equals the built-in default while `models.ini`
does not set the key: choosing "Auto (4 slots)" for parallel requests or `f16` for the
KV cache removes the override instead. The server treats "not set" and "built-in
default" as the same value: such a value sent through the API is saved (it is listed in
`overrides`) but not passed to the model process, so it never restarts a model. A
`LLAMA_ARG_*` variable in the server's environment can get in the way of this, see
[6.3](#63-where-a-value-comes-from).

Other options (chat template, sampling defaults, aliases, tags, `load-on-startup`,
paths, ...) are not part of the per-model settings; they stay in `models.ini`.

### 6.2 Context size: one shared pool or per request

The dialog has two modes for the context:

- **Shared pool**: you set `ctx-size`, the total number of tokens the model keeps for
  all its slots together. One request can grow up to `min(pool, trained context)`
  (`context.max_per_request` in the API); when all K slots are busy each gets about
  `pool / K` (`context.per_request_all_busy`). When the pool is full the requests in
  progress fail with "context size has been exceeded". The panel warns
  (`pool_overcommit`) when `pool / K` is below `min(trained context, 4096)`.
- **Per request x slots**: you set the tokens one request may use and the dialog writes
  `ctx-size = K x per_request` and `kv-unified-per-slot = per_request`. Every slot is
  then capped at `per_request` tokens (and at the trained context), so K requests of
  that length fit at the same time.

An explicit number of parallel requests also makes the KV cache one shared pool
(`kv-unified`), unless `models.ini` sets `kv-unified` for that model itself. That keeps
"context size" meaning the same in both cases; the coupled key is not stored.

### 6.3 Where a value comes from

Every field in the dialog shows the value the model gets and where it comes from, for
example "8,192 tokens from models.ini" or "Auto (4 slots), built-in default"; the other
sources read "GPU for new models", "MODEL_DEVICES" and "server command line". A value
saved in the panel is "custom", with an "x" that removes it again.

`device`, highest first:

| # | Source | Locked? | Origin in the API |
|---|---|---|---|
| 1 | `--device` on the router command line | yes | `router_cli` |
| 2 | `MODEL_DEVICES` entry for this `models.ini` section (Docker) | yes | `model_devices` |
| 3 | `MODEL_DEVICES` `*=` entry, for `models.ini` sections (Docker) | yes | `model_devices` |
| 4 | per-model setting from the panel | | `admin` |
| 5 | `device` in the model's `models.ini` section | | `models_ini` |
| 6 | `device` in `[*]` | | `models_ini` |
| 7 | "GPU for new models" (downloaded / dropped-in models only) | | `default_device` |
| 8 | `PROVISION_ADMIN_DEFAULT_DEVICE` (start value of 7) | | `default_device` |
| 9 | nothing: all visible GPUs, split by free memory | | `default` |

Every other key, highest first: router command line (locked), per-model setting from
the panel, the model's `models.ini` section, `[*]`, the built-in default (`default`).

**Do not set `LLAMA_ARG_*` variables for the keys of [6.1](#61-which-models-and-which-settings)**
(for example `LLAMA_ARG_FLASH_ATTN`, `LLAMA_ARG_N_PARALLEL`, `LLAMA_ARG_CTX_SIZE`,
`LLAMA_ARG_CACHE_TYPE_K`) in the server's environment. The model processes inherit
them, but the panel does not see them: it shows the built-in default as the effective
value and checks the rules of 6.1 against that, and a panel value that equals the
built-in default ("Auto" parallel requests, flash attention `auto`, an `f16` KV cache,
...) is not passed to the model process, so the variable still wins over it. Put such a
value into `[*]` of `models.ini` instead, where the panel sees it and can override it.

### 6.4 Locked fields

| Lock | Applies to | Shown as | `POST` |
|---|---|---|---|
| `MODEL_DEVICES` (Docker entrypoint) | `device` of the `models.ini` sections it was applied to | "Set by the MODEL_DEVICES environment variable of the deployment. Change it there and restart the server." | setting a value: `409 field_locked`; `null` (remove) is allowed |
| router command line | every key of the whitelist given on the server command line | "Set on the server command line, which applies to every model. Change it there." | same |

The server reports a `MODEL_DEVICES` lock only for a model that is a section of
`models.ini` and only when the section really carries that device when the server reads
it (the entrypoint patched it in). Otherwise it reports the warning
`model_devices_not_applied` instead of a lock, for example with a `MODEL_DEVICES` set on
a host where no entrypoint applies it. Entry names that are not sections are ignored
with a warning, as the entrypoint does.

A custom value saved before a lock appeared is kept and shown as not active; remove it
with the "x" (or `null`).

### 6.5 Saving and restarting

| Model state | Main button | Other | What happens |
|---|---|---|---|
| not loaded | **Save** | **Save and load** | Saved; the next load (a request, or **Save and load** right away) uses the new values. |
| failed to start | **Save and load** | **Save** | As above. |
| loaded / loading / sleeping | **Save and restart** | **Restart when idle** | Confirmation first: it shows how many of the model's slots are busy ("2 of 4 slots busy"), read from `GET /slots`. Requests in progress on this model are interrupted. "Restart when idle" waits up to 2 minutes for all slots to be free. |
| no effective change | Save is disabled ("No changes") | | |

A running model is never changed silently: the API refuses a change that needs a
restart unless `restart: true` is sent (`409 restart_required`), and nothing is saved
then.

Order of events for **Save and restart**:

1. The model is stopped first. The server waits until it is down, at most its stop
   timeout plus 10 seconds (the stop timeout is 10 s unless `stop-timeout` is set in
   `models.ini`). If it does not stop in that time (typically a GPU fault: the process
   hangs in the driver), the answer is `504 stop_timeout`, nothing is saved and nothing
   else is touched; restart the container or the machine.
2. The new values are saved and the model list is rebuilt. This is the same as
   **Rescan**: changes in `models.ini`, the model cache or the models directory that
   were not picked up yet are picked up now. Another model that was loaded before and
   is not running after that (its arguments changed by such a file change) is loaded
   again (`restarted_also`); one that could not be loaded again is listed under
   `not_restarted`.
3. The model is started with the new values. If the limit of loaded models is reached
   and nothing can be unloaded (`model limit reached`), the values stay saved and the
   answer carries `load_error`; the model starts on its next request.

The dialog follows the start on the live feed: Stopping, Starting, Loading x%, Ready.
Chat tabs pick up the new context size of the model as soon as it is loaded again.

Changing settings of a model that is not running, and the save itself, never restart
other models except as described in step 2.

### 6.6 Trial, commit and automatic revert

New values are not trusted until the model has started with them. Every saved entry has
a phase:

| Phase | Meaning |
|---|---|
| `pending` | saved, not tried yet (the model was not running) |
| `trial` | a start with these values is under way |
| `committed` | the model reached "loaded" with these values; they become the "last good" values |
| `failed` | the model process ended before it was loaded |

- **Circuit breaker.** After two failed starts in a row with the same new values the
  server goes back to the last good values of that model (or to no custom values, if
  there are none), rebuilds the model list, logs a warning and shows `reverted` with the
  reason, the time and the values that failed in the row. A setting that breaks a model
  therefore does not make the server start it again and again. Only a start whose
  output shows an error line counts ([9](#9-when-a-model-fails-to-start)): a process
  that is stopped, killed or crashes without printing an error (reason `none`) is
  neither counted nor kept as `last_failure`, and such a model is started again on
  every request to it until the setting is changed. Its output tail is still in
  `GET /admin/models/log`.
- **At start-up.** An entry still in `trial` or `failed` (the server was stopped while a
  start with new values was under way or failing) starts with its last good values and
  is shown with `reverted_at_startup`. Entries in `pending` or `committed` apply as
  saved.
- The failure panel ([9](#9-when-a-model-fails-to-start)) also offers **Revert to previous
  settings and restart** at once (or **Revert to the settings that last worked and
  restart** when the previous values failed as well).

### 6.7 Removing custom settings

- **Remove custom settings** in the dialog removes all custom values of the model; the
  model goes back to the `models.ini` / inherited values. The confirmation lists every
  field with its old and new value. The restart rules of [6.5](#65-saving-and-restarting)
  apply. API: `POST /admin/models/settings` with `reset: true`.
- **Delete** of a downloaded model, and **Delete files** of a model of the models
  directory ([17](#17-deleting-models-of-the-models-directory)), ask "Also remove its
  saved settings" (ticked by default). Without it, a later download or upload of a model
  with the same id gets the old settings back (an upload asks first, see
  [6.9](#69-settings-for-a-model-that-is-being-added)).
- Saved settings of a model that is no longer in the list (deleted, renamed, file
  removed) are kept and listed as "saved settings for models that are not in the list"
  (`unattached`), where they can be removed. Nothing is removed automatically.

Limits: at most 256 models with custom settings, a model id of at most 256 bytes, a
value of at most 128 bytes.

### 6.8 Export and import

The dialog exports the settings of one model to a file `<model id>.settings.json`, with
`_` for the characters a file name cannot hold such as `/` and `:` ("only my custom
settings", the default, or "all current values"), and imports such a file. Import only fills the dialog: nothing is saved until you press **Save** /
**Save and restart**, with the fit check as usual. For each field the dialog shows the
current value and the value from the file, each with a checkbox.

```json
{
  "format": "provision-llm-model-settings",
  "version": 1,
  "exported_at": "2026-10-06T10:00:00Z",
  "models": [
    {
      "id": "qwen2.5-chat",
      "about": { "arch": "qwen2", "file": "Qwen2.5-7B-Instruct-Q5_K_M.gguf", "quant": "Q5_K_M", "n_ctx_train": 32768 },
      "settings": { "ctx-size": 131072, "kv-unified-per-slot": 32768, "cache-type-k": "q8_0", "cache-type-v": "q8_0" }
    }
  ]
}
```

- `settings` has exactly the shape of `settings` in `POST /admin/models/settings`.
- `about` describes the model the file was made for. When it differs from the model in
  the dialog (other architecture, quantization or file) the import asks for a
  confirmation.
- The file is not trusted: only the keys of [6.1](#61-which-models-and-which-settings)
  are read; other keys are listed and ignored. A GPU this server does not have is
  shown unticked, a locked field is skipped, a context above the trained context is
  marked. A file with another `format` or `version`, broken JSON or larger than 64 KiB
  is refused.

The same with `curl` and `jq` (see [12](#12-api-reference) for `BASE` and `KEY`). A
model id can contain `/` and `:` (`bartowski/Qwen2.5-7B-Instruct-GGUF:Q4_K_M`), so the
file name has `_` in their place:

```bash
ID=qwen2.5-chat
FILE="$(printf %s "$ID" | tr '/:' '__').settings.json"

# export: the custom settings of one model
curl -s -H "X-Admin-Key: $KEY" --get --data-urlencode "model=$ID" "$BASE/admin/models/settings" \
  | jq --arg t "$(date -u +%Y-%m-%dT%H:%M:%SZ)" '(.models[0] // .) as $m | {
        format: "provision-llm-model-settings", version: 1, exported_at: $t,
        models: [ { id: $m.id,
                    about: { arch: $m.facts.arch, file: $m.facts.files[0].name,
                             quant: $m.facts.quant, n_ctx_train: $m.facts.n_ctx_train },
                    settings: ($m.overrides | map_values(
                        if type == "string" and test("^-?[0-9]+$") then tonumber
                        elif . == "true" then true elif . == "false" then false else . end)) } ] }' \
    > "$FILE"

# import: apply "$FILE" to a model of this server (here the same one; for another model
# set ID to its id and keep FILE). As in the dialog, only the keys the model lists in
# `fields` are sent, without the ones locked on this server (fields.<key>.locked): the
# API refuses unknown keys (400) and locked ones (409 field_locked). "restart": true
# restarts the model if it is running and loads it if it is stopped (at the model limit
# that unloads another idle model); leave it out to only save (a running model then
# answers 409 restart_required).
BODY=$(curl -s -H "X-Admin-Key: $KEY" --get --data-urlencode "model=$ID" "$BASE/admin/models/settings" \
  | jq --arg id "$ID" --slurpfile f "$FILE" '
      if (.fields | type) != "object" then error("no model \($id) on this server") else . end
      | [ .fields | to_entries[] | select(.value.locked == null) | .key ] as $keys
      | { model: $id, restart: true,
          settings: ($f[0].models[0].settings | with_entries(select(.key as $k | $keys | index($k) != null))) }') &&
curl -s -X POST -H "X-Admin-Key: $KEY" -H 'Content-Type: application/json' -d "$BODY" \
    "$BASE/admin/models/settings"
```

### 6.9 Settings for a model that is being added

**Uploaded models.** The upload dialog ([16.2](#162-in-the-panel)) has the placement
group of the settings: GPU, context size and parallel requests, with the role
(embeddings, reranking) under **More**. Through the API every key of
[6.1](#61-which-models-and-which-settings) is accepted, with the rules of
`POST /admin/models/settings`. The values travel with the upload. Right before the files
become visible the server saves them as the model's per-model settings (phase
`pending`), so the first start of the model already uses them, and the trial, commit and
automatic revert of [6.6](#66-trial-commit-and-automatic-revert) apply as to any saved
value. Everything else is set later in the Settings dialog. Cancelling the upload, its
rejection or its expiry drops the values with it. The values are checked again right
before publishing, against the GPUs and locks of that moment (`settings_invalid` when
one no longer fits, for example its GPU is gone).

- **Locks.** A field that is locked for every new model, the GPU by a `MODEL_DEVICES`
  `*=` entry in the `[*]` section of `models.ini` or any key given on the router command
  line ([6.4](#64-locked-fields)), is shown with its lock already when the upload is
  created (`locks` of the create answer, worked out from `[*]` and the command line for
  the id the model will get); a value for it is refused (`409 field_locked`).
- **Saved settings of an earlier model with the same id** (deleted or removed with its
  settings kept, an `orphaned` entry of [6.7](#67-removing-custom-settings)): the dialog
  shows them and asks "Start fresh" / "Keep them". **Start fresh** (the default) drops
  them at publishing; **Keep them** uses them again, with the values of the dialog on
  top. Either way the entry is saved again as `pending`, so the first start of the new
  files is a trial with the automatic revert of
  [6.6](#66-trial-commit-and-automatic-revert).
- **Admin state directory not writable.** The values cannot be saved, so they would be
  lost at the next restart of the server. The dialog says so when the upload is
  created, and the upload stops before publishing as `ready` (`settings_not_saved`):
  publish it without the placement (the model then starts with its inherited values), or
  fix the directory and publish.
- **After publishing**, when a placement was chosen, the upload card runs the fit check
  ([8](#8-will-it-fit)) of the new model at once and shows its verdict; without a
  placement it runs no fit check (**Open settings** has the full check). Before the
  upload only a rough estimate from the file sizes is possible.

**Downloaded models.** The download dialog has no placement of its own. When a download
finishes successfully while the Admin page is open, the page opens the Settings dialog
of the new model (as soon as no other dialog is open), so its GPU and context can be set
right away. A failed or cancelled download, or one that ends while the page is closed,
opens nothing. Until they are saved, a downloaded model gets "GPU for new models"
([5.3](#53-gpu-for-new-models-default_device)) and the `[*]` defaults of `models.ini`.

## 7. GPUs and memory

The Models tab shows one card per GPU: name, a used / total bar, the free memory, and
the models placed on that GPU with their memory. Models without a GPU of their own are
listed on every card as "all GPUs". The cards refresh when the tab is opened, after a
model is loaded, unloaded or failed, after a save, with the **Refresh** button and every
15 seconds while the tab is visible.

Everything is measured on the machine the server runs on, at run time. The router
process itself never creates a GPU context (it only reads device names from the GPU
backends); measurements that need one run in short-lived helper processes.

### 7.1 Which GPUs the server has

**Inventory.** When the router starts, before any model is loaded, it runs a helper
process of its own binary. The helper loads the GPU backends the binary was built with
(CUDA, HIP/ROCm, Vulkan, SYCL, ...), and for every GPU reports its name (`CUDA0`,
`ROCm0`, `Vulkan0`, `SYCL0`, ...), backend, description, type (`gpu`, `igpu`,
`accel`), PCI bus id (when the backend reports one; CUDA, HIP, MUSA and Vulkan do) and
its free and total memory. It creates a short-lived context on each GPU for that, so it
needs a little GPU memory for a second or two, and exits. The result is kept until the
next inventory.

**Re-detect GPUs** (button on the GPU cards, `POST /admin/gpus/rescan`) runs the
inventory again, for example after a driver update or when a GPU was added. It briefly
needs a little memory on every GPU, also on GPUs where models are running. Only one runs
at a time.

If the helper fails or does not answer within about 20 seconds, the server falls back
to the device names of its backends (no PCI ids, no memory) and says why
(`inventory.error`).

### 7.2 Live memory

The memory figures stay current through the GPU vendor's own tool, run as a separate
process, without a GPU context:

| GPUs | Tool | Notes |
|---|---|---|
| NVIDIA | `nvidia-smi` | Any number and mix of GPUs. MIG and vGPU rows are shown as unsupported. In Docker it is mounted by the NVIDIA Container Toolkit when `NVIDIA_DRIVER_CAPABILITIES` contains `utility` (the compose file sets it). |
| AMD | `amd-smi` (JSON), else `rocm-smi --showbus --showmeminfo vram --showproductname --json` | Implemented from the documented output formats and tested with recorded output only. |
| Intel, Vulkan devices without a vendor tool, any GPU without a PCI id | none | The inventory values, "measured ... ago, no live reading" ("memory is shown as measured at startup or at the last re-detect"). |

With `PROVISION_ADMIN_GPU_PROBE=auto` the server runs the tool of every GPU vendor the
inventory found, from `PATH`. `amd-smi` and `rocm-smi` are also looked for in
`$ROCM_PATH/bin` and `/opt/rocm/bin`, where a stock ROCm installation puts them and which
is not on the `PATH` of a systemd service. The variable
([3.1](#31-server-environment-variables)) can name the tool or switch it off. The tool is
started at most once at a time, and one run may take up to 15 seconds (`nvidia-smi` can
be that slow on its first call when nothing keeps the driver loaded). A request waits
for it at most 5 seconds (the card then says `timeout`); the late answer is used by the
next request. A tool that is still running after its 15 seconds (a GPU fault can make
it block in the driver) is reported as `probe_stuck` with its age instead of being
started again.

**Mapping.** Rows of the vendor tool are matched to the server's GPUs by PCI bus id,
never by index or order. `CUDA_DEVICE_ORDER`, `CUDA_VISIBLE_DEVICES`,
`HIP_VISIBLE_DEVICES` and Docker's `device_ids` renumbering therefore do not matter. The
mapping state is shown on the cards:

| `mapping` | Meaning |
|---|---|
| `pci` | matched by PCI bus id |
| `single` | exactly one GPU and one row of the tool |
| `unknown` | could not be matched; the tool's rows are listed separately, the card shows the inventory values, and the fit check uses its own reading of the free memory ([8](#8-will-it-fit)) |

The overall mapping of `GET /admin/gpus` is one of these, or `mixed` (GPUs matched in
different ways) or `none` (no GPU).

A GPU that is hidden from the server (for example by `CUDA_VISIBLE_DEVICES`) is simply a
row of the tool without a match.

While a model on a GPU is loading or stopping, and for about 1.5 seconds after, the
card is marked as settling: the free memory is not final yet.

### 7.3 Memory per model

The segments of a card, per model, with their source:

| Source | Shown as | From |
|---|---|---|
| `reported` | "reported by the model" | the model process's own memory breakdown (model, context and compute buffers per device), read from its output. Works for every backend, in Docker and on the host, but llama.cpp prints this breakdown only at log verbosity 4, so by default the card uses the next source. Setting `LLAMA_ARG_LOG_VERBOSITY=4` in the server's environment is optional (the model processes inherit it; the log gets considerably longer); nothing else depends on it. |
| `measured` | "measured" | host with NVIDIA only: `nvidia-smi --query-compute-apps` matched to the PIDs of the model processes. In Docker the PIDs do not match and this source is skipped. |
| `estimate` | "estimate" | what a fit check ([8](#8-will-it-fit)) of exactly the model's current settings reported for its GPUs, if one was made (a check made while the model was running counts only with llama.cpp's fit off, because the fit saw the model's own memory as taken); else a rough calculation from the model file and the settings. |

The API names the source in `memory.source` and, more exactly, in `memory.by`: `model`
(reported), `nvidia-smi` (measured), `llama.cpp` (estimate from a fit check) or
`formula` (estimate from the model file). Until a model's figures are known it reads
"estimate pending" in the list of the card, and its memory is part of the segment
"<model> (estimate pending) and other use". The rest of the used memory is "other"
(driver, runtime, other processes).

### 7.4 GPU identity: moved and missing GPUs

GPU names such as `CUDA0` are positions, not identities: when a GPU is removed or
`device_ids` changes, `CUDA0` can become another card. When a per-model GPU is saved the
server records its PCI bus id. The row then reports:

- `device_moved`: the name now belongs to a GPU with another PCI bus id ("saved for
  0000:01:00.0, CUDA0 is now 0000:02:00.0"). The value is still applied; check it.
- `device_missing`: the server no longer has a GPU of that name. The saved value is not
  applied and the model uses its inherited GPU (`models.ini`, "GPU for new models" or
  all GPUs) until you choose another one.

### 7.5 When there is no GPU information

| Situation | What the panel says |
|---|---|
| CPU-only build: no GPU backend library next to the binary and no GPUs | "CPU only": "This server build has no GPU backend, so models run on the CPU"; the GPU choice offers CPU only |
| A GPU backend library is there but found no GPU | "GPU backend did not initialise", with the hint to check the driver, permissions such as /dev/kfd or /dev/dri, or whether the container was started without GPUs |
| No vendor tool (or `PROVISION_ADMIN_GPU_PROBE=off`) | the inventory values with "memory is shown as measured at startup or at the last re-detect"; in Docker with NVIDIA additionally the hint to add `utility` to `NVIDIA_DRIVER_CAPABILITIES` |
| Inventory failed or timed out | "GPU detection failed" with the reason; GPU names only; **Re-detect GPUs** |
| The vendor tool failed | its first error line on the card, in red |

## 8. Will it fit?

The Settings dialog checks the values while you edit them, about 1.5 seconds after the
last change and when the dialog opens.

**Checked by llama.cpp.** The server starts a short helper process of its own binary
with exactly the arguments the model would get with the values in the dialog. The
helper uses llama.cpp's own fit machinery (the one behind `--fit` and
`llama-fit-params`): it builds the model and its context without allocating the
weights and reports, per GPU, how much the model, its context (KV cache) and its
compute buffers need, plus the host memory. It takes a few seconds and a short-lived
context on the GPUs the model would use. One check runs at a time, and a check that
takes longer than 30 seconds is stopped. A helper that does not exit after it was
stopped (a process stuck in the GPU driver) blocks further checks with
`409 check_stuck` until it exits; Diagnostics lists it ([10](#10-diagnostics)).

llama.cpp's fit adapts the values left on `auto` (GPU layers, context) to the memory
that is free at that moment, so a result is reused only for 60 seconds, and only while
the model's effective arguments, the inventory and the model's state (running or not)
stay the same and the live free memory of its GPUs moved by less than 256 MiB. A check
of a running model is marked `checked_while_running`: its fit saw the model's own
memory as taken, so with `auto` values it may plan fewer GPU layers or a smaller
context than the model gets after the restart.

**Verdict per GPU**, compared with the memory available there: the free memory now
(vendor tool, else the helper's own reading), plus what the model itself uses on that
GPU if it is running and will be restarted. Other models are never counted as
reclaimable.

| Verdict | Condition | Shown |
|---|---|---|
| `fits` | need + 1 GiB margin <= available | "Fits on CUDA1: 3.1 GiB left" |
| `tight` | need <= available < need + 1 GiB | "Fits on CUDA1, but only 0.4 GiB left" |
| `no` | need > available | "Probably does not fit on CUDA1: short by 1.2 GiB"; the restart button becomes **Restart anyway** |
| `unknown` | the memory of that GPU is not known or still settling, or the check failed | the need is still shown |
| `cpu_only` | top-level verdict only: the model uses no GPU (a CPU-only build, or `device` `none`) | "Runs on the CPU only", with the system memory it needs |

The verdict only warns, it never blocks a save: the margin is the same 1 GiB as
llama.cpp's own fit uses, other processes can change the free memory at any time, and a
model that does not fit fails alone: the server and the other models keep running, the
failure panel shows why, and the automatic revert ([6.6](#66-trial-commit-and-automatic-revert))
takes it back.

The check also catches impossible combinations before anything is saved (for example a
quantized KV cache with flash attention off: `invalid_settings`). Its errors:
`invalid_settings`, `out_of_memory`, `invalid_device`, `model_load_failed`,
`device_missing`, `timeout`, `failed` (the helper could not be started), `unsupported`.

**Rough estimate.** While the check runs, or when it fails, the dialog shows an
estimate calculated from the model file and the settings, labelled "rough estimate".
It is never shown as a measured value.

The dialog also shows the context figures of [6.2](#62-context-size-one-shared-pool-or-per-request):
"Up to N tokens per request; when all K slots are busy about M each".

## 9. When a model fails to start

The router keeps the last 200 lines (at most 64 KiB) of the output of every model
process in memory, without terminal colour codes, for at most 64 models. When a process
ends before the model was loaded (or a loaded model's process ends with an error), the
row in the Models tab shows the reason instead of only "Failed (exit code 1)", with
**Show log**. The Settings dialog shows the same
failure panel after a failed **Save and restart**: the reason, a hint, the output tail
with **Copy**, and **Revert to previous settings and restart**, **Edit settings**,
**Close**.

The reason comes from the last error lines of the output (the last line that names a
specific cause wins):

| Reason | Typical text in the output |
|---|---|
| `out_of_memory` | `out of memory` (e.g. `CUDA error: out of memory`), `... malloc failed` (`cudaMalloc`, `hipMalloc`), `failed to allocate ...`, `unable to allocate ... buffer`, Vulkan `ErrorOutOfDeviceMemory` |
| `invalid_device` | `invalid device: ...` |
| `invalid_settings` | `quantized V cache requires flash_attn`, `K cache type q8_0 with block size 32 does not divide n_embd_head_k` (a quantized KV cache that the head size of this model does not allow), an invalid argument value |
| `model_load_failed` | `error loading model`, `failed to load model`, a missing or broken GGUF file; counts only when no line names a more specific cause |
| `unknown` | another error line: read the output tail |
| `none` | no error line: the process was stopped or killed |

The warning `failed to fit params to free device memory` that llama.cpp prints when
its projection is short of memory is only a hint (`memory_tight`): the load often
continues and succeeds, and it never outranks a real error.

The last failure of each model (time, reason, output tail) is kept separately until a
later failure replaces it. A client request that starts the model again (autoload) does
not wipe it: as long as the settings are broken, every new request to that model tries
to start it again, and the panel keeps showing the latest failure. The automatic revert
([6.6](#66-trial-commit-and-automatic-revert)) stops this after two failures with an
error line; a process that ends without one (`none`) is not counted.

The tail is the same text that `docker compose logs llama-dual` / `journalctl -u
provision-llm` show; it is available only with the admin key.

A model of the models directory whose projector does not fit it (the wrong `mmproj` file
was uploaded or dropped in; the upload checks cannot see that,
[16.4](#164-checks-before-the-model-becomes-visible)) fails at every start.
**Delete files** -> **Remove only the projector** ([17.4](#174-remove-projector)) keeps
the model and drops only the projector.

## 10. Diagnostics

Settings tab, **Diagnostics** (next to the server panel), and `GET /admin/diagnostics`:
what the server found on this machine, each item with a status (`ok`, `warning`,
`error`, `info`) and a one-line hint. **Copy report** copies everything as text.

| Item (`id`) | What it shows | Example hint for a problem |
|---|---|---|
| Deployment (`deployment`) | `container` or `host` (from `/.dockerenv`, `/run/.containerenv` or the cgroup of process 1) | |
| Server binary (`binary`) | version and path | |
| GPU backend (`gpu_backend`) | the GPU backend libraries next to the binary, the backends and how many GPUs they found | "the container may have been started without GPUs (docker: --gpus / device_ids); check the GPU driver and permissions (/dev/nvidia*, /dev/kfd, /dev/dri)" |
| Device inventory (`inventory`) | result and time of the GPU inventory | the reason it failed |
| GPU memory tool (`vendor_tool`) | `PROVISION_ADMIN_GPU_PROBE`, the tools found (path, version, last error) | "nvidia-smi is not available inside the container: add NVIDIA_DRIVER_CAPABILITIES=compute,utility" |
| GPU memory matched to devices (`memory_mapping`) | `pci`, `single`, `unknown`, `mixed`, `none` | "some GPUs could not be matched: their memory is shown as measured by the device inventory" |
| Router without GPU context (`router_gpu_free`) | on a host with NVIDIA: the router's own PID is not among the GPU processes of `nvidia-smi` (in a container it cannot be checked: `info`) | |
| Admin state directory (`state_dir`) | path, writable, looks persistent (in a container: a mount of its own) | "not a volume: the settings are lost when the container is recreated"; "set PROVISION_ADMIN_STATE_DIR to a writable directory" |
| Admin key (`admin_key`) | where it comes from | "no admin key: set PROVISION_ADMIN_KEY or make the state directory writable" |
| Requests through a reverse proxy (`proxy`) | whether an admin request carried `X-Forwarded-For` | "no X-Forwarded-For seen: requests reach the server directly; lock /admin in the reverse proxy or with PROVISION_ADMIN_ALLOW_FROM for remote use" |
| Admin address allow-list (`ip_allowlist`) | the built-in allow-list ([4.3](#43-built-in-address-allow-list-hosts-without-nginx)): `enabled`, `entries`, `trusted_proxies`, and for this request `seen_from` (the address the allow-list checks), `peer` (the TCP peer), `via_forwarded_for`, `note` (why a trusted proxy's `X-Forwarded-For` was not used). `info` while it is off | "PROVISION_ADMIN_ALLOW_FROM: '10.64.0.5/24' has host bits set, it is read as 10.64.0.0/24"; "the trusted proxy 10.64.0.1 is also on the allow-list: ..."; "the trusted proxy range 192.168.0.0/16 is wide: every address in it can claim any client address ..."; "the CORS proxy (--ui-mcp-proxy / --agent) is on: ..."; with trusted proxies but no allow-list: "PROVISION_ADMIN_TRUSTED_PROXIES without PROVISION_ADMIN_ALLOW_FROM only affects the client address the audit log records: any address may use /admin" ([4.3](#43-built-in-address-allow-list-hosts-without-nginx)) |
| `MODEL_DEVICES` (`model_devices`) | its value when set | |
| Per-model settings file (`model_settings`) | file, entries, read-only, `.bak` used, problems | "fix or remove the file, then restart the server" |
| Audit log (`audit`) | the activity log ([18](#18-activity-log)): `persisted`, `error` and `since` (why and since when it is kept in memory), `in_memory` (records waiting in memory), `lost`, `max_bytes` (rotation size), `ring_max`, `files` (name, bytes, store). `warning` while it is kept in memory | "the audit log is kept in memory only (the last 1000 records) since ...: ...; it is written again by itself once the state directory can be written (tried every minute), records kept in memory meanwhile are lost on restart" |
| Helper processes (`stuck_helpers`) | listed only while a fit check or GPU inventory helper has not exited after it was stopped (status `error`): which one and for how long | "the GPU driver may be hanging (check dmesg), restart the server if it does not clear" |

Nothing has to be sent anywhere: the panel shows on each server what was detected there
and why something is unknown.

## 11. Storage

| In the container | On the host (compose default) | Host installation ([13](#13-deployment-on-a-linux-host)) | Content |
|---|---|---|---|
| `/models` (read-only) | `./models` | `/srv/provision-llm/models` | the GGUF files of the `models.ini` sections |
| `/models-rw/hf` | `./models-rw/hf` | `/srv/provision-llm/models-rw/hf` | model cache: everything downloaded with `POST /models` |
| `/models-rw/local` | `./models-rw/local` | `/srv/provision-llm/models-rw/local` | models directory: drop files here; uploads from the panel go here too, and `.provision-upload/` in it holds unfinished uploads |
| `/var/lib/provision-llm-admin` | volume `llama-admin-state` | `/var/lib/provision-llm-admin` | `admin-state.json`, `admin-models.json`, their `.bak` files, `admin.key`, the records of unfinished uploads, the activity log (`admin-audit*.jsonl*`) |
| `/var/lib/provision-llm` | volume `llama-license-state` | `/var/lib/provision-llm` | license state |

**Model cache** (`hf/`). Hugging Face layout, one directory per repository:

```
hf/models--<user>--<repo>/
    blobs/<hash>                      the data; <hash>.downloadInProgress while downloading
    refs/main                         commit the snapshot belongs to
    snapshots/<commit>/<file>.gguf    relative symlink into blobs/
```

A downloaded model appears in the model list as `<user>/<repo>:<QUANT>` with source
`cache`. `DELETE /models` removes the files of that model from the cache. An
interrupted download leaves a `.downloadInProgress` file; `GET /admin/storage` lists
these, and starting the same download again continues where it stopped.

**Models directory** (`local/`). `name.gguf` becomes the model `name` (every `.gguf` in
the file name is removed: `a.gguf.x.gguf` becomes `a.x`); a subdirectory with a GGUF
file (and optionally an `mmproj` file) becomes a model named after the directory. Files
at the top level whose name contains `mmproj` or starts with `mtp-`, `dspark-` or
`dflash-` are not listed, and deeper subdirectories are not looked at. New files show
up after a rescan (`GET /v1/models?reload=1`, the **Rescan** button). Uploads from the
panel are put here ([16](#16-uploading-gguf-files)). These models can be deleted in the
panel, only the files the model uses ([17](#17-deleting-models-of-the-models-directory)),
or by hand: remove the files and rescan.

**Unfinished uploads** (`local/.provision-upload/`). The server keeps the data of uploads
that are not published yet in this folder of the models directory, one subfolder per
upload with its `.part` files, and moves the files of a deleted model into a `trash-*`
subfolder there before it removes them. The model list never shows anything from it:
the scan looks only at the files directly inside the first level of subfolders, and this
folder holds only folders. Do not put files into it, do not edit it and do not mount
anything over it; the server removes what it no longer needs: an upload 72 hours after
its last progress, counted while the server runs; a trash folder as soon as it is
written (and a leftover one at the next start); a subfolder that belongs to no upload
after an hour, unless another server holds it (see below). Each upload subfolder holds
its `.part` files, a `.lock` file and a `session.json` hint (offsets only, the id of
the server that writes it and how long the upload has been idle); the server checks the
hint against the files and never trusts it alone. The record of each upload (name,
settings, state) is `<state dir>/uploads/<id>.json` in the admin state directory, next
to `<state dir>/uploads/owner`, the id this server writes into its hints; the folder
holds only the data. It must be on the same filesystem as the models directory, which
the server checks. `GET /admin/storage` lists the uploads with their size.

Several servers that share one models directory leave each other's uploads alone
([16.6](#166-resuming)): a server never takes over or removes an upload whose hint names
another server; it removes such a folder only when that upload could have expired
(72 hours minus the idle time in its hint) plus 24 hours have passed since its hint was
last written, and only when no process holds its lock. Each of them must be able to
list this folder: on Linux the server gives `.provision-upload` and its subfolders mode
`0770` and its files `0660` (group read and write); see **Ownership** below for the
owner and group.

Both kinds of added models start with the `[*]` defaults of `models.ini` (context
size, GPU layers, flash attention, offline) plus the GPU from
[5.3](#53-gpu-for-new-models-default_device). Their own GPU, context size, role
(embeddings, pooling) and the other settings of [6.1](#61-which-models-and-which-settings)
are set in the panel; options outside that list still need a section in `models.ini`.

**Ownership.** In Docker the server runs as root, so what it downloads into `hf/`
belongs to root on the host: remove downloaded models through the API
(`DELETE /models`, **Delete** in the panel) rather than by hand. Create the two
subdirectories yourself before the first start, so that you own them and can drop
files into `local/` without `sudo`:

```bash
mkdir -p models-rw/hf models-rw/local
```

If they are missing, the entrypoint creates them and gives them the owner of
`models-rw`, which is root when Docker had to create `models-rw` itself.

Files and folders that the server publishes after an upload, and everything in
`.provision-upload`, are made group-writable (folders `0770`, files `0660`; folders also
get the set-group-id bit when the models directory has it). When the server runs as root
and the models directory belongs to another user (Docker: the owner of `local/`), it also
gives them the owner and group of the models directory, before they become visible, so
that you can manage them like files you dropped in yourself. A server that runs as its
own user (the host installation) keeps its own user and the group the system gives new
files. Where the owner cannot be changed (for example an NFS share that maps root to
`nobody`) the upload goes on and carries the warning `chown_failed`. Rootless Docker
needs nothing: the container's root is your own user on the host.

**Disk space.** There is no quota and no reserve. The pre-flight check compares the
size of a download with the free space and warns; it never blocks a download. An upload
does the same ([16.8](#168-disk-space)): it warns, and the server refuses only a write
that actually fails (`507`, the upload is kept). On the default layouts the models
directory, the model cache, the admin state and the license state are often on one
filesystem, so a full disk affects all of them; on Linux the upload warns when the models
directory shares its filesystem with the admin state (it does not compare it with the
license state or the model cache). The license counter, its ledger, the installed
license file and the machine id are written so that a failed write leaves the previous
file intact (every write, the sync and the close are checked, and a short file is never
renamed over a good one): a full disk does not make a licensed server report its state
as tampered. If a full disk would hurt other services, point `MODELS_RW_DIR` at a
filesystem of its own.
The filesystem should support symbolic links (any native Linux filesystem does);
without them the cache falls back to moving files and still works.

**State.** `admin-state.json` (server-wide settings) and `admin-models.json` (per-model
settings) are written atomically; the previous version of each is kept as `.bak`.

- A damaged `admin-state.json` does not stop the server: it starts with the start-up
  defaults, reports the problem in `GET /admin/status` (`state.error`) and leaves the
  file alone until settings are saved again.
- A damaged `admin-models.json`, or one written by a newer version: the server tries
  the `.bak` at start-up and says so in `GET /admin/status`
  (`state.model_settings`). The file is then **read-only** for the panel: saving
  per-model settings answers `409 model_settings_read_only` until the file is fixed or
  removed. Data the server could not read is never overwritten.
- Keys the server does not know (written by a newer version) are kept when the file is
  rewritten.
- `admin-state.json` and `admin-models.json` are separate on purpose: an older image
  ignores `admin-models.json` and leaves it alone, and a newer one finds it again.

**Activity log.** `admin-audit.jsonl`, for the refused requests
`admin-audit-refusals.jsonl` and for the effects of the public model routes
`admin-audit-observed.jsonl`, each with its previous generation `.1`, in the admin state
directory: appended and synced per record, rotated at 4 MiB, at most 24 MiB in all,
mode `0600` ([18.5](#185-storage-rotation-and-retention)). While they cannot be written
the log is kept in memory ([18.6](#186-when-the-log-cannot-be-written)).

To reset the settings and the key, stop the server, remove the files and start it
again. Not while it runs: the server keeps the settings in memory and writes
`admin-models.json` again whenever a model with saved settings starts or loads, so
what you remove or restore under a running server can be overwritten again.

```bash
# Docker: the files are removed by a one-off container on the same volume
docker compose stop llama-dual
docker compose run --rm --no-deps -T --entrypoint sh llama-dual -c \
    'rm -f /var/lib/provision-llm-admin/admin-state.json* /var/lib/provision-llm-admin/admin-models.json* /var/lib/provision-llm-admin/admin.key'
docker compose start llama-dual
# host
sudo systemctl stop provision-llm
sudo rm -f /var/lib/provision-llm-admin/admin-state.json* /var/lib/provision-llm-admin/admin-models.json* /var/lib/provision-llm-admin/admin.key
sudo systemctl start provision-llm
```

Leave out `admin.key` to keep the key, or remove only `admin-models.json*` to drop the
per-model settings. Removing the whole state also drops the records of unfinished
uploads; their data in `.provision-upload/` is then removed by the server. The commands
above leave the activity log alone; add `admin-audit*.jsonl*` to clear it as well. Or
remove the whole Docker volume (`docker volume ls` shows its name):

```bash
docker compose down
docker volume rm <project>_llama-admin-state
docker compose up -d
```

Do **not** use `docker compose down -v` for this: it deletes every volume of the
stack, the license volume `llama-license-state` included, and a licensed container
then needs a new activation. Either way the next start uses the compose defaults and
issues a new key.

## 12. API reference

JSON in, JSON out. The machine-readable description is in the server's OpenAPI
document (`/openapi.json`, Swagger UI at `/docs`, tag **Admin**). Every answer of an
admin endpoint carries `Cache-Control: no-store`.

Errors use the server's usual shape; per-model validation and lock errors also name the
key in `param`, and errors with more detail carry it next to `type` (for example `by` of
`name_taken`, `offset` of `offset_mismatch`):

```json
{ "error": { "code": 401, "message": "a valid X-Admin-Key header is required", "type": "admin_key_required" } }
```

| Status | `error.type` | When |
|---|---|---|
| 400 | `invalid_request_error` | body is not valid, or a value fails validation; nothing is saved |
| 401 | `admin_key_required` | `X-Admin-Key` missing or wrong (recorded in the activity log, [18.3](#183-refused-requests)) |
| 403 | `admin_forbidden` | the client address is not on the allow-list: answered by nginx ([4.1](#41-address-allow-list-nginx)), or by the server's own allow-list ([4.3](#43-built-in-address-allow-list-hosts-without-nginx)), whose message names the address it saw; also every `/admin` request while the server's allow-list has an invalid entry ("The admin API is locked: ..."). The server's own `403` answers are recorded in the activity log ([18.3](#183-refused-requests)), those of nginx are not |
| 404 | | the admin layer is disabled, or the server is not a router |
| 404 | `not_found_error` | unknown model id (per-model routes). The server replaces the body of every `404` with its generic `{"error": {"code": 404, "message": "File Not Found", "type": "not_found_error"}}` |
| 409 | `default_device_locked` | `default_device` is fixed by `MODEL_DEVICES` |
| 409 | `field_locked` | a per-model key is locked by `MODEL_DEVICES` or the router command line |
| 409 | `restart_required` | the model is running and the change needs a restart, but `restart` was not set |
| 409 | `model_settings_read_only` | `admin-models.json` is damaged or from a newer version |
| 409 | `check_busy` | another fit check is running |
| 409 | `check_stuck` | the previous fit check helper was stopped but has not exited (stuck in the GPU driver); no new one is started while it lives |
| 409 | `rescan_busy` | a GPU re-detection is already running |
| 409 | `rescan_stuck` | the same for the GPU inventory helper |
| 409 | `upload_unavailable`, `name_taken`, `too_many_uploads`, `offset_mismatch`, `content_mismatch`, `chunk_in_progress`, `other_client_active`, `upload_busy` | the upload routes, see [12.15](#1215-post-adminmodelsupload) and [12.16](#1216-post-adminmodelsuploadchunk) |
| 409 | `model_running`, `not_deletable` | `POST /admin/models/delete`, see [12.18](#1218-post-adminmodelsdelete); it answers `409 not_deletable` (`reason: not_found`), not `404`, for an unknown model |
| 409 | `file_dropped` | a chunk for a file that was dropped from its upload |
| 410 | `upload_gone` | the upload is unknown, expired, cancelled or rejected (the upload routes answer `410` for it, never `404`) |
| 411 | `length_required` | a chunk without `Content-Length` |
| 413 | `chunk_too_large` | a chunk larger than `chunk_max` (a `413` without this JSON comes from a proxy) |
| 415 | `unsupported_media_type` | a chunk that is not `application/octet-stream` |
| 422 | `not_gguf`, `file_rejected` | a file of an upload is not a GGUF file, or the check found it unusable |
| 429 | `upload_slots_busy` | too many chunks are being written at once |
| 507 | `disk_full` | the disk is full while a chunk is written; the upload is kept |
| 500 | `server_error` | a file could not be written (nothing was applied), or rebuilding the model list failed (`reload_error`, the previous per-model values are back, see [12.8](#128-post-adminmodelssettings)) |
| 500 | `write_failed` | an upload chunk could not be written for another reason than a full disk; the upload is kept |
| 503 | `admin_key_unavailable` | the server has no admin key |
| 504 | `stop_timeout` | the model did not stop in time; nothing was saved |

The examples assume:

```bash
BASE=http://10.64.0.5:8096
KEY=$(docker compose exec -T llama-dual cat /var/lib/provision-llm-admin/admin.key)
# host installation: KEY=$(sudo cat /var/lib/provision-llm-admin/admin.key)
```

**Older servers.** `GET /admin/status` lists the optional parts the server has in
`features`. The web UI shows the per-model settings, the GPU cards, the failure tails,
the fit check, the diagnostics, the upload and the deletion of models-directory models,
and the Activity tab only when the server lists them, so a browser that still has a newer
web UI cached keeps working against an older server.

### 12.1 `GET /admin/status`

The probe. Needs no key; the web UI calls it once to decide whether to show the Admin
page.

```bash
curl -s $BASE/admin/status
```

```json
{ "admin": true, "authorized": false, "key_configured": true }
```

With a valid key the answer is extended (more fields may be added later):

```bash
curl -s -H "X-Admin-Key: $KEY" $BASE/admin/status
```

```json
{
  "admin": true, "authorized": true, "key_configured": true, "version": 1,
  "key_source": "generated",
  "features": ["model_settings", "gpus", "model_log", "diagnostics", "fit_check", "upload", "delete", "audit"],
  "models": { "max": 3, "running": 2, "total": 5 },
  "state": {
    "dir": "/var/lib/provision-llm-admin", "writable": true, "error": "",
    "model_settings": { "file": "/var/lib/provision-llm-admin/admin-models.json", "error": "", "read_only": false, "backup_loaded": false, "entries": 1 }
  },
  "download": { "available": true, "offline": false },
  "upload": { "available": true, "reason": null, "message": null },
  "audit": { "persisted": true, "error": null, "since": null, "in_memory": 0, "lost": 0, "max_bytes": 4194304, "ring_max": 1000 },
  "dirs": { "cache": "/models-rw/hf", "models_dir": "/models-rw/local", "preset": "/config/models.ini" },
  "license_build": false,
  "ip_allowlist": false,
  "warnings": []
}
```

- `key_source`: `env`, `file` or `generated`.
- `features`: the optional parts this server has (see "Older servers" above).
- `download.available`: the binary can make HTTPS downloads and the router itself is
  not in `--offline` mode. (`offline = true` in `models.ini` does not count: it only
  concerns the model processes.)
- `upload.available`: GGUF files can be uploaded; otherwise `upload.reason` is
  `no_models_dir`, `models_dir_missing`, `not_writable`, `staging_unusable` or
  `staging_other_filesystem` ([16.1](#161-what-can-be-uploaded)), and `upload.message`
  says what to do.
- `features` `upload` and `delete`: the routes of [12.14](#1214-get-adminmodelsupload) to
  [12.18](#1218-post-adminmodelsdelete); `audit`: the activity log,
  [12.19](#1219-get-adminaudit).
- `audit`: the activity log ([18](#18-activity-log)). `persisted: false` while records
  of any of its three files are kept in memory
  ([18.6](#186-when-the-log-cannot-be-written)), with `error` and `since`; `in_memory`:
  records waiting in memory; `lost`: records the full memory rings dropped meanwhile;
  `max_bytes`: the size at which a file is rotated; `ring_max`: the size of each ring
  (one per file).
- `state.error`: why the saved settings could not be read; empty when fine.
- `state.model_settings`: the per-model settings file; `error` says why it could not be
  read, `backup_loaded` that its `.bak` was used instead, `read_only` that saving is
  refused, `entries` how many models have saved settings.
- `warnings`: `default_device_not_present` when "GPU for new models" names a GPU this
  server does not have; `audit_not_persisted` while the activity log is kept in memory
  only ([18.6](#186-when-the-log-cannot-be-written)).
- `ip_allowlist`: the server's own address allow-list is on
  ([4.3](#43-built-in-address-allow-list-hosts-without-nginx)). Only in the answer with
  a valid key; the details are in Diagnostics.

A wrong key is not an error here: the answer is the short one with
`"authorized": false`. An address the allow-list refuses (nginx or the server's own)
gets `403` `admin_forbidden` here as on every other `/admin` route.

### 12.2 `GET /admin/settings`

```bash
curl -s -H "X-Admin-Key: $KEY" $BASE/admin/settings
```

```json
{
  "models_max": 3,
  "default_device": "CUDA0",
  "default_device_locked": false,
  "pinned": ["qwen2.5-chat", "gte-qwen2-embed"],
  "limits": { "models_max_min": 1, "models_max_max": 16 },
  "known_devices": ["CUDA0", "CUDA1"],
  "build_devices": ["CUDA0", "CUDA1"],
  "warnings": [],
  "models": [
    { "id": "qwen2.5-chat", "source": "preset", "status": "loaded", "pinned": true },
    { "id": "gte-qwen2-embed", "source": "preset", "status": "loaded", "pinned": true }
  ]
}
```

- `models[].source`: `preset` (section of `models.ini`), `cache` (downloaded) or
  `models_dir` (dropped in).
- `models[].status`: `unloaded`, `loading`, `loaded`, `sleeping`, `downloading`,
  `downloaded`.
- `default_device_locked`: see [5.3](#53-gpu-for-new-models-default_device).
- `pinned`: the pins in effect that are in the model list.
- `known_devices`: devices mentioned by the model presets plus the current
  `default_device` (a hint list). `build_devices`: the GPUs this server's backends
  have, the list the GPU choices are made from (empty for a CPU-only build).
- `warnings`: `default_device_not_present` when `default_device` names a GPU this
  server does not have (it is then not applied).
- The answer may carry additional fields. Currently: `pinned_ignored` (configured pins
  that are not in effect because one slot must stay free) and `pinned_unknown`
  (configured names that are not in the model list; one among the first
  `models_max - 1` names is protected as soon as it appears, see
  [5.2](#52-pinned-models-pinned)).

### 12.3 `POST /admin/settings`

Body: any subset of `models_max`, `default_device`, `pinned`. Fields that are left out
keep their value. The request is validated as a whole; on success the settings are
saved, applied and returned in the format of `GET /admin/settings`.

```bash
curl -s -X POST -H "X-Admin-Key: $KEY" -H 'Content-Type: application/json' \
    -d '{ "models_max": 4, "pinned": ["qwen2.5-chat"] }' $BASE/admin/settings
```

| Field | Accepted |
|---|---|
| `models_max` | integer within `limits` |
| `default_device` | `""` (clear), `none`, or device names separated by `,` or `;`, each a letter followed by letters, digits or `_` and ending in a digit (`CUDA0`, `Vulkan1`) |
| `pinned` | array of ids from the model list; duplicates are removed |

Rejected with `400`: a value outside these rules, a `models_max` below the number of
models marked `load-on-startup`, or a result in which `pinned` is not shorter than
`models_max` (without `pinned` in the body the pins in effect are counted, see
[5.2](#52-pinned-models-pinned)). Rejected with `409`: `default_device` while it is
locked.

### 12.4 `GET /admin/storage`

```bash
curl -s -H "X-Admin-Key: $KEY" $BASE/admin/storage
```

```json
{
  "dirs": [
    { "kind": "cache", "path": "/models-rw/hf", "exists": true, "writable": true, "total_bytes": 480000000000, "free_bytes": 120000000000 },
    { "kind": "models_dir", "path": "/models-rw/local", "exists": true, "writable": true, "total_bytes": 480000000000, "free_bytes": 120000000000 },
    { "kind": "state", "path": "/var/lib/provision-llm-admin", "exists": true, "writable": true, "total_bytes": 100000000000, "free_bytes": 60000000000 }
  ],
  "partial_downloads": [ { "path": "/models-rw/hf/models--user--repo/blobs/<hash>.downloadInProgress", "bytes": 123456789 } ],
  "uploads": [ { "id": "9f1c2b7e4a6d4c0e8b3a5f2d1e0c9b8a", "name": "gemma-3-4b", "state": "receiving", "bytes": 1258291200,
                  "total_bytes": 3341008960, "received_bytes": 1258291200, "expires_at": "2026-10-09T10:20:00Z" } ],
  "upload_staging": { "path": "/models-rw/local/.provision-upload", "bytes": 1258291200, "trash_bytes": 0 },
  "audit": { "persisted": true, "total_bytes": 5263772, "max_bytes": 4194304,
             "files": [ { "name": "admin-audit.jsonl", "bytes": 1069056, "store": "changes" },
                        { "name": "admin-audit.jsonl.1", "bytes": 4194120, "store": "changes" },
                        { "name": "admin-audit-refusals.jsonl", "bytes": 184, "store": "refusals" },
                        { "name": "admin-audit-observed.jsonl", "bytes": 412, "store": "observed" } ] }
}
```

`total_bytes` / `free_bytes` describe the filesystem the directory is on. The
`models_dir` entry is missing when the server runs without `--models-dir`. `uploads`:
the unfinished uploads ([16](#16-uploading-gguf-files)), each as in the list of
[12.14](#1214-get-adminmodelsupload) (shortened above) plus what it takes on disk
(`bytes`); `upload_staging`: the folder of the unfinished uploads, what they take
together and what still waits in the trash of deleted models (`trash_bytes`). `audit`:
the files of the activity log ([18.5](#185-storage-rotation-and-retention)) with their
size (`store`: `changes`, `refusals` or `observed`), their total, and the rotation size.

### 12.5 `POST /admin/models/download/check`

Advisory pre-flight for a download. It downloads nothing and never blocks a download.

```bash
curl -s -X POST -H "X-Admin-Key: $KEY" -H 'Content-Type: application/json' \
    -d '{ "model": "bartowski/Qwen2.5-7B-Instruct-GGUF:Q4_K_M" }' $BASE/admin/models/download/check
```

```json
{
  "model": "bartowski/Qwen2.5-7B-Instruct-GGUF:Q4_K_M",
  "repo": "bartowski/Qwen2.5-7B-Instruct-GGUF", "quant": "Q4_K_M",
  "hf_reachable": true, "exists": true, "already_present": false,
  "files": [ { "name": "Qwen2.5-7B-Instruct-Q4_K_M.gguf", "bytes": 4683073536 } ],
  "total_bytes": 4683073536, "free_bytes": 120000000000, "enough_space": true,
  "warnings": []
}
```

`model` is `user/repo` or `user/repo:QUANT` (letters, digits, `.`, `_`, `-`); anything
else is a `400`. For a well-formed request the answer is always `200`, also when
Hugging Face cannot be reached. The server talks only to the Hugging Face endpoint it
is configured with (`MODEL_ENDPOINT`, default huggingface.co), with its `HF_TOKEN` if
there is one, for at most about 10 seconds.

| `warnings` code | Meaning |
|---|---|
| `hf_unreachable` | the Hugging Face endpoint did not answer |
| `repo_not_found` | no such repository, or it is gated / private and the token gives no access |
| `no_gguf_for_quant` | the repository has no GGUF file for this quantization |
| `preset_repository` | the repository holds a preset (`preset.ini`): only that file is downloaded, and no model appears in the list |
| `quant_not_specified` | no `:QUANT` given; the server picks one (Q4_K_M, then Q8_0, then the first GGUF file) |
| `not_enough_space` | `total_bytes` is larger than `free_bytes` |
| `already_present` | the model is already in the model list |
| `offline_mode` | the router runs with `--offline`, a download cannot work |
| `size_unknown` | the file sizes could not be determined |

`enough_space` is simply `total_bytes <= free_bytes`. `files` includes the `mmproj`
file of a multimodal model when the download would fetch it. For a preset repository
`files` lists only `preset.ini`, and `no_gguf_for_quant` and `quant_not_specified` are
not reported: the quantization plays no part in such a download.

### 12.6 Downloading and deleting (existing routes, no admin key)

```bash
# start the download; progress comes on the event feed
curl -s -X POST -H 'Content-Type: application/json' \
    -d '{ "model": "bartowski/Qwen2.5-7B-Instruct-GGUF:Q4_K_M" }' $BASE/models
curl -N $BASE/models/sse

# cancel a running download
curl -s -X POST -H 'Content-Type: application/json' \
    -d '{ "model": "bartowski/Qwen2.5-7B-Instruct-GGUF:Q4_K_M" }' $BASE/models/unload

# delete a downloaded model
curl -s -X DELETE "$BASE/models?model=bartowski/Qwen2.5-7B-Instruct-GGUF:Q4_K_M"
# ... and its saved per-model settings (what "Also remove its saved settings" does)
curl -s -X POST -H "X-Admin-Key: $KEY" -H 'Content-Type: application/json' \
    -d '{ "model": "bartowski/Qwen2.5-7B-Instruct-GGUF:Q4_K_M", "reset": true }' $BASE/admin/models/settings
```

Write the quantization tag in upper case (`:Q4_K_M`); the web UI does that for you.

`DELETE /models` deletes only downloaded models (source `cache`). A model of the models
directory is deleted with `POST /admin/models/delete` ([12.18](#1218-post-adminmodelsdelete)).
A download no longer takes an `@` in a file path of the repository for the user part of
the URL, so it cannot be sent, with `HF_TOKEN`, to another host.

These routes carry no admin key, so the activity log does not know who called them; it
records the finished or failed download and the removed model as "public API (unknown)",
and the `reset` above as a change of the admin who sent it
([18.4](#184-what-cannot-be-recorded)).

### 12.7 `GET /admin/models/settings`

All models with their per-model settings, plus saved settings of models that are not in
the list. With `?model=<id>` the answer is that one model's row (plus `state`), with the
model facts read from its file if they are not known yet. For an id that is only saved
settings (an `orphaned` entry of `unattached`: the model is gone) `?model=` answers `200`
with that entry plus `state` (`exists: false`, no `fields`), so a `404` is not the test
for "has saved settings".

```bash
curl -s -H "X-Admin-Key: $KEY" $BASE/admin/models/settings
curl -s -H "X-Admin-Key: $KEY" --get --data-urlencode "model=qwen2.5-chat" $BASE/admin/models/settings
```

```json
{
  "schema": [
    { "key": "ctx-size", "group": "placement", "default": null, "type": "int", "min": 256, "max": 1048576 },
    { "key": "cache-type-k", "group": "advanced", "default": "f16", "type": "enum", "values": ["f16", "bf16", "q8_0", "q4_0"], "pair": "cache-type-v" }
  ],
  "build_devices": ["CUDA0", "CUDA1"],
  "models": [
    {
      "id": "qwen2.5-chat", "source": "preset", "status": "loaded", "exists": true, "hidden": false,
      "overrides": { "ctx-size": "32768" }, "kept": {},
      "fields": {
        "ctx-size": { "effective": "32768", "default": null, "origin": "admin", "inherited": "8192", "inherited_origin": "models_ini",
                      "override": "32768", "locked": null, "active": true, "inactive_reason": null },
        "device": { "effective": "CUDA1", "default": null, "origin": "models_ini", "inherited": "CUDA1", "inherited_origin": "models_ini",
                    "override": null, "locked": null, "active": false, "inactive_reason": null }
      },
      "phase": "committed", "failures": 0, "last_good": { "ctx-size": "32768" }, "updated": "2026-10-06T10:00:00Z",
      "reverted": null, "reverted_at_startup": false,
      "last_failure": null,
      "last_run": { "status": "loaded", "port": 39123, "running": true, "failed": false, "exit_code": 0, "reason": null },
      "device_moved": null, "device_missing": null,
      "apply_error": "",
      "context": { "slots": 4, "unified": true, "pool": 32768, "per_slot_cap": null, "n_ctx_train": 32768,
                   "max_per_request": 32768, "per_request_all_busy": 8192 },
      "facts_status": "ready",
      "facts": { "arch": "qwen2", "n_layer": 28, "n_ctx_train": 32768, "quant": "Q5_K_M", "files": [ { "name": "Qwen2.5-7B-Instruct-Q5_K_M.gguf", "bytes": 5444000000 } ] },
      "warnings": []
    }
  ],
  "unattached": [ { "id": "user/old-repo:Q4_K_M", "kind": "orphaned", "overrides": { "device": "CUDA0" }, "phase": "committed", "failures": 0, "updated": "2026-10-01T09:00:00Z", "exists": false } ],
  "state": { "file": "/var/lib/provision-llm-admin/admin-models.json", "error": "", "read_only": false, "backup_loaded": false, "problems": [] },
  "warnings": []
}
```

`schema` (list answer only): one entry per key of [6.1](#61-which-models-and-which-settings),
enough to build a form: `key`, `group` (`placement`, `advanced`, `role`), the built-in
`default` (or `null`), `type` (`device`, `int`, `ngl`, `enum`, `bool`) with `min` /
`max`, the allowed `values`, `auto` (the number that means "auto", `-1` for `parallel`)
and `pair` (`cache-type-k` and `cache-type-v` must be equal).

Per model:

- `overrides`: the custom values saved in the panel (canonical spelling).
- `kept`: values of the model's entry in `admin-models.json` that the server does not
  use (an unknown key or an invalid value, for example written by a newer version or by
  hand); they are written back unchanged.
- `hidden`: `true` for a model that `GET /v1/models` does not list but that still
  answers when it is requested. Such a model is not among `models`; its saved settings
  are listed in `unattached` with `kind: hidden`, and `?model=` gives its row.
- `fields`: for every key of [6.1](#61-which-models-and-which-settings), the
  `effective` value the model process gets, the built-in `default` when known, its
  `origin` (`admin`, `models_ini`, `default_device`, `model_devices`, `router_cli`,
  `default`), the `inherited` value without the custom one, the saved `override`,
  `locked` (`null`, or `{ "by": "model_devices" | "router_cli", "value": ... }`), and
  `active` / `inactive_reason` (a saved value that a lock or a missing GPU overrides).
- `phase`, `failures`, `last_good`, `reverted`, `reverted_at_startup`: see
  [6.6](#66-trial-commit-and-automatic-revert).
- `last_failure`: the last failed start (time, reason, exit code, output tail), see
  [9](#9-when-a-model-fails-to-start); `last_run`: the current process.
- `device_moved` / `device_missing`: see [7.4](#74-gpu-identity-moved-and-missing-gpus).
- `context`: what the context settings mean per request ([6.2](#62-context-size-one-shared-pool-or-per-request)).
- `facts_status`: `ready`, `pending` (being read in the background), `error`,
  `unsupported`. The list carries the facts that are already known; `?model=` reads
  them if needed.
- `facts`: what the GGUF file says: `arch`, `n_layer`, `n_ctx_train`, `n_embd`, the head
  counts and sizes, `files`, `quant`, the tensor sizes the rough estimate uses,
  `kv_bytes_per_token` per KV cache type (`f16`, `bf16`, `q8_0`, `q4_0`; `null` when the
  type is not possible for the model, for example `q8_0` / `q4_0` with a head size that
  is not a multiple of 32, or for a recurrent or MLA model), `kv_estimate` (`exact`,
  `approximate`, `unsupported`) and `kv_note`, which says why. With `facts_status`
  `error`, `facts.error` says what is wrong with the file, for example
  `the GGUF metadata of ... is not plausible (...)`, a `split.count` that does not match
  the file name, or a missing part of a split model.
- `warnings`: `model_devices_not_applied`, `device_moved`, `device_missing`,
  `default_device_not_present`, `pool_overcommit`, `ctx_above_train`,
  `ubatch_above_ctx`, `kv_per_slot_no_effect`, `pooling_without_embeddings`,
  `apply_error`.

`unattached[].kind`: `orphaned` (the model is gone), `hidden` (a cache entry the list
does not show), `unreadable` (an entry of the file that could not be read; kept as it
is). The top-level `warnings` can carry `default_device_not_present` and
`model_devices_unknown_section:<name>`.

An id that is neither a model nor saved settings: `404 not_found_error`.

### 12.8 `POST /admin/models/settings`

Set or remove custom values of one model.

```json
{ "model": "qwen2.5-chat", "settings": { "ctx-size": 32768, "cache-type-k": "q8_0", "cache-type-v": "q8_0" }, "restart": true }
```

- `settings`: a value sets a custom value, `null` removes it, keys that are not given
  stay as they are. Numbers and booleans may be sent as JSON numbers / booleans or as
  strings.
- `reset: true` removes all custom values of the model (also for an entry in
  `unattached`).
- `restart: true` restarts a running model with the new values, and loads a stopped one
  ("Save and load"). Without it a change to a running model is refused
  (`409 restart_required`) and a stopped model is only saved.

```bash
curl -s -X POST -H "X-Admin-Key: $KEY" -H 'Content-Type: application/json' \
    -d '{ "model": "qwen2.5-chat", "settings": { "ctx-size": 32768 }, "restart": true }' \
    $BASE/admin/models/settings
```

Answer `200`:

```json
{
  "model": { "id": "qwen2.5-chat", "status": "loading", "phase": "trial" },
  "result": {
    "changed": true, "reloaded": true, "restarted": true, "was_running": true,
    "restarted_also": [], "not_restarted": [],
    "load_error": "", "reload_error": ""
  },
  "previous": { "ctx-size": null },
  "warnings": []
}
```

- `model`: the row as in `GET /admin/models/settings` (shortened above).
- `result.changed: false`: the effective values did not change (for example the same
  value again, or a built-in default); nothing was reloaded or restarted.
- `restarted_also` (ids) / `not_restarted` (`{ "id", "error" }`): other models, see
  [6.5](#65-saving-and-restarting).
- `load_error`: saved, but the model could not be started now (for example
  `model limit reached`); it starts on its next request.
- `previous`: the custom values before this change, in the shape of `settings`
  (`null` = was not set). Sending it back undoes the change.

Errors: `400 invalid_request_error` (`param` names the key), `404 not_found_error`,
`409 field_locked` (`param` names the key), `409 restart_required`,
`409 model_settings_read_only`, `500 server_error`, `504 stop_timeout`.

`500 server_error` comes in two cases. The settings file could not be written: nothing
is applied, and a model that was stopped for the restart is started again with its old
values. Or the model list could not be rebuilt after the save (for example `models.ini`
was broken in the meantime): the previous custom values are put back, in memory and in
the file, and the error object says what happened to the models:

```json
{ "error": { "code": 500, "type": "server_error",
             "message": "reloading the model list failed, the previous settings are restored: failed to parse server config file: /config/models.ini",
             "reload_error": "failed to parse server config file: /config/models.ini", "restarted": true } }
```

- `reload_error`: why the model list could not be rebuilt.
- `restarted` (only when the model was running and had been stopped for the restart):
  `true` when it runs again with its previous values, `false` with `restart_error`
  when it could not be started.
- `restarted_also` / `not_restarted` (only when not empty): the other models, as in the
  `200` answer.

### 12.9 `POST /admin/models/settings/check`

The fit check of [8](#8-will-it-fit) for values that are not saved.

```bash
curl -s -X POST -H "X-Admin-Key: $KEY" -H 'Content-Type: application/json' \
    -d '{ "model": "qwen2.5-chat", "settings": { "ctx-size": 65536 } }' \
    $BASE/admin/models/settings/check
```

```json
{
  "ok": true, "model": "qwen2.5-chat", "checked_at": "2026-10-06T10:00:05Z", "cached": false, "checked_while_running": false,
  "verdict": "no", "margin": 1073741824,
  "devices": [
    { "device": "CUDA1", "description": "NVIDIA GeForce GTX 1080 Ti",
      "need": { "model": 5444000000, "context": 3758096384, "compute": 305000000, "total": 9507096384 },
      "free": 2400000000, "total": 11811160064, "free_source": "vendor_tool",
      "own_usage": 0, "own_usage_source": null, "available": 2400000000,
      "settling": false, "verdict": "no" }
  ],
  "host": { "model": 560000000, "context": 0, "compute": 0 }, "host_bytes": 560000000,
  "error": null, "warnings": []
}
```

- `settings`: the values to check, in the shape of `POST /admin/models/settings`; keys
  that are not given keep their current value.
- per device: `free` now (`free_source`: `vendor_tool`, else `helper`, the check's own
  reading), `own_usage` of the model there when it is running and would be restarted,
  `available` = free + own usage, and the `verdict` (`fits`, `tight`, `no`, `unknown`);
  the top-level `verdict` is the worst of them (`no`, then `unknown`, then `tight`), or
  `cpu_only` when the model uses no GPU (no `devices`; see `host` for its memory).
- `ok: false` with `error: { "code", "message", "line" }`: `invalid_settings`,
  `out_of_memory`, `invalid_device`, `model_load_failed`, `device_missing`, `timeout`,
  `failed` (the helper could not be started), `unsupported`.
- `cached: true`: the result of an earlier check, reused under the rules of
  [8](#8-will-it-fit) (same effective arguments, inventory and running state, at most
  60 seconds old, free memory moved by less than 256 MiB). `checked_while_running`: the
  dry run was made while the model was running.
- Also in the answer: `args` (the command line of the dry run), `duration_ms` (of a new
  check), and what llama.cpp's fit made of the values: `n_ctx`, `n_gpu_layers`, `fit`
  (`success`, `failure`, `error`, or `off` when the fit was switched off), plus the
  model's `n_ctx_train` and `n_layer`.

One check at a time (`409 check_busy`; `409 check_stuck` while a stopped helper has not
exited), at most 30 seconds.

### 12.10 `GET /admin/gpus`

The GPU cards. `?refresh=1` asks the vendor tool again (at most once a second) instead
of using the last few seconds' result.

```bash
curl -s -H "X-Admin-Key: $KEY" "$BASE/admin/gpus?refresh=1"
```

```json
{
  "build_devices": ["CUDA0", "CUDA1"],
  "gpu_backend": { "state": "ok", "message": "2 GPU device(s) found", "libraries": ["libggml-cuda.so"], "backends": [ { "name": "CUDA", "devices": 2 } ] },
  "inventory": {
    "ok": true, "source": "helper", "error": "", "message": "",
    "time": "2026-10-06T09:58:00Z", "age_ms": 120000, "duration_ms": 2300,
    "devices": [
      { "name": "CUDA0", "backend": "CUDA", "vendor": "nvidia", "description": "NVIDIA GeForce GTX 1080 Ti", "type": "gpu",
        "device_id": "0000:01:00.0", "pci_bus_id": "0000:01:00.0", "memory_free": 11500000000, "memory_total": 11811160064 },
      { "name": "CUDA1", "backend": "CUDA", "vendor": "nvidia", "description": "NVIDIA GeForce GTX 1080 Ti", "type": "gpu",
        "device_id": "0000:02:00.0", "pci_bus_id": "0000:02:00.0", "memory_free": 11500000000, "memory_total": 11811160064 }
    ],
    "backends": ["CUDA"]
  },
  "probe": { "mode": "auto", "source": "nvidia-smi", "ok": true, "error": "", "message": "", "age_ms": 820, "stale": false, "mapping": "pci",
             "tools": [ { "tool": "nvidia-smi", "path": "/usr/bin/nvidia-smi", "ok": true, "error": "", "message": "", "age_ms": 820, "rows": 2 } ] },
  "margin": 1073741824, "settle_ms": 1500,
  "gpus": [
    { "device": "CUDA0", "backend": "CUDA", "vendor": "nvidia", "type": "gpu", "description": "NVIDIA GeForce GTX 1080 Ti",
      "pci_bus_id": "0000:01:00.0", "mapping": "pci", "tool": "nvidia-smi", "index": 0, "uuid": "GPU-0a1b2c3d",
      "name": "NVIDIA GeForce GTX 1080 Ti", "unsupported": false,
      "total": 11811160064, "used": 3100000000, "free": 8711160064, "memory_source": "vendor_tool", "memory_age_ms": 820,
      "settling": false, "same_gpu_as": [],
      "models": [ { "id": "gte-qwen2-embed", "status": "loaded", "all_gpus": false, "pinned": true, "facts_status": "ready",
                    "memory": { "bytes": 2400000000, "source": "reported", "by": "model" } } ] }
  ],
  "unmatched": [],
  "warnings": []
}
```

- `gpu_backend.state`: `ok`, `cpu_only` (no GPU backend in this build), `no_devices`
  (a GPU backend is there but found no GPU), see [7.5](#75-when-there-is-no-gpu-information).
- `inventory`: the last GPU inventory ([7.1](#71-which-gpus-the-server-has));
  `pci_bus_id` is `null` when the backend reports none.
- `probe.mode`: `auto`, `off`, `tool`, `file`; `probe.source`: `nvidia-smi`, `amd-smi`,
  `rocm-smi`, `file`, `multiple`, `none`; `probe.error`: `disabled`, `not_found`,
  `timeout`, `failed`, `parse_error`, `unsupported`, `probe_stuck`;
  `probe.mapping`: `pci`, `single`, `unknown`, `mixed`, `none` ([7.2](#72-live-memory)).
- per GPU: `mapping` of that card, `memory_source` `vendor_tool`, `inventory` or
  `none` with `memory_age_ms` (`total` / `used` / `free` are `null` with `none`),
  `settling` while a model on it loads or stops, `same_gpu_as` for the other names of
  the same physical GPU (for example `CUDA0` and `Vulkan0`).
- per model: `memory.source` `reported`, `measured` or `estimate` and `memory.by`
  `model`, `nvidia-smi`, `llama.cpp` or `formula` ([7.3](#73-memory-per-model));
  `memory` is `null` until it is known.
- `unmatched`: rows of the vendor tool without a GPU of the server.
- `warnings`: `gpu_backend_not_initialised`, `vendor_rows_not_matched`.

### 12.11 `POST /admin/gpus/rescan`

Runs the GPU inventory again ("Re-detect GPUs", [7.1](#71-which-gpus-the-server-has))
and answers like `GET /admin/gpus`. It briefly needs a little memory on every GPU.
`409 rescan_busy` while an inventory is still running, `409 rescan_stuck` while a
stopped inventory helper has not exited (stuck in the GPU driver).

```bash
curl -s -X POST -H "X-Admin-Key: $KEY" $BASE/admin/gpus/rescan
```

### 12.12 `GET /admin/models/log`

The output tail of a model's latest process and its last failure.

```bash
curl -s -H "X-Admin-Key: $KEY" --get --data-urlencode "model=gte-qwen2-embed" $BASE/admin/models/log
```

```json
{
  "model": "gte-qwen2-embed",
  "status": "unloaded",
  "instance": { "port": 41235, "running": false, "started": "2026-10-06T10:02:01Z", "started_ms": 1791280921348, "lines_total": 412 },
  "reason": {
    "code": "out_of_memory",
    "message": "Out of GPU memory: the model, its context (KV cache) or its compute buffers do not fit",
    "hint": "lower the context size, use KV cache precision q8_0, offload fewer GPU layers or choose another GPU",
    "line": "llama_kv_cache: failed to allocate buffer for kv cache",
    "hints": []
  },
  "lines": ["llama_kv_cache: failed to allocate buffer for kv cache"],
  "truncated": true,
  "last_failure": {
    "time": "2026-10-06T10:02:11Z", "time_ms": 1791280931502, "port": 41235, "exit_code": 1, "before_loaded": true,
    "reason": { "code": "out_of_memory", "message": "Out of GPU memory: the model, its context (KV cache) or its compute buffers do not fit", "hint": "", "line": "llama_kv_cache: failed to allocate buffer for kv cache", "hints": [] },
    "lines": ["llama_kv_cache: failed to allocate buffer for kv cache"], "truncated": true,
    "failures": 1, "settings_reverted": false
  }
}
```

- `reason.code`: see [9](#9-when-a-model-fails-to-start); `none` when the process
  ended without an error line (stopped or killed). `reason` is `null` while a running
  process has shown no error. `reason.hints` can carry `memory_tight`.
- `last_failure.settings_reverted`: this failure made the server go back to the last
  good settings ([6.6](#66-trial-commit-and-automatic-revert)).
- `instance.started` / `instance.started_ms`: when the process wrote its first line, in
  whole seconds and in milliseconds since the epoch. `started_ms` is comparable with
  `last_failure.time_ms` (when the failure was recorded): an instance with
  `started_ms > last_failure.time_ms` is a later start, not the one that failed.
- An empty tail gives empty `lines`, not an error.

### 12.13 `GET /admin/diagnostics`

The items of [10](#10-diagnostics).

```bash
curl -s -H "X-Admin-Key: $KEY" $BASE/admin/diagnostics
```

```json
{
  "generated_at": "2026-10-06T10:05:00Z",
  "items": [
    { "id": "deployment", "label": "Deployment", "status": "ok", "value": "container", "hint": "running in a container (Docker / Podman / Kubernetes)" },
    { "id": "vendor_tool", "label": "GPU memory tool", "status": "warning",
      "value": { "mode": "auto", "setting": "auto", "tools": [], "error": "not_found" },
      "hint": "nvidia-smi was not found on PATH; nvidia-smi is not available inside the container: add NVIDIA_DRIVER_CAPABILITIES=compute,utility" },
    { "id": "proxy", "label": "Requests through a reverse proxy", "status": "ok", "value": true, "hint": "X-Forwarded-For seen" }
  ]
}
```

`status`: `ok`, `warning`, `error`, `info`. `value` is whatever fits the item (a
string, a boolean, an object or `null`). The items come in the order of the table in
[10](#10-diagnostics); `stuck_helpers` is added at the end only while a helper hangs.
The `ip_allowlist` item describes the request it answers (`seen_from`, `peer`), so call
it from the client whose address you want to check.

### 12.14 `GET /admin/models/upload`

The upload of [16](#16-uploading-gguf-files). Without `id`: whether uploads are possible
on this server, the fixed limits, and every upload the server knows (from every browser
and admin, so a page that lost its files can offer to continue them).

```bash
curl -s -H "X-Admin-Key: $KEY" $BASE/admin/models/upload
```

```json
{
  "available": true, "reason": null, "message": null,
  "limits": { "chunk_max": 16777216, "chunk_default": 8388608, "chunk_min": 245760, "max_sessions": 4, "max_writers": 4,
              "max_files": 256, "ttl_h": 72, "ttl_s": 259200, "max_header_bytes": 67108864 },
  "sessions": [
    { "id": "9f1c2b7e4a6d4c0e8b3a5f2d1e0c9b8a", "name": "gemma-3-4b", "state": "receiving", "substate": null, "blocked_by": null,
      "total_bytes": 3341008960, "received_bytes": 1258291200, "n_files": 2, "auto_publish": true,
      "created": "2026-10-06T10:00:00Z", "last_activity": "2026-10-06T10:20:00Z",
      "error": null, "blocked": null, "expires_at": "2026-10-09T10:20:00Z" }
  ],
  "staging_bytes": 1258291200,
  "models_dir": "/models-rw/local",
  "settings_writable": true, "settings_error": null
}
```

- `available` / `reason` / `message`: as `upload` in `GET /admin/status`; the reasons are in
  [16.1](#161-what-can-be-uploaded) (also `staging_unusable`: `.provision-upload` is a
  file or a symbolic link, remove it).
- `limits`: the constants of [16.7](#167-limits-and-memory) in bytes (`chunk_min` is the
  smallest chunk the page steps down to, `max_header_bytes` the largest metadata section
  of a file); `ttl_h` / `ttl_s` the expiry.
- `sessions`: one summary per upload, finished ones included for 24 hours.
- `staging_bytes`: what the unfinished uploads take on disk.
- `settings_writable` / `settings_error`: whether the placement of an upload can be saved
  (the admin state directory); otherwise an upload with settings stops as `ready`
  (`settings_not_saved`), see [6.9](#69-settings-for-a-model-that-is-being-added).

With `?id=<upload id>`: one upload, with everything a client needs to continue it. The id
is 32 hexadecimal characters (anything else is a `400`); an unknown or removed upload is
`410 upload_gone`.

```bash
curl -s -H "X-Admin-Key: $KEY" "$BASE/admin/models/upload?id=9f1c2b7e4a6d4c0e8b3a5f2d1e0c9b8a"
```

```json
{
  "id": "9f1c2b7e4a6d4c0e8b3a5f2d1e0c9b8a", "name": "gemma-3-4b",
  "state": "checking", "substate": "load_check", "blocked_by": null,
  "total_bytes": 3341008960, "received_bytes": 3341008960, "n_files": 2, "auto_publish": true,
  "created": "2026-10-06T10:00:00Z", "last_activity": "2026-10-06T11:02:00Z",
  "error": null, "blocked": null, "expires_at": null,
  "files": [
    { "index": 0, "name": "gemma-3-4b-it-Q4_K_M.gguf", "size": 2489757856, "offset": 2489757856, "durable_offset": 2489757856,
      "complete": true, "dropped": false, "role": "model",
      "sha256": "2f4c8f1e9b0d7a6c5e3b1a9d8c7f6e5d4c3b2a19f8e7d6c5b4a3928170615243", "sha256_expected": null,
      "head_crc32": "4a3b2c1d", "final_name": "gemma-3-4b.gguf",
      "header": { "state": "ok", "arch": "gemma3", "type": "model" },
      "error": null },
    { "index": 1, "name": "mmproj-gemma-3-4b-it-f16.gguf", "size": 851251104, "offset": 851251104, "durable_offset": 851251104,
      "complete": true, "dropped": false, "role": "projector",
      "sha256": "8d1e5a7c3b9f2e4d6a8c0b1f3e5d7c9a2b4f6e8d0c1a3b5e7f9d2c4b6a8e0f13", "sha256_expected": null,
      "head_crc32": "9e8f7a6b", "final_name": "mmproj-gemma-3-4b.gguf",
      "header": { "state": "ok", "arch": "clip", "type": "mmproj" },
      "error": null }
  ],
  "settings": { "device": "CUDA1", "ctx-size": "16384" }, "settings_mode": "replace",
  "plan": { "layout": "folder", "target": "gemma-3-4b", "id": "gemma-3-4b",
            "files": [ { "index": 0, "final_name": "gemma-3-4b.gguf", "role": "model", "split_no": null },
                       { "index": 1, "final_name": "mmproj-gemma-3-4b.gguf", "role": "projector", "split_no": null } ] },
  "facts": { "arch": "gemma3", "name": "Gemma 3 4b It", "n_params": 3880263168, "n_ctx_train": 131072, "n_layer": 34,
             "file_type": 15, "n_tensors": 444 },
  "result": null, "warnings": [],
  "idle_ms": 0, "last_client_idle_ms": 1500,
  "recovered": false, "recovered_without_record": false, "record_persisted": true,
  "chunk_max": 16777216, "chunk_default": 8388608,
  "progress": null,
  "checks": [
    { "step": "header", "file": 0, "status": "ok", "message": "" },
    { "step": "header", "file": 1, "status": "ok", "message": "" }
  ]
}
```

Per file (`files[]`):

- `index`: the position in `files` of the create request; the `file=` of the chunk route.
- `name`, `size`: as declared when the upload was created.
- `offset`: the bytes the server has accepted; the next chunk starts here.
- `durable_offset`: the bytes known to be on disk (synced at the last checkpoint). After a
  restart of the server `offset` goes back to this value.
- `complete`: `offset` equals `size`. `dropped`: the file was taken out of the upload
  (`drop_files` of [12.17](#1217-post-adminmodelsuploadupdate-publish-cancel)).
- `role`: `model` or `projector` once the server has read the file's header (`adapter`
  or `imatrix` for such a file, which is then refused), `null` before.
- `sha256`: computed while the file is received, so it is there as soon as the file is
  complete (after a restart of the server it is read again from disk, substate
  `hashing`); `sha256_expected`: the expected value given at create or update.
- `head_crc32`: the CRC-32 of the first MiB (8 hexadecimal digits), once that much has
  arrived: a client compares it with the file it has before it continues
  ([16.6](#166-resuming)); `null` when the server does not know it (then send the first
  MiB with `verify=1`, [12.16](#1216-post-adminmodelsuploadchunk)).
- `final_name`: the name in the models directory, decided by the classification as soon
  as the header of every file is known (possibly while other files are still sent).
- `header`: the header check of this file. `state`: `none` (no bytes yet), `scanning`
  (waiting for the rest of the metadata), `queued`, `ok`, `warning` (the metadata alone
  did not load; decided by the load check at the end), `failed` (the check could not run;
  it runs again), `error` (the file is refused, see `error`). Once the metadata was read:
  `arch` and `type`, the file's `general.architecture` and `general.type` as the converter
  wrote them (`model`, `adapter`, `imatrix`, `mmproj` from the current converter; older
  projectors have none or a value such as `clip-vision`), `null` when absent. The role
  does not come from `type`: a file whose architecture is `clip` is the projector. `split`
  (`no`, `count`) for a shard. `message` only with a warning, a failure or a refusal: the
  reason.
- `error`: why this file cannot be used (`code`, `message`; the codes are in
  [16.4](#164-checks-before-the-model-becomes-visible)), or `null`.

The upload:

| `state` | Detail | Meaning |
|---|---|---|
| `receiving` | | chunks are accepted |
| `checking` | `substate`: `queued` (waiting for its turn: one upload is checked at a time), `hashing` (with `progress`), `classifying`, `load_check` | the checks of [16.4](#164-checks-before-the-model-becomes-visible) |
| `ready` | `blocked_by`: `manual` (automatic publishing is off, or the upload was rebuilt without its record), `name_taken` (rename it), `publish_error`, `check_failed` (the check could not run), `sha256_mismatch`, `settings_not_saved` (the admin state directory cannot be written), `settings_invalid` (the settings are no longer valid, for example a GPU is gone); `blocked` has the details | all data is there; waits for `update`, `publish` or `cancel` ([12.17](#1217-post-adminmodelsuploadupdate-publish-cancel)) |
| `publishing` | | the settings entry, the rename and the rebuild of the model list |
| `published` | `result`: the model and its final files; a `reload_error` when the model list could not be rebuilt ([16.5](#165-where-the-files-end-up)) | done; listed for 24 hours |
| `rejected` | `error`: `code`, `message`, `file` (the index of the file the verdict is about, or `null`) | the files cannot be used as a model; the data is deleted; listed for 24 hours |
| `cancelled`, `expired` | `expired`: `error.code` is `expired` | the data is deleted; listed for 24 hours |

`blocked` always has `code` and `message`, and per `blocked_by`:

| `blocked_by` | `blocked.code` and more |
|---|---|
| `manual` | `manual` |
| `name_taken` | `name_taken`, `by` (`model`, `preset`, `alias`, `file`, `upload`), `other` (the name it collides with), `case_variant` |
| `sha256_mismatch` | `sha256_mismatch`, `file`, `expected`, `actual` |
| `check_failed` | why the check could not run: `timeout`, `spawn_failed`, `no_result` (the helper ended without an answer, for example killed for its memory), `check_error` (for example "the check ran out of memory"), `hash_failed`, `check_failed` (a header check), `staging_failed` (arranging the files under their final names); on Windows `path_too_long` with `max_name_length` (a split model whose staged path is too long for llama.cpp: rename the upload to at most that many characters, then Retry). After a restart of the server, a file that is now refused under its final name gives its own code (for example `not_gguf`) and `file`: Retry or cancel, it can no longer be dropped |
| `publish_error` | `staging_failed`, `upload_unavailable` (the models directory is not usable now), `staging_other_filesystem`, `publish_failed` (with `errno`) |
| `settings_not_saved` | `settings_not_saved` |
| `settings_invalid` | the error of the settings check (for example `invalid_request_error`, `field_locked`) and `param` |

- `checks`: one entry per file whose header check has started: `step` (`header`), `file`,
  `status` (the `header.state` of that file), `message` (empty while all is well,
  otherwise the warning or the reason).
- `plan`: the final layout, once the classification has decided it: `layout` (`file` or
  `folder`), `target` (the name in the models directory), `id`, and `files[]` with
  `index`, `final_name`, `role` and `split_no` (`null` for a single file).
- `facts`: what the checks read about the model. After the header check of the model
  file: `arch`, `name` (`general.name`), `n_params`, `n_ctx_train`, `n_layer`, `file_type`
  (`general.file_type`, the quantization as llama.cpp numbers it), `n_tensors` and, for a
  split set, `split`. After the load check also `n_vocab`, `desc` (llama.cpp's short
  description, when it gives one) and, with a projector, `vision` / `audio`.
- `result` (`published`): `model`, `layout`, `target`, `rename` (how the files were moved
  into place: `renameat2`, `link`, `check+rename`, `renameatx_np` on macOS, `MoveFileEx`
  on Windows), `reload_error`, `settings_saved` (a per-model settings entry was written),
  and `files[]` with `name` (relative to the models directory), `role`, `size` and
  `sha256`. An upload whose publish had finished when the server stopped shows only
  `model`, `target` and `recovered`.
- `warnings`: those of the create answer, plus `chown_failed` ([11](#11-storage)) and
  `recovered_without_record`.
- `last_activity`, `expires_at` (set only while the upload is `receiving` or `ready`),
  `idle_ms`: the expiry of [16.6](#166-resuming), counted while the server runs.
- `last_client_idle_ms`: the time since the last chunk from any sender; below 30 000 a
  chunk from another sender gets `409 other_client_active`.
- `recovered`: the server took the upload over after a restart; `record_persisted`:
  its record is in the admin state directory (otherwise it survives a restart only
  without its settings); `recovered_without_record`: it was rebuilt from its staging
  hint alone, without its settings and without automatic publishing (after its checks it
  waits as `ready` / `manual`).

### 12.15 `POST /admin/models/upload`

Creates the upload of one model, or with `"dry_run": true` only checks it.

```bash
curl -s -X POST -H "X-Admin-Key: $KEY" -H 'Content-Type: application/json' -d '{
        "name": "gemma-3-4b",
        "files": [ { "name": "gemma-3-4b-it-Q4_K_M.gguf", "size": 2489757856, "last_modified": 1759744800000 },
                   { "name": "mmproj-gemma-3-4b-it-f16.gguf", "size": 851251104 } ],
        "settings": { "device": "CUDA1", "ctx-size": 16384 },
        "dry_run": true }' $BASE/admin/models/upload
```

| Field | Meaning |
|---|---|
| `name` | the model id, rules of [16.3](#163-names) |
| `files[]` | 1 to 256 entries: `name` (the original file name: shown, and used to match the files after a reload; never a path), `size` (bytes, at least 24), `last_modified` (optional, milliseconds since the epoch, a hint for that matching), `sha256` (optional, 64 hexadecimal characters: the expected SHA-256) |
| `settings` | optional: per-model settings for the model's first start ([6.9](#69-settings-for-a-model-that-is-being-added)), in the shape of `settings` of `POST /admin/models/settings` |
| `settings_mode` | `replace` (default): saved settings of an earlier model with this id are dropped at publishing; `keep`: they are used again, the values of `settings` on top |
| `auto_publish` | default `true`: publish as soon as the checks pass; `false`: wait as `ready` (`manual`) |
| `dry_run` | `true`: only check; nothing is created (answer `200`, `upload.id` is `null`) |

Answer `201`:

```json
{
  "upload": { "id": "9f1c2b7e4a6d4c0e8b3a5f2d1e0c9b8a", "name": "gemma-3-4b", "state": "receiving", "files": [], "expires_at": "2026-10-09T10:00:00Z" },
  "disk": { "free_bytes": 120000000000, "total_bytes": 480000000000, "needed_bytes": 3341008960, "other_uploads_bytes": 0,
            "shares_disk_with": ["admin_state"] },
  "warnings": ["shares_disk_with_state"],
  "locks": { "flash-attn": { "by": "router_cli", "value": "on" } },
  "existing_settings": { "ctx-size": "8192" }
}
```

- `upload`: the new upload, the full view of `GET ?id=` (shortened above). A dry run
  answers `200` with only `id` (`null`), `name`, `files` (`index`, `name`, `size`),
  `settings` (as they would be saved), `chunk_max` and `chunk_default`.
- `disk`: the filesystem of the models directory: `free_bytes`, `total_bytes`, what this
  upload needs (`needed_bytes`) and what other unfinished uploads still need
  (`other_uploads_bytes`); `shares_disk_with`: other data on the same filesystem
  (`admin_state`; checked on Linux only).
- `warnings` (they never block): `not_enough_space` (the files are larger than the free
  space minus what the other unfinished uploads still need), `larger_than_disk` (larger
  than the whole filesystem), `shares_disk_with_state` (a full disk would also stop the
  admin state from being saved), `state_not_writable` (the admin state directory cannot
  be written: with settings, the publish stops at `settings_not_saved`),
  `record_not_persisted` (the record of the upload could not be written to the admin
  state directory), see [16.8](#168-disk-space); and the value warnings of
  `POST /admin/models/settings` (for example `ctx_above_train`, `device_missing`).
- `locks`: the settings that are locked for this model, `{ key: { "by", "value" } }`:
  `by` is `router_cli` (given on the router command line) or `model_devices` (the GPU of
  a `MODEL_DEVICES` `*=` entry in the `[*]` section), see [6.4](#64-locked-fields).
- `existing_settings` (only when there are some): the saved settings of an earlier model
  with this id (an `orphaned` entry of `GET /admin/models/settings`,
  [6.7](#67-removing-custom-settings)). The dialog asks "Start fresh" / "Keep them".

Errors:

| Status | `error.type` | When |
|---|---|---|
| 400 | `invalid_request_error` (`param`) | a field is missing or out of its rules |
| 400 | `invalid_name` (`reason`: `empty`, `too_long`, `charset`, `mmproj`, `gguf`, `draft_prefix`, `shard_suffix`, `trailing_dot`, `reserved`) | the name breaks a rule of [16.3](#163-names) |
| 400 | `unsupported_file` (`reason` `draft`, `file`) | a file name says it is a draft model (`mtp-`, `dspark-`, `dflash-`, `eagle3-`) |
| 409 | `upload_unavailable` (`reason`) | uploads are not possible here ([16.1](#161-what-can-be-uploaded)) |
| 409 | `name_taken` (`by`: `model`, `preset`, `alias`, `file`, `upload`; `other`, `case_variant`) | the name collides with a model id (`model`; `preset` for a `models.ini` section), an alias, an entry of the models directory (`file`) or another unfinished upload (`upload`); `other` is the name it collides with, `case_variant` says that they differ only in case |
| 409 | `field_locked` (`param`) | a setting against a lock of [6.4](#64-locked-fields) |
| 409 | `too_many_uploads` (`sessions`) | 4 uploads are unfinished; `sessions` lists them (a dry run is not counted) |

### 12.16 `POST /admin/models/upload/chunk`

Appends one chunk of one file, or with `verify=1` only compares it.

```
POST /admin/models/upload/chunk?id=<upload id>&file=<index>&offset=<byte offset>[&takeover=1][&verify=1]
Content-Type: application/octet-stream
Content-Length: <length>
X-Admin-Key: <key>
X-Upload-Client: <random id of the browser tab or script>
X-Chunk-CRC32: <CRC-32 of the body, 8 hexadecimal digits>          (optional)

<the bytes offset .. offset + length - 1 of the file>
```

Answer `200`:

```json
{ "offset": 16777216, "durable_offset": 0, "file_complete": false, "state": "receiving",
  "file": 0, "received_bytes": 16777216, "total_bytes": 3341008960, "file_error": null }
```

"Current" is the file's `offset` on the server:

| The chunk | Result |
|---|---|
| starts at the current offset | it is appended |
| starts before it | the part the server already has is compared byte for byte with what it stored (`409 content_mismatch` when it differs: not the same file); the rest, if any, is appended. So sending a chunk again is always safe, also one that lies wholly inside the stored bytes (`200`, nothing is written) |
| starts after it | `409 offset_mismatch`, with the server's `offset` |
| reaches beyond the declared size | `400 beyond_declared_size` |

**`verify=1`** compares the body with the stored bytes and never writes: the way to check
that a file picked again (after a reload, or by a script that continues an upload) is the
one the server has, before sending more. The usual probe is the first MiB of the file
(`offset=0`), when `head_crc32` of [12.14](#1214-get-adminmodelsupload) is `null` or not
used, and the last MiB before the server's offset. Answer `200`:

```json
{ "verified": true, "verified_bytes": 1048576, "offset": 10485760, "durable_offset": 10485760,
  "file_complete": false, "state": "receiving" }
```

`verified_bytes` is how much of the body overlapped the stored bytes; a body beyond the
server's offset is not compared and not stored. A difference is `409 content_mismatch`
(with the `offset` of the compared block that differs), a probe that starts after the
server's offset `409 offset_mismatch`. The sender rule of `X-Upload-Client` (below)
applies to a probe too, but a probe does not make its sender the one that writes.

- The body is at most `chunk_max` (16 MiB) and needs a `Content-Length`. A request with
  any `Transfer-Encoding` (also `chunked` next to a `Content-Length`) or without a
  `Content-Length` is `411 length_required`; a `Content-Encoding` other than `identity`
  is `415`: the length decides what is admitted, so it must be the real size of the raw
  body. The server handles a chunk only once it has the whole body, so its offset only
  ever moves by whole chunks: a chunk cut off in transit changes nothing.
- `X-Upload-Client` names the sender (send it with every chunk). A chunk from another
  sender within 30 seconds of the last one is `409 other_client_active` (with `idle_ms`);
  `takeover=1` makes this sender the one that writes, and the other one gets
  `other_client_active` at its next chunk.
- `X-Chunk-CRC32` (CRC-32 as in zip and gzip): a chunk damaged on the way is refused
  (`400 crc_mismatch`) and can be sent again at once.
- The first chunk of a file shows whether it is a GGUF file (`422 not_gguf` right away;
  what was stored of that file is discarded). As soon as the metadata of a file has
  arrived, the header check of [16.4](#164-checks-before-the-model-becomes-visible) runs
  in the background. A file that it finds unusable is marked (`files[].error` of
  [12.14](#1214-get-adminmodelsupload), `file_error` of this answer), and from then on
  its chunks get `422 file_rejected` with `file` and `reason` (the code of
  [16.4](#164-checks-before-the-model-becomes-visible), for example `adapter`); drop it
  from the upload or cancel.
- Answered **before the body is read**, in this order: an address the allow-list refuses
  (`403`), a missing or wrong key (`401`; `503` when the server has no key), a missing
  `Content-Length` or any `Transfer-Encoding` (`411`), a `Content-Encoding` (`415`), a
  `Content-Length` above `chunk_max` (`413 chunk_too_large`), a malformed `id` (`400`),
  an upload that is over (`410 upload_gone`) or busy (`409 upload_busy`), another sender
  (`409 other_client_active`), a chunk of this upload already on its way
  (`409 chunk_in_progress`), too many chunks at once (`429 upload_slots_busy`). The
  server then closes the connection, so a client that is still sending may see a reset
  connection instead of the answer; it asks `GET ?id=` and sends again. Behind a reverse
  proxy such an early answer can also arrive as a `502` from the proxy, which saw the
  connection close while it was still sending the body: treat a `5xx` that is not the
  server's JSON like a dropped connection.
- From outside the admin layer: `503` "Loading model" while the server starts, `401` from
  `--api-key` (send the same `Authorization` header as for every other request), and
  `502` / `504` from a proxy. Every answer of 400 or above closes the connection; the
  client simply connects again.

| Status | `error.type` | When |
|---|---|---|
| 400 | `invalid_request_error` (`param`) | `id`, `file`, `offset` or `X-Chunk-CRC32` missing or invalid |
| 400 | `beyond_declared_size` | the chunk reaches beyond the declared size of the file |
| 400 | `crc_mismatch` | `X-Chunk-CRC32` does not match the body |
| 409 | `offset_mismatch` (`offset`) | the chunk starts after the server's offset |
| 409 | `content_mismatch` (`offset`) | the bytes sent again differ from the stored ones: not the same file |
| 409 | `chunk_in_progress` | another chunk of this upload is being received |
| 409 | `other_client_active` (`idle_ms`) | another sender wrote within the last 30 seconds; `takeover=1` |
| 409 | `upload_busy` (`state`) | the upload is being checked or published, or waits as `ready` |
| 409 | `file_dropped` | the file was dropped from the upload |
| 410 | `upload_gone` (`state`) | unknown, expired, cancelled, rejected or published upload |
| 411 | `length_required` | the chunk has no `Content-Length`, or a `Transfer-Encoding` |
| 413 | `chunk_too_large` (`chunk_max`), as JSON | the body is larger than `chunk_max`. A `413` without this JSON comes from a proxy in front of the server: send smaller chunks ([16.9](#169-behind-a-reverse-proxy)) |
| 415 | `unsupported_media_type` | the body is not `application/octet-stream`, or it has a `Content-Encoding` |
| 422 | `not_gguf`, `file_rejected` (`file`, `reason`) | the file is not GGUF, or the header check found it unusable (`reason`: the code); only that file is affected |
| 429 | `upload_slots_busy` (`retry_after_s`) | 4 chunks are being received already |
| 507 | `disk_full` (`free_bytes`, `offset`, `durable_offset`) | the disk is full (or a quota is reached), or the chunk does not fit into the free space (checked before it is written); the chunk is discarded and the upload is kept |
| 500 | `write_failed` (`offset`) | another error while writing or reading the staged file; the chunk is discarded and the upload is kept |

### 12.17 `POST /admin/models/upload/update`, `.../publish`, `.../cancel`

**`update`** changes an upload that is `receiving` or `ready`: `id`, and any of `name`
(checked as at create; nothing is uploaded again), `settings` (replaces the settings of
the upload), `settings_mode`, `auto_publish`, `files` (`[{ "index": 0, "sha256": "..." }]`:
set, or with `""` remove, the expected SHA-256 of a file) and `drop_files` (`[1]`: take
files out of the upload, for example one that is not GGUF; their data is deleted at
once). When the files that are left are all complete, the checks start. An upload that
waits as `ready` stays there after an update: send `publish` next (it judges the new name
or settings then). The answer is `{ "upload": ..., "warnings": [...], "locks": {...} }`
(plus `existing_settings` when there are some). Refused: dropping every file (`400`,
cancel instead); dropping a file once the checks have arranged the files under their
final names (`409 upload_busy` with `staged: true`: cancel the upload and upload again
without that file); `409 upload_busy` while it is being checked or published; `410
upload_gone` once it is over.

```bash
curl -s -X POST -H "X-Admin-Key: $KEY" -H 'Content-Type: application/json' \
    -d '{ "id": "9f1c2b7e4a6d4c0e8b3a5f2d1e0c9b8a", "name": "gemma-3-4b-vision" }' \
    $BASE/admin/models/upload/update
```

**`publish`** (`{ "id": ... }`) continues an upload that waits as `ready`: with
`auto_publish` off (`manual`), after a rename (`name_taken`), after a `publish_error`
or a `settings_invalid` (the settings are checked again). For `check_failed` and
`sha256_mismatch` it runs the checks again (Retry; the SHA-256 is not computed again);
with `"ignore_sha256_mismatch": true` a differing SHA-256 is accepted ("Publish anyway").
`"without_settings": true` publishes without the settings of the upload
(`settings_not_saved`, `settings_invalid`: the model then starts with its inherited
values). Publishing also switches `auto_publish` on. The answer is `202` with
`{ "upload": ... }` (`publishing` or `checking`); follow it with `GET ?id=`. An upload
that is already published answers `200`; one that is publishing or checking answers
`202` with its state. `409 upload_busy` while it is still receiving, `410 upload_gone`
for a rejected, cancelled or expired one.

**`cancel`** (`{ "id": ... }`) ends an upload that is not published yet and deletes its
data: `200` with `{ "cancelled": true, "freed_bytes": ..., "upload": ... }`. An upload
that is being checked stops after the current step. An upload that is already over
answers `200` with `freed_bytes` 0 (`cancelled` is `true` only when it had been
cancelled). `409 upload_busy` while it is being published.

```bash
curl -s -X POST -H "X-Admin-Key: $KEY" -H 'Content-Type: application/json' \
    -d '{ "id": "9f1c2b7e4a6d4c0e8b3a5f2d1e0c9b8a" }' $BASE/admin/models/upload/publish
curl -s -X POST -H "X-Admin-Key: $KEY" -H 'Content-Type: application/json' \
    -d '{ "id": "9f1c2b7e4a6d4c0e8b3a5f2d1e0c9b8a", "ignore_sha256_mismatch": true }' $BASE/admin/models/upload/publish
curl -s -X POST -H "X-Admin-Key: $KEY" -H 'Content-Type: application/json' \
    -d '{ "id": "9f1c2b7e4a6d4c0e8b3a5f2d1e0c9b8a" }' $BASE/admin/models/upload/cancel
```

### 12.18 `POST /admin/models/delete`

Deletes a model of the models directory, or only its projector
([17](#17-deleting-models-of-the-models-directory)). `DELETE /models` stays as it was, for
downloaded models only.

| Field | Default | Meaning |
|---|---|---|
| `model` | | the model id |
| `dry_run` | `false` | only report what would be deleted |
| `stop` | `false` | stop the model first if it is running; without it a running model is `409 model_running` |
| `remove_settings` | `true` | also remove its saved settings, its pin and what the server cached about it (not with `projector_only`: the model keeps its settings) |
| `projector_only` | `false` | only the model's own projector file (**Remove only the projector** in the panel) |

```bash
curl -s -X POST -H "X-Admin-Key: $KEY" -H 'Content-Type: application/json' \
    -d '{ "model": "gemma-3-4b", "dry_run": true }' $BASE/admin/models/delete
```

The dry run (`200`):

```json
{
  "model": "gemma-3-4b", "source": "models_dir", "projector_only": false,
  "files": [ { "path": "gemma-3-4b/gemma-3-4b.gguf", "bytes": 2489757856, "role": "model" },
             { "path": "gemma-3-4b/mmproj-gemma-3-4b.gguf", "bytes": 851251104, "role": "projector" } ],
  "n_files": 2, "total_bytes": 3341008960,
  "kept": [ { "path": "gemma-3-4b/README.md", "bytes": 5120, "kind": "file" },
            { "path": "mtp-shared-draft.gguf", "bytes": 52428800, "kind": "file", "shared": true } ],
  "n_kept": 2, "kept_truncated": false, "kept_bytes": 52433920,
  "folder": "gemma-3-4b", "folder_removed": false,
  "running": false, "status": "unloaded", "pinned": false, "has_settings": true,
  "warnings": [], "dry_run": true
}
```

- `files`: what would be deleted, as paths relative to the models directory, with their
  `role` (`model`, `shard`, `projector`, `draft`). Only what the models directory gives
  this model: its file (and the shards named after the first one), and for a model in a
  folder the projector and draft model in that folder.
- `kept`: what stays: the other entries of the model's folder ("Kept: not part of this
  model"; `kind` `file`, `dir` or `symlink`; `bytes` is `null` for a folder) and, with
  `shared: true`, a projector or draft model that the model uses but every model gets (a
  draft model on the router command line, a projector or draft model in the `[*]` section
  of `models.ini`). At most 100 are listed; `n_kept` is the number of all entries that
  stay, and `kept_truncated` is `true` when the list holds fewer; `kept_bytes` the size of
  all files that stay.
- `folder` (`null` for a top-level file); `folder_removed`: the folder would be empty and
  is removed.
- `running`, `status`, `pinned`, `has_settings`: the model as it is now.
- `warnings`: `running` (it will be stopped), `folder_keeps_other_model` (another GGUF file
  stays in the folder: after the delete the folder may be listed again under this id,
  with that file).

The answer of the real delete (`200`):

```json
{
  "model": "gemma-3-4b", "deleted": true, "freed_bytes": 3341008960,
  "files": [ { "path": "gemma-3-4b/gemma-3-4b.gguf", "bytes": 2489757856, "role": "model" },
             { "path": "gemma-3-4b/mmproj-gemma-3-4b.gguf", "bytes": 851251104, "role": "projector" } ],
  "kept": [ { "path": "gemma-3-4b/README.md", "bytes": 5120, "kind": "file" } ], "folder_removed": false,
  "settings_removed": true, "pin_removed": false, "trash_left": true, "reload_error": null
}
```

- `deleted: true`, or `projector_removed: true` with `projector_only`.
- `files`, `kept` as in the dry run; `freed_bytes` the size of the deleted files.
- `trash_left`: always `true`: the files are out of the model list and wait in the trash,
  which the server deletes in the background right away (on Linux the space comes back
  once no model process has the file open).
- `reload_error`: the model list could not be rebuilt afterwards (for example a broken
  `models.ini`); the files are gone all the same.

Errors: `400 invalid_request_error`, `409 model_running`, `409 not_deletable` with
`reason`: `not_found` (no such model, or one that is still being downloaded; the route
never answers `404`), `cache` (a downloaded model: use `DELETE /models`), `preset` (a
`models.ini` section, or a model whose model file is set in `models.ini` or on the
router command line instead of the one the models directory gives it), `no_projector`
(`projector_only` for a model without its own projector, also when its projector is
the shared one of every model), `outside_models_dir`, `symlink`, `missing`,
`other_filesystem`, `no_models_dir`, `no_file` ([17.3](#173-what-is-refused)); `409
upload_unavailable` (`reason`: the models directory cannot be used for the trash right
now, see [16.1](#161-what-can-be-uploaded)); `504 stop_timeout` (nothing was deleted);
`500 server_error` (the trash folder could not be created, the model's folder could not
be opened, or a file could not be moved; what was moved is put back).

### 12.19 `GET /admin/audit`

The activity log of [18](#18-activity-log), newest record first, one page at a time.
Present when `features` lists `audit`.

| Parameter | Meaning |
|---|---|
| `limit` | records per page, 1 to 500; default 100 |
| `before` | only records older than this one: the `next_before` of the previous page. Leave it out for the newest records |
| `kind` | only these kinds, separated by commas: `change`, `refusal`, `server`, `observed` (another value: `400`) |
| `action` | only these actions ([18.1](#181-what-is-recorded)), separated by commas. A name also matches the actions below it: `upload` (or `upload.*`) matches `upload.create`, `upload.published`, ... |
| `result` | only these results, separated by commas: `ok`, `error`, `failed`, `rejected`, `refused` |
| `format` | `json` (default), or `jsonl`: the log as a file instead of a page (below) |

```bash
curl -s -H "X-Admin-Key: $KEY" "$BASE/admin/audit?limit=3"
```

```json
{
  "entries": [
    { "seq": 1204, "id": "1204", "kind": "change", "action": "model_settings.update", "result": "ok",
      "ts": 1791364364381, "time": "2026-10-07T09:12:44Z", "target": "qwen2.5-chat",
      "actor": { "type": "client", "peer": "172.18.0.3", "client": "172.18.0.3", "via_proxy": false,
                 "forwarded_for": "10.64.0.7", "forwarded_trusted": false,
                 "user_agent": "Mozilla/5.0 (X11; Linux x86_64; rv:131.0) Gecko/20100101 Firefox/131.0",
                 "label": "172.18.0.3 (X-Forwarded-For 10.64.0.7, unverified)" },
      "details": { "changed": true, "changes": { "ctx-size": { "from": null, "to": "32768" } },
                   "was_running": true, "restart": true, "restarted": true },
      "persisted": true, "open": false },
    { "seq": 1201, "id": "1201", "kind": "refusal", "action": "access.refused", "result": "refused",
      "ts": 1791364152410, "time": "2026-10-07T09:09:12Z", "target": "/admin/settings",
      "actor": { "type": "client", "peer": "10.64.1.9", "client": "10.64.1.9", "via_proxy": false,
                 "user_agent": "curl/8.5.0", "label": "10.64.1.9" },
      "details": { "status": 401, "code": "admin_key_required", "reason": "wrong_key", "probe": false,
                   "count": 12, "first": "2026-10-07T09:09:12Z", "last": "2026-10-07T09:10:02Z" },
      "count": 12, "updated_seq": 1203, "persisted": true, "open": false },
    { "seq": 1200, "id": "1200", "kind": "observed", "action": "download.finished", "result": "ok",
      "ts": 1791363330120, "time": "2026-10-07T08:55:30Z", "target": "bartowski/Qwen2.5-7B-Instruct-GGUF:Q4_K_M",
      "actor": { "type": "public_api", "label": "public API (unknown)" }, "details": {},
      "persisted": true, "open": false }
  ],
  "next_before": "1200",
  "matched": 1180,
  "persisted": true,
  "audit": { "persisted": true, "error": null, "since": null, "in_memory": 0, "lost": 0, "max_bytes": 4194304, "ring_max": 1000 },
  "warnings": []
}
```

- `entries`: the records, newest first. Each has the fields of its line in the file
  ([18.5](#185-storage-rotation-and-retention)): `seq`, `kind`, `action`, `result`, `ts`
  (milliseconds since 1970) and `time` (UTC), `target` (the model id, upload name, path
  or file; `""` when there is none), `actor` ([18.2](#182-who-addresses-and-proxies)),
  `details` ([18.1](#181-what-is-recorded)), `count` for a merged record, `truncated` for
  one that had to be cut down to its head ([18.5](#185-storage-rotation-and-retention)).
  The answer adds:
  - `id`: the record's `seq` as a string;
  - `persisted`: `false` for a record that is only in memory;
  - `open`: `true` for a merged record whose 60 seconds are not over: its `count` still
    grows;
  - `updated_seq`: for a merged record, the `seq` of its latest update (a window still
    counting has no update yet: its own `seq`); `seq` stays the number of its first line,
    so paging is stable;
  - `unreadable: true` for a line whose start can be read but not the rest (cut by a
    crash, edited by hand), with only `seq`, `kind`, `action` and `result`; a line without
    even that is skipped.
- `next_before`: send it as `before` for the next older page; `null` when there is
  nothing older (with the filters applied).
- `matched`: how many records match the filters and `before` in all, this page included.
- `persisted`: `false` while any of the three files of the log is kept in memory
  ([18.6](#186-when-the-log-cannot-be-written)). `audit` and `warnings`
  (`audit_not_persisted`) as in [12.1](#121-get-adminstatus).

**Download.** `?format=jsonl` answers the lines themselves as a file
(`Content-Type: application/x-ndjson`,
`Content-Disposition: attachment; filename="provision-admin-audit.jsonl"`): every line of
the three files, of their `.1` and of the memory rings, sorted by `seq` (oldest first).
A merged record that ended appears with its first line and, when it counted more than
the first request, its update (`group` = the `seq` of the first line), which has the
final count. A window still counting appears at
the place of its first line, with its current count; its update line follows when it
ends. The filters and `before` apply as for a page; `limit` does not.

```bash
curl -s -H "X-Admin-Key: $KEY" -o admin-audit.jsonl "$BASE/admin/audit?format=jsonl"
```

Errors: `400 invalid_request_error` for a parameter that is not valid (`param` names it:
`before`, `limit`, `kind`, `format`). The route only reads: calling it is not recorded.

## 13. Deployment on a Linux host

The same server without Docker: a systemd service and, optionally, the host's nginx
in front of it, any GPU vendor or none. Without nginx the server's own address
allow-list restricts `/admin` ([4.3](#43-built-in-address-allow-list-hosts-without-nginx)).
The files are in `deploy/linux/`:

| File | Installed as |
|---|---|
| `provision-llm.service` | `/etc/systemd/system/provision-llm.service` |
| `provision-llm.env.example` | `/etc/provision-llm/provision-llm.env` (every admin and router setting, with comments) |
| `nginx-provision-llm.conf` | optional ([13.5](#135-nginx)); Debian / Ubuntu: `/etc/nginx/sites-available/provision-llm` (+ link in `sites-enabled`); RHEL / Fedora / SUSE / Arch: `/etc/nginx/conf.d/provision-llm.conf` |

Nothing of the Docker entrypoint is needed: the admin layer, the GPU view and the fit
check work the same. Differences to Docker: `MODEL_DEVICES` does not exist (put
`device = ...` into `models.ini` or set the GPU per model in the panel), and there is no
start-up guard for `load-on-startup` (keep `PROVISION_LLM_MODELS_MAX` at least as large
as the number of preloaded models, otherwise the server refuses to start with
`number of models to load on startup (N) exceeds models_max (M)`).

### 13.1 Build

Requirements: git, CMake, a C++ compiler (gcc / g++ 12 or newer, as the Docker build
uses), the OpenSSL 3 headers (HTTPS model downloads, see below), Node.js 20 or newer
with npm (the web UI is built from source, `LLAMA_BUILD_UI=ON`; `npm ci` needs access
to the npm registry), plus the toolkit of your GPU:

```bash
# Debian / Ubuntu (Fedora / RHEL: gcc-c++ cmake git openssl-devel)
sudo apt-get install build-essential cmake git libssl-dev ca-certificates
# Node.js: the distribution package is often older than 20; the Docker build uses the
# official tarball from nodejs.org, e.g.
curl -fsSL https://nodejs.org/dist/v22.14.0/node-v22.14.0-linux-x64.tar.xz \
    | sudo tar -xJ -C /usr/local --strip-components=1
node --version
```

Without Node.js the build goes on with a warning and the server has no web UI (the API
works, the Admin page does not exist).

Configure once, with the flags for your GPUs, then build the server:

```bash
git clone <this repository> provision-llm && cd provision-llm

GPU_FLAGS="-DGGML_CUDA=ON"     # the flags for your GPUs, see the table below
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    -DGGML_NATIVE=OFF -DGGML_BACKEND_DL=ON -DGGML_CPU_ALL_VARIANTS=ON \
    -DLLAMA_BUILD_UI=ON -DLLAMA_OPENSSL=ON -DLLAMA_BUILD_TESTS=OFF \
    $GPU_FLAGS
cmake --build build --config Release -j"$(nproc)" --target llama-server
ls build/bin/provision-server
```

| GPUs | GPU flags | Needs |
|---|---|---|
| NVIDIA | `-DGGML_CUDA=ON` (optionally `-DCMAKE_CUDA_ARCHITECTURES="61;86"`) | CUDA toolkit (`nvcc`), NVIDIA driver |
| AMD | `-DGGML_HIP=ON -DGPU_TARGETS=gfx1030` (your GPU's target), run cmake with `HIPCXX="$(hipconfig -l)/clang" HIP_PATH="$(hipconfig -R)"` in front | ROCm |
| any vendor through Vulkan (AMD, Intel, NVIDIA) | `-DGGML_VULKAN=ON` | `libvulkan-dev glslc spirv-headers` (or the Vulkan SDK) |
| Intel through SYCL | `-DGGML_SYCL=ON` with the oneAPI compilers, see [docs/backend/SYCL.md](backend/SYCL.md) | oneAPI |
| several | combine them, e.g. `-DGGML_CUDA=ON -DGGML_VULKAN=ON` | |
| none | no flag: CPU only | |

`GGML_BACKEND_DL=ON` builds every backend as its own library that the server loads at
start-up from the directory of the binary, the same layout as the Docker image. A GPU
backend whose driver is missing is simply not used. More detail per backend:
[docs/build.md](build.md).

HTTPS model downloads need TLS in the binary, from one of two places: the system
OpenSSL 3.0 or newer (`-DLLAMA_OPENSSL=ON`, the default; needs the OpenSSL headers), or
`-DLLAMA_BUILD_BORINGSSL=ON`, which fetches and builds BoringSSL during the configure
step instead (no system OpenSSL needed, but access to boringssl.googlesource.com; the
way to go where the system OpenSSL is older, e.g. RHEL 8). With neither, cmake prints
`OpenSSL not found, HTTPS support disabled` (or, for an OpenSSL older than 3.0,
`Performing Test OPENSSL_VERSION_SUPPORTED - Failed`), HTTPS downloads do not work and
the panel shows downloads as unavailable.

### 13.2 Account, directories, files

```bash
# the service account (no login, no home of its own outside /var/lib)
sudo useradd --system --user-group --home-dir /var/lib/provision-llm --shell /usr/sbin/nologin provision-llm
# AMD and Intel GPUs: access to /dev/kfd and /dev/dri/renderD* (NVIDIA needs nothing)
for g in render video; do getent group "$g" > /dev/null && sudo usermod -aG "$g" provision-llm; done

# binary + libraries, side by side, owned by root (the build puts every library in build/bin)
sudo install -d -m 0755 /opt/provision-llm
sudo install -m 0755 build/bin/provision-server /opt/provision-llm/
sudo cp -P build/bin/*.so* /opt/provision-llm/

# models: read-only GGUF files for models.ini, the model cache and the drop-in directory
sudo install -d -m 0755 /srv/provision-llm /srv/provision-llm/models
sudo install -d -m 0750 -o provision-llm -g provision-llm /srv/provision-llm/models-rw /srv/provision-llm/models-rw/hf
sudo install -d -m 2775 -o provision-llm -g provision-llm /srv/provision-llm/models-rw/local
sudo usermod -aG provision-llm "$USER"   # you may then drop .gguf files into models-rw/local

# configuration
sudo install -D -m 0640 -o root -g provision-llm deploy/linux/provision-llm.env.example /etc/provision-llm/provision-llm.env
sudo install -m 0644 deploy/linux/provision-llm.service /etc/systemd/system/
```

Owners and modes:

| Path | Owner | Mode | Content |
|---|---|---|---|
| `/opt/provision-llm` | root | 0755 | `provision-server` and its `.so` files |
| `/etc/provision-llm/provision-llm.env` | root:provision-llm | 0640 | settings, may hold the admin key / `HF_TOKEN` |
| `/etc/provision-llm/models.ini` | root:provision-llm | 0640 | model presets |
| `/srv/provision-llm/models` | root (or you) | 0755, files 0644 | GGUF files named in `models.ini` |
| `/srv/provision-llm/models-rw/hf` | provision-llm | 0750 | model cache (`LLAMA_CACHE`) |
| `/srv/provision-llm/models-rw/local` | provision-llm, group-writable | 2775 | models directory (`--models-dir`): dropped-in and uploaded models |
| `/var/lib/provision-llm-admin` | provision-llm | 0700 | admin state, created by systemd (`StateDirectory=`) |
| `/var/lib/provision-llm` | provision-llm | 0700 | license state and home of the account, created by systemd |

Any other paths work too; change them in the environment file (`LLAMA_CACHE`,
`PROVISION_LLM_ARGS`) and, for the binary, in the unit. Put the model directories on a
filesystem with room for the models you plan to download or upload. Uploads from the
panel go into the models directory ([16](#16-uploading-gguf-files)), so it must be
writable for `provision-llm`. Delete uploaded models in the panel
([17](#17-deleting-models-of-the-models-directory)) rather than by hand: files written by
the service may not be writable for the members of its group. A model file in a home
directory is readable for the service but the unit makes `/home` read-only for it
(`ProtectHome=read-only`).

### 13.3 `models.ini` on a host

Start from `docker-dual/models.ini`: change the `model = /models/...` paths to
`/srv/provision-llm/models/...`, and change or remove the `device = CUDAn` lines to match
this machine (`provision-server --list-devices` prints the names). A minimal file:

```ini
version = 1

[*]
offline = true

[qwen2.5-chat]
model           = /srv/provision-llm/models/Qwen2.5-7B-Instruct-Q5_K_M.gguf
jinja           = true
ctx-size        = 8192
load-on-startup = true
tags            = chat, default
```

Without `device` a model uses all visible GPUs; choose a GPU per model in the panel
later. Keep the number of `load-on-startup = true` sections at or below
`PROVISION_LLM_MODELS_MAX`. Without any preset (remove `--models-preset` from
`PROVISION_LLM_ARGS`) the server serves only downloaded and dropped-in models.

```bash
sudo install -m 0640 -o root -g provision-llm models.ini /etc/provision-llm/models.ini
```

### 13.4 Settings

Edit `/etc/provision-llm/provision-llm.env`. Every variable is explained in the file;
the important ones:

| Variable | Default in the file | Meaning |
|---|---|---|
| `PROVISION_LLM_HOST`, `PROVISION_LLM_PORT` | `127.0.0.1`, `8080` | where the server listens; keep loopback when nginx is in front; without nginx the LAN address or `0.0.0.0`, together with `PROVISION_ADMIN_ALLOW_FROM` |
| `PROVISION_LLM_MODELS_MAX` | `3` | max loaded models (`--models-max`) |
| `PROVISION_LLM_ARGS` | `--models-preset /etc/provision-llm/models.ini --models-dir /srv/provision-llm/models-rw/local` | further router arguments; no per-model options here, they would be locked for every model. `--models-dir` is also where uploads go: without it the panel cannot upload |
| `LLAMA_CACHE` | `/srv/provision-llm/models-rw/hf` | model cache for downloads |
| `PROVISION_ADMIN` | `1` | the admin layer |
| `PROVISION_ADMIN_ALLOW_FROM`, `PROVISION_ADMIN_TRUSTED_PROXIES` | commented out (off) | the server's own `/admin` allow-list, for a host without nginx ([4.3](#43-built-in-address-allow-list-hosts-without-nginx)) |
| `PROVISION_ADMIN_*` | | as in [3.1](#31-server-environment-variables) |

The `PROVISION_LLM_*` names exist only for the unit's command line. Every other
variable in the file also reaches the model processes, which inherit the router's
environment (the `PROVISION_ADMIN_*` variables excepted).

### 13.5 nginx

Optional. The site is one way to restrict `/admin` (and `/cors-proxy`) to the admin
addresses; the other is the server's own allow-list
([4.3](#43-built-in-address-allow-list-hosts-without-nginx)) on a host without nginx.
Both together also work: then set `PROVISION_ADMIN_TRUSTED_PROXIES=127.0.0.1` (the site
sends `X-Forwarded-For`) and do not list `127.0.0.1` in `PROVISION_ADMIN_ALLOW_FROM`. The
server then re-checks the address nginx forwards, but it does not restrict the host
itself: any process on the host can send its own `X-Forwarded-For` straight to
`127.0.0.1:8080` and name an allowed address, so for the host only the admin key applies
(as with the site alone). `PROVISION_ADMIN_TRUSTED_PROXIES=127.0.0.1` alone, without the
server's allow-list, makes the activity log record the address nginx saw as verified
instead of `127.0.0.1` ([18.2](#182-who-addresses-and-proxies)); the same caveat about
processes on the host applies to what the log records.

```bash
# Debian / Ubuntu
sudo install -m 0644 deploy/linux/nginx-provision-llm.conf /etc/nginx/sites-available/provision-llm
sudo ln -s ../sites-available/provision-llm /etc/nginx/sites-enabled/provision-llm
# RHEL / Fedora / SUSE / Arch
sudo install -m 0644 deploy/linux/nginx-provision-llm.conf /etc/nginx/conf.d/provision-llm.conf
#   Arch only: the stock /etc/nginx/nginx.conf reads nothing from conf.d; add this line to
#   its http { } block:   include /etc/nginx/conf.d/*.conf;

# SELinux (RHEL / Fedora; `getenforce` prints Enforcing): nginx may listen only on the
# HTTP ports of the policy (80, 443, 8008, ...) and connect to no other port, so allow
# port 8096 and the connection to the server (semanage: policycoreutils-python-utils)
sudo semanage port -a -t http_port_t -p tcp 8096 || sudo semanage port -m -t http_port_t -p tcp 8096
sudo setsebool -P httpd_can_network_connect 1

sudo nginx -t && sudo systemctl enable nginx && sudo systemctl reload-or-restart nginx
```

The site has the same rules as the Docker front door ([4.1](#41-address-allow-list-nginx)):
`/admin`, `/admin/...` and `/cors-proxy` only for the addresses of the allow-list (the
`geo` block at the top of the file; by default only the host itself, `127.0.0.1` and
`::1`), everything else forwarded unchanged with streaming. It listens on port 8096, the
port of the Docker deployment. Add your admin workstations to the `geo` block:

```nginx
geo $provision_llm_admin_allowed {
    default         0;
    127.0.0.1       1;
    ::1             1;
    10.64.0.11      1;
    10.64.0.0/24    1;
}
```

A refused request appears in `/var/log/nginx/access.log` with status 403 and the client
address that nginx saw. Open port 8096 in the host firewall for the clients of the API.
If you change the port in `listen`, run the `semanage port` line for the new port too.

### 13.6 First start

```bash
# what the server finds on this machine (as the service account)
sudo -u provision-llm env LD_LIBRARY_PATH=/opt/provision-llm /opt/provision-llm/provision-server --list-devices

sudo systemctl daemon-reload
sudo systemctl enable --now provision-llm
journalctl -u provision-llm -f

curl -fsS http://127.0.0.1:8096/health
curl -fsS http://127.0.0.1:8096/v1/models | jq -r '.data[].id'
curl -s http://127.0.0.1:8096/admin/status            # {"admin":true,"authorized":false,...}
sudo cat /var/lib/provision-llm-admin/admin.key       # the admin key
```

Then open `http://<host>:8096/` from an allowed address: the sidebar shows **Admin**,
and **Settings -> Diagnostics** shows what was detected (GPU backends, GPUs, vendor
tool, state directory).

`--list-devices` creates a short-lived context on every GPU, like the inventory; run it
once, not in a loop.

### 13.7 Upgrade and rollback

```bash
cd provision-llm && git pull
cmake --build build --config Release -j"$(nproc)" --target llama-server

# the new version in a directory of its own: no library of the old build stays behind
sudo rm -rf /opt/provision-llm.new && sudo install -d -m 0755 /opt/provision-llm.new
sudo install -m 0755 build/bin/provision-server /opt/provision-llm.new/
sudo cp -P build/bin/*.so* /opt/provision-llm.new/

sudo systemctl stop provision-llm
# keep the admin state and the running version for a rollback (replaces an older .prev)
sudo tar czf /root/provision-llm-admin-$(date +%F).tgz -C /var/lib provision-llm-admin
sudo rm -rf /opt/provision-llm.prev && sudo mv /opt/provision-llm /opt/provision-llm.prev
sudo mv /opt/provision-llm.new /opt/provision-llm
# compare deploy/linux/ with your installed copies and take over new settings
diff deploy/linux/provision-llm.env.example /etc/provision-llm/provision-llm.env
diff deploy/linux/provision-llm.service /etc/systemd/system/provision-llm.service
sudo systemctl daemon-reload
sudo systemctl start provision-llm
```

Rollback:

```bash
sudo systemctl stop provision-llm
sudo rm -rf /opt/provision-llm && sudo mv /opt/provision-llm.prev /opt/provision-llm
# only if the admin state must go back too:
sudo rm -rf /var/lib/provision-llm-admin && sudo tar xzf /root/provision-llm-admin-<date>.tgz -C /var/lib
sudo systemctl start provision-llm
```

The upgrade notes of [14.1](#141-every-update-record-the-image-and-back-up-the-admin-state)
about the state files apply here as well.

## 14. Upgrading an existing Docker host

### 14.1 Every update: record the image and back up the admin state

CI pushes one rolling tag per flavour (`:dev`, `:release`); `docker compose pull`
replaces the image the host has, and no older version stays available. Before every
update:

```bash
cd /path/to/the/stack

# 1. the image the running container uses: the only way back
docker image inspect --format '{{index .RepoDigests 0}}' \
    "$(docker inspect --format '{{.Image}}' llama-dual)" | tee image-before-update.txt

# 2. the admin state (settings, per-model settings, key)
docker compose exec -T llama-dual tar czf - -C /var/lib/provision-llm-admin . \
    > admin-state-$(date +%F).tgz
```

Rollback to that image, and if needed the state:

```bash
# only if the admin state must go back too: with the server stopped (a running server
# writes its files again), replace the content of the volume with the backup
docker compose stop llama-dual
docker compose run --rm --no-deps -T --entrypoint sh llama-dual -c \
    'find /var/lib/provision-llm-admin -mindepth 1 -delete && tar xzf - -C /var/lib/provision-llm-admin' \
    < admin-state-<date>.tgz

# the image of before the update
LLAMA_DUAL_IMAGE="$(cat image-before-update.txt)" docker compose up -d
```

Set `LLAMA_DUAL_IMAGE` in `.env` to stay on that image until you update deliberately.
An image built locally (`docker compose build`) has no repository digest; give it a
tag of its own instead (`docker tag "$(docker inspect --format '{{.Image}}' llama-dual)"
llama-dual:before-update`) and use that tag as `LLAMA_DUAL_IMAGE`.

### 14.2 From the stage-1 admin panel to this version

For a host that already runs the admin panel (`docker-dual/nginx/` exists):

1. Record the image and back up the admin state ([14.1](#141-every-update-record-the-image-and-back-up-the-admin-state)).
2. Copy the new `docker-compose.yml` (it adds `NVIDIA_DRIVER_CAPABILITIES` and
   `PROVISION_ADMIN_GPU_PROBE`); `nginx/default.conf.template` and `models.ini` are
   unchanged by this version. Re-apply your own edits.
3. `docker compose pull && docker compose up -d`.

What changes:

- No migration. `admin-state.json` keeps its format; `admin-models.json` appears with
  the first saved per-model setting. Nothing restarts because of the update itself
  beyond the container re-creation.
- At start-up the server first runs the GPU inventory ([7.1](#71-which-gpus-the-server-has)),
  which briefly needs a little memory on every GPU, then loads the models as before.
- An older image started with the new volume ignores `admin-models.json` and leaves it
  in place; the next update to this version uses it again.
- To drop the per-model settings: **Remove custom settings** per model, or remove
  `admin-models.json*` ([11](#11-storage)).
- This version adds the upload of GGUF files and the deletion of models of the models
  directory ([16](#16-uploading-gguf-files), [17](#17-deleting-models-of-the-models-directory)).
  Both use `/models-rw/local`, which the compose file already mounts; nothing changes in
  `docker-compose.yml`, the nginx template or `models.ini` for them.
- It also adds the activity log ([18](#18-activity-log)): `admin-audit.jsonl` appears in
  the admin state volume at the first start, beginning with `audit.started`; earlier
  actions are not in it. An older image leaves the file alone.

### 14.3 From a host without the admin panel: before you start

The host: `docker-compose.yml` + `models.ini` + `models/` in one directory, the server
published directly on port 8096.

- The stack directory keeps its name and location. Compose derives the project name
  from it, and the license volume is `<project>_llama-license-state`; under another
  project name the server would start with an empty license volume.
- The host must be able to pull the nginx image. If Docker Hub is not reachable,
  mirror `nginx:1.27-alpine` into your registry and set `NGINX_IMAGE`.
- Pick the place for added models: `./models-rw` next to the compose file, or another
  path via `MODELS_RW_DIR`. It needs room for the models you plan to download.
- Decide which addresses may use the admin API (`ADMIN_ALLOW_RULES`).
- Record the running image ([14.1](#141-every-update-record-the-image-and-back-up-the-admin-state)).
- The server image must be the one built from the same revision as these files
  (the rolling `:dev` / `:release` tag after the CI build). With an older image the
  admin layer is absent (`/admin` answers 404, downloads are not kept on the host), and
  the new `models.ini` needs `MODELS_MAX` of 2 or more: an older image has no start-up
  guard, so with `MODELS_MAX=1` its router refuses to start
  (`number of models to load on startup (2) exceeds models_max (1)`) and restarts in a
  loop. If `MODELS_MAX=1` is set on the host, keep the old `models.ini` until the new
  image is pulled. Step 4 below checks which image you got.

### 14.4 Steps

```bash
cd /path/to/the/stack

# 1. keep the current files for rollback
cp docker-compose.yml docker-compose.yml.before-admin
cp models.ini models.ini.before-admin

# 2. copy from the repository (docker-dual/):
#      docker-compose.yml
#      models.ini                     (re-apply your own edits, if you had any)
#      nginx/default.conf.template    (new directory)
#    and create the model storage yourself, so that you own it (see 11. Storage):
mkdir -p models-rw/hf models-rw/local

# 3. settings, e.g. in .env
#      ADMIN_ALLOW_RULES="allow 10.64.0.11;"

# 4. fetch the images and check them BEFORE touching the running stack
docker compose pull
#    the server image has the new entrypoint: prints a number greater than 0
#    (0 means an older image, see 14.3)
docker compose run --rm --no-deps -T --entrypoint grep llama-dual -c models.startup.ini /entrypoint.sh
#    the nginx configuration is valid AND was rendered from the template: after
#    "syntax is ok" / "test is successful" it prints the /admin location with your rules
docker compose run --rm --no-deps -T nginx nginx -T | grep -A3 'location = /admin {'

# 5. apply: re-creates llama-dual (models load again) and starts nginx on the same port
docker compose up -d
```

### 14.5 Check

```bash
BASE=http://<host>:8096

# the API as before
curl -fsS $BASE/health
curl -fsS $BASE/v1/models | jq -r '.data[].id'

# both shipped models are preloaded now
docker compose logs llama-dual | grep -i "(startup) loading model"

# streaming is not buffered: the lines must appear one by one, not all at the end
curl -N -s $BASE/v1/chat/completions -H 'Content-Type: application/json' \
    -d '{"model":"qwen2.5-chat","stream":true,"messages":[{"role":"user","content":"Count from 1 to 20."}]}'

# the event feed stays open and reports changes right away. Start it, then unload and
# load the embedding model from another terminal (pinning does not block an explicit
# unload). Events for gte-qwen2-embed with "status":"unloaded", then "loading" and
# "loaded" must appear the moment they happen; send the load after "unloaded" arrived.
curl -N -s $BASE/models/sse
curl -s -X POST $BASE/models/unload -H 'Content-Type: application/json' -d '{"model":"gte-qwen2-embed"}'
curl -s -X POST $BASE/models/load -H 'Content-Type: application/json' -d '{"model":"gte-qwen2-embed"}'

# admin: from an address that is NOT allowed -> 403 with "type":"admin_forbidden"
curl -s -o /dev/null -w '%{http_code}\n' $BASE/admin/status
# admin: from an allowed address -> 200 {"admin":true,"authorized":false,...}
curl -s $BASE/admin/status
# spelling tricks from a not-allowed address: 403, 403 and 400
curl -s -o /dev/null -w '%{http_code}\n' --path-as-is "$BASE/%61dmin/status"
curl -s -o /dev/null -w '%{http_code}\n' --path-as-is "$BASE/x/../admin/status"
curl -s -o /dev/null -w '%{http_code}\n' --path-as-is "$BASE/%u0061dmin/status"
# the CORS proxy is locked the same way: 403 with "type":"admin_forbidden" from a not-allowed address
curl -s "$BASE/cors-proxy?url=http://127.0.0.1:8080/admin/status"

# which address did nginx see?
docker compose logs --tail 20 nginx

# the admin key, and the admin layer's own view
docker compose exec llama-dual cat /var/lib/provision-llm-admin/admin.key
docker compose logs llama-dual | grep "admin:"

# GPU memory is visible inside the container (prints the GPUs, not "not found")
docker compose exec llama-dual sh -c 'echo $NVIDIA_DRIVER_CAPABILITIES; command -v nvidia-smi'
```

Then open the web UI from an allowed address: the sidebar shows **Admin**, and
**Settings -> Diagnostics** shows what the server detected.

### 14.6 What changes for users

- Address and port stay the same. All existing routes, request and response formats
  stay the same.
- `/admin/...` now answers `403` (from addresses that are not allowed) instead of `404`.
  So does `/cors-proxy`, which the server answers with `403` anyway while the MCP proxy
  is off (as in this stack).
- While the server restarts, clients get `502 Bad Gateway` from nginx instead of a
  refused connection.
- The server sees nginx as the client of every request; the real client address is in
  the nginx access log and in `X-Forwarded-For`.
- Up to three models are loaded at once instead of two, and the embedding model is
  loaded at start-up instead of on first use. The two shipped models are pinned: a
  third model can no longer push them out.
- A model that is in the model cache is loaded without contacting Hugging Face
  (`offline = true`).
- Models downloaded through `POST /models` before the upgrade lived inside the old
  container and are gone with it, as with every re-creation of that container. From
  now on they are kept in `models-rw/hf` on the host.
- If `MODELS_MAX=1` or `2` is set on the host (shell or `.env`), it still applies; see
  [5.2](#52-pinned-models-pinned) for what that means for the pins.

### 14.7 Rollback

```bash
docker compose down              # NOT "down -v": that would delete the license volume
cp docker-compose.yml.before-admin docker-compose.yml
cp models.ini.before-admin models.ini
LLAMA_DUAL_IMAGE="$(cat image-before-update.txt)" docker compose up -d
```

The old compose file works with the new image as well (the admin layer stays off,
nothing is published through nginx). The `llama-admin-state` volume and `models-rw/`
are left in place and are simply not used; remove them when you are sure.

Smaller steps back, without changing files:

- Admin layer off, proxy stays: `PROVISION_ADMIN=0 docker compose up -d`.
- Close `/admin` for everyone: `ADMIN_ALLOW_RULES= docker compose up -d`.
- Back to the previous model limit: `MODELS_MAX=2 docker compose up -d` (and reset the
  value in the panel if one was saved there).

## 15. Troubleshooting

Start with **Settings -> Diagnostics** ([10](#10-diagnostics)): it names most of the
problems below and what to do.

- **Port 8096 does not answer after `up -d`, `docker compose ps` shows nginx
  restarting.** The nginx configuration is invalid, almost always because of
  `ADMIN_ALLOW_RULES` (missing semicolon). `docker compose logs nginx` names the line;
  fix the value and run `docker compose up -d`.
- **`docker compose up` or `run` stops with `bind source path does not exist`
  (`.../nginx/default.conf.template`).** The `nginx/` directory was not copied next to
  `docker-compose.yml` (step 2 of [14.4](#144-steps)). Copy it and run the command again.
- **Port 8096 answers "Welcome to nginx!" on `/` and `404` for every API route.** nginx
  runs with its stock configuration because the template was not rendered, for
  example under a compose file that mounts an empty `./nginx` directory. Use the
  shipped `docker-compose.yml` (it refuses to start without the template) and check
  with the `nginx -T` command of step 4 in [14.4](#144-steps).
- **`502 Bad Gateway`.** The server is down or still starting (`docker compose ps`,
  `docker compose logs llama-dual`; host: `systemctl status provision-llm`,
  `journalctl -u provision-llm`). Host with SELinux: nginx may not connect at all
  until `setsebool -P httpd_can_network_connect 1`.
- **Host: nothing listens on port 8096, the nginx error log says
  `bind() to 0.0.0.0:8096 failed (13: Permission denied)`.** SELinux lets nginx listen
  only on the HTTP ports of its policy: run the `semanage port` line of
  [13.5](#135-nginx), then `sudo systemctl reload-or-restart nginx`. On Arch, with
  nothing in the log: `nginx.conf` does not include `conf.d/*.conf` ([13.5](#135-nginx)).
- **`llama-dual` restarts in a loop, its log ends with
  `number of models to load on startup (2) exceeds models_max (1)`.** An older server
  image (no start-up guard) with the new `models.ini` and `MODELS_MAX=1`. Pull the new
  image, or keep the old `models.ini` or raise `MODELS_MAX` until then, see
  [14.3](#143-from-a-host-without-the-admin-panel-before-you-start). On a host
  installation: raise `PROVISION_LLM_MODELS_MAX` or set fewer `load-on-startup` sections.
- **The Admin item does not appear in the web UI.** The probe `GET /admin/status`
  failed ([4.4](#44-the-admin-item-in-the-web-ui)): `403` means your address is not on
  the allow-list (check the nginx log for the address it saw; with the server's own
  allow-list the `admin: refused ... from` line of the server log names it, and so does
  the Admin page in a browser that holds a saved admin key); `404` means the admin layer
  is off (`PROVISION_ADMIN`) or the image is older than the admin feature.
- **`#/admin` opens the chat instead of the Admin page.** The address is refused
  (`403`), see the previous point.
- **Every `/admin` request answers 403 "The admin API is locked".**
  `PROVISION_ADMIN_ALLOW_FROM` or `PROVISION_ADMIN_TRUSTED_PROXIES` has an entry the
  server cannot read; the error at startup names it
  ([4.3](#43-built-in-address-allow-list-hosts-without-nginx)). Fix the value and
  restart the server.
- **The server's allow-list refuses a listed client behind a proxy.** The server sees
  the proxy's address: list the proxy in `PROVISION_ADMIN_TRUSTED_PROXIES` and make
  sure it sends `X-Forwarded-For`. The refused-request log line says which address was
  used and why `X-Forwarded-For` was not.
- **`/admin` answers 403 although the address is listed.** nginx sees another address
  than you expect (NAT, a proxy in between, a request made on the Docker host). The
  log line `access forbidden by rule, client: ...` shows it (host site: the 403 lines of
  the access log).
- **The key is refused after a restart.** Without the `llama-admin-state` volume the
  generated key lives in the container and changes when the container is re-created.
  Keep the volume, or set `PROVISION_ADMIN_KEY`.
- **The log says `PROVISION_ADMIN_KEY_FILE (...) is not usable`.** The file cannot be
  read, is empty, or is not ASCII / UTF-8 text (a UTF-16 file from PowerShell's `>`);
  the server uses the state directory's key instead. See [4.2](#42-admin-key).
- **The log says the state directory cannot be created.** The server runs as an account
  that may not create `/var/lib/provision-llm-admin` (for example started by hand).
  Set `PROVISION_ADMIN_STATE_DIR` to a writable, persistent directory, or use the
  systemd unit, which creates it.
- **`Permission denied` when copying a file into `models-rw/local`.** The directory was
  created by Docker or the container and belongs to root. Take it over with
  `sudo chown "$(id -u):$(id -g)" models-rw models-rw/local`, or create the directories
  yourself before the first start ([11. Storage](#11-storage)).
- **A changed `MODELS_MAX` has no effect.** A value saved in the panel takes
  precedence, see [5. Settings](#5-settings).
- **`[entrypoint] warning: more load-on-startup models than may be loaded at once`.**
  The limit is lower than the number of preloaded models; the entrypoint switched the
  preload off for the models it names. Raise `MODELS_MAX` (or the value in the panel)
  if you want them preloaded.
- **A downloaded model fails to load with `invalid device`.** "GPU for new models"
  names a device the server does not have; the panel shows `default_device_not_present`.
  `docker compose run --rm llama-dual --list-devices` (host:
  `provision-server --list-devices`) lists the valid names.
- **A section of `models.ini` with `hf-repo` no longer downloads its model on first
  load.** That is `offline = true` in `[*]`; add `offline = false` to that section or
  download the model from the panel.
- **A download fails immediately.** `GET /admin/status` shows whether downloads are
  available at all; the pre-flight check tells whether Hugging Face is reachable from
  the server. Behind a proxy or without internet access set `MODEL_ENDPOINT` to a
  mirror, or drop the GGUF file into `models-rw/local` instead.
- **The GPU cards show no live memory ("memory is shown as measured at startup").** No vendor tool
  was found. Docker with NVIDIA: `NVIDIA_DRIVER_CAPABILITIES` must contain `utility`
  (the shipped compose file sets it; check with the last command of [14.5](#145-check)).
  Host: install the vendor tool (`nvidia-smi` comes with the driver; `amd-smi` /
  `rocm-smi` with ROCm) or name it in `PROVISION_ADMIN_GPU_PROBE`. Intel and Vulkan
  GPUs have no supported tool: use **Re-detect GPUs** for fresh figures.
- **"GPU backend did not initialise".** The binary has a GPU backend but it found no
  GPU: driver missing or not loaded, the service account lacks the `render` / `video`
  group (AMD: `/dev/kfd`; AMD and Intel: `/dev/dri/renderD*`), or the container was
  started without GPUs (`deploy.resources` in the compose file, NVIDIA Container
  Toolkit).
- **`mapping: unknown` on the GPU cards.** The vendor tool's rows could not be matched
  to the server's GPUs by PCI bus id (the backend reports no PCI id, or the inventory
  failed). Run **Re-detect GPUs**; the cards then use the inventory values.
- **`probe_stuck` or the vendor tool's error in red.** The vendor tool hangs or fails,
  usually a GPU or driver fault. Check `nvidia-smi` / `amd-smi` on the host; a reboot is
  often needed.
- **The fit check or Re-detect GPUs answers `409 check_stuck` / `409 rescan_stuck`.** An
  earlier helper process was stopped but has not exited, usually because it hangs in
  the GPU driver; Diagnostics lists it as `stuck_helpers`. Check the driver (`dmesg`)
  and restart the server if it does not clear.
- **Save and restart answers `504 stop_timeout`.** The model process did not stop,
  typically because it hangs in the driver after a GPU fault. Nothing was saved and the
  other models are not affected. Restart the container (or the machine).
- **A model's settings were reverted (`reverted`, `reverted_at_startup`).** The new
  values made the model fail twice in a row (or the server stopped during a failing
  start). The failure panel shows why; change the values and try again.
- **Saving per-model settings answers `409 model_settings_read_only`.**
  `admin-models.json` is damaged or from a newer version; `GET /admin/status`
  (`state.model_settings.error`) says which. Fix or remove the file ([11](#11-storage)).
- **`device_moved` / `device_missing` on a model.** The GPU the setting was saved for
  now has another name or is gone ([7.4](#74-gpu-identity-moved-and-missing-gpus)).
  Choose the GPU again.
- **`model_devices_not_applied`.** `MODEL_DEVICES` names the model, but its section does
  not carry that device: the model is not a `models.ini` section, the name is mistyped,
  or `MODEL_DEVICES` is set where no entrypoint applies it (a host installation).
- **A model fails with `out_of_memory`.** Lower the context size, use KV cache `q8_0`,
  fewer GPU layers or another GPU; the fit check in the dialog shows the need per GPU.
- **The fit check says `timeout` or stays `unknown`.** The check takes longer than 30
  seconds (a very large model on slow storage) or the GPU memory is not known; the
  dialog falls back to the rough estimate. Saving still works.
- **The Upload GGUF tab shows a reason instead of the drop zone.** `upload.reason` in
  `GET /admin/status` says why ([16.1](#161-what-can-be-uploaded)): no `--models-dir`
  (Docker: the `/models-rw` mount; host: `PROVISION_LLM_ARGS`), the directory is missing
  or not writable for the server, `.provision-upload` is a file or a symbolic link, or
  something is mounted over it. Fix it and restart the server.
- **The upload says "A proxy limits the request size; the upload uses smaller pieces".**
  A proxy in front of the server refuses large requests (a `413` that is not the
  server's own). It still works, only slower; for full speed add the two nginx lines of
  [16.9](#169-behind-a-reverse-proxy) to that proxy.
- **Chunks answered `502` by a proxy.** The server refused a chunk before its body (for
  example while the upload is busy) and closed the connection; the page continues by
  itself ([16.9](#169-behind-a-reverse-proxy)). Many in a row: see the server log.
- **The upload stops with "The disk is full" (`507`).** Free space on the filesystem of
  the models directory, then **Resume**; or **Cancel**, which frees what was received.
- **"This is not the same file" (`content_mismatch`) when continuing an upload.** The file
  picked again differs from the one that was being sent (another file with the same
  name and size, or a newer version). Pick the right file, or cancel and upload the new
  one.
- **"Another browser tab or client is uploading this model right now"
  (`other_client_active`).** Another browser tab, browser or admin is sending this
  upload. **Take over** continues it here; the other one stops.
- **An upload was rejected with "This server build does not support the model
  architecture"** (`unknown_architecture`). The model needs a newer llama.cpp. Update
  the image (or rebuild the binary), then upload again.
- **An upload waits as `ready`.** `blocked_by` says why ([12.14](#1214-get-adminmodelsupload)):
  `name_taken` (rename it), `sha256_mismatch` (check the expected value; publish anyway
  or cancel), `check_failed` (retry; the check timed out, ran out of memory or could not
  read a file, the data is kept; on Windows `path_too_long`: rename it to at most
  `max_name_length` characters, [16.5](#165-where-the-files-end-up)), `settings_not_saved`
  (the admin state directory is not writable: publish without the placement, or fix the
  directory), `settings_invalid` (a setting is no longer valid, for example its GPU is
  gone: publish without the placement, or change the upload's settings with `update`),
  `manual` or `publish_error` (publish).
- **An uploaded model is not in the list after publishing, with `reload_error`.** The
  files are published but the model list could not be rebuilt (for example a broken
  `models.ini`). Fix the cause and press **Rescan**.
- **Delete answers `409 model_running`.** The model started in the meantime: the dialog
  then says it is running and offers **Stop and delete** (`stop` in the API).
  `504 stop_timeout`: the model did not stop, nothing was deleted; see **Save and
  restart answers `504 stop_timeout`** above.
- **Delete answers `409 not_deletable`.** The model is not a model of the models
  directory (a downloaded model: use **Delete**; a `models.ini` section, or a model whose
  file is set in `models.ini` or on the router command line: edit that), or one of its
  files lies outside the models directory, is a symbolic link or is on another
  filesystem; **Remove only the projector** is refused for a projector that every model
  gets ([17.3](#173-what-is-refused)).
- **A multimodal model fails at every start after an upload.** The projector probably
  does not fit the model. **Delete files** -> **Remove only the projector**
  ([17.4](#174-remove-projector)), then upload the model again with the right projector
  if you need image input.
- **The Activity tab says the log is kept in memory only** (`audit_not_persisted` in
  `GET /admin/status`). The admin state directory is not writable, or a write to the log
  failed (a full disk). The Diagnostics items "Audit log" and "Admin state directory"
  say why. Download the log if the records matter and fix the directory: the server tries
  again every minute and then writes the records it kept, no restart needed
  ([18.6](#186-when-the-log-cannot-be-written)).
- **The Activity tab shows the address of nginx, with the real address as
  "unverified".** The server cannot check the `X-Forwarded-For` of a proxy that is not
  listed in `PROVISION_ADMIN_TRUSTED_PROXIES`; in the Docker stack that is the default.
  [18.2](#182-who-addresses-and-proxies) shows how to trust the nginx of the stack.
- **An action is missing from the Activity tab.** Loading, unloading, downloading and
  `DELETE /models` go through the public routes and show only by their effects
  ([18.4](#184-what-cannot-be-recorded)); files added to or removed from the models
  directory by hand leave no record at all; requests that nginx or `--api-key` refused
  never reach the admin layer; read-only requests are not recorded.

## 16. Uploading GGUF files

An admin can add a model to the running server by uploading its GGUF files from the
browser: Admin page, **Models** tab, **Add model** -> **Upload GGUF**, or drop the files
onto the Models tab. The files go into the models directory (`--models-dir`; in Docker
`/models-rw/local`). This server build checks them before the model appears in the
list, and the model's first start already uses the GPU and context chosen in the
dialog. It works on a host and in Docker, with any GPUs or none, over a LAN, a VPN or a
reverse proxy, and it survives a dropped connection, a reload of the page and a restart
of the server. Files dropped onto the Models tab ("Drop GGUF files to upload them") open
the same dialog on its **Upload GGUF** tab.

### 16.1 What can be uploaded

- **One upload is one model**: a single `.gguf` file, or a complete split set
  (`<name>-00001-of-00003.gguf`, ...), plus at most one projector (`mmproj` file) of a
  multimodal model.
- **Several models at once**: pick or drop all files together. The dialog groups them by
  file name: shards that share `<prefix>-NNNNN-of-NNNNN` are one model, every other file
  is a model of its own, and a single projector is paired with the single model when
  that is unambiguous (otherwise the dialog asks). The models are uploaded one after the
  other; the others wait in a queue, each with its own name and placement.
- **Not supported**: draft models for speculative decoding (`mtp-`, `dspark-`,
  `dflash-`, `eagle3-` files), adapters (LoRA), imatrix files, adding a projector to a
  model that is already there, and replacing the files of an existing model. Upload
  under a new name, or delete the old model first ([17](#17-deleting-models-of-the-models-directory)).
- **Needed**: the server runs with `--models-dir`, and the directory exists and is
  writable for the server. Docker: the `/models-rw` mount of the shipped compose file;
  the entrypoint then creates `local/` and passes `--models-dir`
  ([3.3](#33-what-the-entrypoint-does)). Host: `--models-dir` in `PROVISION_LLM_ARGS`
  ([13.4](#134-settings)). A models directory cannot be added while the server runs:
  restart it after the change. Uploads never go into the model cache (`LLAMA_CACHE`) or
  into the read-only `/models`.

When uploads are not possible, the **Upload GGUF** tab shows the reason and the fix
instead of the drop zone (`upload` in `GET /admin/status`, [12.1](#121-get-adminstatus)):

| `reason` | Meaning | Fix |
|---|---|---|
| `no_models_dir` | the server runs without `--models-dir` | Docker: mount `/models-rw` (the shipped compose file does). Host: add `--models-dir /srv/provision-llm/models-rw/local` to `PROVISION_LLM_ARGS`. Then restart the server |
| `models_dir_missing` | the directory named by `--models-dir` does not exist | create it ([13.2](#132-account-directories-files)), restart |
| `not_writable` | the server cannot write into it | owner and mode, see [11](#11-storage) and [13.2](#132-account-directories-files) |
| `staging_unusable` | `.provision-upload` in the models directory is a file or a symbolic link, or cannot be created | remove it; the server creates the folder itself |
| `staging_other_filesystem` | `.provision-upload` inside the models directory is on another filesystem (something is mounted there) | remove that mount: publishing is a rename and needs one filesystem |

### 16.2 In the panel

1. **Pick.** Drop the files or **Choose files**. The browser reads the GGUF header of each
   file itself ("Reading the file headers...", at most 64 MiB of each file, nothing is
   sent yet) and shows one row per file with its role (model, part k of n, projector),
   size, architecture, quantization and training context (for a projector: images and /
   or audio). It groups the files into models, asks which model a projector belongs to
   when that is not clear, and points out problems before anything is sent: missing parts
   of a split model, parts of different models, a shard number in the name that differs
   from the one in the file. Files that cannot be uploaded are listed under "Not
   uploaded" with the reason (a draft model, not a GGUF file, an unsupported GGUF
   version, an adapter, an imatrix file, a file picked twice, a projector without its
   model). This reading is only a hint: the server checks every file itself
   ([16.4](#164-checks-before-the-model-becomes-visible)).
2. **Name.** Proposed from the model file (the shard suffix and `.gguf` removed; never
   from the projector), and editable. The server checks it ([16.3](#163-names)) together
   with the free space; warnings never block (**Upload anyway**). Optionally, under
   "Verify with SHA-256", the expected SHA-256 of each file (as Hugging Face shows it on
   the page of the file). When saved settings of an earlier model with the same id
   exist, the dialog asks "Start fresh" (the default) or "Keep them", see
   [6.9](#69-settings-for-a-model-that-is-being-added).
3. **Placement** (optional): GPU, context size and parallel requests; the role ("Used
   for": embeddings, reranking) under **More**. Fields that are locked for new models
   show their lock. The switch "Publish automatically when the checks pass" is on by
   default.
4. **Upload.** Progress per file and in total (only what the server confirmed), speed
   and time left; **Pause**, **Resume**, **Cancel** (asks first; the uploaded part is
   deleted on the server). Further models wait as "waiting for its turn". The status
   line says, for example, "Connection problem, retrying in N s", "The server is
   restarting, retrying in N s", "Paused: the server cannot be reached", "The disk is
   full" (**Resume** once there is space) or "Another browser tab or client is uploading
   this model right now" (**Take over**). When the page has to use smaller pieces it says
   "A proxy limits the request size; the upload uses smaller pieces" and shows the nginx
   lines of [16.9](#169-behind-a-reverse-proxy). A file the server refuses while the
   upload runs is listed with the reason and **Drop this file** (the rest goes on), or
   cancel the upload. The upload runs in the page, not in the dialog: closing the dialog
   does not stop it (an "Uploading N%" indicator stays in the admin header). Closing or
   reloading the browser tab does (the browser asks first); pick the same files again to
   continue ([16.6](#166-resuming)).
5. **Checks.** "Computing the SHA-256..." (only after a restart of the server: otherwise
   the SHA-256 is computed while the file is received), "Reading the files..." (the
   classification), "Loading the model metadata with llama.cpp..." (the load check), or
   "Waiting for another check to finish...". The list of files shows "header checked"
   with the architecture and role of each file.
6. **Result.**
   - **Published**: "Published as *id*: it is in the model list, not loaded yet", per
     file the final name, role, size and SHA-256 (copyable), and **Open settings** and
     **Load now**. When a placement was chosen, the fit of that placement is checked by
     itself and shown there ([6.9](#69-settings-for-a-model-that-is-being-added)).
     Nothing has started the model.
   - **Rejected**: the reason in plain words ([16.4](#164-checks-before-the-model-becomes-visible)).
   - **Ready**: what it waits for, with the matching action: **Publish** (`manual`,
     `publish_error`), **Retry the checks** (`check_failed`), a corrected value with
     **Save and check again** or **Publish anyway** (`sha256_mismatch`), a new name with
     **Rename and publish** (`name_taken`), a shorter name with **Rename and check
     again** (Windows, `check_failed` with `path_too_long`,
     [16.5](#165-where-the-files-end-up)), **Publish without the placement**
     (`settings_not_saved`, `settings_invalid`) and, for `settings_not_saved`, **Retry**.
   - **Not started** (the create request was refused): the reason, with **Try again**,
     or a new name and **Try again with this name** when the name was the problem.

A banner on the Models tab and in the Upload tab lists the unfinished uploads ("N
unfinished uploads": each with its name, "12.3 GB of 40 GB uploaded" or what it waits for,
and the time left until it expires) with **Resume (pick the same files)** or **Show**, and
**Cancel**. Uploads started in another browser or by another admin are listed too.

### 16.3 Names

The name becomes the model id (`"model": "<name>"` in requests).

- Form: a letter or digit first, then letters, digits, `.`, `_` and `-`, at most 128
  characters (`^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$`).
- Refused, without regard to case: names that contain `mmproj` or `.gguf`, that start
  with `mtp-`, `dspark-`, `dflash-` or `eagle3-`, that contain a shard suffix
  `-NNNNN-of-NNNNN`, that end in `.`, and the reserved device names of Windows (`CON`,
  `NUL`, `COM1`, ...). The scanner of the models directory would take such a file for a
  projector, a draft or a shard, or change the id.
- Unique, without regard to case, among: every model id and alias the server lists
  (model cache, models directory and `models.ini`, so the name of a `models.ini` section
  counts too), every entry of the models directory as the scanner would name it (a
  folder name, or a file name with every `.gguf` removed, so `x.gguf.gguf` counts as
  `x`), and the names of the other unfinished uploads. A collision is `409 name_taken`
  with what the name collides with (`by`).
- Checked when the upload is created, when the name is changed, and again right before
  the files are published. When a model of that name appeared in the meantime, the
  upload waits as `ready` (`name_taken`): give it another name and publish; nothing is
  uploaded again.

### 16.4 Checks before the model becomes visible

No uploaded byte is trusted. The model appears in the list only after this server build
has shown that it can load the files.

| When | What is checked | If it fails |
|---|---|---|
| creating the upload | the name, 1 to 256 files of at least 24 bytes each, the format of an expected SHA-256 (64 hexadecimal characters), the settings with the rules of `POST /admin/models/settings` | `400` / `409`, nothing is created |
| every chunk | the offset rules, the optional CRC32 of the chunk, nothing beyond the declared size | the chunk is refused, the upload stays ([12.16](#1216-post-adminmodelsuploadchunk)) |
| the first bytes of each file | `GGUF` and version 2 or 3 | `422 not_gguf`: only that file is marked, the rest of the upload stays |
| as soon as a file's metadata has arrived, a few seconds after its start | the header check (below), on the partly received file | a definitive problem marks that file or rejects the upload within seconds, before the rest is sent |
| as soon as the metadata of every file is known (often before the last byte) | the classification: exactly one model (a single file, or a complete split set whose `split.no` / `split.count` match), at most one projector, and a projector only together with a model; the final names | `rejected` |
| after the last byte | the sizes; the SHA-256 (computed while receiving, or read again from disk after a restart of the server); the expected SHA-256 | a mismatch makes the upload wait as `ready` (`sha256_mismatch`) |
| then | the header check of every file again where it did not finish, and the classification again | as above |
| then | the load check, on the files under their final names: a projector with the call the server itself makes at every rescan (it must report vision or audio input); the model with llama.cpp's own loader, metadata and vocabulary only (`vocab_only`), on the first shard: tensor data within the file, the complete split set, the architecture, the vocabulary. The weights are not read | `rejected`, or `ready` (`check_failed`) when the check itself could not run |

**The header check** reads the GGUF metadata from the partly received file and decides
within seconds, before the rest is sent. An adapter or an imatrix file (`general.type`),
a projector whose metadata has values of the wrong type or that reports neither vision
nor audio, metadata that cannot be parsed, and metadata larger than 64 MiB make that file
unusable. An architecture this build does not know (also a model file without
`general.architecture`) rejects the upload. What the metadata alone cannot settle (the
loader needs more than the metadata) is a `warning` ("the metadata alone did not load
(...); checked again when the upload is complete") and is decided by the load check at
the end. The later parts of a split set hold only the split keys; they are checked
together with the first part. The check also reads what the dialog and `facts`
([12.14](#1214-get-adminmodelsupload)) show: the architecture, `general.name`, the size in
parameters, the training context, the number of layers and the quantization
(`general.file_type`). Its `message` is empty when the file is fine.

**What rejects, and what does not.** Only a definitive verdict rejects. A verdict about
one file makes only that file unusable; a verdict about the model rejects the upload
(`rejected`; its data is deleted at once). Everything that is not the files' fault keeps
the data:

| Outcome | State | What to do |
|---|---|---|
| one file cannot be used: not GGUF or an unsupported GGUF version (`not_gguf`), an adapter (`adapter`) or imatrix file (`imatrix`), a projector with wrongly typed metadata (`projector_bad_metadata`) or without vision / audio (`projector_without_modality`), metadata that cannot be parsed (`malformed`) or larger than 64 MiB (`header_too_large`) | that file is marked with its reason (`files[].error`; its chunks get `422`), the rest of the upload stays; the checks wait until the file is dropped | drop the file from the upload (**Drop this file**, `drop_files`), or cancel |
| the files cannot be a model: an architecture this build does not support (`unknown_architecture`), a file shorter than its own metadata says (`truncated`), an incomplete or inconsistent split set (`split_incomplete`, `split_mismatch`), two models (`two_models`), two projectors (`two_projectors`), a projector without its model (`projector_without_model`), the loader's own refusal (`load_failed`); in the load check also the codes of the first table row for the model or the projector | `rejected`, with the reason | for an unsupported architecture update the image or the binary, then upload again; otherwise fix the files |
| the check could not finish: it timed out (slow storage), ran out of memory, could not be started, or could not read a file | `ready`, `check_failed` | **Retry the checks** (`publish`); the data is kept |
| the SHA-256 differs from the expected one | `ready`, `sha256_mismatch` | the dialog shows both values: **Publish anyway** (a mistyped or wrong expected value), correct the expected value (**Save and check again**), or **Cancel** (frees the space) |

A file that becomes unusable only after the files were arranged under their final
names (they were changed on disk while the server was down, or a newer build checks them
more strictly) can no longer be dropped: the upload waits as `ready` / `check_failed`
with that file's code; **Retry the checks** or cancel. `rejected` uploads also show
`staging_missing` when their data disappeared while the server was stopped. The
loader's `clip_as_model` (a projector loaded as the model) cannot occur through an
upload, because a file with the architecture `clip` is always taken as the projector.

**Not checked before publishing**: whether the projector fits the model (its width,
`n_embd`). That needs the projector's weights. A mismatch shows at the first start of the
model (the failure panel, [9](#9-when-a-model-fails-to-start)); **Delete files** ->
**Remove only the projector** ([17.4](#174-remove-projector)) then keeps the model
without it.

The checks run in a short-lived helper process of the server binary, one upload at a
time (others wait as `queued`), with at most 120 seconds per phase and without any GPU
backend, so they need no GPU memory. The helper may use at most 8 GiB of memory: on Linux
a limit of its address space (`RLIMIT_AS`), and it is the first process the kernel's OOM
killer picks (`oom_score_adj` 1000); on Windows a job object with a memory limit. A file
whose metadata section is larger than 64 MiB is refused before it is parsed. A crafted
file cannot take the memory of the running models; a check that runs out of memory
counts as `check_failed`, not as a verdict about the file.

### 16.5 Where the files end up

The server decides the final names from the id; the original file names are only shown.

| Upload | In the models directory |
|---|---|
| one file | `<id>.gguf` |
| a split set | `<id>/<id>-00001-of-0000N.gguf` ... `<id>/<id>-0000N-of-0000N.gguf`, numbered from `split.no` and `split.count` |
| a model with a projector | `<id>/<id>.gguf` and `<id>/mmproj-<id>.gguf` |
| a split set with a projector | the shards as above and `<id>/mmproj-<id>.gguf` |

- Shards are never put at the top level, where every shard would be listed as a model of
  its own; the loader needs exactly this lower-case shard pattern.
- Publishing is one rename on the same filesystem, and a set is one folder: a rescan
  never sees half of it. It never replaces anything: when an entry of that name
  appeared in the meantime the upload waits as `ready` (`name_taken`), and a rename
  that fails leaves it `ready` (`publish_error`) with **Publish** to try again.
- Before publishing, the checks arrange the files under their final names inside the
  upload's folder in `.provision-upload`. A move that fails there is undone, so a retry
  starts from a clean state. When a crash interrupts the first arrangement, the next
  start moves the files back (an upload rebuilt without its record cannot do that: the
  files that were moved already are sent again). A crash in the middle of renaming an
  upload that was already arranged (a rename after `name_taken` or `path_too_long`) is
  not repaired: the upload then waits as `ready` / `check_failed`; cancel it and upload
  again.
- Order: first the model's per-model settings are saved
  ([6.9](#69-settings-for-a-model-that-is-being-added)), then the files become visible,
  then the model list is rebuilt. A request that arrives in between (automatic loading)
  therefore already gets the chosen settings, never the defaults. When the rename fails,
  the previous settings entry is put back.
- The model appears **not loaded**; it starts on its first request or with **Load now**.
- If the model list cannot be rebuilt after the rename (for example `models.ini` was
  broken in the meantime), the files and the settings stay: the upload is `published`
  with `reload_error`, and the next rescan lists the model. Unlike a failed save of
  per-model settings ([12.8](#128-post-adminmodelssettings)), nothing is rolled back.
- The files and folders are group-writable (`0660` / `0770`) and, when the server runs
  as root (Docker), get the owner and group of the models directory, like the entrypoint
  does for `hf/` and `local/`; all of this is set before they become visible
  ([11](#11-storage), **Ownership**). They are synced to disk, together with their
  directories, before the rename, and the models directory after it.
- Windows only: the load check of a split model runs on the copy in
  `.provision-upload`, whose path is about 55 characters longer than the published one,
  and llama.cpp loads a split model only from a path of at most 259 characters. A name
  that makes that path too long waits as `ready` / `check_failed` (`path_too_long`, with
  the longest name that fits in `max_name_length`): **Rename and check again** (API: an
  `update` with the new name, then `publish`); nothing is uploaded again.

### 16.6 Resuming

| Event | What happens |
|---|---|
| A dropped connection, `429`, `502`, `503`, `504`, or another `5xx` that is not the server's JSON | The page asks the server how far it got and continues from there, re-sending up to 1 MiB before that point (compared byte for byte, so nothing is stored twice). It waits 1 second at first, growing to 30 seconds. After 30 minutes without success the upload shows "Paused: the server cannot be reached" and is retried every minute while the tab is open. |
| A reload of the page, or the tab was closed | The browser no longer has the files. The banner lists the unfinished uploads; **Resume (pick the same files)**. They are matched by name and size; the page then compares the CRC-32 of the first MiB of each file with the server's `head_crc32` (or, when the server does not know it, sends that MiB as a `verify=1` probe, [12.16](#1216-post-adminmodelsuploadchunk)), and the server compares the up to 1 MiB that is sent again before its offset. A different file with the same name and size is refused ("This is not the same file", `content_mismatch`). Files that were already complete need no new pick. The admin key is still there for the browser session. |
| The server restarts (systemd `Restart=on-failure`, a container restart, an image update) | Each file goes back to its last checkpoint (`durable_offset`): at most about 256 MiB or 15 seconds of data are sent again. The checks read the files again from disk for the SHA-256. An upload that was being checked or published is checked again from the files on disk and then published; files that were already published are recognised as such. An upload whose record in the admin state directory is missing (the directory was not writable) is rebuilt from its folder alone: without its settings, and it waits as `ready` / `manual` after its checks. |
| Another browser tab or another admin sends to the same upload | A chunk from another tab within 30 seconds of the last one is refused (`409 other_client_active`); that tab offers **Take over**, and the first tab stops at its next chunk. A reloaded tab keeps its own id (it is kept for the tab session), so it does not count as another tab. |
| The admin key was changed | The next chunk gets `401`; the page asks for the new key and continues. |
| The disk is full | `507 disk_full`. The chunk that failed is discarded and the upload is kept: free space, then **Resume**; or **Cancel** (frees what was received). |

**Expiry.** An upload is removed after 72 hours without progress. The time counts only
while the server runs (a server that was down over a weekend does not remove the uploads
at start-up), and it starts again when an upload becomes `ready`. Finished uploads
(`published`, `rejected`, `cancelled`, `expired`) stay visible for 24 hours; the data of
rejected, cancelled and expired uploads is deleted at once.

**Several servers on one models directory** (for example GPU hosts sharing a network
share): each server holds a lock on every upload it has open (`flock` on the upload's
`.lock` file on Linux, an exclusive handle on Windows; the file names the process, host
and server), and writes its own server id into the upload's `session.json` hint
(`<state dir>/uploads/owner`; a server whose state directory cannot be written derives
the id from its host name and directories, so it still knows its own uploads after a
restart). Another server never takes over an upload whose hint names another server, not
even while that server is down, and never touches its lock; it removes such a folder only
when the upload could have expired (72 hours minus the idle time in the hint) plus 24
hours have passed since the hint was last written, and no process holds its lock. A hint
without a server id (written by an older build) is taken over at start-up by the server
that finds its lock free. Every server must be able to list `.provision-upload` in that
directory: on Linux the server makes it group-readable and -writable
([11](#11-storage)).

### 16.7 Limits and memory

Fixed; there is nothing to tune per host.

| Limit | Value |
|---|---|
| chunk size | at most 16 MiB; the page starts with 8 MiB, grows while chunks are fast, and steps down to 960, 480 and 240 KiB when something in front of the server refuses large requests |
| chunks in flight | one per upload at a time |
| unfinished uploads | 4 (`409 too_many_uploads` for a fifth) |
| chunks written at the same time | 4 over all uploads (`429 upload_slots_busy`) |
| files per upload | 256 |
| file size | no limit of its own (64-bit offsets) |
| checks | one upload at a time |
| expiry | 72 hours without progress |

**Memory of the server.** A chunk is held in memory while it is received, about 3.5
times its size: with 4 uploads at 16 MiB that is at most about 220 MiB. The answers that
need no body are given on the chunk route **before** the body is read: an address the
allow-list refuses, no or a wrong key, no `Content-Length` or any `Transfer-Encoding`
(`411`), a `Content-Encoding` (`415`), a `Content-Length` above 16 MiB, an upload that is
over, busy in its checks or written by another sender, a chunk of the same upload on its
way, too many chunks at once ([12.16](#1216-post-adminmodelsuploadchunk)). So a body is
read only when its real size is known and a slot is free. Every other route reads the
body first, as before (at most 100 MB per request, the server's general limit).

**Memory of the browser.** It reads one chunk of a file at a time (at most 16 MiB) and
never holds a whole file.

**Speed.** The server computes the SHA-256 while it receives (roughly 200 to 300 MB/s
per CPU core, not measured), which may limit an upload above about 2 Gbit/s. It syncs the
files every 256 MiB or 15 seconds and, on Linux, tells the kernel to drop the written
data from the page cache, so a 40 GB upload does not push the weights of running CPU
models out of memory.

### 16.8 Disk space

- **Warn only**, the same rule as the download check: when the upload is created (and
  in the dry run of the dialog) the declared size is compared with the free space minus
  what other unfinished uploads still need: `not_enough_space`, or `larger_than_disk`
  when the files are larger than the whole filesystem. The dialog asks "Upload anyway?".
  On Linux it also warns (`shares_disk_with_state`) when the models directory is on the
  same filesystem as the admin state directory: a full disk then also stops settings
  from being saved. The license state and the model cache are not compared; on the
  default layouts they are often on that filesystem too ([11](#11-storage)).
- **The server refuses only facts**: a chunk that does not fit into the free space
  (checked before it is written) or a write that fails because the disk is full (`507`,
  the upload is kept), and bytes beyond the declared size.
- No preallocation, no reserve, and nothing is deleted to make room.
- Publishing needs no extra space: it is a rename on the same filesystem.
- `GET /admin/storage` lists the unfinished uploads with their size
  ([12.4](#124-get-adminstorage)).

### 16.9 Behind a reverse proxy

**The shipped configurations need no change**: the nginx front door of `docker-dual/` and
the host site of `deploy/linux/` pass request bodies of any size without buffering them,
with long timeouts, and their `/admin/` location already carries the allow-list.

**Your own nginx or ingress-nginx with its defaults** (from the nginx documentation, not
verified here):

- A request body limit of 1 MiB (`client_max_body_size 1m`; ingress-nginx
  `proxy-body-size` 1m): the page steps down to 960 KiB chunks by itself and keeps
  working, slower.
- Request buffering on: every chunk is written to the proxy's disk first. Harmless.
- Timeouts of 60 seconds: chunks are answered quickly, and the long steps (checks,
  publishing) answer at once (`202`) and are then polled.

For full speed, give the `/admin/` location of that proxy a larger body limit and turn
off request buffering there:

```nginx
location ^~ /admin/ {
    # ... your allow rules and proxy_pass, as before ...
    client_max_body_size 32m;
    proxy_request_buffering off;
}
```

**Early answers arrive as `502`.** An answer the server gives before it reads the body (a
busy upload, another sender, too many chunks at once,
[12.16](#1216-post-adminmodelsuploadchunk)) closes the connection. A proxy without
request buffering (the snippet above, and the shipped configurations) is then still
passing the body on and answers with its own `502 Bad Gateway` page instead of the
server's JSON (seen with nginx 1.27); with request buffering on, the server's JSON
arrives. The page treats a `5xx` that is not the server's JSON like a dropped
connection: it asks the server where the upload is and goes on. Scripts should do the
same, as the script of [16.10](#1610-with-curl) does after every chunk that fails.

ingress-nginx: the annotations `nginx.ingress.kubernetes.io/proxy-body-size: "32m"` and
`nginx.ingress.kubernetes.io/proxy-request-buffering: "off"` on the ingress of the admin
path (not verified here). A CDN with a 100 MB request limit (for example Cloudflare) is
fine for these chunk sizes (not verified here).

**Allow-lists.** The upload routes are `/admin` routes: nginx's allow-list
([4.1](#41-address-allow-list-nginx)) and the server's own
([4.3](#43-built-in-address-allow-list-hosts-without-nginx)) apply to them. Over a VPN
with NAT, nginx or the server sees the address of the VPN gateway, not the one of the
workstation: allow that address (or the VPN's range) on purpose.

**`--api-key`.** The page sends the same `Authorization` header with every chunk as with
every other request.

### 16.10 With curl

The upload API is plain HTTP and can be driven from a script (see [12](#12-api-reference)
for `BASE` and `KEY`, and [12.14](#1214-get-adminmodelsupload) onwards for every field).
This script uploads one file with `curl` and `jq`. It prints the id of the upload; to
continue after an interruption, run it again with that id as the third argument, and it
goes on from where the server is, after `verify=1` probes have shown that the file is the
one the server has. For a split set or a projector, list every file in `files` and send
each one with its own index (`file=0`, `file=1`, ...).

```bash
#!/usr/bin/env bash
# upload-gguf.sh FILE NAME [ID]      BASE and KEY as in section 12 of docs/provision-admin.md
set -u
FILE=$1 NAME=$2 ID=${3:-}
SIZE=$(stat -c %s "$FILE")                  # macOS: stat -f %z "$FILE"
CHUNK=$((8 * 1024 * 1024))                  # behind a proxy with a 1 MiB limit: $((960 * 1024))
H=(-H "X-Admin-Key: $KEY")
fail() { echo "$1" >&2; exit 1; }
send() {    # send OFFSET LENGTH [&verify=1]: these bytes of FILE as one chunk; prints the answer
    tail -c +"$(($1 + 1))" "$FILE" | head -c "$2" |
        curl -s --fail-with-body -X POST "${H[@]}" -H "X-Upload-Client: $CLIENT" \
            -H 'Content-Type: application/octet-stream' --data-binary @- \
            "$BASE/admin/models/upload/chunk?id=$ID&file=0&offset=$1${3:-}"
}
post_json() {
    curl -s --fail-with-body -X POST "${H[@]}" -H 'Content-Type: application/json' -d "$2" "$BASE$1"
}
status() {  # the upload as the server has it; waits up to 5 minutes for a server that restarts
    local a i
    for i in $(seq 60); do
        a=$(curl -s --fail-with-body "${H[@]}" "$BASE/admin/models/upload?id=$ID") && { echo "$a"; return 0; }
        case $(jq -r '.error.code // empty' <<< "$a" 2> /dev/null) in 4??) echo "$a"; return 1 ;; esac
        sleep 5
    done
    echo "$a"; return 1
}

if [ -z "$ID" ]; then
    BODY=$(jq -n --arg n "$NAME" --arg f "$(basename "$FILE")" --argjson s "$SIZE" \
        '{ name: $n, files: [ { name: $f, size: $s } ], settings: { "ctx-size": 16384 } }')
    # 1. dry run: the name, disk space warnings, saved settings of an earlier model of this name
    A=$(post_json /admin/models/upload "$(jq '. + { dry_run: true }' <<< "$BODY")") || fail "$A"
    jq '{ warnings, existing_settings }' <<< "$A"
    # 2. create the upload
    A=$(post_json /admin/models/upload "$BODY") || fail "$A"
    ID=$(jq -r '.upload.id' <<< "$A")
    echo "upload $ID"
fi
CLIENT="curl-$(hostname)-${ID:0:8}"         # the same "browser tab" when run again

# 3. send the file, one chunk at a time, from where the server is
A=$(status) || fail "$A"
OFF=$(jq '.files[0].offset' <<< "$A")
if [ "$OFF" -gt 0 ]; then                   # continuing: is this the file the server has?
    M=$((OFF < 1048576 ? OFF : 1048576))    # verify=1 compares and stores nothing
    A=$(send 0 "$M" '&verify=1') || fail "$A"                    # the first MiB
    A=$(send $((OFF - M)) "$M" '&verify=1') || fail "$A"         # the MiB before the offset
fi
FAILS=0
while [ "$OFF" -lt "$SIZE" ]; do
    if A=$(send "$OFF" "$CHUNK"); then
        OFF=$(jq '.offset' <<< "$A")
        FAILS=0
    else
        echo "${A:-no answer}" >&2                          # what went wrong
        FAILS=$((FAILS + 1)); [ "$FAILS" -le 10 ] || fail "giving up at byte $OFF"
        sleep 5
        A=$(status) || fail "$A"
        [ "$(jq -r '.state' <<< "$A")" = receiving ] || break    # rejected, cancelled, ...
        OFF=$(jq '.files[0].offset' <<< "$A")                   # where the server is now
    fi
    printf '\r%s / %s bytes' "$OFF" "$SIZE"
done
echo

# 4. the server checks the file and publishes it by itself; wait for the end
while A=$(status); do
    case $(jq -r '.state' <<< "$A") in published|ready|rejected|cancelled|expired) break ;; esac
    sleep 2
done
jq '{ state, blocked_by, error, files: [ (.files // [])[] | { name, role, size, sha256 } ] }' <<< "$A"
```

- `--data-binary @-` reads the chunk into memory first, so `curl` sends a
  `Content-Length`. No `Expect:` header workaround is needed.
- When it continues an upload, the two `verify=1` probes
  ([12.16](#1216-post-adminmodelsuploadchunk)) compare the first MiB and the MiB before
  the server's offset with the bytes the server stored; a different file stops the
  script with `409 content_mismatch` before anything is written.
- Every failure is handled the same way: ask the server where it is, then send from
  there. Sending a chunk again is always safe ([12.16](#1216-post-adminmodelsuploadchunk)).
  Do not add `curl --retry` to these calls: on a retry `curl` does not take back the
  error answer it already wrote, and the output then holds two answers.
- `tail -c +N` seeks in a regular file, so continuing does not read the file from the
  start.
- `--fail-with-body` needs curl 7.76 or newer.
- An upload that ends as `ready` waits for something: see `blocked_by` in
  [12.14](#1214-get-adminmodelsupload) and publish it with
  [12.17](#1217-post-adminmodelsuploadupdate-publish-cancel).

## 17. Deleting models of the models directory

Admin page, **Models** tab: rows of models from the models directory (source
`models_dir`: uploaded ones and ones dropped in by hand) have **Delete files**. It opens
the dialog "Delete model files?"; for a model with its own projector the same dialog
offers **Remove only the projector** ([17.4](#174-remove-projector)). Downloaded models
(source `cache`) keep their **Delete**, the public `DELETE /models`
([12.6](#126-downloading-and-deleting-existing-routes-no-admin-key)); sections of
`models.ini` are removed in `models.ini`.

### 17.1 What is deleted

- **Only the files the models directory gives this model**: its GGUF file, or every
  shard of a split model (the `-NNNNN-of-NNNNN` names the loader derives from the first
  one), and for a model in a folder the projector (`mmproj`) and the draft model in that
  folder, if it has them.
- **Everything else stays**: other `.gguf` files in the same folder (for example other
  quantizations that `huggingface-cli download --local-dir` put there), READMEs,
  subfolders. The dialog lists them as "Kept: not part of this model". A projector or
  draft model that every model gets (a draft model on the router command line, a
  projector or draft model in the `[*]` section of `models.ini`) is listed as kept too,
  marked "used by every model". The folder of the model is removed only when it is empty
  afterwards.
- **The dialog first shows what would happen** (a dry run): every file with its size and
  the total, the files that are kept, whether the model is running or pinned and whether
  it has saved settings. To confirm, type the model id; the button reads **Delete**, or
  **Stop and delete** for a running model.
- **"Also remove its saved settings"** (ticked by default) also removes the model's
  per-model settings, its pin, and what the server cached about it (facts, fit results,
  output tail). Otherwise the old settings would attach again to a model that comes back
  under the same id; unticked, they stay under "saved settings for models that are not in
  the list" ([6.7](#67-removing-custom-settings)).
- **How.** The files are first moved into a trash folder inside `.provision-upload`
  ([11](#11-storage)), which takes them out of the model list at once, then the model list
  is rebuilt, and the server deletes the trash in the background (`trash_left`; what is
  still there shows as `trash_bytes` in `GET /admin/storage`). On Linux the space comes
  back once no model process has the file open any more. On Windows a file that is still
  open for a moment is retried for up to 5 seconds.

### 17.2 A running model

- Without `stop` a running model is refused (`409 model_running`). The dialog says "The
  model is running: it is stopped first, its requests fail." and sends `stop`.
- With `stop` the server stops the model and waits at most its stop timeout plus 10
  seconds, as **Save and restart** does ([6.5](#65-saving-and-restarting)). If it does not
  stop in time the answer is `504 stop_timeout` and nothing is deleted. A model that a
  request started again in the meantime (automatic loading) is stopped the same way
  before the model list is rebuilt.

### 17.3 What is refused

`409 not_deletable`, with the `reason`:

- there is no model with this id, or it is still being downloaded (`not_found`; this
  route never answers `404`);
- the model is not from the models directory: a downloaded model (`cache`: use
  **Delete** / `DELETE /models`), a section of `models.ini`, or a model of the models
  directory that a `models.ini` section of the same name merges with (`preset`: change
  `models.ini`);
- its model file is not the one the models directory gives it, because `models.ini` or
  the router command line sets another one (`preset`);
- a file it uses lies outside the models directory or in a deeper folder
  (`outside_models_dir`), its folder or file in the models directory is a symbolic link
  or not a regular file (`symlink`), or its model or projector file is missing
  (`missing`);
- a file is on another filesystem or busy, for example a mount point inside the models
  directory (`other_filesystem`; what was moved already is put back);
- the server has no models directory, or it cannot be opened (`no_models_dir`), or the
  model has no model file (`no_file`);
- **Remove only the projector** for a model without a projector of its own: it has
  none, or its projector is the one every model gets (`no_projector`).

### 17.4 Remove projector

For a model in a folder of the models directory that has its own projector, **Delete
files** -> **Remove only the projector** switches the dialog to "Remove the projector?";
its button **Remove the projector** moves only the projector file (`mmproj`, the file
the model uses) into the trash and rebuilds the model list. The model stays, with its
settings and pin, and loses only its image / audio input. It is the way out when the
projector does not fit the model (every start fails; the failure panel of
[9](#9-when-a-model-fails-to-start) points here) or was not wanted. The rules for a
running model ([17.2](#172-a-running-model)) apply. A projector that every model gets
(the `[*]` section of `models.ini`) is not removed from here. A projector cannot be added
to an existing model from the panel: delete the model and upload it again with the
projector.

### 17.5 With curl

```bash
# what would be deleted; nothing is changed
curl -s -X POST -H "X-Admin-Key: $KEY" -H 'Content-Type: application/json' \
    -d '{ "model": "qwen3-8b", "dry_run": true }' $BASE/admin/models/delete | jq .

# delete; "stop": true stops the model first if it is running,
# "remove_settings": false keeps its saved settings
curl -s -X POST -H "X-Admin-Key: $KEY" -H 'Content-Type: application/json' \
    -d '{ "model": "qwen3-8b", "stop": true }' $BASE/admin/models/delete

# only the projector of a model
curl -s -X POST -H "X-Admin-Key: $KEY" -H 'Content-Type: application/json' \
    -d '{ "model": "gemma-3-4b", "projector_only": true, "stop": true }' $BASE/admin/models/delete
```

## 18. Activity log

The server keeps a log of what was done through the admin API (the server log and
Diagnostics call it the audit log): which settings were changed and to what, which
models were uploaded or deleted, which requests were refused, what the server did by
itself (an automatic revert, an expired upload), and what it saw happen through the
public model routes. Admin page, **Activity** tab, and
`GET /admin/audit` ([12.19](#1219-get-adminaudit)). There is nothing to configure: the
log starts with the admin layer and is written into the admin state directory
([18.5](#185-storage-rotation-and-retention)).

There are no user accounts. Everyone who has the admin key is the same admin, so "who"
in this log means "from which address, and with which browser or tool, the admin key was
presented" ([18.2](#182-who-addresses-and-proxies)).

### 18.1 What is recorded

One record per action: the time, the actor ([18.2](#182-who-addresses-and-proxies)), the
action, its target (a model id, an upload name or a path), the result and details. The
`kind` of a record says where it comes from:

| Kind | Records |
|---|---|
| `change` | an admin request with the admin key that changes something, with its outcome |
| `refusal` | refused admin requests (`401`, `403`), merged ([18.3](#183-refused-requests)) |
| `server` | what the server did by itself |
| `observed` | an effect of the public model routes, seen on the model status feed; who caused it is not known |

| Action | Kind | When | Target | Details |
|---|---|---|---|---|
| `settings.update` | `change` | **Save** of the Settings tab, `POST /admin/settings` | `settings` | `changes`: every setting that changed (`models_max`, `default_device`, `pinned`) as `{"from": ..., "to": ...}`; `changed`; `keys`: the keys that were sent; `reload_failed` when the model list could not be rebuilt after a change of `default_device` |
| `model_settings.update` | `change` | **Save**, **Save and restart** and the import of the Settings dialog, `POST /admin/models/settings` | model id | `changes`: every key that differs, as `{"from": ..., "to": ...}` with the stored text of the value (`"32768"`; `null` = not set); `changed`; `was_running`, `restart`, `restarted`; `load_failed` when the model could not be started now |
| `model_settings.reset` | `change` | **Remove custom settings**, and "Also remove its saved settings" of **Delete** for a downloaded model (`POST /admin/models/settings` with `reset: true`) | model id | `changes`: the values that were removed |
| `gpus.rescan` | `change` | **Re-detect GPUs**, `POST /admin/gpus/rescan` | `gpus` | `devices` found; `error` when the detection failed (result `failed`) |
| `upload.create` | `change` | an upload is started ([16](#16-uploading-gguf-files)) | the name | upload `id`, `bytes`, `files` |
| `upload.takeover` | `change` | **Take over** of an upload another tab or client was sending | the name | upload `id` |
| `upload.cancel` | `change` | **Cancel** of an upload | the name | upload `id`, `bytes` received |
| `upload.published` | `change` | the checks passed and the model was published | model id | upload `id`, `bytes`, `layout` (`file` or `folder`), `sha256` of every file, `arch`, `settings` (the placement chosen for the upload) |
| `upload.rejected` | `change` | a check refused the files ([16.4](#164-checks-before-the-model-becomes-visible)) | the name | upload `id`, `bytes` received, `code` (the reason), `file` |
| `upload.expired` | `server` | 72 hours without progress | the name | upload `id`, `bytes` received, `code` (`expired`) |
| `upload.resumed_after_restart` | `server` | the server found an unfinished upload when it started | the name | upload `id`, `state` |
| `upload.swept` | `server` | the server removed a leftover folder from `.provision-upload` ([11](#11-storage)). Recorded once the removal is done: `ok`; `failed` once when the folder cannot be removed (the sweep tries again every minute), then `ok` once a later sweep removed it | the folder | `folder` (relative to the models directory), `foreign` (another server's), `name` |
| `model.delete` | `change` | **Delete files**, `POST /admin/models/delete` | model id | `files`, `bytes` freed, `paths` (the first 16, relative to the models directory), `projector_only` (`false`), `stopped`, `settings_removed`, `pin_removed`, `folder_removed`, `reload_failed` |
| `model.remove_projector` | `change` | **Remove only the projector** | model id | as `model.delete`, with `projector_only: true` |
| `model_settings.reverted` | `server` | the automatic revert after two failed starts ([6.6](#66-trial-commit-and-automatic-revert)) | model id | `reason` of the failure, `failures`, `to` (`last_good` or `models_ini`) |
| `model_settings.reverted_at_startup` | `server` | settings that were being tried or failing when the server stopped are not applied again at start-up | model id | `phase` (`trial` or `failed`), `to` |
| `audit.started` | `server` | the server started | | `version` of the server, `persisted`, `max_bytes` |
| `audit.persisted_again` | `server` | a file of the log is written to disk again after a write error ([18.6](#186-when-the-log-cannot-be-written)); one per file, written into that file | the file | `store` (`changes`, `refusals` or `observed`), `not_persisted_since`, `lost` |
| `access.refused` | `refusal` | `401` or `403` answers ([18.3](#183-refused-requests)) | the path | `status`, `code`, `reason`, `probe`, `count`, `first`, `last` |
| `access.refused_more` | `refusal` | the summary of refusals beyond the caps ([18.3](#183-refused-requests)) | | `count`, `first`, `last`, `clients`, `sample` (addresses), `items` (paths) |
| `download.finished`, `download.failed` | `observed` | a download with `POST /models` ended | model id | |
| `model.removed` | `observed` | a downloaded model was removed with `DELETE /models` ([12.6](#126-downloading-and-deleting-existing-routes-no-admin-key)). Files removed from the models directory by hand leave no record ([18.4](#184-what-cannot-be-recorded)) | model id | |
| `model.start_failed` | `observed` | a model process ended with an error before it was loaded ([9](#9-when-a-model-fails-to-start)) | model id | `reason`, `exit_code`, `failures`, `settings_reverted` |
| `observed.more` | `observed` | more than 30 `observed` records in a minute: the rest are counted in one summary | | `count`, `first`, `last`, `items` (actions and model ids) |

- **Results.** `ok`; `error` for an admin request that was answered with an error (a
  value that fails validation, a locked field, `409 restart_required`, a failed save, a
  refused upload create): nothing was changed, and the details carry the `status` and
  the error type (`code`); `failed` when the action itself did not work (a GPU
  re-detection, a download, a model start); `rejected` for an upload the checks refused;
  `refused` for a refused request.
- **Changes, not reads.** Requests that only read (every `GET`, the fit check
  `POST /admin/models/settings/check`, the download check
  `POST /admin/models/download/check`, the dry run of a delete or an upload create, the
  status probe) are not recorded. Nor are the single chunks of an upload: only its
  start, take-over, end and cancel.
- **Values.** A setting is recorded as its old and new value, each at most 128 bytes of
  JSON (a longer one is cut and stored as text). Files are named relative to the models
  directory; nothing names a path outside it ([18.8](#188-privacy)).
- **Uploads.** The upload `id` links the records of one upload. `upload.published` and
  `upload.rejected` are the outcome of the server's checks, recorded when they end, also
  after a restart of the server: they carry the actor of the admin who created the upload
  (kept with the upload's record in the state directory, [11](#11-storage)). Only an
  upload the server had to rebuild without its record is published under the actor
  `server`.
- **Routine model traffic is skipped.** Loading, unloading and evicting a model, and
  automatic loading on the first request, are not recorded, whoever caused them; only
  the `observed` actions above are. Anyone who can reach the public API can cause those,
  so they are capped at 30 a minute; the rest are counted in one `observed.more` record.
  Like the refusals they have a file and a memory ring of their own
  ([18.5](#185-storage-rotation-and-retention)), so they never replace a change record.

### 18.2 Who: addresses and proxies

Every record of an admin request carries what the server knows about its sender
(`actor` in the API, type `client`):

| Field | Meaning |
|---|---|
| `peer` | the TCP peer of the request: the machine that opened the connection to the server. Behind nginx or another proxy this is the proxy (`unix socket` for a request over a Unix socket) |
| `client` | the client address as the server's address logic finds it ([4.3](#43-built-in-address-allow-list-hosts-without-nginx)): the peer, or, only when the peer is listed in `PROVISION_ADMIN_TRUSTED_PROXIES`, the address taken from its `X-Forwarded-For`. The same address the built-in allow-list checks; the activity log uses it whether that allow-list is on or off |
| `via_proxy` | `client` was taken from the `X-Forwarded-For` of a trusted proxy |
| `forwarded_for` | only when the request had one: the raw `X-Forwarded-For` header as it arrived (several header lines joined with `, `, in the order they arrived), at most 128 bytes |
| `forwarded_trusted` | with `forwarded_for`: whether the peer is a trusted proxy. When it is not, the header is whatever the sender wrote (any client can send one) and counts as unverified |
| `note` | why the `X-Forwarded-For` of a trusted proxy was not used (missing, sent in more than one header line, an entry that is not an address) |
| `user_agent` | only when the request had one: the `User-Agent` header, at most 128 bytes: which browser or tool |
| `label` | the actor in one line, as the panel shows it |

The `label` reads `10.64.0.7` (a direct request), `10.64.0.7 via trusted proxy 127.0.0.1`
(taken from a trusted proxy), or `172.18.0.3 (X-Forwarded-For 10.64.0.7, unverified)` (a
header the server cannot check). Records of the server itself have the actor type
`server`; the `observed` records the type `public_api`, label "public API (unknown)":
the public routes carry no admin key, and the server does not record who called them. A
merged record of many clients (the status probe of refused browsers, the summary of a
flood) has the type `various`.

What that means in the usual setups:

| Setup | `peer` | `client` | `X-Forwarded-For` | `label` |
|---|---|---|---|---|
| Host without a proxy, an admin on the LAN | `10.64.0.7` | `10.64.0.7` | none | `10.64.0.7` |
| The same, with a client that sends `X-Forwarded-For: 192.0.2.1` itself | `10.64.0.7` | `10.64.0.7` | `192.0.2.1`, unverified | `10.64.0.7 (X-Forwarded-For 192.0.2.1, unverified)` |
| Docker (`docker-dual/`) as shipped | the nginx container, e.g. `172.18.0.3` | `172.18.0.3` | `10.64.0.7`, unverified | `172.18.0.3 (X-Forwarded-For 10.64.0.7, unverified)` |
| Docker with nginx trusted (below) | `172.30.96.10` | `10.64.0.7` | `10.64.0.7`, trusted | `10.64.0.7 via trusted proxy 172.30.96.10` |
| Host nginx site ([13.5](#135-nginx)) with `PROVISION_ADMIN_TRUSTED_PROXIES=127.0.0.1` | `127.0.0.1` | `10.64.0.7` | `10.64.0.7`, trusted | `10.64.0.7 via trusted proxy 127.0.0.1` |
| Over a VPN with NAT | the VPN gateway | the VPN gateway | | the VPN gateway |

**Docker.** In the shipped stack every admin request reaches the server from nginx, and
the compose file sets no trusted proxy. The log then records the nginx container as the
peer, and the `X-Forwarded-For` that nginx passes on as **unverified**. In this stack its
last entry is in fact the address nginx saw (nginx appends it, and the server container
is not published), but the server cannot tell it apart from a header that a process on
the compose network wrote itself. To have the address recorded as verified, give nginx a
fixed address and list it in `PROVISION_ADMIN_TRUSTED_PROXIES`, for example with a
`docker-compose.override.yml` next to `docker-compose.yml` (compose reads it by itself;
the shipped files stay as they are):

```yaml
services:
  nginx:
    networks:
      default:
        ipv4_address: 172.30.96.10
  llama-dual:
    environment:
      PROVISION_ADMIN_TRUSTED_PROXIES: "172.30.96.10"
networks:
  default:
    ipam:
      config:
        - subnet: 172.30.96.0/24
          gateway: 172.30.96.1
```

Pick a subnet that is free on the host, then `docker compose down` and
`docker compose up -d` (the network is created again; the models load again). With
`-f docker-compose.yml` on the command line compose does not read the override file:
name it too (`-f docker-compose.yml -f docker-compose.override.yml`). The access rule
stays in nginx ([4.1](#41-address-allow-list-nginx)): without
`PROVISION_ADMIN_ALLOW_FROM` the trusted proxy changes only what the activity log
records, and the startup note in the log and in Diagnostics says so
([4.3](#43-built-in-address-allow-list-hosts-without-nginx)). Requests made on the
Docker host itself then show the gateway address (`172.30.96.1`), as in the nginx log.

**What a recorded address proves.** It is the address the request came from, or the one
a trusted proxy vouched for; nothing more. Every account on an allowed host, every
process that can reach the server past nginx, and every trusted proxy address can act
under any address it can send from or claim ([4.1](#41-address-allow-list-nginx),
[4.3](#43-built-in-address-allow-list-hosts-without-nginx)). The `User-Agent` is written
by the client and proves nothing.

### 18.3 Refused requests

A request that the server answers with `401` (no or a wrong admin key) or `403` (the
server's own allow-list, [4.3](#43-built-in-address-allow-list-hosts-without-nginx)) is
recorded as `access.refused`, also when the early check of the upload chunk route
([16.7](#167-limits-and-memory)) refuses it before its body is read. So that a stream of
refused requests can neither fill the disk nor push the record of a real change out of
the log:

- **Merged.** Refusals of one client, for one path, with one status and reason are one
  record per 60 seconds, with a `count` and the time of the first and the last request.
  `reason` is `no_key` or `wrong_key` for a `401`, and `not_allowed` or
  `allow_list_locked` (an invalid entry locks `/admin`) for a `403`; `code` is the error
  type. The record is written at once with the first request; when its 60 seconds are
  over, the final count is written as an update of the same record (in the file a second
  line with `group` set to the record's `seq`). A crash loses at most the count of the
  last minute.
- **The status probe.** Every page load of the web UI calls `GET /admin/status`, so the
  `403` answers of the probe are merged per path, status and reason over all clients
  (`probe: true`), with the number of clients and a sample of their addresses (actor
  "browsers of refused addresses").
- **Capped.** At most 60 new refusal records start in a minute, and at most 256 merge
  windows are open at once. Beyond that, the rest of the minute goes into one summary,
  `access.refused_more`, with the count, the number of clients (up to 256 counted), a
  sample of 8 addresses and up to 16 paths.
- **Apart.** Refusals have a file and a memory ring of their own
  ([18.5](#185-storage-rotation-and-retention)): however many there are, they never
  replace a change record.

The status probe without a key is not a refusal (it answers `200` with
`"authorized": false`). Other early answers of the chunk route (`409`, `411`, `413`,
`415`, `429`) are part of an upload and are not recorded.

### 18.4 What cannot be recorded

The admin layer does not move or wrap the routes that existed before
([2](#2-what-stays-public-by-design)), so it cannot see who uses them. Not in this log:

- who called `POST /models` (download), `DELETE /models`, `POST /models/load`,
  `POST /models/unload` or caused an automatic load: only their effects appear, as the
  `observed` records of [18.1](#181-what-is-recorded) with the actor "public API
  (unknown)";
- inference requests (chat, completions, embeddings, ...);
- requests that nginx refused: they never reach the server; see the nginx log
  (`docker compose logs nginx`; host: the access log of the site);
- requests that `--api-key` refused: that check runs before the admin layer;
- what was changed on the host: an edited `models.ini`, files dropped into or removed
  from the models directory by hand (the rescan updates the model list and records
  nothing; `model.removed` comes only from `DELETE /models`), edited state files;
- requests that only read (see [18.1](#181-what-is-recorded));
- records that were kept in memory only and lost at a restart
  ([18.6](#186-when-the-log-cannot-be-written)).

### 18.5 Storage, rotation and retention

| File in the admin state directory | Content |
|---|---|
| `admin-audit.jsonl` | the records of the kinds `change` and `server`, one JSON object per line, oldest first |
| `admin-audit-refusals.jsonl` | the `refusal` records ([18.3](#183-refused-requests)) |
| `admin-audit-observed.jsonl` | the `observed` records ([18.1](#181-what-is-recorded)) |
| `admin-audit.jsonl.1`, `admin-audit-refusals.jsonl.1`, `admin-audit-observed.jsonl.1` | the previous generation of each |

A file is created with its first record. The `audit.persisted_again` record of a file
([18.6](#186-when-the-log-cannot-be-written)) is written into that file, so the refusal
and the observed file can hold a `server` record too.

- **Written once per record.** Each record is appended and synced to disk (`fsync`) at
  once; admin actions are rare, so this costs nothing noticeable. A write that fails is
  cut back. A line cut anyway (a crash in the middle of a write, or a write whose cut
  failed too) stays on a line of its own: the next record starts on a new line, and the
  cut one is listed as `unreadable` ([12.19](#1219-get-adminaudit)). On Linux the files
  have mode `0600` (the server also sets it on an older file it owns); on Windows they
  take the access list of the state directory ([3.5](#35-outside-docker)).
- **A line is at most 4 KiB.** Longer details are replaced by
  `{"truncated": true, "text": ...}` with the first KiB of their JSON text. A record
  that still does not fit keeps only `seq`, `kind`, `action`, `result`, `ts`, `time` and
  `"truncated": true`.
- **Numbering.** `seq` numbers the records of all three files in the order they were
  recorded, and goes on across restarts (the server reads the last number back from the
  files).
- **Rotation.** When a file would grow past 4 MiB it becomes `.1` (an older `.1` is
  replaced) and a new file is started. The log therefore holds the last 4 to 8 MiB of
  change records, several thousand admin actions, and as much again of refusals and of
  observed records: at most 24 MiB in all. There is no time limit: the size alone bounds
  how far back the log goes and how long addresses are kept. `GET /admin/storage` lists
  the files with their size (`audit`).
- **Restarts and updates** keep the files, like the rest of the admin state (a Docker
  volume, `/var/lib/provision-llm-admin` on a host). Each start adds `audit.started`.
- **Reset.** The reset of [11](#11-storage) (removing `admin-state.json*`,
  `admin-models.json*` and `admin.key`) leaves the log alone. To clear it, stop the
  server and remove `admin-audit*.jsonl*` as well; removing the whole state volume
  removes it too.
- **Backup.** The files are plain JSON lines. Copy them with the rest of the state
  directory ([14.1](#141-every-update-record-the-image-and-back-up-the-admin-state)), or
  download them from the Activity tab or with `GET /admin/audit?format=jsonl`.
- Do not edit the files while the server runs: it appends to them. The server never
  reads them to decide anything except the next `seq`; a line it cannot read at all is
  skipped.

### 18.6 When the log cannot be written

The log never holds up an admin action and never makes one fail. When the state
directory is not writable at start-up, or a write fails later (a full disk, a removed
volume, an I/O error):

- the records go to memory instead: a ring of the last 1000 records for each file (the
  changes, the refusals and the observed records; each file is written or kept in
  memory on its own); what was written to disk before stays there;
- `GET /admin/status` carries the warning `audit_not_persisted` and the `audit` object
  ([12.1](#121-get-adminstatus)) with the error and since when; the Diagnostics item
  "Audit log" ([10](#10-diagnostics)) says the same; `GET /admin/audit` answers
  `"persisted": false`;
- the Activity tab shows a banner that the log is kept in memory only;
- the server tries the file again every minute. When a write works again, it first
  writes what the ring holds, in order, then records `audit.persisted_again` in that
  file with the time the problem started and how many records the full ring had to drop
  (`lost`). Every file that was kept in memory gets one such record, also when its ring
  was empty: after a state directory that could not be written at start-up there are
  three. The record exists only once it is on disk: while nothing can be written, no
  such record appears;
- records still in memory when the server stops are lost. Download the log first if
  they matter.

The same holds for a state directory that is missing or not writable at start-up: once
it is fixed ([3.1](#31-server-environment-variables), the Diagnostics item "Admin state
directory"), the server writes to it within a minute, without a restart.

### 18.7 In the panel

Admin page, **Activity** tab (shown when the server lists the feature `audit`, see
[12.1](#121-get-adminstatus)):

- a table with the newest records first and the columns **Time** (local time, the UTC
  time as a tooltip), **Actor**, **Action**, **Target** and **Result**; on a phone each
  record is one block. Above it, how many records there are ("Showing 100 of 1180
  records").
- **Actor**: the client address; under it "via trusted proxy 127.0.0.1" when the address
  came from a trusted proxy, or the `X-Forwarded-For` value with the badge **via proxy,
  unverified** when the peer is not a trusted proxy
  ([18.2](#182-who-addresses-and-proxies)). Records of the server itself show "Server",
  the `observed` ones "Public API" (caller unknown), a merged record of several clients
  "Several clients" or "Browsers of refused addresses" with the number of clients.
- **Action**: what happened, in words ("Model settings changed", "Request refused", ...;
  the action name as a tooltip; a change that was answered with an error reads
  "... failed"), with a one-line summary, and the badges **counting** (a merged record
  whose minute is not over: **Refresh** shows the final count) and **memory only** (not
  on disk yet, [18.6](#186-when-the-log-cannot-be-written)).
- Click the time of a record to open its details: **Details**, **Who** (client address,
  TCP peer, `X-Forwarded-For`, note, `User-Agent`), **Record** (number, kind, action,
  time, where it is kept) and **Raw record (JSON)** with **Copy record**.
- Filters: the kind (**All**, **Changes**, **Server**, **Public API**, **Refusals**;
  several can be on at once), the action (**All actions**, **Settings**, **Uploads**,
  **Deletes**, **Downloads**, **Failed starts**, **GPU re-detection**, **Refused
  requests**, **Audit log**) and the result (**Any result**, **OK**, **Errors and
  failures**, **Refused**). **Clear filters** shows everything again.
- **Refresh** reads the newest records again; **Load more (N older)** fetches the next
  100 older ones.
- **Download JSONL** saves the records that match the current filters as a JSON-lines
  file, oldest first ([12.19](#1219-get-adminaudit)); without filters, the whole log.
- While the log is kept in memory, a banner "The audit log is kept in memory only" says
  since when and why, and how many records were already dropped
  ([18.6](#186-when-the-log-cannot-be-written)).

The tab only reads: nothing in it changes or removes records.

### 18.8 Privacy

- **Never recorded:** the admin key (`X-Admin-Key`), the `Authorization` header (the
  server's `--api-key`), `HF_TOKEN` or any other token, request bodies, and paths
  outside the models directory. As a second line of defence the server replaces the
  admin key and `HF_TOKEN` by `[redacted]` wherever they would appear in a record, and in
  the line the server log prints for each record (`admin: audit: ...`); a value is
  replaced before it is cut, so no part of a key is left either.
  Settings values are the ones of [6.1](#61-which-models-and-which-settings) and
  [5](#5-settings): device names, numbers, model ids.
- **Recorded:** client addresses, `X-Forwarded-For`, the `User-Agent`, model ids, upload
  and file names (relative to the models directory), file sizes and SHA-256 values,
  and settings values. Client addresses can be personal data where you operate: the
  size limit of [18.5](#185-storage-rotation-and-retention) is the only retention limit;
  remove the files to clear it.
- **Who can read it:** whoever has the admin key (`GET /admin/audit`, the Activity tab),
  and the accounts that can read the admin state directory (on a host the service
  account and root; in Docker everyone who can run `docker` on the host).
- Values from the request cannot start a new record or hide in one: the target (model
  id, upload name, path), the `User-Agent` and `X-Forwarded-For` keep printable ASCII
  only (every other byte becomes `?`) and are cut; everything is stored as JSON strings,
  and the panel shows every value as text.
- The record of an unfinished upload in the state directory (`uploads/<id>.json`, [11](#11-storage))
  keeps the actor of the admin who created it, for the records the checks write later;
  it is removed with that record, at the latest 24 hours after the upload ended.

### 18.9 With curl

```bash
# the newest 50 records
curl -s -H "X-Admin-Key: $KEY" "$BASE/admin/audit?limit=50" | jq .

# the next older page: before = next_before of the previous answer
curl -s -H "X-Admin-Key: $KEY" "$BASE/admin/audit?limit=50&before=1180" | jq .

# only the refused requests; every upload action; deletions and removed projectors
curl -s -H "X-Admin-Key: $KEY" "$BASE/admin/audit?kind=refusal" | jq .
curl -s -H "X-Admin-Key: $KEY" "$BASE/admin/audit?action=upload" | jq .
curl -s -H "X-Admin-Key: $KEY" "$BASE/admin/audit?action=model.delete,model.remove_projector" | jq .

# the whole log as a file
curl -s -H "X-Admin-Key: $KEY" -o admin-audit.jsonl "$BASE/admin/audit?format=jsonl"
```
