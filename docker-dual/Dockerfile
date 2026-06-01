# syntax=docker/dockerfile:1.7
#
# llama.cpp dual-model image (chat + embeddings) via built-in router (--models-preset).
# Build from the repository root:
#     docker build -f docker-dual/Dockerfile -t llama-dual .
# or  cd docker-dual && docker compose build
#

# CUDA 12.2.2 is pinned to match the 535-series NVIDIA driver baseline
# on the target host.  Drivers are backward-compatible, so this image
# also runs fine on hosts with newer drivers (R550 / R570 / ...);
# downgrading the runtime only loses Blackwell-specific compile-time
# kernels (MXFP4 / FP4 tensor cores), which need CUDA 12.8 sources.
# Hosts with newer GPUs will JIT the 75/80-virtual PTX to their native
# arch at first run (Ampere code path, not Blackwell-optimized).
#
# Host driver minimums (raising CUDA_VERSION also raises this floor):
#   12.2.x -> >=535.54  (current pin)
#   12.4.x -> >=550.54
#   12.6.x -> >=560.28
#   12.8.x -> >=570.86  (also unlocks Blackwell sm_120a / MXFP4)
# Ubuntu 22.04 is the highest LTS that nvidia/cuda publishes for 12.2.2.
ARG UBUNTU_VERSION=22.04
ARG CUDA_VERSION=12.2.2
ARG BASE_CUDA_DEV_CONTAINER=nvidia/cuda:${CUDA_VERSION}-devel-ubuntu${UBUNTU_VERSION}
ARG BASE_CUDA_RUN_CONTAINER=nvidia/cuda:${CUDA_VERSION}-runtime-ubuntu${UBUNTU_VERSION}

# -----------------------------------------------------------------------------
# build stage
# -----------------------------------------------------------------------------
FROM ${BASE_CUDA_DEV_CONTAINER} AS build

# "default" lets ggml/src/ggml-cuda/CMakeLists.txt pick the arch list
# for the installed toolkit.  Override with a semicolon-separated list
# (e.g. "61-real;86-real") to slim the image for a known GPU set.
# Note: Blackwell sm_120a / MXFP4 kernels require CUDA >= 12.8 and are
# NOT available in this 12.2.2-based image; trying to add 120a-real here
# will fail at ptxas time.
ARG CUDA_ARCH=default

# Trial / evaluation build.  TRIAL=1 compiles the server as a request-limited
# trial (see tools/server/CMakeLists.txt and src/license/Trial.cpp): it serves
# at most TRIAL_LIMIT inference requests (web UI and API alike) and then
# refuses with HTTP 403.  Health checks and metadata endpoints are exempt.
# The signed counter lives at /var/lib/provision-ocr/trial.dat inside the
# container (mount a volume there to persist it across container recreation).
ARG TRIAL=0
ARG TRIAL_LIMIT=50

# CUDA 12.2 supports gcc up to 12; Ubuntu 22.04 ships gcc-11 by default
# but we install gcc-12 explicitly for slightly better C++20 coverage in
# common/.  Skip gcc-14: not in jammy archives and incompatible with 12.2.
# libssl-dev provides libcrypto for the trial counter's HMAC-SHA256 (the
# CMake trial block reuses the OpenSSL::Crypto target that cpp-httplib finds).
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        gcc-12 g++-12 \
        build-essential \
        cmake \
        git \
        ca-certificates \
        libcurl4-openssl-dev \
        libssl-dev \
        libgomp1 && \
    rm -rf /var/lib/apt/lists/*

ENV CC=gcc-12 CXX=g++-12 CUDAHOSTCXX=g++-12

WORKDIR /src
COPY . /src

RUN EXTRA_CMAKE_ARGS="" && \
    if [ "${CUDA_ARCH}" != "default" ]; then \
        EXTRA_CMAKE_ARGS="${EXTRA_CMAKE_ARGS} -DCMAKE_CUDA_ARCHITECTURES=${CUDA_ARCH}"; \
    fi && \
    if [ "${TRIAL}" = "1" ]; then \
        echo "*** TRIAL build: request limit = ${TRIAL_LIMIT} ***"; \
        EXTRA_CMAKE_ARGS="${EXTRA_CMAKE_ARGS} -DLLAMA_SERVER_TRIAL=ON -DLLAMA_SERVER_TRIAL_LIMIT=${TRIAL_LIMIT}"; \
    fi && \
    cmake -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DGGML_NATIVE=OFF \
        -DGGML_CUDA=ON \
        -DGGML_BACKEND_DL=ON \
        -DGGML_CPU_ALL_VARIANTS=ON \
        ${EXTRA_CMAKE_ARGS} \
        -DLLAMA_CURL=ON \
        -DLLAMA_BUILD_TESTS=OFF \
        -DCMAKE_EXE_LINKER_FLAGS=-Wl,--allow-shlib-undefined && \
    cmake --build build --config Release -j"$(nproc)"

# Collect runtime artifacts.  With GGML_BACKEND_DL=ON the CPU backend is
# emitted as several .so files (one per microarch: sse4.2, avx, avx2,
# avx512, ...) and ggml picks the right one at startup -- so the image
# runs on any x86_64 host from Sandy Bridge upward without recompiling.
RUN mkdir -p /out/bin /out/lib && \
    # The server target may be named provision-server (rebrand) or
    # llama-server depending on tools/server/CMakeLists.txt; normalize to
    # llama-server so the entrypoint path stays stable either way.
    { cp build/bin/provision-server /out/bin/llama-server 2>/dev/null || \
      cp build/bin/llama-server     /out/bin/llama-server; } && \
    find build -name "*.so*" -exec cp -P {} /out/lib/ \;

# -----------------------------------------------------------------------------
# runtime stage
# -----------------------------------------------------------------------------
FROM ${BASE_CUDA_RUN_CONTAINER} AS runtime

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        libgomp1 \
        libcurl4 \
        ca-certificates \
        curl \
        tini && \
    apt-get autoremove -y && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/* /tmp/* /var/tmp/*

# With GGML_BACKEND_DL=ON, ggml-backend-reg.cpp dlopens backend .so files
# from the *executable's directory* (not LD_LIBRARY_PATH).  Child processes
# spawned by the router via execv inherit cwd / argv[0] but lose any
# LD_LIBRARY_PATH-based discovery, so the .so MUST sit next to llama-server.
# This mirrors .devops/cuda.Dockerfile which copies /app/lib/ -> /app.
COPY --from=build /out/lib/ /app/
COPY --from=build /out/bin/ /app/

# Router preset + entrypoint baked from the build context.
COPY docker-dual/models.ini    /config/models.ini
COPY docker-dual/entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

ENV LD_LIBRARY_PATH=/app:${LD_LIBRARY_PATH}
ENV LLAMA_ARG_HOST=0.0.0.0
ENV PORT=8080
ENV MODELS_MAX=1
ENV MODELS_CONFIG=/config/models.ini

WORKDIR /app

EXPOSE 8080
VOLUME ["/models"]

HEALTHCHECK --interval=30s --timeout=5s --start-period=60s --retries=3 \
    CMD curl -fsS "http://127.0.0.1:${PORT}/health" || exit 1

ENTRYPOINT ["/usr/bin/tini", "--", "/entrypoint.sh"]
