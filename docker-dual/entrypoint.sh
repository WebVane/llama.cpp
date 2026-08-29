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
    awk -v overrides="${MODEL_DEVICES}" '
        BEGIN {
            n = split(overrides, pairs, ",")
            for (i = 1; i <= n; i++) {
                p = pairs[i]
                gsub(/^[ \t]+|[ \t]+$/, "", p)
                if (p == "") continue
                eq = index(p, "=")
                if (eq == 0) {
                    printf "[entrypoint] warning: ignoring MODEL_DEVICES entry without \"=\": %s\n", p > "/dev/stderr"
                    continue
                }
                name = substr(p, 1, eq - 1); dev = substr(p, eq + 1)
                gsub(/^[ \t]+|[ \t]+$/, "", name)
                gsub(/^[ \t]+|[ \t]+$/, "", dev)
                gsub(/;/, ",", dev)
                want[name] = dev
            }
        }
        /^[ \t]*\[[^]]*\][ \t]*([;#].*)?$/ {
            sec = $0
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
                printf "device = %s\n", dev
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

exec /app/llama-server \
    --host 0.0.0.0 \
    --port "${PORT}" \
    --models-preset "${MODELS_CONFIG}" \
    --models-max "${MODELS_MAX}" \
    "$@"
