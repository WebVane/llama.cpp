# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Read this first: AI policy

**Read [AGENTS.md](AGENTS.md) before doing any work.** llama.cpp does **not** accept fully or predominantly AI-generated PRs. AI is permitted only as an assistant to a human author who fully understands and can defend every line. Concretely:

- Never draft PR descriptions, commit messages, or replies to reviewer comments — those must be human.
- Disclosure is required when AI meaningfully contributed to code.
- Before implementing on a contributor's behalf, confirm they understand both the problem and the relevant code; prefer pointing them at the relevant files over writing the change yourself.
- For a new model or new architectural change, the initial PR should focus on **CPU support only**; backends (CUDA, Metal, Vulkan, …) follow in separate PRs.
- Adding a new `ggml_type` (quantization) carries a high bar — see [CONTRIBUTING.md](CONTRIBUTING.md) for the required perplexity / KL-divergence / perf evidence before proposing one.

## Repository layout

The project is a C/C++ inference engine for GGUF models with optional Python tooling for conversion. The major boundaries:

- **`ggml/`** — Tensor library and pluggable backends. Each backend has its own subdirectory under `ggml/src/` (`ggml-cuda/`, `ggml-metal/`, `ggml-vulkan/`, `ggml-sycl/`, `ggml-hip/`, `ggml-cpu/`, `ggml-blas/`, `ggml-rpc/`, `ggml-opencl/`, `ggml-cann/`, `ggml-musa/`, `ggml-webgpu/`, `ggml-zendnn/`, `ggml-zdnn/`, `ggml-hexagon/`, `ggml-openvino/`, `ggml-virtgpu/`). Backends register through `ggml-backend-reg.cpp` and can be built as dynamic libraries (`-DGGML_BACKEND_DL=ON`).
- **`src/`** — The `llama` library: model loader, KV cache (incl. iSWA / hybrid / recurrent variants), graph builder, sampling, grammar, vocab, adapter (LoRA), quantization. **`src/models/<arch>.cpp`** holds the per-architecture graph builders — one file per supported model family. New model architectures land here.
- **`include/llama.h`** — Stable C public API. `include/llama-cpp.h` is a C++ helper. API changes are tracked in [issue #9289](https://github.com/ggml-org/llama.cpp/issues/9289).
- **`common/`** — Shared utilities used by tools and tests (not part of the library API): `arg.{h,cpp}` (CLI parsing), `sampling.*`, `chat.*`, `chat-peg-parser.*` and `chat-auto-parser*` (model output parsing — see `docs/autoparser.md` and `docs/development/parsing.md`), `jinja/` (in-tree Jinja engine, see `common/jinja/README.md`), `json-schema-to-grammar.*`, `speculative.*`, `ngram-*`, `download.*`, `hf-cache.*`.
- **`tools/`** — User-facing programs. Each subdirectory builds a `llama-<name>` binary: `cli`, `server` (OpenAI-compatible HTTP server, includes `webui/`), `perplexity`, `llama-bench`, `quantize`, `imatrix`, `mtmd` (multimodal), `tts`, `rpc`, `tokenize`, `gguf-split`, `export-lora`, `cvector-generator`, `parser`, `fit-params`, `batched-bench`, `completion`.
- **`examples/`** — Smaller, more focused demos (`simple`, `simple-chat`, `embedding`, `lookup`, `lookahead`, `parallel`, `passkey`, `retrieval`, `speculative`, `training`, mobile bindings under `llama.android` / `llama.swiftui`, etc.).
- **`gguf-py/`** — Python GGUF library (constants, tensor mappings, reader/writer). The conversion scripts in the repo root (`convert_hf_to_gguf.py`, `convert_lora_to_gguf.py`, `convert_llama_ggml_to_gguf.py`) depend on it.
- **`tests/`** — C++ unit/integration tests (built as separate executables per file) plus tokenizer comparison scripts and chat parser snapshots in `tests/snapshots/`.
- **`docs/`** — Build, backend, and development docs. Notable: `docs/build.md`, `docs/development/HOWTO-add-model.md`, `docs/autoparser.md`, `docs/multimodal.md`, `docs/ops.md` (auto-generated op support matrix).
- **`ci/run.sh`** — The "full local CI" entry point referenced in CONTRIBUTING.

## Build

The project uses CMake. From the repo root:

```bash
cmake -B build
cmake --build build --config Release -j
```

Backend selection is via `-DGGML_<NAME>=ON` flags: `GGML_CUDA`, `GGML_METAL` (on by default on macOS), `GGML_VULKAN`, `GGML_HIP`, `GGML_SYCL`, `GGML_BLAS`, `GGML_MUSA`, `GGML_CANN`, `GGML_OPENCL`, `GGML_ZENDNN`, `GGML_WEBGPU`, `GGML_OPENVINO`, `GGML_CPU_KLEIDIAI`. See `docs/build.md` for per-backend details.

Useful flags:

- `-DGGML_NATIVE=OFF` — build a portable binary instead of one tuned for the current CPU/GPU. For CUDA also pass `-DCMAKE_CUDA_ARCHITECTURES="86;89;..."` to target specific compute capabilities.
- `-DBUILD_SHARED_LIBS=OFF` — static build.
- `-DGGML_BACKEND_DL=ON` — build backends as separate dynamic libraries that load at runtime (lets one binary serve multiple GPUs/machines).
- `-DLLAMA_CURL=OFF` — disable the curl-based `-hf` model downloader.

CMake presets exist in `CMakePresets.json` for common Windows/Linux/macOS toolchain combinations, e.g. `cmake --preset x64-windows-llvm-release` or `cmake --preset arm64-windows-llvm-release`.

Binaries are emitted to `build/bin/` (or `build/bin/Release/` for multi-config generators on Windows).

## Tests

C++ tests build alongside the project and are registered with CTest:

```bash
ctest --test-dir build -L main --output-on-failure        # main suite
ctest --test-dir build -R test-tokenizer                  # filter by name
ctest --test-dir build -R test-backend-ops --output-on-failure   # ggml op correctness
```

`test-backend-ops` is the canonical check when modifying a `ggml` op or backend — run it with at least two backends built so cross-backend numerical consistency is verified. Add new test cases here when you add or change an op.

`test-chat.cpp`, `test-chat-template.cpp`, `test-chat-peg-parser.cpp`, and `test-chat-auto-parser.cpp` exercise the chat-template / output-parsing pipeline, including the snapshots in `tests/snapshots/`.

For full CI behavior (CPU + optional GPU):

```bash
mkdir tmp
bash ./ci/run.sh ./tmp/results ./tmp/mnt              # CPU only
GG_BUILD_CUDA=1 bash ./ci/run.sh ./tmp/results ./tmp/mnt
```

`ci/run.sh` selects which heavy workflows run via `GG_BUILD_*` env vars (`CUDA`, `SYCL`, `MUSA`, …).

### Server tests (Python)

Located in `tools/server/tests/`. Build `llama-server` first, then:

```bash
cd tools/server/tests
pip install -r requirements.txt
./tests.sh                                            # full suite
./tests.sh unit/test_chat_completion.py -v -x        # one file
./tests.sh unit/test_chat_completion.py::test_invalid_chat_completion_req
SLOW_TESTS=1 ./tests.sh                              # downloads models
```

Set `LLAMA_CACHE` to a persistent dir to avoid re-downloading models. Use `DEBUG_EXTERNAL=1` to run `llama-server` yourself in a debugger and let the tests connect to it.

### Web UI (`tools/server/webui/`)

SvelteKit app whose static build is checked in to `tools/server/public/`. Standard scripts:

```bash
npm install
npm run dev        # Vite dev + Storybook (start llama-server separately)
npm run check      # svelte-check / type check
npm run lint       # prettier + eslint
npm run test       # ui + client + unit + e2e (Playwright)
npm run build      # writes static assets used by llama-server
```

## Performance / quality verification

CONTRIBUTING.md asks contributors who change inference paths to confirm perplexity and performance haven't regressed:

```bash
./build/bin/llama-perplexity -m model.gguf -f wikitext.test.raw
./build/bin/llama-bench -m model.gguf
```

`scripts/compare-llama-bench.py` and `scripts/compare-commits.sh` help compare runs. Wikitext / HellaSwag / Winogrande fetchers live under `scripts/get-*.sh`.

## Coding & naming conventions (from CONTRIBUTING.md)

- 4-space indentation, brackets on the same line; pointer/reference styling is `void * ptr`, `int & a`.
- Public API uses sized integer types (`int32_t`, `size_t` for sizes/offsets).
- `struct foo {}` — not `typedef struct foo {} foo;`. In C++, omit the `struct`/`enum` keyword unless needed: `llama_context * ctx`, not `struct llama_context * ctx`.
- Avoid fancy STL / templates / extra dependencies / extra headers. Prefer plain `for` loops. Cross-platform compatibility matters.
- Filenames: C/C++ are `lowercase-with-dashes.{h,c,cpp}`; Python is `lowercase_with_underscores.py`.
- `snake_case` for everything; enum values are `UPPER_CASE` and prefixed with the enum name (`LLAMA_VOCAB_TYPE_BPE`).
- Functions follow `<class>_<method>` with `<method>` = `<action>_<noun>` (e.g. `llama_model_init`, `llama_sampler_chain_remove`); `get` may be omitted; `_t` suffix marks opaque types.
- Naming optimizes for **longest common prefix**: prefer `number_small`/`number_big` over `small_number`/`big_number`.
- `clang-format` (clang-tools v15+) for formatting doubt; project config is in `.clang-format`.
- **Tensor convention**: row-major; dimension 0 = columns, 1 = rows, 2 = matrices. `C = ggml_mul_mat(ctx, A, B)` means `C^T = A·B^T` ⇔ `C = B·A^T` — note this is unconventional.

Pre-commit hooks (`.pre-commit-config.yaml`) run trailing-whitespace, end-of-file-fixer, YAML check, and `flake8` for Python.

## Adding a new model architecture

The full procedure is in [`docs/development/HOWTO-add-model.md`](docs/development/HOWTO-add-model.md). Cross-cutting touch points:

1. **Conversion** — register a `TextModel`/`MmprojModel` subclass in `convert_hf_to_gguf.py`; add the arch enum and tensor list in `gguf-py/gguf/constants.py`; add tensor-name mappings in `gguf-py/gguf/tensor_mapping.py`.
2. **Architecture metadata** — add `LLM_ARCH_*` in `src/llama-arch.{h,cpp}` (name, tensor list, hyperparams).
3. **Graph builder** — add `src/models/<arch>.cpp` and wire it into model loading in `src/llama-model.cpp`.
4. Verify with `cli`, `server`, `perplexity`, `quantize`, `imatrix` per the doc, then run `test-backend-ops` if any new ops were added.

For multimodal projector additions, also see `tools/mtmd/` and `docs/multimodal.md`.

## Working with chat templates / output parsing

llama.cpp ships its own Jinja engine (`common/jinja/`) and parses model output (reasoning, content, tool calls) using a PEG parser, with a higher-level "auto-parser" that derives markers from the chat template:

- `docs/development/parsing.md` — PEG parser reference.
- `docs/autoparser.md` — auto-parser design.
- `common/jinja/README.md` — Jinja engine architecture and security model (input marking).

When changing parsing logic, the corresponding `tests/test-chat*.cpp` files and `tests/snapshots/*.schema` are usually the right place to extend coverage.

## Server scope

If a feature is being added to `tools/server/`, check `tools/server/README-dev.md` first to confirm the change is in scope before implementing.
