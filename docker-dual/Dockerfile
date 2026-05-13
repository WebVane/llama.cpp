# syntax=docker/dockerfile:1.7
#
# llama.cpp dual-model image (chat + embeddings) via built-in router (--models-preset).
# Build from the repository root:
#     docker build -f docker-dual/Dockerfile -t llama-dual .
# or  cd docker-dual && docker compose build
#

ARG UBUNTU_VERSION=24.04
ARG CUDA_VERSION=12.8.1
ARG BASE_CUDA_DEV_CONTAINER=nvidia/cuda:${CUDA_VERSION}-devel-ubuntu${UBUNTU_VERSION}
ARG BASE_CUDA_RUN_CONTAINER=nvidia/cuda:${CUDA_VERSION}-runtime-ubuntu${UBUNTU_VERSION}

# -----------------------------------------------------------------------------
# build stage
# -----------------------------------------------------------------------------
FROM ${BASE_CUDA_DEV_CONTAINER} AS build

# "default" (recommended): let ggml/src/ggml-cuda/CMakeLists.txt pick a
# sensible list for the installed CUDA toolkit -- this is the only path
# that includes the "accelerated" 120a-real/121a-real variants needed by
# the MXFP4 kernels on Blackwell.  Override with a semicolon-separated
# list (e.g. "75;86;89") to slim the image for a known GPU set; avoid
# plain "120" / "all-major" on CUDA 12.8+ as ptxas will reject MXFP4.
ARG CUDA_ARCH=default

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        gcc-14 g++-14 \
        build-essential \
        cmake \
        git \
        ca-certificates \
        libcurl4-openssl-dev \
        libgomp1 && \
    rm -rf /var/lib/apt/lists/*

ENV CC=gcc-14 CXX=g++-14 CUDAHOSTCXX=g++-14

WORKDIR /src
COPY . /src

RUN if [ "${CUDA_ARCH}" != "default" ]; then \
        EXTRA_CMAKE_ARGS="-DCMAKE_CUDA_ARCHITECTURES=${CUDA_ARCH}"; \
    fi && \
    cmake -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DGGML_NATIVE=OFF \
        -DGGML_CUDA=ON \
        ${EXTRA_CMAKE_ARGS} \
        -DLLAMA_CURL=ON \
        -DLLAMA_BUILD_TESTS=OFF \
        -DCMAKE_EXE_LINKER_FLAGS=-Wl,--allow-shlib-undefined && \
    cmake --build build --config Release --target llama-server -j"$(nproc)"

# Collect runtime artifacts (binary + any produced shared libs from ggml/cuda).
RUN mkdir -p /out/bin /out/lib && \
    cp build/bin/llama-server /out/bin/ && \
    find build -name "*.so*" -exec cp -P {} /out/lib/ \; || true

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

# Place llama-server and any shipped .so into /app, expose them on the loader path.
COPY --from=build /out/lib/ /app/lib/
COPY --from=build /out/bin/ /app/

# Router preset + entrypoint baked from the build context.
COPY docker-dual/models.ini    /config/models.ini
COPY docker-dual/entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

ENV LD_LIBRARY_PATH=/app/lib:${LD_LIBRARY_PATH}
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
