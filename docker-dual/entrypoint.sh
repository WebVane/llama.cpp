#!/usr/bin/env bash
set -euo pipefail

# Wrapper around llama-server that launches it in router mode with a baked
# preset. Knobs are exposed as env vars so users can tune the container
# without rebuilding the image.

: "${PORT:=8080}"
: "${MODELS_MAX:=1}"
: "${MODELS_CONFIG:=/config/models.ini}"

if [[ ! -f "${MODELS_CONFIG}" ]]; then
    echo "[entrypoint] error: models preset not found at ${MODELS_CONFIG}" >&2
    exit 1
fi

exec /app/llama-server \
    --host 0.0.0.0 \
    --port "${PORT}" \
    --models-preset "${MODELS_CONFIG}" \
    --models-max "${MODELS_MAX}" \
    "$@"
