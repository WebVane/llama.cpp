#!/usr/bin/env bash
set -euo pipefail

# Wrapper around llama-server that launches it in router mode with a baked
# preset. Knobs are exposed as env vars so users can tune the container
# without rebuilding the image.

: "${PORT:=8080}"
: "${MODELS_MAX:=1}"
: "${MODELS_CONFIG:=/config/models.ini}"
# Per-model GPU override, applied on top of the preset file:
#   MODEL_DEVICES="qwen2.5-chat=CUDA1,gte-qwen2-embed=CUDA0"
# <section> is a section name from models.ini; "*" applies to EVERY section
# (a section-specific entry still wins), e.g. "*=CUDA0" forces everything
# onto one GPU.  <dev> is anything --device accepts: CUDA0, CUDA1, none (CPU
# only).  To split one model over several GPUs separate them with a
# semicolon ("big=CUDA0;CUDA1"), since the comma already separates entries.
# The patched preset is written to /tmp/models.ini once at startup; the
# router reads that copy, so edits to the mounted file need a restart.
# The admin panel shows a device set this way as locked ("set by
# MODEL_DEVICES"); the server checks that the patched section really carries
# it before it reports the lock.
: "${MODEL_DEVICES:=}"

if [[ ! -f "${MODELS_CONFIG}" ]]; then
    echo "[entrypoint] error: models preset not found at ${MODELS_CONFIG}" >&2
    exit 1
fi

if [[ -n "${MODEL_DEVICES}" ]]; then
    # The mounted preset is read-only, so write the patched copy elsewhere.
    patched="/tmp/models.ini"
    # For every section that has an override (its own entry, else "*"): drop
    # any device/dev/LLAMA_ARG_DEVICE key already present in that section and
    # insert ours right after the header.  Headers may carry a trailing
    # comment ("[name] ; text"), like the preset parser allows.  Unknown
    # section names are reported but do not abort startup (a typo should not
    # take the whole service down).
    # A preset saved with Windows line endings (CRLF) is matched without the
    # trailing CR, like the preset parser does, and the inserted line gets the
    # same line ending as its header; a CR or newline in MODEL_DEVICES itself
    # (a .env file written on Windows) is dropped as well.
    awk -v overrides="${MODEL_DEVICES}" '
        BEGIN {
            n = split(overrides, pairs, ",")
            for (i = 1; i <= n; i++) {
                p = pairs[i]
                gsub(/^[ \t\r\n]+|[ \t\r\n]+$/, "", p)
                if (p == "") continue
                eq = index(p, "=")
                if (eq == 0) {
                    printf "[entrypoint] warning: ignoring MODEL_DEVICES entry without \"=\": %s\n", p > "/dev/stderr"
                    continue
                }
                name = substr(p, 1, eq - 1); dev = substr(p, eq + 1)
                gsub(/^[ \t\r\n]+|[ \t\r\n]+$/, "", name)
                gsub(/^[ \t\r\n]+|[ \t\r\n]+$/, "", dev)
                gsub(/;/, ",", dev)
                want[name] = dev
            }
        }
        { line = $0; eol = ""; if (sub(/\r$/, "", line)) eol = "\r" }
        line ~ /^[ \t]*\[[^]]*\][ \t]*([;#].*)?$/ {
            sec = line
            sub(/^[ \t]*\[/, "", sec); sub(/\].*$/, "", sec)
            gsub(/^[ \t]+|[ \t]+$/, "", sec)
            print
            strip = 0
            dev = ""
            if (sec in want) {
                dev = want[sec]
            } else if ("*" in want) {
                dev = want["*"]
            }
            if (dev != "") {
                printf "device = %s%s\n", dev, eol
                seen[sec] = 1
                strip = 1
            }
            next
        }
        strip && /^[ \t]*(device|dev|LLAMA_ARG_DEVICE)[ \t]*=/ { next }
        { print }
        END {
            for (name in want) {
                if (name != "*" && !(name in seen)) {
                    printf "[entrypoint] warning: MODEL_DEVICES names section \"%s\" which is not in %s\n", name, ENVIRON["MODELS_CONFIG"] > "/dev/stderr"
                }
            }
        }
    ' "${MODELS_CONFIG}" > "${patched}"
    echo "[entrypoint] applied MODEL_DEVICES=${MODEL_DEVICES} -> ${patched}"
    MODELS_CONFIG="${patched}"
fi

# ---------------------------------------------------------------------------
# Admin panel support (see docs/provision-admin.md).  Everything below is
# inert unless docker-compose.yml asks for it (PROVISION_ADMIN=1, the
# /models-rw mount), and none of it may abort startup: a failure here only
# costs the optional feature, the preset models must still come up.
# ---------------------------------------------------------------------------

# Optional settings arrive from compose as "${VAR:-}", i.e. set but empty when
# the operator did not provide them.  Empty means "not set": drop them so the
# server falls back to its defaults (key file / generated key, default state
# directory) instead of seeing an empty key or path.
for _var in PROVISION_ADMIN_KEY PROVISION_ADMIN_KEY_FILE PROVISION_ADMIN_STATE_DIR; do
    if [[ -z "${!_var:-}" ]]; then
        unset "${_var}"
    fi
done
unset _var

# Writable model storage.  docker-compose.yml bind-mounts a host directory at
# /models-rw; the image itself has no such directory, so with an older compose
# file this block is skipped and downloads keep going to ~/.cache inside the
# container, as before.
#   /models-rw/hf     Hugging Face cache (LLAMA_CACHE): models downloaded from
#                     the admin panel / POST /models
#   /models-rw/local  --models-dir: drop a .gguf file (or a directory with one)
#                     here, then rescan (GET /v1/models?reload=1)
extra_args=()
models_rw="/models-rw"
if [[ -d "${models_rw}" ]]; then
    if [[ -w "${models_rw}" ]]; then
        # The container runs as root.  A subdirectory created here is handed
        # to the owner of the mounted directory, as if the operator had
        # created it on the host; otherwise only root could drop a file into
        # local/.  Subdirectories that already exist are left alone.
        rw_owner="$(stat -c '%u:%g' "${models_rw}" 2>/dev/null)" || rw_owner=""
        for _sub in hf local; do
            if [[ -d "${models_rw}/${_sub}" ]]; then
                continue
            fi
            if mkdir -p "${models_rw}/${_sub}" 2>/dev/null; then
                if [[ -n "${rw_owner}" ]]; then
                    chown "${rw_owner}" "${models_rw}/${_sub}" 2>/dev/null || true
                fi
            else
                echo "[entrypoint] warning: cannot create ${models_rw}/${_sub}" >&2
            fi
        done
        unset _sub rw_owner
    else
        echo "[entrypoint] warning: ${models_rw} is not writable: downloaded models will not be kept on the host" >&2
    fi
    if [[ -d "${models_rw}/hf" ]]; then
        # An explicit LLAMA_CACHE from the environment wins.
        export LLAMA_CACHE="${LLAMA_CACHE:-${models_rw}/hf}"
    fi
    # The router refuses to start when --models-dir does not exist, so pass
    # it only when the directory is really there.
    if [[ -d "${models_rw}/local" ]]; then
        extra_args+=(--models-dir "${models_rw}/local")
    fi
fi

# Startup-preload guard.  The router refuses to start when more sections ask
# for `load-on-startup` than it may keep loaded ("number of models to load on
# startup (N) exceeds models_max (M)").  The shipped models.ini preloads both
# models, which needs a limit of 2 or more (docker-compose.yml uses 3), while
# the image default is still MODELS_MAX=1 (plain `docker run`).  Instead of
# failing, keep the key for the first <limit> sections in file order and
# switch it off for the rest: they load on first use, exactly as before the
# key was added.
#
# <limit> is MODELS_MAX, lowered to the "Max loaded models" value saved from
# the admin panel when there is one: the router starts with the saved value,
# whatever MODELS_MAX says.  0 = unlimited.  Taking the lower of the two is
# safe whichever of them the server ends up using.
#
# Only sections of the preset file are seen here, and like MODEL_DEVICES this
# writes a patched copy, so later edits to the mounted file need a restart.
models_limit=0
if [[ "${MODELS_MAX}" =~ ^[0-9]+$ ]]; then
    models_limit=$((10#${MODELS_MAX}))
fi
admin_flag="${PROVISION_ADMIN:-}"
admin_flag="${admin_flag//[[:space:]]/}"
case "${admin_flag,,}" in
    1|true|on)
        admin_state="${PROVISION_ADMIN_STATE_DIR:-/var/lib/provision-llm-admin}/admin-state.json"
        saved_max=""
        if [[ -r "${admin_state}" ]]; then
            saved_max="$(sed -n '/"models_max"/{s/.*"models_max"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p;q;}' "${admin_state}" 2>/dev/null)" || saved_max=""
        fi
        if [[ "${saved_max}" =~ ^[0-9]+$ ]]; then
            saved_max=$((10#${saved_max}))
            if [[ "${saved_max}" -gt 0 ]] && { [[ "${models_limit}" -eq 0 ]] || [[ "${saved_max}" -lt "${models_limit}" ]]; }; then
                models_limit="${saved_max}"
            fi
        fi
        ;;
esac
if [[ "${models_limit}" -gt 0 ]]; then
    demote="$(awk -v limit="${models_limit}" '
        function trim(s) { gsub(/^[ \t]+|[ \t]+$/, "", s); return s }
        function truthy(v) { return v == "on" || v == "enabled" || v == "true" || v == "1" }
        { sub(/\r$/, "") }
        /^[ \t]*\[[^]]*\][ \t]*([;#].*)?$/ {
            sec = $0
            sub(/^[ \t]*\[/, "", sec); sub(/\].*$/, "", sec)
            sec = trim(sec)
            if (!(sec in seen)) { seen[sec] = 1; order[++n] = sec }
            has[sec] = 0
            next
        }
        sec != "" && /^[ \t]*(load-on-startup|__PRESET_LOAD_ON_STARTUP)[ \t]*=/ {
            v = $0
            sub(/^[^=]*=/, "", v); sub(/[ \t]*[;#].*$/, "", v)
            has[sec] = 1; val[sec] = trim(v)
        }
        END {
            glob = 0
            if (("*" in has) && has["*"]) glob = truthy(val["*"])
            kept = 0
            for (i = 1; i <= n; i++) {
                s = order[i]
                if (s == "*") continue
                on = has[s] ? truthy(val[s]) : glob
                if (!on) continue
                if (kept < limit) { kept++; continue }
                print s
            }
        }
    ' "${MODELS_CONFIG}" 2>/dev/null)" || demote=""
    if [[ -n "${demote}" ]]; then
        guarded="/tmp/models.startup.ini"
        if DEMOTE_SECTIONS="${demote}" awk '
            BEGIN {
                n = split(ENVIRON["DEMOTE_SECTIONS"], names, "\n")
                for (i = 1; i <= n; i++) if (names[i] != "") drop[names[i]] = 1
            }
            { line = $0; sub(/\r$/, "", line) }
            line ~ /^[ \t]*\[[^]]*\][ \t]*([;#].*)?$/ {
                sec = line
                sub(/^[ \t]*\[/, "", sec); sub(/\].*$/, "", sec)
                gsub(/^[ \t]+|[ \t]+$/, "", sec)
                print
                strip = (sec in drop)
                if (strip) print "load-on-startup = false"
                next
            }
            strip && line ~ /^[ \t]*(load-on-startup|__PRESET_LOAD_ON_STARTUP)[ \t]*=/ { next }
            { print }
        ' "${MODELS_CONFIG}" > "${guarded}" 2>/dev/null; then
            echo "[entrypoint] warning: more load-on-startup models than may be loaded at once (limit ${models_limit}, MODELS_MAX=${MODELS_MAX}); not preloading: $(echo "${demote}" | tr '\n' ' ')-> ${guarded}" >&2
            MODELS_CONFIG="${guarded}"
        else
            echo "[entrypoint] warning: could not write ${guarded}; the server may refuse to start (load-on-startup models exceed the limit ${models_limit})" >&2
        fi
    fi
fi

exec /app/llama-server \
    --host 0.0.0.0 \
    --port "${PORT}" \
    --models-preset "${MODELS_CONFIG}" \
    --models-max "${MODELS_MAX}" \
    ${extra_args[@]+"${extra_args[@]}"} \
    "$@"
