# tests for the per-model settings of the Provision admin layer and what comes with them (stage 2 of
# tools/server/server-admin.cpp, see docs/provision-admin.md): /admin/models/settings, the dry-run fit check
# by llama.cpp itself (/admin/models/settings/check), the GPU view (/admin/gpus, /admin/gpus/rescan), the output
# tail of the model children (/admin/models/log) and /admin/diagnostics.
#
# Everything runs on a CPU-only build and without the network:
#   - the models are copies of the tiny model in the shared cache (in --models-dir and in a models.ini of the test);
#   - the GPUs of a test come from a canned device inventory (PROVISION_ADMIN_INVENTORY=file:) and canned output
#     of the vendor tools in their real formats (PROVISION_ADMIN_GPU_PROBE=file:, nvidia-smi / amd-smi / rocm-smi);
#   - a device that a canned inventory lists but the build does not have gives a real, cheap failed start
#     ("invalid device"), which drives the two-phase commit and the automatic revert.
# Tests that need a CPU-only build (or the real device inventory of one) skip themselves on a GPU build.
# The routers of this file listen on 8380-8399 (one port per xdist worker, see _port()).

import calendar
import glob
import re
import shutil
import struct
import sys
import threading
import urllib.parse

import pytest
from utils import *

# the helpers of the stage-1 admin tests; they work on the module globals bound in create_server() below
import test_admin as stage1
from test_admin import ADMIN_KEY, MODEL_A, MODEL_B, SHARED_CACHE_DIR

server: ServerProcess
state_dir: str
work_dir: Any

MiB = 1024 * 1024
GiB = 1024 * MiB

SETTING_KEYS = [
    "device", "ctx-size", "kv-unified-per-slot", "parallel", "n-gpu-layers", "flash-attn", "cache-type-k", "cache-type-v",
    "ubatch-size", "embeddings", "pooling", "reranking",
]
FEATURES = ["model_settings", "gpus", "model_log", "diagnostics", "fit_check", "upload", "delete"]  # stage 3: upload, delete
DIAGNOSTIC_ITEMS = [
    "deployment", "binary", "gpu_backend", "inventory", "vendor_tool", "memory_mapping", "router_gpu_free", "state_dir",
    "admin_key", "proxy", "ip_allowlist", "model_devices", "model_settings",
]
MODELS_FILE = "admin-models.json"

NEW_ROUTES = [
    ("GET", "/admin/models/settings", None),
    ("GET", "/admin/models/settings?model=tinyA", None),
    ("POST", "/admin/models/settings", {"model": "tinyA", "settings": {"ctx-size": 1024}}),
    ("POST", "/admin/models/settings/check", {"model": "tinyA", "settings": {"device": "none"}}),
    ("GET", "/admin/models/log?model=tinyA", None),
    ("GET", "/admin/gpus", None),
    ("GET", "/admin/gpus?refresh=1", None),
    ("POST", "/admin/gpus/rescan", {}),
    ("GET", "/admin/diagnostics", None),
]

# what a model child prints about its own memory at verbosity >= 4, copied from the log of a real docker-dual host
BREAKDOWN_LINE = "common_memory_breakdown_print: |   - CUDA1 (GTX 1080 Ti) | 11165 = 10667 + (8835 =  4829 +    3584 +     422) +       -8337 |"

# canned output of the vendor tools, in the formats the real tools print
NVIDIA_SHUFFLED = """nvidia-smi
0, 00000000:02:00.0, GPU-bbbbbbbb-0000-0000-0000-000000000002, 4096, 1000, 3096, NVIDIA GeForce GTX 1050 Ti
1, 00000000:01:00.0, GPU-aaaaaaaa-0000-0000-0000-000000000001, 11264, 2264, 9000, NVIDIA GeForce GTX 1080 Ti
"""
NVIDIA_THREE = """nvidia-smi
0, 00000000:01:00.0, GPU-aaaa, 11264, 2264, 9000, NVIDIA GeForce GTX 1080 Ti
1, 00000000:02:00.0, GPU-bbbb, 4096, 1000, 3096, NVIDIA GeForce GTX 1050 Ti
2, 00000000:03:00.0, GPU-cccc, 24576, 100, 24476, NVIDIA GeForce RTX 3090
"""
NVIDIA_AND_AMD = """nvidia-smi
0, 00000000:01:00.0, GPU-aaaa, 11264, 2264, 9000, NVIDIA GeForce GTX 1080 Ti
amd-smi
[{"gpu": 0, "bdf": "0000:c1:00.0", "uuid": "c1ff74a1-0000-1000-8000-000000000000", "mem_usage": {"total_vram": {"value": 196592, "unit": "MB"}, "used_vram": {"value": 283, "unit": "MB"}, "free_vram": {"value": 196309, "unit": "MB"}}}]
"""
# amd-smi list --json, then amd-smi metric --mem-usage --json (a newer release: "gpu_data" wrapper, string values)
AMD_SMI = """amd-smi
[{"gpu": 0, "bdf": "0000:03:00.0", "uuid": "a1", "kfd_id": 1, "node_id": 2, "partition_id": 0},
 {"gpu": 1, "bdf": "0000:0a:00.0", "uuid": "a2", "kfd_id": 3, "node_id": 3, "partition_id": 0}]
---
{"gpu_data": [{"gpu": 0, "mem_usage": {"total_vram": "24560 MB", "used_vram": "60 MB", "free_vram": "24500 MB"}},
              {"gpu": 1, "mem_usage": {"total_vram": {"value": 16368, "unit": "MB"}, "used_vram": {"value": 16000, "unit": "MB"}, "free_vram": {"value": 368, "unit": "MB"}}}]}
"""
# rocm-smi --showbus --showmeminfo vram --showproductname --json (the cards in another order than the devices)
ROCM_SMI = """rocm-smi
{"card0": {"PCI Bus": "0000:0a:00.0", "VRAM Total Memory (B)": "17163091968", "VRAM Total Used Memory (B)": "12582912", "Card series": "Navi 21 [Radeon RX 6800/6800 XT / 6900 XT]", "Card model": "0x73bf", "Card vendor": "Advanced Micro Devices, Inc. [AMD/ATI]", "Card SKU": "D41205"},
 "card1": {"PCI Bus": "0000:03:00.0", "VRAM Total Memory (B)": "25753026560", "VRAM Total Used Memory (B)": "1073741824", "Card series": "Navi 31 [Radeon RX 7900 XT/7900 XTX]"},
 "system": {"Driver version": "6.7.0"}}
"""
# the same with the lines the Python tools print around their JSON (stdout and stderr are read together)
AMD_SMI_WARNINGS = (AMD_SMI.replace("amd-smi\n", "amd-smi\nUser is missing the following required groups: render, video. "
                                                  "Please add user to these groups.\n", 1)
                    + "/usr/lib/python3/dist-packages/amdsmi/amdsmi_interface.py:1203: DeprecationWarning: mem_usage is deprecated\n")
ROCM_SMI_WARNINGS = ROCM_SMI.replace("rocm-smi\n", "rocm-smi\nWARNING: AMD GPU device(s) is/are in a low-power state. "
                                                    "Check power control/runtime_status\n\n", 1)


def _port() -> int:
    """8380 + the number of the xdist worker: the routers of this file never share a port"""
    worker = os.environ.get("PYTEST_XDIST_WORKER", "")
    n = int(worker[2:]) if worker.startswith("gw") and worker[2:].isdigit() else 0
    return 8380 + n % 20


@pytest.fixture(autouse=True)
def create_server(monkeypatch, tmp_path):
    global server, state_dir, work_dir
    server = ServerPreset.router()
    # no context size, slot count or batch size on the router command line: they would lock those settings
    server.n_ctx = None
    server.n_slots = None
    server.n_batch = None
    server.models_max = 3
    server.server_port = _port()
    work_dir = tmp_path
    state_dir = str(tmp_path / "admin-state")
    # the server inherits os.environ (see ServerProcess.start), so start every test from a clean slate
    for name in list(os.environ):
        upper = name.upper()
        if upper.startswith("PROVISION_ADMIN") or upper == "MODEL_DEVICES" or upper.startswith("LLAMA_ARG_"):
            monkeypatch.delenv(name)
    stage1.server = server
    stage1.env = monkeypatch
    stage1.state_dir = state_dir


#
# helpers
#


def _enable(**variables: str | None) -> None:
    stage1._enable_admin(**variables)


def _model_copy(name: str) -> str:
    """a copy of the tiny model outside of the cache and of --models-dir (for a models.ini section)"""
    return stage1._make_preset_model(work_dir, name)


def _ini_text(sections: str = "", star: str = "n-gpu-layers = 99", tiny_p: str = "ctx-size = 512") -> str:
    path = work_dir / "tinyP.gguf"
    model = path.as_posix() if path.exists() else _model_copy("tinyP")
    return "version = 1\n\n[*]\n" + star + "\n\n" + stage1._preset_section("tinyP", model, tiny_p) + "\n" + sections


def _write_ini(text: str, crlf: bool = False) -> str:
    data = text.replace("\r\n", "\n")
    if crlf:
        data = data.replace("\n", "\r\n")
    path = work_dir / "models.ini"
    path.write_bytes(data.encode("utf-8"))
    return str(path)


def _setup(sections: str = "", star: str = "n-gpu-layers = 99", tiny_p: str = "ctx-size = 512", models_dir=("tinyA", "tinyB"),
           crlf: bool = False) -> None:
    """the models of a test: tinyA and tinyB in --models-dir, tinyP (ctx-size 512) and `sections` in models.ini,
    [*] with `star`, and the models of the shared cache"""
    if models_dir and server.models_dir is None:
        server.models_dir = stage1._make_models_dir(work_dir, *models_dir)
    server.models_preset = _write_ini(_ini_text(sections, star, tiny_p), crlf)


def _section(name: str, extra: str = "", model: str | None = None) -> str:
    return stage1._preset_section(name, model or _model_copy(name), extra)


def _fixture_file(name: str, text: str) -> str:
    path = work_dir / "fixtures" / name
    path.parent.mkdir(exist_ok=True)
    path.write_bytes(text.encode("utf-8"))
    return str(path)


def _dev(name: str, backend: str, description: str, pci: str | None, total: int, free: int, kind: str = "gpu") -> dict:
    d = {"name": name, "backend": backend, "description": description, "type": kind, "memory_total": total, "memory_free": free}
    if pci is not None:
        d["device_id"] = pci
    return d


def _inventory(devices: list, name: str = "inventory.json", directives: str = "") -> str:
    """a canned device inventory: the JSON the inventory helper prints ('# sleep <ms>' / '# exit <code>' lines are
    directives of the process that prints it)"""
    return _fixture_file(name, directives + json.dumps({"ok": True, "backends": ["CPU"], "devices": devices}) + "\n")


def _two_gpus() -> list:
    return [_dev("CUDA0", "CUDA", "NVIDIA GeForce GTX 1080 Ti", "0000:01:00.0", 11811160064, 11000000000),
            _dev("CUDA1", "CUDA", "NVIDIA GeForce GTX 1050 Ti", "0000:02:00.0", 4294967296, 3000000000)]


def _fake_gpus() -> list:
    """devices this build cannot open (CPU-only, or fewer GPUs): a child started on one fails with 'invalid device'"""
    return [_dev("CUDA6", "CUDA", "NVIDIA GeForce GTX 1080 Ti", "0000:06:00.0", 11811160064, 11000000000),
            _dev("CUDA7", "CUDA", "NVIDIA GeForce GTX 1050 Ti", "0000:07:00.0", 4294967296, 3000000000)]


def _admin_get(path: str, **kw) -> dict:
    res = stage1._admin("GET", path, **kw)
    assert res.status_code == 200, f"GET {path} -> {res.status_code}: {res.body}"
    return res.body


def _quote(model: str) -> str:
    return urllib.parse.quote(model, safe="")


def _row(model: str) -> dict:
    return _admin_get("/admin/models/settings?model=" + _quote(model))


def _list() -> dict:
    return _admin_get("/admin/models/settings")


def _list_row(model: str) -> dict:
    rows = {r["id"]: r for r in _list()["models"]}
    assert model in rows, f"{model} not in the list: {sorted(rows)}"
    return rows[model]


def _save(model: str, settings: dict | None = None, *, restart: bool | None = None, reset: bool | None = None,
          expect: int = 200, timeout: float = 90) -> dict:
    body: dict = {"model": model}
    if settings is not None:
        body["settings"] = settings
    if restart is not None:
        body["restart"] = restart
    if reset is not None:
        body["reset"] = reset
    res = stage1._admin("POST", "/admin/models/settings", data=body, timeout=timeout)
    assert res.status_code == expect, f"POST /admin/models/settings {body} -> {res.status_code}: {res.body}"
    return res.body


def _check(model: str, settings: dict | None = None, expect: int = 200, timeout: float = 90) -> dict:
    body: dict = {"model": model}
    if settings is not None:
        body["settings"] = settings
    res = stage1._admin("POST", "/admin/models/settings/check", data=body, timeout=timeout)
    assert res.status_code == expect, f"POST /admin/models/settings/check {body} -> {res.status_code}: {res.body}"
    return res.body


def _assert_error(res: ServerResponse, status: int, error_type: str, param: str | None = None) -> None:
    stage1._assert_error(res, status, error_type)
    if param is not None:
        assert res.body["error"].get("param") == param, f"expected param {param}: {res.body}"


def _args(model: str) -> list:
    return stage1._models()[model]["status"]["args"]


def _flag(model_or_args, flag: str) -> str | None:
    args = _args(model_or_args) if isinstance(model_or_args, str) else model_or_args
    return args[args.index(flag) + 1] if flag in args else None


def _gpus(refresh: bool = False) -> dict:
    return _admin_get("/admin/gpus" + ("?refresh=1" if refresh else ""))


def _card(body: dict, device: str) -> dict:
    for g in body["gpus"]:
        if g["device"] == device:
            return g
    raise AssertionError(f"no card for {device}: {[g['device'] for g in body['gpus']]}")


def _card_model(card: dict, model: str) -> dict:
    for m in card["models"]:
        if m["id"] == model:
            return m
    raise AssertionError(f"{model} is not on the card of {card['device']}: {[m['id'] for m in card['models']]}")


def _log(model: str) -> dict:
    return _admin_get("/admin/models/log?model=" + _quote(model))


def _diagnostics(headers: dict | None = None) -> dict:
    body = _admin_get("/admin/diagnostics", headers=headers)
    assert [i["id"] for i in body["items"]] == DIAGNOSTIC_ITEMS
    return {i["id"]: i for i in body["items"]}


def _models_file() -> dict:
    with open(os.path.join(state_dir, MODELS_FILE), encoding="utf-8") as f:
        return json.load(f)


def _file_bytes(name: str) -> bytes:
    with open(os.path.join(state_dir, name), "rb") as f:
        return f.read()


def _write_models_file(doc: dict | str) -> None:
    os.makedirs(state_dir, exist_ok=True)
    text = doc if isinstance(doc, str) else json.dumps(doc, indent=2)
    with open(os.path.join(state_dir, MODELS_FILE), "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def _wait_row(model: str, predicate, timeout: float = 60, what: str = "") -> dict:
    deadline = time.time() + timeout
    row = None
    while time.time() < deadline:
        row = _row(model)
        if predicate(row):
            return row
        time.sleep(0.2)
    raise AssertionError(f"timed out waiting for {model}: {what}; last row: {json.dumps(row)[:3000]}")


def _wait_failure(model: str, timeout: float = 60) -> dict:
    """the output tail of a failed start, once the admin layer has seen the failure"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        body = _log(model)
        if body["last_failure"] is not None:
            return body
        time.sleep(0.2)
    raise AssertionError(f"no failed start of {model} seen")


def _load_async(model: str) -> None:
    res = server.make_request("POST", "/models/load", data={"model": model}, timeout=60)
    assert res.status_code == 200, f"loading {model} failed: {res.body}"


def _gpu_backend_state() -> str:
    return _gpus()["gpu_backend"]["state"]


def _skip_unless_cpu_only_build() -> None:
    if _gpu_backend_state() != "cpu_only":
        pytest.skip("this test needs a CPU-only build of the server")  # ty: ignore[too-many-positional-arguments]


def _skip_on_gpu_build() -> None:
    """the same for a router with a canned inventory (its GPU backend state is "ok" then): the build itself has no GPU
    backend library next to the binary and no GPU backend registry"""
    backend = _gpus()["gpu_backend"]
    if backend["libraries"] or any(b["name"].lower() not in ("cpu", "blas", "rpc", "meta") for b in backend["backends"]):
        pytest.skip("this test needs a CPU-only build of the server")  # ty: ignore[too-many-positional-arguments]


def _props_n_ctx(model: str) -> int:
    res = server.make_request("GET", "/props?model=" + _quote(model))
    assert res.status_code == 200, f"GET /props?model={model} -> {res.status_code}: {res.body}"
    return res.body["default_generation_settings"]["n_ctx"]


#
# minimal GGUF reader / writer (no gguf-py, no numpy): fixtures derived from the tiny model
#

_GGUF_SCALARS = {0: "<B", 1: "<b", 2: "<H", 3: "<h", 4: "<I", 5: "<i", 6: "<f", 7: "<?", 10: "<Q", 11: "<q", 12: "<d"}
_GGUF_TYPE_SIZE = {0: 4, 1: 2, 30: 2}  # bytes per element of F32, F16, BF16


def _gguf_parse(data: bytes) -> dict:
    pos = 0

    def take(n: int) -> bytes:
        nonlocal pos
        out = data[pos:pos + n]
        pos += n
        return out

    def u32() -> int:
        return struct.unpack("<I", take(4))[0]

    def u64() -> int:
        return struct.unpack("<Q", take(8))[0]

    def gstr() -> str:
        return take(u64()).decode("utf-8", errors="replace")

    def value(t: int):
        if t == 8:
            return gstr()
        if t == 9:
            et, n = u32(), u64()
            return [value(et) for _ in range(n)]
        fmt = _GGUF_SCALARS[t]
        return struct.unpack(fmt, take(struct.calcsize(fmt)))[0]

    assert take(4) == b"GGUF", "not a GGUF file"
    version = u32()
    n_tensors, n_kv = u64(), u64()
    kv: dict = {}
    kv_raw: list = []
    for _ in range(n_kv):
        start = pos
        key = gstr()
        kv[key] = value(u32())
        kv_raw.append((key, data[start:pos]))
    tensors: list = []
    for _ in range(n_tensors):
        start = pos
        name = gstr()
        dims = [u64() for _ in range(u32())]
        ggml_type = u32()
        info = data[start:pos]
        tensors.append({"name": name, "dims": dims, "type": ggml_type, "offset": u64(), "info": info})
    alignment = int(kv.get("general.alignment", 32))
    data_start = (pos + alignment - 1) // alignment * alignment
    return {"version": version, "kv": kv, "kv_raw": kv_raw, "tensors": tensors, "alignment": alignment, "data": data[data_start:]}


def _gguf_info(path: str) -> dict:
    with open(path, "rb") as f:
        return _gguf_parse(f.read())


def _gguf_tensor_bytes(t: dict) -> int:
    if t["type"] not in _GGUF_TYPE_SIZE:
        pytest.skip(f"the fixture model has a tensor of ggml type {t['type']}")  # ty: ignore[too-many-positional-arguments]
    n = _GGUF_TYPE_SIZE[t["type"]]
    for d in t["dims"]:
        n *= d
    return n


def _gguf_enc_str(s: str) -> bytes:
    b = s.encode("utf-8")
    return struct.pack("<Q", len(b)) + b


def _gguf_value_u32(n: int) -> bytes:
    """a typed GGUF value (type + payload) for _gguf_copy(values=...)"""
    return struct.pack("<II", 4, n)


def _gguf_value_u64(n: int) -> bytes:
    return struct.pack("<IQ", 10, n)


def _gguf_value_str_array(items: list) -> bytes:
    return struct.pack("<IIQ", 9, 8, len(items)) + b"".join(_gguf_enc_str(s) for s in items)


def _gguf_copy(src: str, dst, strings: dict | None = None, drop_tensors: tuple = (), values: dict | None = None) -> str:
    """a copy of a GGUF file with some string values (or typed values, see _gguf_value_*) replaced or added and some
    tensors left out"""
    g = _gguf_info(src)
    strings = dict(strings or {})
    values = dict(values or {})
    enc = _gguf_enc_str

    kvs = []
    for key, raw in g["kv_raw"]:
        if key in strings:
            kvs.append(enc(key) + struct.pack("<I", 8) + enc(strings.pop(key)))
        elif key in values:
            kvs.append(enc(key) + values.pop(key))
        else:
            kvs.append(raw)
    for key, text in strings.items():
        kvs.append(enc(key) + struct.pack("<I", 8) + enc(text))
    for key, value in values.items():
        kvs.append(enc(key) + value)
    ends = sorted(t["offset"] for t in g["tensors"]) + [len(g["data"])]
    infos = []
    blob = b""
    for t in g["tensors"]:
        if t["name"] in drop_tensors:
            continue
        end = ends[ends.index(t["offset"]) + 1]  # the next tensor (offsets and sizes are padded to the alignment)
        infos.append(t["info"] + struct.pack("<Q", len(blob)))
        blob += g["data"][t["offset"]:end]
    head = b"GGUF" + struct.pack("<IQQ", g["version"], len(infos), len(kvs)) + b"".join(kvs) + b"".join(infos)
    head += b"\0" * ((-len(head)) % g["alignment"])
    with open(dst, "wb") as f:
        f.write(head + blob)
    return str(dst)


#
# admin layer disabled, key, routes
#


def test_models_admin_disabled_has_no_new_routes():
    """without PROVISION_ADMIN the stage-2 routes do not exist, no helper process runs and saved settings are ignored"""
    global server
    _setup()
    # a canned inventory that would take 20 s: the router starts at once because no inventory runs
    slow = _inventory(_two_gpus(), directives="# sleep 30000\n")
    _write_models_file({"version": 1, "models": {"tinyA": {"settings": {"ctx-size": "1024"}, "phase": "committed"}}})
    before = _file_bytes(MODELS_FILE)
    _enable(PROVISION_ADMIN=None, PROVISION_ADMIN_INVENTORY="file:" + slow, PROVISION_ADMIN_GPU_PROBE="file:" + slow)
    server.log_path = str(work_dir / "server.log")
    t0 = time.time()
    server.start()
    assert time.time() - t0 < 15

    for method, path, data in NEW_ROUTES:
        stage1._assert_not_found(stage1._admin(method, path, data))
        stage1._assert_not_found(stage1._admin(method, path, data, key=None))
    assert _flag("tinyA", "--ctx-size") is None
    server.stop()
    assert _file_bytes(MODELS_FILE) == before
    assert sorted(os.listdir(state_dir)) == [MODELS_FILE]
    assert "admin:" not in stage1._read_text(server.log_path)


def test_models_routes_need_the_admin_key():
    global server
    _setup()
    _enable()
    server.start()

    for method, path, data in NEW_ROUTES:
        for key in (None, "", "wrong-key", ADMIN_KEY.lower()):
            res = stage1._admin(method, path, data, key=key)
            _assert_error(res, 401, "admin_key_required")
            assert {k.lower(): v for k, v in res.headers.items()}.get("cache-control") == "no-store"
        # the key in another header is not an admin key
        _assert_error(stage1._admin(method, path, data, key=None, headers={"Authorization": f"Bearer {ADMIN_KEY}"}),
                      401, "admin_key_required")
    # a rejected request changes nothing
    assert not os.path.exists(os.path.join(state_dir, MODELS_FILE))
    assert _flag("tinyA", "--ctx-size") is None

    for method, path, data in NEW_ROUTES:
        res = stage1._admin(method, path, data)
        assert res.status_code == 200, f"{method} {path} -> {res.status_code}: {res.body}"
        assert {k.lower(): v for k, v in res.headers.items()}.get("cache-control") == "no-store", f"{method} {path}"
    res = stage1._admin("POST", "/admin/models/settings", data={"model": "tinyA", "settings": {"ctx-size": 0}})
    _assert_error(res, 400, "invalid_request_error", "ctx-size")
    assert {k.lower(): v for k, v in res.headers.items()}.get("cache-control") == "no-store"


def test_models_routes_not_reachable_through_predict_route():
    """the Vertex AI compat route dispatches to handlers by name: the stage-2 admin handlers are not among them"""
    global server
    _setup()
    _enable()
    server.gcp_compat = True
    server.start()

    formats = [
        "/admin/models/settings", "adminModelsSettings", "/admin/models/settings/check", "adminModelsSettingsCheck",
        "/admin/models/log", "adminModelsLog", "/admin/gpus", "adminGpus", "/admin/gpus/rescan", "adminGpusRescan",
        "/admin/diagnostics", "adminDiagnostics",
    ]
    instances = [{"@requestFormat": fmt, "model": "tinyA", "settings": {"ctx-size": 1024}} for fmt in formats]
    instances.append({"@requestFormat": "/props"})
    res = stage1._admin("POST", "/predict", data={"instances": instances})
    assert res.status_code == 200
    predictions = res.body["predictions"]
    assert len(predictions) == len(instances)
    for fmt, prediction in zip(formats, predictions):
        assert set(prediction) == {"error"}, f"{fmt} was dispatched: {prediction}"
        assert "no handler registered" in prediction["error"]["message"]
    assert predictions[-1]["role"] == "router"
    assert not os.path.exists(os.path.join(state_dir, MODELS_FILE))


def test_models_status_and_settings_report_stage2_fields():
    global server
    _setup()
    _enable(PROVISION_ADMIN_INVENTORY="file:" + _inventory(_two_gpus()), PROVISION_ADMIN_GPU_PROBE="off")
    server.start()

    status = stage1._admin_status()
    assert status["features"] == FEATURES
    assert status["warnings"] == []
    model_settings = status["state"]["model_settings"]
    assert stage1._same_path(model_settings["file"], os.path.join(state_dir, MODELS_FILE))
    assert model_settings["error"] == ""
    assert model_settings["read_only"] is False
    assert model_settings["backup_loaded"] is False
    assert model_settings["entries"] == 0
    # the unauthenticated probe tells nothing new
    assert stage1._admin("GET", "/admin/status", key=None).body == {"admin": True, "authorized": False, "key_configured": True}

    settings = stage1._settings()
    assert settings["build_devices"] == ["CUDA0", "CUDA1"]
    assert settings["warnings"] == []
    # the stage-1 model items are unchanged
    for item in settings["models"]:
        assert set(item) == {"id", "source", "status", "pinned"}

    _save("tinyA", {"ctx-size": 1024})
    assert stage1._admin_status()["state"]["model_settings"]["entries"] == 1


#
# listing, schema, precedence
#


def test_models_settings_list():
    global server
    _setup()
    _enable()
    server.start()

    body = _list()
    schema = {s["key"]: s for s in body["schema"]}
    assert [s["key"] for s in body["schema"]] == SETTING_KEYS
    assert schema["device"] == {"key": "device", "group": "placement", "default": None, "type": "device"}
    assert schema["ctx-size"] == {"key": "ctx-size", "group": "placement", "default": None, "type": "int", "min": 256, "max": 1048576}
    assert schema["kv-unified-per-slot"]["type"] == "int" and schema["kv-unified-per-slot"]["group"] == "placement"
    assert schema["parallel"] == {"key": "parallel", "group": "placement", "default": "-1", "type": "int", "min": 1, "max": 16, "auto": -1}
    assert schema["n-gpu-layers"] == {"key": "n-gpu-layers", "group": "advanced", "default": "auto", "type": "ngl", "min": 0, "max": 999,
                                      "values": ["auto", "all"]}
    assert schema["flash-attn"] == {"key": "flash-attn", "group": "advanced", "default": "auto", "type": "enum", "values": ["on", "off", "auto"]}
    assert schema["cache-type-k"]["values"] == ["f16", "bf16", "q8_0", "q4_0"] and schema["cache-type-k"]["pair"] == "cache-type-v"
    assert schema["cache-type-v"]["default"] == "f16" and schema["cache-type-v"]["pair"] == "cache-type-k"
    assert schema["ubatch-size"] == {"key": "ubatch-size", "group": "advanced", "default": "512", "type": "int", "min": 32, "max": 8192}
    assert schema["embeddings"] == {"key": "embeddings", "group": "role", "default": "false", "type": "bool"}
    assert schema["pooling"]["values"] == ["none", "mean", "cls", "last", "rank"] and schema["pooling"]["default"] is None
    assert schema["reranking"]["type"] == "bool"

    assert body["unattached"] == []
    assert body["warnings"] == []
    assert body["state"]["read_only"] is False and body["state"]["error"] == "" and body["state"]["problems"] == []
    rows = {r["id"]: r for r in body["models"]}
    # every source, under the id /v1/models shows
    assert rows["tinyP"]["source"] == "preset"
    assert rows["tinyA"]["source"] == "models_dir"
    assert rows[MODEL_B]["source"] == "cache"
    # the same models as /models (cache models aside: another test may download or delete one meanwhile)
    listed = stage1._models()
    assert {i for i, r in rows.items() if r["source"] != "cache"} == {i for i, m in listed.items() if m["source"] != "cache"}
    assert MODEL_B in listed
    for row in rows.values():
        assert row["exists"] is True
        assert row["overrides"] == {} and row["kept"] == {}
        assert row["phase"] is None and row["failures"] == 0 and row["last_good"] is None and row["reverted"] is None
        assert row["reverted_at_startup"] is False and row["last_failure"] is None
        assert sorted(row["fields"]) == sorted(SETTING_KEYS)
        assert row["facts_status"] in ("ready", "pending", "error", "unsupported")
        assert row["last_run"]["running"] is False
    # values from models.ini: the section and [*]
    p = rows["tinyP"]["fields"]
    assert p["ctx-size"] == {"effective": "512", "default": None, "inherited": "512", "inherited_origin": "models_ini", "locked": None,
                             "override": None, "active": False, "inactive_reason": None, "origin": "models_ini"}
    assert p["n-gpu-layers"]["effective"] == "99" and p["n-gpu-layers"]["origin"] == "models_ini"
    a = rows["tinyA"]["fields"]
    assert a["ctx-size"]["effective"] is None and a["ctx-size"]["origin"] == "default" and a["ctx-size"]["inherited_origin"] == "default"
    assert a["n-gpu-layers"]["inherited"] == "99"
    assert a["flash-attn"] == {"effective": None, "default": "auto", "inherited": None, "inherited_origin": "default", "locked": None,
                               "override": None, "active": False, "inactive_reason": None, "origin": "default"}


def test_models_settings_precedence():
    """admin override > models.ini section > [*] > built-in default, for every model source"""
    global server
    _setup(star="n-gpu-layers = 99\nctx-size = 384")
    _enable()
    server.start()

    assert _flag("tinyP", "--ctx-size") == "512"
    assert _flag("tinyA", "--ctx-size") == "384"
    assert _flag(MODEL_B, "--ctx-size") == "384"
    assert _row("tinyA")["fields"]["ctx-size"]["inherited"] == "384"

    for model, value in (("tinyP", 1024), ("tinyA", 2048), (MODEL_B, 768)):
        body = _save(model, {"ctx-size": value})
        assert body["result"]["changed"] is True
        f = body["model"]["fields"]["ctx-size"]
        assert f["effective"] == str(value) and f["origin"] == "admin" and f["override"] == str(value) and f["active"] is True
        assert _flag(model, "--ctx-size") == str(value)
    assert _row("tinyP")["fields"]["ctx-size"]["inherited"] == "512"

    body = _save("tinyP", reset=True)
    assert body["result"]["changed"] is True
    assert body["previous"] == {"ctx-size": "1024"}
    assert body["model"]["fields"]["ctx-size"]["origin"] == "models_ini"
    assert _flag("tinyP", "--ctx-size") == "512"


def test_models_device_precedence_and_identity():
    """a per-model device beats "GPU for new models"; that one only fills models without a device"""
    global server
    _setup(_section("tinyD", "device = CUDA0"))
    _enable(PROVISION_ADMIN_INVENTORY="file:" + _inventory(_two_gpus()), PROVISION_ADMIN_GPU_PROBE="off",
            PROVISION_ADMIN_DEFAULT_DEVICE="CUDA1")
    server.start()

    # models.ini models are never filled; cache and models_dir models without a device are
    assert _flag("tinyP", "--device") is None
    assert _flag("tinyA", "--device") == "CUDA1"
    f = _row("tinyA")["fields"]["device"]
    assert f["origin"] == "default_device" and f["inherited"] == "CUDA1" and f["inherited_origin"] == "default_device"

    # the same device as the fill: nothing changes for the child, nothing restarts
    body = _save("tinyA", {"device": "cuda1"})
    assert body["result"]["changed"] is False
    assert body["model"]["overrides"] == {"device": "CUDA1"}  # stored in the spelling of the device inventory
    body = _save("tinyA", {"device": "CUDA0"})
    assert body["result"]["changed"] is True
    assert body["model"]["fields"]["device"]["origin"] == "admin"
    assert _flag("tinyA", "--device") == "CUDA0"
    # the PCI id is saved with the device
    assert _models_file()["models"]["tinyA"]["device_pci"] == {"CUDA0": "0000:01:00.0"}
    # an admin device beats the models.ini section
    _save("tinyD", {"device": "CUDA1"})
    assert _flag("tinyD", "--device") == "CUDA1"
    assert _row("tinyD")["fields"]["device"]["inherited"] == "CUDA0"
    # several devices, in the given order; at most 4; none
    _save("tinyB", {"device": "cuda1, CUDA0"})
    assert _flag("tinyB", "--device") == "CUDA1,CUDA0"
    _assert_error(stage1._admin("POST", "/admin/models/settings", data={"model": "tinyB", "settings": {"device": "CUDA0,CUDA0"}}),
                  400, "invalid_request_error", "device")
    _save("tinyB", {"device": "none"})
    assert _flag("tinyB", "--device") == "none"


def test_models_device_moved_and_missing():
    """a saved device whose PCI id is now behind another name, or that is not there any more, is reported"""
    global server
    _setup()
    inventory = _inventory(_two_gpus())
    _enable(PROVISION_ADMIN_INVENTORY="file:" + inventory, PROVISION_ADMIN_GPU_PROBE="off")
    server.start()
    _save("tinyA", {"device": "CUDA1"})
    _save("tinyB", {"device": "CUDA0"})
    server.stop()

    # the GTX 1080 Ti went away, the GTX 1050 Ti is CUDA0 now
    _inventory([_dev("CUDA0", "CUDA", "NVIDIA GeForce GTX 1050 Ti", "0000:02:00.0", 4294967296, 3000000000)])
    server.start()
    row = _row("tinyA")
    assert row["device_missing"] == {"devices": ["CUDA1"], "message": row["device_missing"]["message"]}
    assert "device_missing" in row["warnings"] and "apply_error" in row["warnings"]
    assert row["fields"]["device"]["active"] is False and row["fields"]["device"]["inactive_reason"] == "device_missing"
    assert row["apply_error"] == "device: device_missing"
    assert _flag("tinyA", "--device") is None  # not applied: the model keeps its inherited device
    row = _row("tinyB")
    assert row["device_moved"] == {"device": "CUDA0", "saved_pci": "0000:01:00.0", "current_pci": "0000:02:00.0", "now_named": None}
    assert "device_moved" in row["warnings"]
    assert _flag("tinyB", "--device") == "CUDA0"
    # the saved entry is kept as it was
    assert _models_file()["models"]["tinyA"]["settings"] == {"device": "CUDA1"}


def test_models_default_device_not_present():
    global server
    _setup()
    _enable(PROVISION_ADMIN_INVENTORY="file:" + _inventory(_two_gpus()), PROVISION_ADMIN_GPU_PROBE="off",
            PROVISION_ADMIN_DEFAULT_DEVICE="CUDA5")
    server.start()

    assert "default_device_not_present" in stage1._admin_status()["warnings"]
    settings = stage1._settings()
    assert settings["default_device"] == "CUDA5" and "default_device_not_present" in settings["warnings"]
    assert "default_device_not_present" in _list()["warnings"]
    assert "default_device_not_present" in _row("tinyA")["warnings"]
    assert "default_device_not_present" not in _row("tinyP")["warnings"]  # a models.ini model is never filled
    assert _flag("tinyA", "--device") is None

    stage1._post_settings({"default_device": "CUDA1"})
    assert _flag("tinyA", "--device") == "CUDA1"
    assert stage1._admin_status()["warnings"] == []


#
# saving and applying
#


def test_models_settings_save_stopped_model():
    global server
    _setup()
    _enable()
    server.start()

    body = _save("tinyA", {"ctx-size": 1024})
    assert body["result"] == {"changed": True, "reloaded": True, "restarted": False, "was_running": False, "restarted_also": [],
                              "not_restarted": [], "load_error": "", "reload_error": ""}
    assert body["previous"] == {"ctx-size": None}
    assert body["warnings"] == []
    row = body["model"]
    assert row["overrides"] == {"ctx-size": "1024"} and row["phase"] == "pending" and row["failures"] == 0
    assert isinstance(row["updated"], str) and row["updated"].endswith("Z")
    assert _flag("tinyA", "--ctx-size") == "1024"
    assert stage1._model_status("tinyA") == "unloaded"

    # the same value, also in another spelling: nothing changes, nothing is reloaded
    for value in (1024, "1024", "01024", " 1024"):
        body = _save("tinyA", {"ctx-size": value})
        assert body["result"]["changed"] is False and body["result"]["reloaded"] is False, value

    # keys that are not given stay; values are stored in one spelling
    body = _save("tinyA", {"ubatch-size": "256", "n-gpu-layers": "ALL", "flash-attn": True})
    assert body["model"]["overrides"] == {"ctx-size": "1024", "ubatch-size": "256", "n-gpu-layers": "all", "flash-attn": "on"}
    args = _args("tinyA")
    assert _flag(args, "--ubatch-size") == "256" and _flag(args, "--n-gpu-layers") == "all" and _flag(args, "--flash-attn") == "on"
    # null removes a key; previous has the old value of every key the request touched or had
    body = _save("tinyA", {"ubatch-size": None, "n-gpu-layers": None, "flash-attn": None})
    assert body["model"]["overrides"] == {"ctx-size": "1024"}
    assert body["previous"] == {"ctx-size": "1024", "ubatch-size": "256", "n-gpu-layers": "all", "flash-attn": "on"}
    assert _flag("tinyA", "--ubatch-size") is None
    assert _flag("tinyA", "--n-gpu-layers") == "99"  # back to [*]

    # canonical spellings of every kind
    body = _save("tinyB", {"device": "NONE", "parallel": "auto", "embeddings": "yes", "n-gpu-layers": -1, "cache-type-k": "Q8_0",
                           "cache-type-v": "q8_0", "pooling": "MEAN", "reranking": 0, "kv-unified-per-slot": 512})
    assert body["model"]["overrides"] == {"device": "none", "parallel": "-1", "embeddings": "true", "n-gpu-layers": "auto",
                                          "cache-type-k": "q8_0", "cache-type-v": "q8_0", "pooling": "mean", "reranking": "false",
                                          "kv-unified-per-slot": "512"}
    for flag, value in (("--device", "none"), ("--n-gpu-layers", "auto"), ("--cache-type-k", "q8_0"), ("--pooling", "mean"),
                        ("--kv-unified-per-slot", "512")):
        assert _flag("tinyB", flag) == value, flag
    assert "--embedding" in _args("tinyB") or "--embeddings" in _args("tinyB")
    # a built-in default is not rendered for the child
    assert _flag("tinyB", "--parallel") is None and "--reranking" not in _args("tinyB")


def test_models_built_in_defaults_do_not_restart():
    """choosing "Auto (4)" or "f16" for a running model is no change: no 409, no restart"""
    global server
    _setup()
    _enable()
    server.start()
    stage1._load_model_and_wait("tinyB")
    port = _flag("tinyB", "--port")

    body = _save("tinyB", {"parallel": -1, "cache-type-k": "f16", "cache-type-v": "F16", "flash-attn": "auto", "ubatch-size": 512})
    assert body["result"]["changed"] is False and body["result"]["reloaded"] is False
    assert body["model"]["overrides"] == {"parallel": "-1", "cache-type-k": "f16", "cache-type-v": "f16", "flash-attn": "auto",
                                          "ubatch-size": "512"}
    # stored for a model that already runs with them: committed right away
    assert body["model"]["phase"] == "committed"
    args = _args("tinyB")
    for flag in ("--parallel", "--cache-type-k", "--cache-type-v", "--flash-attn", "--ubatch-size", "--kv-unified"):
        assert flag not in args, flag
    body = _save("tinyB", {"parallel": None, "cache-type-k": None, "cache-type-v": None, "flash-attn": None, "ubatch-size": None})
    assert body["result"]["changed"] is False
    assert stage1._model_status("tinyB") == "loaded" and _flag("tinyB", "--port") == port


def test_models_parallel_couples_kv_unified():
    global server
    _setup(_section("tinyK", "kv-unified = false"))
    _enable()
    server.start()

    body = _save("tinyA", {"parallel": 3})
    args = _args("tinyA")
    assert _flag(args, "--parallel") == "3" and "--kv-unified" in args
    assert body["model"]["context"]["slots"] == 3 and body["model"]["context"]["unified"] is True
    _save("tinyA", {"parallel": None})
    assert "--kv-unified" not in _args("tinyA") and "--parallel" not in _args("tinyA")

    # the preset decides kv-unified itself: no coupling
    body = _save("tinyK", {"parallel": 3})
    args = _args("tinyK")
    assert _flag(args, "--parallel") == "3" and "--kv-unified" not in args and "--no-kv-unified" in args
    assert body["model"]["context"]["unified"] is False
    # never stored
    assert _models_file()["models"]["tinyK"]["settings"] == {"parallel": "3"}


def test_models_fit_check_cache_follows_the_model_state():
    """a cached dry run answers again only while the model is in the same state: llama.cpp's fit plans for the memory
    free at that moment, and a running model holds memory its own check sees as used"""
    global server
    _setup()
    _enable()
    server.start()
    draft = {"device": "none", "ctx-size": 1024}
    first = _check("tinyA", draft)
    assert first["ok"] is True and first["cached"] is False and first["checked_while_running"] is False
    assert _check("tinyA", draft)["cached"] is True

    stage1._load_model_and_wait("tinyA")
    running = _check("tinyA", draft)
    assert running["ok"] is True and running["cached"] is False and running["checked_while_running"] is True
    assert _check("tinyA", draft)["cached"] is True


def test_models_running_model_needs_restart():
    global server
    _setup()
    _enable()
    server.start()
    stage1._load_model_and_wait("tinyA")
    stage1._load_model_and_wait("tinyB")
    port_b = _flag("tinyB", "--port")

    # no silent unload: 409 and nothing saved
    res = stage1._admin("POST", "/admin/models/settings", data={"model": "tinyA", "settings": {"ctx-size": 1024}})
    _assert_error(res, 409, "restart_required")
    assert _row("tinyA")["overrides"] == {}
    assert not os.path.exists(os.path.join(state_dir, MODELS_FILE))
    assert stage1._model_status("tinyA") == "loaded"

    body = _save("tinyA", {"ctx-size": 1024}, restart=True)
    result = body["result"]
    assert result["changed"] and result["reloaded"] and result["restarted"] and result["was_running"]
    assert result["restarted_also"] == [] and result["not_restarted"] == [] and result["load_error"] == ""
    assert body["model"]["phase"] in ("trial", "committed")
    stage1._wait_for_model_status("tinyA", "loaded")
    assert _flag("tinyA", "--ctx-size") == "1024"
    assert _props_n_ctx("tinyA") == 1024
    row = _wait_row("tinyA", lambda r: r["phase"] == "committed", what="phase committed")
    assert row["last_good"] == {"ctx-size": "1024"} and row["failures"] == 0
    # the other running model was not touched
    assert stage1._model_status("tinyB") == "loaded" and _flag("tinyB", "--port") == port_b

    # a fit check of a running model works too
    check = _check("tinyA", {"ctx-size": 2048, "device": "none"})
    assert check["ok"] is True and check["n_ctx"] == 2048


def test_models_save_and_load_restarts_models_the_rescan_stopped():
    global server
    _setup()
    _enable()
    server.start()
    stage1._load_model_and_wait("tinyP")

    # the models.ini section of the running tinyP changes on disk; the save of another model rescans the sources
    _write_ini(_ini_text(tiny_p="ctx-size = 768"))
    body = _save("tinyB", {"ctx-size": 512}, restart=True)  # tinyB is stopped: Save and load
    result = body["result"]
    assert result["restarted"] is True and result["was_running"] is False
    assert result["restarted_also"] == ["tinyP"] and result["not_restarted"] == []
    stage1._wait_for_model_status("tinyP", "loaded")
    assert _flag("tinyP", "--ctx-size") == "768"
    stage1._wait_for_model_status("tinyB", "loaded")
    assert _flag("tinyB", "--ctx-size") == "512"
    _wait_row("tinyB", lambda r: r["phase"] == "committed", what="phase committed")


def test_models_failed_reload_restores_the_settings_and_the_model():
    """the model list cannot be rebuilt at the moment of a save (models.ini broken meanwhile): 500 with reload_error,
    the previous settings stay in memory and on disk, and the model stopped for the restart runs again as before"""
    global server
    _setup()
    _enable()
    server.start()
    _save("tinyB", {"ubatch-size": 64})
    stage1._load_model_and_wait("tinyP")
    stage1._load_model_and_wait("tinyA")
    before = _models_file()
    ini = work_dir / "models.ini"
    good = ini.read_bytes()
    ini.write_bytes(good + b"\n[broken\n")

    res = stage1._admin("POST", "/admin/models/settings", data={"model": "tinyP", "settings": {"ctx-size": 1024}, "restart": True},
                        timeout=90)
    _assert_error(res, 500, "server_error")
    err = res.body["error"]
    assert "failed to parse" in err["reload_error"] and err["restarted"] is True and "restart_error" not in err
    assert _models_file() == before
    stage1._wait_for_model_status("tinyP", "loaded")
    assert _flag("tinyP", "--ctx-size") == "512"  # its models.ini value, not the one of the failed save
    assert stage1._model_status("tinyA") == "loaded"  # never stopped
    ini.write_bytes(good)
    row = _row("tinyP")
    assert row["overrides"] == {} and row["phase"] is None


def test_models_settings_persist_across_restart():
    global server
    _setup()
    _enable()
    server.start()
    _save("tinyA", {"ctx-size": 2048})
    stage1._load_model_and_wait("tinyA")
    _wait_row("tinyA", lambda r: r["phase"] == "committed", what="phase committed")
    _save("tinyB", {"ubatch-size": 64})

    stage1._restart_server()
    # applied by the very first model list of the new process
    assert _flag("tinyA", "--ctx-size") == "2048"
    assert _flag("tinyB", "--ubatch-size") == "64"
    row = _row("tinyA")
    assert row["overrides"] == {"ctx-size": "2048"} and row["phase"] == "committed" and row["last_good"] == {"ctx-size": "2048"}
    assert row["reverted_at_startup"] is False
    stage1._load_model_and_wait("tinyA")
    assert _props_n_ctx("tinyA") == 2048

    doc = _models_file()
    assert doc["version"] == 1
    entry = doc["models"]["tinyA"]
    assert entry["settings"] == {"ctx-size": "2048"} and entry["last_good"] == {"ctx-size": "2048"}
    assert entry["phase"] == "committed" and entry["failures"] == 0 and entry["device_pci"] == {}
    assert isinstance(entry["updated"], str)
    assert doc["models"]["tinyB"]["phase"] == "pending" and doc["models"]["tinyB"]["last_good"] is None
    # atomic writes: the previous version is kept, no temporary file is left
    assert os.path.exists(os.path.join(state_dir, MODELS_FILE + ".bak"))
    assert not os.path.exists(os.path.join(state_dir, MODELS_FILE + ".tmp"))
    # the stage-1 state file is a different file: model saves never touch it
    assert not os.path.exists(stage1._state_file())
    stage1._post_settings({"models_max": 2})
    state_before = _file_bytes("admin-state.json")
    _save("tinyA", {"ctx-size": 1024}, restart=True)
    _save("tinyB", reset=True)
    assert _file_bytes("admin-state.json") == state_before


def test_models_settings_reset():
    global server
    _setup()
    _enable()
    server.start()
    _save("tinyA", {"ctx-size": 1024, "ubatch-size": 128})

    body = _save("tinyA", reset=True)
    assert body["result"]["changed"] is True
    assert body["previous"] == {"ctx-size": "1024", "ubatch-size": "128"}
    assert body["model"]["overrides"] == {} and body["model"]["phase"] is None
    assert _flag("tinyA", "--ctx-size") is None and _flag("tinyA", "--ubatch-size") is None
    assert "tinyA" not in _models_file()["models"]
    # nothing to reset
    assert _save("tinyA", reset=True)["result"]["changed"] is False
    assert _save("tinyB", reset=True)["result"]["changed"] is False


def test_models_reset_of_a_running_model_needs_restart():
    global server
    _setup()
    _enable()
    server.start()
    _save("tinyA", {"ctx-size": 1024})
    stage1._load_model_and_wait("tinyA")
    assert _flag("tinyA", "--ctx-size") == "1024"

    # like a change: never a silent restart, nothing removed
    _assert_error(stage1._admin("POST", "/admin/models/settings", data={"model": "tinyA", "reset": True}), 409, "restart_required")
    assert _row("tinyA")["overrides"] == {"ctx-size": "1024"} and "tinyA" in _models_file()["models"]
    assert stage1._model_status("tinyA") == "loaded"

    body = _save("tinyA", reset=True, restart=True)
    assert body["result"]["restarted"] is True and body["result"]["was_running"] is True
    assert body["model"]["overrides"] == {} and body["previous"] == {"ctx-size": "1024"}
    stage1._wait_for_model_status("tinyA", "loaded")
    assert _flag("tinyA", "--ctx-size") is None
    assert "tinyA" not in _models_file()["models"]


def test_models_save_during_reloads_keeps_the_override():
    """a reload that read the old settings while a save happened is redone with the new ones"""
    global server
    _setup()
    _enable()
    server.start()

    stop = threading.Event()

    def reload_loop():
        while not stop.is_set():
            try:
                server.make_request("GET", "/models?reload=1", timeout=30)
            except Exception:
                pass

    threads = [threading.Thread(target=reload_loop, daemon=True) for _ in range(2)]
    for t in threads:
        t.start()
    try:
        for value in (512, 768, 1024, 1280, 1536):
            _save("tinyA", {"ctx-size": value})
    finally:
        stop.set()
        for t in threads:
            t.join(30)
    assert _flag("tinyA", "--ctx-size") == "1536"
    stage1._models(reload=True)
    assert _flag("tinyA", "--ctx-size") == "1536"
    assert _row("tinyA")["overrides"] == {"ctx-size": "1536"}


#
# locks
#


def test_models_router_command_line_locks():
    global server
    _setup()
    _enable()
    server.start()
    _save("tinyA", {"ctx-size": 1024, "ubatch-size": 128})
    server.stop()

    server.n_ctx = 384  # --ctx-size on the router command line: it wins for every model
    server.start()
    row = _row("tinyA")
    f = row["fields"]["ctx-size"]
    assert f["locked"] == {"by": "router_cli", "value": "384"} and f["origin"] == "router_cli" and f["effective"] == "384"
    # the override saved before the lock is kept, but not active
    assert f["override"] == "1024" and f["active"] is False and f["inactive_reason"] == "router_cli"
    assert row["fields"]["ubatch-size"]["active"] is True
    assert row["apply_error"] == "" and "apply_error" not in row["warnings"]
    # locked before any save, for every model
    assert _row("tinyB")["fields"]["ctx-size"]["locked"] == {"by": "router_cli", "value": "384"}
    assert _flag("tinyA", "--ctx-size") == "384"

    res = stage1._admin("POST", "/admin/models/settings", data={"model": "tinyB", "settings": {"ctx-size": 1024}})
    _assert_error(res, 409, "field_locked", "ctx-size")
    assert "command line" in res.body["error"]["message"]
    # the fit check ignores a locked value and says so
    check = _check("tinyB", {"ctx-size": 2048, "device": "none"})
    assert "field_locked:ctx-size" in check["warnings"]
    assert check["ok"] is True and check["n_ctx"] == 384
    # null on a locked key removes the stale override
    _save("tinyA", {"ctx-size": None})
    assert _row("tinyA")["overrides"] == {"ubatch-size": "128"}


def test_models_model_devices_section_lock():
    global server
    # what the docker-dual entrypoint makes of MODEL_DEVICES: a device line in the section
    _setup(tiny_p="ctx-size = 512\ndevice = none")
    _enable(MODEL_DEVICES="tinyP=none,tinyA=CUDA1")
    server.start()

    row = _row("tinyP")
    assert row["fields"]["device"]["locked"] == {"by": "model_devices", "value": "none"}
    assert row["fields"]["device"]["origin"] == "model_devices"
    res = stage1._admin("POST", "/admin/models/settings", data={"model": "tinyP", "settings": {"device": "none"}})
    _assert_error(res, 409, "field_locked", "device")
    assert "MODEL_DEVICES" in res.body["error"]["message"]
    # null is allowed, the other keys stay editable
    _save("tinyP", {"device": None})
    _save("tinyP", {"ctx-size": 640})
    assert _flag("tinyP", "--ctx-size") == "640"
    # an entry that is not a models.ini section locks nothing and is reported
    assert _row("tinyA")["fields"]["device"]["locked"] is None
    assert "model_devices_unknown_section:tinyA" in _list()["warnings"]
    assert _diagnostics()["model_devices"]["value"] == "tinyP=none,tinyA=CUDA1"


@pytest.mark.parametrize("patched", [True, False])
def test_models_model_devices_with_crlf_models_ini(patched: bool):
    """a models.ini with Windows line endings: the lock is reported only when the device line is really there"""
    global server
    _setup(tiny_p="ctx-size = 512\ndevice = none" if patched else "ctx-size = 512", crlf=True)
    assert b"\r\n" in open(server.models_preset, "rb").read()
    _enable(MODEL_DEVICES="tinyP=none")
    server.start()

    row = _row("tinyP")
    if patched:
        assert row["fields"]["device"]["locked"] == {"by": "model_devices", "value": "none"}
        assert "model_devices_not_applied" not in row["warnings"]
        assert _flag("tinyP", "--device") == "none"
        assert _flag("tinyP", "--ctx-size") == "512"
    else:
        # the entrypoint did not apply it: nothing enforces it, so it is no lock
        assert row["fields"]["device"]["locked"] is None
        assert "model_devices_not_applied" in row["warnings"]
        _save("tinyP", {"device": "none"})
        assert _flag("tinyP", "--device") == "none"


def test_models_model_devices_star_locks_every_device():
    global server
    _setup(star="n-gpu-layers = 99\ndevice = none")
    _enable(MODEL_DEVICES="*=none")
    server.start()

    rows = _list()["models"]
    assert rows
    for row in rows:
        assert row["fields"]["device"]["locked"] == {"by": "model_devices", "value": "none"}, row["id"]
    assert stage1._settings()["default_device_locked"] is True
    _assert_error(stage1._admin("POST", "/admin/models/settings", data={"model": "tinyA", "settings": {"device": "none"}}),
                  409, "field_locked", "device")
    _save("tinyA", {"ctx-size": 1024})
    assert _flag("tinyA", "--ctx-size") == "1024"


#
# validation
#


INVALID_SETTINGS = [
    ({"foo": 1}, "foo"),
    ({"cache-type": "q8_0"}, "cache-type"),
    ({"ctx-size": 0}, "ctx-size"),
    ({"ctx-size": -1}, "ctx-size"),
    ({"ctx-size": "abc"}, "ctx-size"),
    ({"ctx-size": 2 ** 31}, "ctx-size"),
    ({"ctx-size": 255}, "ctx-size"),
    ({"ctx-size": 1048577}, "ctx-size"),
    ({"ctx-size": 1.5}, "ctx-size"),
    ({"ctx-size": True}, "ctx-size"),
    ({"ctx-size": [1024]}, "ctx-size"),
    ({"ctx-size": "1" * 200}, "ctx-size"),
    ({"kv-unified-per-slot": 100}, "kv-unified-per-slot"),
    ({"parallel": 0}, "parallel"),
    ({"parallel": 17}, "parallel"),
    ({"parallel": -2}, "parallel"),
    ({"parallel": "x"}, "parallel"),
    ({"n-gpu-layers": "x"}, "n-gpu-layers"),
    ({"n-gpu-layers": 1000}, "n-gpu-layers"),
    ({"n-gpu-layers": "1e3"}, "n-gpu-layers"),
    ({"flash-attn": "maybe"}, "flash-attn"),
    ({"pooling": "foo"}, "pooling"),
    ({"embeddings": "maybe"}, "embeddings"),
    ({"reranking": 2}, "reranking"),
    ({"ubatch-size": 16}, "ubatch-size"),
    ({"ubatch-size": 8193}, "ubatch-size"),
    ({"device": "CUDA42"}, "device"),  # not a device of this server
    ({"device": ""}, "device"),
    ({"device": "a b"}, "device"),
    ({"device": 0}, "device"),
    ({"device": "none,none"}, "device"),
    ({"cache-type-k": "q5_1", "cache-type-v": "q5_1"}, "cache-type-k"),
    # cross-field rules
    ({"cache-type-k": "q8_0", "cache-type-v": "q8_0", "flash-attn": "off"}, "flash-attn"),
    ({"cache-type-k": "q8_0"}, "cache-type-k"),
    ({"cache-type-v": "q4_0"}, "cache-type-v"),
    ({"cache-type-k": "q8_0", "cache-type-v": "q4_0"}, "cache-type-v"),
]

INVALID_BODIES = [
    ({"model": "tinyA", "settings": "not an object"}, 400, "invalid_request_error", "settings"),
    ({"model": "tinyA", "settings": [1, 2]}, 400, "invalid_request_error", "settings"),
    ({"model": "tinyA"}, 400, "invalid_request_error", "settings"),
    ({"model": "", "settings": {"ctx-size": 1024}}, 400, "invalid_request_error", "model"),
    ({"model": 5, "settings": {"ctx-size": 1024}}, 400, "invalid_request_error", "model"),
    ({"settings": {"ctx-size": 1024}}, 400, "invalid_request_error", "model"),
    ({"model": "tinyA", "settings": {"ctx-size": 1024}, "restart": "yes"}, 400, "invalid_request_error", None),
    ({"model": "tinyA", "reset": 1}, 400, "invalid_request_error", None),
    ({"model": "no-such-model", "settings": {"ctx-size": 1024}}, 404, "not_found_error", None),
    ({"model": "no-such-model", "reset": True}, 404, "not_found_error", None),
    ({"model": "tiny\u0001A", "settings": {"ctx-size": 1024}}, 404, "not_found_error", None),
]


def test_models_settings_validation():
    """every invalid request is refused with the field it is about, and nothing is saved or applied"""
    global server
    _setup()
    _enable()
    server.start()
    args_before = _args("tinyA")

    problems = []
    for settings, param in INVALID_SETTINGS:
        res = stage1._admin("POST", "/admin/models/settings", data={"model": "tinyA", "settings": settings})
        error = res.body.get("error", {}) if isinstance(res.body, dict) else {}
        if res.status_code != 400 or error.get("type") != "invalid_request_error" or error.get("param") != param or not error.get("message"):
            problems.append(f"{settings}: {res.status_code} {res.body}")
        # the fit check validates the same way
        res = stage1._admin("POST", "/admin/models/settings/check", data={"model": "tinyA", "settings": settings})
        error = res.body.get("error", {}) if isinstance(res.body, dict) else {}
        if res.status_code != 400 or error.get("param") != param:
            problems.append(f"check {settings}: {res.status_code} {res.body}")
    for body, status, error_type, param in INVALID_BODIES:
        res = stage1._admin("POST", "/admin/models/settings", data=body)
        error = res.body.get("error", {}) if isinstance(res.body, dict) else {}
        if res.status_code != status or error.get("type") != error_type or (param is not None and error.get("param") != param):
            problems.append(f"{body}: {res.status_code} {res.body}")
    for raw in ('{"model": "tinyA", "settings": ', "[1, 2]", '"tinyA"', ""):
        res = stage1._post_raw("/admin/models/settings", raw)
        if res.status_code != 400 or res.body["error"]["type"] != "invalid_request_error":
            problems.append(f"raw {raw!r}: {res.status_code} {res.body}")
    for body, param in (({"settings": {}}, "model"), ({"model": "no-such-model"}, None)):
        res = stage1._admin("POST", "/admin/models/settings/check", data=body)
        if res.status_code != (404 if param is None else 400):
            problems.append(f"check {body}: {res.status_code} {res.body}")
    assert problems == []

    assert not os.path.exists(os.path.join(state_dir, MODELS_FILE))
    assert _args("tinyA") == args_before
    assert _row("tinyA")["overrides"] == {}


def test_models_settings_warnings_and_context():
    global server
    _setup()
    _enable()
    server.start()
    assert _row("tinyA")["facts_status"] == "ready"  # ?model= reads the facts: n_ctx_train is known from now on

    assert "pooling_without_embeddings" in _save("tinyA", {"pooling": "mean"})["warnings"]
    assert "pooling_without_embeddings" not in _save("tinyA", {"embeddings": True})["warnings"]
    _save("tinyA", {"pooling": None, "embeddings": None})
    assert "ctx_above_train" in _save("tinyA", {"ctx-size": 65536, "parallel": 2})["warnings"]  # above 2048 x 2 slots
    assert "pool_overcommit" in _save("tinyA", {"ctx-size": 1024, "parallel": 8})["warnings"]  # 128 tokens per busy slot
    assert "kv_per_slot_no_effect" in _save("tinyA", {"ctx-size": 1024, "parallel": 2, "kv-unified-per-slot": 4096})["warnings"]
    assert "ubatch_above_ctx" in _save("tinyA", {"ctx-size": 256, "parallel": None, "kv-unified-per-slot": None,
                                                 "ubatch-size": 512})["warnings"]

    # "per request x slots": 4 slots that can each use their 2048 tokens at the same time
    body = _save("tinyA", {"ctx-size": 8192, "parallel": 4, "kv-unified-per-slot": 2048, "ubatch-size": None})
    assert body["warnings"] == []
    assert body["model"]["context"] == {"slots": 4, "unified": True, "pool": 8192, "per_slot_cap": 2048, "n_ctx_train": 2048,
                                        "max_per_request": 2048, "per_request_all_busy": 2048}
    assert _flag("tinyA", "--kv-unified-per-slot") == "2048" and "--kv-unified" in _args("tinyA")
    # fewer tokens per slot than the model was trained for: the pool is overcommitted
    body = _save("tinyA", {"ctx-size": 4096, "kv-unified-per-slot": 1024})
    assert body["warnings"] == ["pool_overcommit"]
    assert body["model"]["context"]["max_per_request"] == 1024 and body["model"]["context"]["per_request_all_busy"] == 1024
    # "shared pool" with auto slots: one request may use the whole pool (capped by the training context)
    body = _save("tinyA", {"ctx-size": 3000, "parallel": None, "kv-unified-per-slot": None})
    assert body["model"]["context"] == {"slots": 4, "unified": True, "pool": 3072, "per_slot_cap": None, "n_ctx_train": 2048,
                                        "max_per_request": 2048, "per_request_all_busy": 768}
    body = _save("tinyA", {"ctx-size": None})
    assert body["model"]["context"]["pool"] is None and body["model"]["context"]["max_per_request"] is None


def test_models_settings_entry_limit():
    global server
    _setup()
    _write_models_file({"version": 1, "models": {f"orphan-{i:03d}": {"settings": {"ctx-size": "1024"}, "phase": "committed"}
                                                 for i in range(256)}})
    _enable()
    server.start()

    body = _list()
    assert len(body["unattached"]) == 256 and {u["kind"] for u in body["unattached"]} == {"orphaned"}
    res = stage1._admin("POST", "/admin/models/settings", data={"model": "tinyA", "settings": {"ctx-size": 1024}})
    _assert_error(res, 400, "invalid_request_error", "model")
    assert _save("orphan-000", reset=True)["result"]["changed"] is True
    _save("tinyA", {"ctx-size": 1024})
    assert len(_models_file()["models"]) == 256


#
# the settings file
#


def test_models_file_corrupt_uses_backup():
    global server
    _setup()
    _enable()
    server.start()
    _save("tinyA", {"ubatch-size": 64})
    _save("tinyB", {"ctx-size": 768})
    server.stop()

    # the previous version is good, the current one is broken: the router starts with the previous one, read-only
    shutil.copyfile(os.path.join(state_dir, MODELS_FILE), os.path.join(state_dir, MODELS_FILE + ".bak"))
    _write_models_file("{ this is not json")
    broken = _file_bytes(MODELS_FILE)
    server.start()

    ms = stage1._admin_status()["state"]["model_settings"]
    assert ms["read_only"] is True and ms["backup_loaded"] is True and "invalid JSON" in ms["error"]
    assert _list()["state"]["read_only"] is True
    assert _flag("tinyA", "--ubatch-size") == "64" and _flag("tinyB", "--ctx-size") == "768"
    for body in ({"model": "tinyA", "settings": {"ctx-size": 1024}}, {"model": "tinyA", "reset": True}):
        _assert_error(stage1._admin("POST", "/admin/models/settings", data=body), 409, "model_settings_read_only")
    diag = _diagnostics()["model_settings"]
    assert diag["status"] == "error" and diag["value"]["read_only"] is True and diag["value"]["backup_loaded"] is True
    server.stop()
    assert _file_bytes(MODELS_FILE) == broken


def test_models_file_corrupt_without_backup():
    global server
    _setup()
    _write_models_file('{"version": 1, "models": {"tinyA": {"settings": {"ctx-size": "1024"}}}')  # cut short
    broken = _file_bytes(MODELS_FILE)
    _enable()
    server.start()

    ms = stage1._admin_status()["state"]["model_settings"]
    assert ms["read_only"] is True and ms["backup_loaded"] is False and ms["error"] != ""
    assert _flag("tinyA", "--ctx-size") is None
    _assert_error(stage1._admin("POST", "/admin/models/settings", data={"model": "tinyB", "settings": {"ctx-size": 1024}}),
                  409, "model_settings_read_only")
    server.stop()
    assert _file_bytes(MODELS_FILE) == broken
    assert sorted(os.listdir(state_dir)) == [MODELS_FILE]


def test_models_file_unknown_version():
    """a file of a newer version is never applied and never overwritten (not even from its .bak)"""
    global server
    _setup()
    _write_models_file({"version": 2, "models": {"tinyA": {"settings": {"ctx-size": "999"}}}})
    shutil.copyfile(os.path.join(state_dir, MODELS_FILE), os.path.join(state_dir, MODELS_FILE + ".bak"))
    newer = _file_bytes(MODELS_FILE)
    _enable()
    server.start()

    ms = stage1._admin_status()["state"]["model_settings"]
    assert ms["read_only"] is True and ms["backup_loaded"] is False and "version" in ms["error"]
    assert _row("tinyA")["overrides"] == {} and _flag("tinyA", "--ctx-size") is None
    _assert_error(stage1._admin("POST", "/admin/models/settings", data={"model": "tinyA", "settings": {"ctx-size": 1024}}),
                  409, "model_settings_read_only")
    server.stop()
    assert _file_bytes(MODELS_FILE) == newer


def test_models_file_unknown_and_unusable_content_is_kept():
    global server
    _setup()
    _write_models_file({
        "version": 1,
        "future_top_level": {"keep": True},
        "models": {
            "tinyA": {"settings": {"ctx-size": "abc", "ubatch-size": "128", "future-key": "v"}, "phase": "committed",
                      "extra_field": "x"},
            "tinyB": "not an object",
        },
    })
    _enable()
    server.start()

    # a hand-edited value that is not valid never reaches a child; the router starts anyway
    row = _row("tinyA")
    assert row["overrides"] == {"ubatch-size": "128"}
    assert row["kept"] == {"ctx-size": "abc", "future-key": "v"}
    assert _flag("tinyA", "--ubatch-size") == "128" and _flag("tinyA", "--ctx-size") is None
    body = _list()
    assert len(body["state"]["problems"]) >= 3
    unreadable = [u for u in body["unattached"] if u["kind"] == "unreadable"]
    assert unreadable == [{"id": "tinyB", "kind": "unreadable", "overrides": {}, "raw": "not an object", "exists": False}]
    # the model tinyB itself is fine and has no settings
    assert _row("tinyB")["overrides"] == {}

    # rewriting the file keeps everything this server does not understand
    _save("tinyP", {"ctx-size": 640})
    doc = _models_file()
    assert doc["future_top_level"] == {"keep": True}
    assert doc["models"]["tinyB"] == "not an object"
    assert doc["models"]["tinyA"]["extra_field"] == "x"
    assert doc["models"]["tinyA"]["settings"] == {"ctx-size": "abc", "ubatch-size": "128", "future-key": "v"}
    # a new value for a kept key replaces it
    body = _save("tinyA", {"ctx-size": 1024})
    assert body["model"]["kept"] == {"future-key": "v"}
    assert _models_file()["models"]["tinyA"]["settings"] == {"ctx-size": "1024", "ubatch-size": "128", "future-key": "v"}
    assert _flag("tinyA", "--ctx-size") == "1024"


#
# two-phase commit, automatic revert
#


def test_models_failed_settings_are_reverted():
    """pending -> trial -> failed; the second failed start in a row reverts to the last good settings"""
    global server
    _setup()
    server.log_path = str(work_dir / "server.log")
    _enable(PROVISION_ADMIN_INVENTORY="file:" + _inventory(_fake_gpus()), PROVISION_ADMIN_GPU_PROBE="off")
    server.start()

    _save("tinyA", {"ctx-size": 1024})
    stage1._load_model_and_wait("tinyA")
    _wait_row("tinyA", lambda r: r["phase"] == "committed" and r["last_good"] == {"ctx-size": "1024"}, what="committed")

    # a device the inventory lists, but this build cannot open: the child fails before LOADED
    body = _save("tinyA", {"device": "CUDA6"}, restart=True)
    assert body["result"]["restarted"] is True
    assert body["model"]["phase"] in ("trial", "failed")
    assert _flag("tinyA", "--device") == "CUDA6"
    row = _wait_row("tinyA", lambda r: r["phase"] == "failed", what="phase failed")
    assert row["failures"] == 1 and row["reverted"] is None
    failure = row["last_failure"]
    assert failure["reason"]["code"] == "invalid_device" and failure["before_loaded"] is True
    assert failure["failures"] == 1 and failure["settings_reverted"] is False and failure["exit_code"] != 0
    assert any("CUDA6" in line for line in failure["lines"])
    assert row["last_run"]["reason"]["code"] == "invalid_device"

    # a client request starts it again with the same settings (autoload): the second failure reverts them
    stage1._tokenize("tinyA", timeout=120)
    row = _wait_row("tinyA", lambda r: r["reverted"] is not None, what="reverted")
    assert row["reverted"]["reason"] == "invalid_device"
    assert row["reverted"]["settings"] == {"ctx-size": "1024", "device": "CUDA6"}
    assert row["reverted"]["failures"] == 2
    assert row["overrides"] == {"ctx-size": "1024"} and row["phase"] == "committed" and row["failures"] == 0
    assert row["last_failure"]["settings_reverted"] is True and row["last_failure"]["failures"] == 2
    stage1._wait_until(lambda: _flag("tinyA", "--device") is None, 30, "the presets without the device")
    assert _flag("tinyA", "--ctx-size") == "1024"
    assert _models_file()["models"]["tinyA"]["reverted"]["reason"] == "invalid_device"

    # the next start works; the last failure stays visible
    stage1._load_model_and_wait("tinyA")
    log = _log("tinyA")
    assert log["instance"]["running"] is True and log["reason"] is None
    assert log["last_failure"]["reason"]["code"] == "invalid_device"
    server.stop()
    assert "reverted to its last good settings" in stage1._read_text(server.log_path)


def test_models_failed_settings_without_last_good_are_removed():
    global server
    _setup()
    server.log_path = str(work_dir / "server.log")
    _enable(PROVISION_ADMIN_INVENTORY="file:" + _inventory(_fake_gpus()), PROVISION_ADMIN_GPU_PROBE="off")
    server.start()

    body = _save("tinyB", {"device": "CUDA7", "ubatch-size": 64})
    assert body["model"]["phase"] == "pending" and body["model"]["last_good"] is None
    _load_async("tinyB")
    _wait_row("tinyB", lambda r: r["phase"] == "failed" and r["failures"] == 1, what="first failure")
    _load_async("tinyB")
    row = _wait_row("tinyB", lambda r: r["reverted"] is not None, what="reverted")
    assert row["overrides"] == {} and row["reverted"]["settings"] == {"device": "CUDA7", "ubatch-size": "64"}
    stage1._wait_until(lambda: _flag("tinyB", "--device") is None, 30, "the presets without the device")
    assert _flag("tinyB", "--ubatch-size") is None
    server.stop()
    assert "reverted to its models.ini values" in stage1._read_text(server.log_path)


def test_models_startup_reverts_settings_that_were_being_tried():
    global server
    _setup()
    _write_models_file({"version": 1, "models": {
        "tinyB": {"settings": {"ctx-size": "777"}, "last_good": {"ctx-size": "512"}, "phase": "trial", "failures": 0},
        "tinyP": {"settings": {"ctx-size": "640"}, "last_good": None, "phase": "failed", "failures": 1},
        "tinyA": {"settings": {"ctx-size": "1024"}, "last_good": None, "phase": "pending", "failures": 0},
        MODEL_B: {"settings": {"ctx-size": "1280"}, "last_good": {"ctx-size": "1280"}, "phase": "committed", "failures": 0},
    }})
    _enable()
    server.start()

    row = _row("tinyB")
    assert row["reverted_at_startup"] is True and row["overrides"] == {"ctx-size": "512"} and row["phase"] == "committed"
    assert row["reverted"]["reason"] == "startup" and row["reverted"]["settings"] == {"ctx-size": "777"}
    assert _flag("tinyB", "--ctx-size") == "512"
    row = _row("tinyP")
    assert row["reverted_at_startup"] is True and row["overrides"] == {}
    assert _flag("tinyP", "--ctx-size") == "512"  # back to models.ini
    # pending and committed apply as saved
    assert _row("tinyA")["reverted_at_startup"] is False and _flag("tinyA", "--ctx-size") == "1024"
    assert _row(MODEL_B)["reverted_at_startup"] is False and _flag(MODEL_B, "--ctx-size") == "1280"
    # the revert is written to the file
    doc = _models_file()["models"]
    assert doc["tinyB"]["settings"] == {"ctx-size": "512"} and doc["tinyB"]["phase"] == "committed"
    assert doc["tinyB"]["reverted"]["reason"] == "startup"

    # reported until the next restart; the record of what was reverted stays
    stage1._restart_server()
    row = _row("tinyB")
    assert row["reverted_at_startup"] is False and row["reverted"]["reason"] == "startup"
    assert row["overrides"] == {"ctx-size": "512"} and _flag("tinyB", "--ctx-size") == "512"


#
# entries of models that are not in the list
#


def test_models_orphaned_entry():
    global server
    _setup(models_dir=("tinyA", "tinyX"))
    _enable()
    server.start()
    _save("tinyX", {"ctx-size": 384})

    path = os.path.join(server.models_dir, "tinyX.gguf")
    deadline = time.time() + 10
    while True:
        try:
            os.remove(path)
            break
        except PermissionError:  # Windows: the facts reader may hold the file open for a moment
            if time.time() > deadline:
                raise
            time.sleep(0.1)
    stage1._models(reload=True)

    body = _list()
    assert "tinyX" not in {r["id"] for r in body["models"]}
    assert [u["id"] for u in body["unattached"]] == ["tinyX"]
    entry = body["unattached"][0]
    assert entry["kind"] == "orphaned" and entry["exists"] is False and entry["overrides"] == {"ctx-size": "384"}
    one = _row("tinyX")
    assert one["exists"] is False and one["kind"] == "orphaned" and "state" in one
    # values for a model that is not there: 404; the Delete dialog removes the entry with a reset
    _assert_error(stage1._admin("POST", "/admin/models/settings", data={"model": "tinyX", "settings": {"ctx-size": 512}}),
                  404, "not_found_error")
    body = _save("tinyX", reset=True)
    assert body["result"]["changed"] is True and body["model"] == {"id": "tinyX", "exists": False, "overrides": {}}
    assert body["previous"] == {"ctx-size": "384"}
    assert _list()["unattached"] == []
    stage1._assert_not_found(stage1._admin("GET", "/admin/models/settings?model=tinyX"))

    # the model comes back: no settings come with it
    shutil.copyfile(stage1._tiny_gguf(), path)
    stage1._models(reload=True)
    assert _flag("tinyX", "--ctx-size") is None


#
# facts of a model file
#


def test_models_facts_of_the_tiny_model():
    global server
    _setup()
    _enable()
    server.start()

    path = os.path.join(server.models_dir, "tinyA.gguf")
    g = _gguf_info(path)
    kv = g["kv"]
    arch = kv["general.architecture"]
    n_layer = kv[f"{arch}.block_count"]
    n_head = kv[f"{arch}.attention.head_count"]
    n_head_kv = kv[f"{arch}.attention.head_count_kv"]
    head_dim = kv[f"{arch}.embedding_length"] // n_head
    sizes = {t["name"]: _gguf_tensor_bytes(t) for t in g["tensors"]}
    layer_bytes = [sum(b for name, b in sizes.items() if name.startswith(f"blk.{i}.")) for i in range(n_layer)]

    row = _row("tinyA")
    assert row["facts_status"] == "ready"
    f = row["facts"]
    assert f["error"] == ""
    assert f["arch"] == arch == "llama"
    assert f["n_layer"] == n_layer == 5
    assert f["n_ctx_train"] == kv[f"{arch}.context_length"] == 2048
    assert f["n_embd"] == kv[f"{arch}.embedding_length"]
    assert f["n_head"] == n_head and f["n_head_kv"] == n_head_kv == 4
    assert f["d_k"] == head_dim and f["d_v"] == head_dim
    assert f["n_vocab"] == len(kv["tokenizer.ggml.tokens"])
    # bytes of the KV cache per token: K and V rows of every layer; a quantized type only for a head size that is a
    # multiple of its block size (32), else llama.cpp refuses it (the tiny model has 8)
    quant_ok = head_dim % 32 == 0
    assert f["kv_bytes_per_token"] == {
        "f16": n_layer * 2 * n_head_kv * head_dim * 2,
        "bf16": n_layer * 2 * n_head_kv * head_dim * 2,
        "q8_0": n_layer * 2 * n_head_kv * head_dim // 32 * 34 if quant_ok else None,
        "q4_0": n_layer * 2 * n_head_kv * head_dim // 32 * 18 if quant_ok else None,
    }
    assert f["kv_bytes_per_token"]["f16"] == 640
    assert f["kv_estimate"] == "exact" and f["sliding_window"] == 0 and f["causal"] is True and f["pooling"] is None
    # tensor bytes per layer, the output layer (with its norm) and the input embeddings add up to the total
    assert f["layer_bytes"] == layer_bytes
    assert f["output_bytes"] == sizes["output.weight"] + sizes["output_norm.weight"]
    assert f["token_embd_bytes"] == sizes["token_embd.weight"]
    assert f["tied_embeddings"] is False
    assert f["tensor_bytes"] == sum(sizes.values())
    assert sum(f["layer_bytes"]) + f["output_bytes"] + f["token_embd_bytes"] + f["other_bytes"] == f["tensor_bytes"]
    assert f["files"] == [{"name": "tinyA.gguf", "bytes": os.path.getsize(path)}] and f["file_bytes"] == os.path.getsize(path)
    assert f["quant"] == "F32" and f["mmproj_bytes"] == 0
    assert row["context"]["n_ctx_train"] == 2048


def test_models_facts_no_quantized_kv_size_for_an_unsupported_head_size():
    global server
    _setup()
    _enable()
    server.start()

    f = _row("tinyA")["facts"]
    assert f["d_k"] == 8 and f["kv_bytes_per_token"]["f16"] == 640
    c = _check("tinyA", {"device": "none", "cache-type-k": "q8_0", "cache-type-v": "q8_0"})
    assert c["ok"] is False and "does not divide n_embd_head_k=8" in c["error"]["line"]
    # what llama.cpp cannot allocate has no size, and the note says why
    assert f["kv_bytes_per_token"]["q8_0"] is None and f["kv_bytes_per_token"]["q4_0"] is None
    assert "not possible for this model" in f["kv_note"] and f["kv_estimate"] == "exact"


def test_models_facts_tied_embeddings():
    """a model without output.weight: llama.cpp uses a copy of token_embd as the output layer"""
    global server
    _setup()
    tied = _gguf_copy(stage1._tiny_gguf(), work_dir / "tinyT.gguf", drop_tensors=("output.weight",))
    _write_ini(_ini_text(stage1._preset_section("tinyT", (work_dir / "tinyT.gguf").as_posix())))
    _enable()
    server.start()

    untied = _row("tinyA")["facts"]
    f = _row("tinyT")["facts"]
    assert f["error"] == ""
    assert f["tied_embeddings"] is True
    assert f["output_bytes"] == f["token_embd_bytes"] + (untied["output_bytes"] - untied["token_embd_bytes"])  # + output_norm
    assert f["tensor_bytes"] == sum(_gguf_tensor_bytes(t) for t in _gguf_info(tied)["tensors"])
    assert f["tensor_bytes"] < untied["tensor_bytes"]
    assert f["files"][0]["bytes"] == os.path.getsize(tied)
    # the model really works like that, and the dry run agrees
    check = _check("tinyT", {"device": "none", "ctx-size": 512})
    assert check["ok"] is True and check["n_ctx"] == 512
    stage1._load_model_and_wait("tinyT")
    assert stage1._tokenize("tinyT").status_code == 200


def test_models_facts_of_damaged_model_files():
    """metadata no real model has: the router reads the facts of every model it lists (in the background too), so such
    a file gives facts with an error, or without the bad value, never an abort of the router or a read without end"""
    global server
    tiny = stage1._tiny_gguf()
    arch = _gguf_info(tiny)["kv"]["general.architecture"]
    files = {
        # an array of strings where llama.cpp has a number (gguf_get_arr_data() aborts on a string array)
        "tinyX": _gguf_copy(tiny, work_dir / "tinyX.gguf", values={f"{arch}.attention.head_count": _gguf_value_str_array(["8"] * 5)}),
        # a layer count no model has (a loop to it would not end)
        "tinyY": _gguf_copy(tiny, work_dir / "tinyY.gguf", values={f"{arch}.block_count": _gguf_value_u32(0xFFFFFFFF)}),
        # a split count that does not match the file name (a list of that many parts would not fit in memory)
        "tinyZ": _gguf_copy(tiny, work_dir / "tinyZ-00001-of-00002.gguf", values={"split.count": _gguf_value_u64(1 << 40)}),
    }
    _setup("".join(stage1._preset_section(name, path.replace("\\", "/")) for name, path in files.items()))
    _enable()
    server.start()

    _list()  # queues a background read of every listed model
    t0 = time.time()
    x = _row("tinyX")
    assert x["facts_status"] == "ready" and x["facts"]["n_head"] == 0 and x["facts"]["n_layer"] == 5
    y = _row("tinyY")
    assert y["facts_status"] == "error" and "block_count = 4294967295" in y["facts"]["error"]
    z = _row("tinyZ")
    assert z["facts_status"] == "error" and "does not match the file name" in z["facts"]["error"]
    assert time.time() - t0 < 10
    # the router and its background reader go on
    stage1._wait_until(lambda: _list_row("tinyA")["facts_status"] == "ready", 30, "facts of a good model after the damaged ones")
    assert server.make_request("GET", "/health").status_code == 200


def test_models_facts_are_warmed_in_the_background():
    """facts of the running, pinned and load-on-startup models are read without anyone asking; the list queues the rest"""
    global server
    _setup(tiny_p="ctx-size = 512\nload-on-startup = true")
    server.log_path = str(work_dir / "server.log")
    _enable(PROVISION_ADMIN_INVENTORY="file:" + _inventory(_two_gpus()), PROVISION_ADMIN_GPU_PROBE="off",
            PROVISION_ADMIN_PINNED="tinyB")

    def background_reads() -> str:
        lines = stage1._read_text(server.log_path).splitlines() if os.path.exists(server.log_path) else []
        return "\n".join(line for line in lines if "(background read)" in line)

    def facts_status(model: str) -> str:
        # the GPU cards queue the running, pinned and load-on-startup models too, never the others
        return _card_model(_card(_gpus(), "CUDA0"), model)["facts_status"]

    server.start()
    # before any admin request: only the warm-up at registration can have read them
    stage1._wait_until(lambda: re.search(r"tinyP\.gguf: ready \(background read\)", background_reads()) is not None and
                       re.search(r"tinyB\.gguf: ready \(background read\)", background_reads()) is not None, 30,
                       "the background read of the load-on-startup and the pinned model at registration")
    assert "tinyA.gguf" not in background_reads()
    assert facts_status("tinyP") == "ready" and facts_status("tinyB") == "ready"
    assert facts_status("tinyA") == "pending"
    assert _list_row("tinyA")["facts_status"] == "pending"  # queued now
    stage1._wait_until(lambda: facts_status("tinyA") == "ready", 30, "facts queued by the list")
    assert _list_row("tinyA")["facts"]["n_layer"] == 5


#
# fit check: a dry run by llama.cpp itself in a helper process
#


def test_models_fit_check_dry_run():
    global server
    _setup()
    _enable()
    server.start()
    facts = _row("tinyA")["facts"]

    # device none: the same result on any build (the model on the CPU only)
    c = _check("tinyA", {"ctx-size": 1024, "device": "none"})
    assert c["ok"] is True and c["error"] is None and c["model"] == "tinyA"
    assert c["cached"] is False and isinstance(c["duration_ms"], int) and isinstance(c["checked_at"], str)
    assert c["verdict"] == "cpu_only" and c["devices"] == []
    assert c["margin"] == GiB
    assert c["n_ctx"] == 1024 and c["n_ctx_train"] == 2048 and c["n_layer"] == 5
    assert c["fit"] in ("success", "failure", "error", "off")
    assert c["warnings"] == []
    assert _flag(c["args"], "--ctx-size") == "1024" and _flag(c["args"], "--device") == "none"
    assert "--port" not in c["args"] and "--host" not in c["args"]
    host = c["host"]
    # what llama.cpp reserves without loading: all weights, the KV cache of exactly this context, the compute buffers
    assert facts["tensor_bytes"] <= host["model"] <= facts["file_bytes"]
    assert host["context"] == facts["kv_bytes_per_token"]["f16"] * 1024
    assert host["compute"] > 0
    assert c["host_bytes"] == host["model"] + host["context"] + host["compute"]

    # the same model, arguments and inventory: from the cache
    c2 = _check("tinyA", {"ctx-size": 1024, "device": "none"})
    assert c2["cached"] is True and c2["host"] == host
    # another draft runs again
    c3 = _check("tinyA", {"ctx-size": 2048, "device": "none"})
    assert c3["ok"] is True and c3["cached"] is False and c3["n_ctx"] == 2048
    assert c3["host"]["context"] == facts["kv_bytes_per_token"]["f16"] * 2048
    # what only llama.cpp knows: a quantized KV cache needs whole q8_0 blocks per head (this model has 8 values per head)
    c3 = _check("tinyA", {"ctx-size": 2048, "device": "none", "cache-type-k": "q8_0", "cache-type-v": "q8_0", "flash-attn": "on"})
    assert c3["ok"] is False and c3["error"]["code"] == "invalid_settings"
    assert "does not divide" in c3["error"]["line"]

    # the draft is merged over the stored settings; null removes a stored one; nothing is saved by a check
    _save("tinyB", {"ubatch-size": 64})
    c4 = _check("tinyB", {"ctx-size": 512, "device": "none"})
    assert _flag(c4["args"], "--ubatch-size") == "64" and _flag(c4["args"], "--ctx-size") == "512"
    c5 = _check("tinyB", {"ubatch-size": None, "device": "none"})
    assert "--ubatch-size" not in c5["args"]
    assert _row("tinyB")["overrides"] == {"ubatch-size": "64"}
    assert _row("tinyA")["overrides"] == {}
    assert _flag("tinyB", "--ctx-size") is None

    # errors of the request itself
    _assert_error(stage1._admin("POST", "/admin/models/settings/check", data={"model": "no-such-model"}), 404, "not_found_error")
    _assert_error(stage1._admin("POST", "/admin/models/settings/check", data={"model": "tinyA", "settings": "x"}),
                  400, "invalid_request_error", "settings")


def test_models_fit_check_mmproj_is_not_included():
    global server
    found = glob.glob(os.path.join(SHARED_CACHE_DIR, "models--ggml-org--tinygemma3-GGUF", "snapshots", "*", "mmproj*.gguf"))
    if not found:
        pytest.skip("the tinygemma3 projector is not in the cache")  # ty: ignore[too-many-positional-arguments]
    mmproj = work_dir / "mmproj-tiny.gguf"
    shutil.copyfile(found[0], mmproj)
    _setup(_section("tinyM", f"mmproj = {mmproj.as_posix()}"))
    _enable()
    server.start()

    c = _check("tinyM", {"device": "none", "ctx-size": 512})
    assert c["ok"] is True and "mmproj_not_included" in c["warnings"]
    assert _row("tinyM")["facts"]["mmproj_bytes"] == os.path.getsize(mmproj)


def test_models_fit_check_rejected_settings_from_the_dry_run():
    """a combination llama.cpp refuses (quantized V cache without flash attention, from models.ini) fails the dry run"""
    global server
    _setup(_section("tinyQ", "cache-type-k = q8_0\ncache-type-v = q8_0\nflash-attn = off"))
    _enable()
    server.start()

    for _ in range(2):  # a failed dry run is not cached
        c = _check("tinyQ", {"device": "none"})
        assert c["ok"] is False and c["cached"] is False
        assert c["error"]["code"] == "invalid_settings"
        assert "quantized V cache" in c["error"]["line"]
        assert c["error"]["message"] != ""
        assert c["verdict"] == "unknown" and c["host"] is None and c["devices"] == []
        assert _flag(c["args"], "--flash-attn") == "off"
    # touching one of those keys is checked before any dry run
    _assert_error(stage1._admin("POST", "/admin/models/settings/check", data={"model": "tinyQ", "settings": {"flash-attn": "off"}}),
                  400, "invalid_request_error", "flash-attn")
    # flash attention on gets past that rule, but this model cannot have a q8_0 KV cache at all (8 values per head)
    c = _check("tinyQ", {"device": "none", "flash-attn": "on"})
    assert c["ok"] is False and c["error"]["code"] == "invalid_settings" and "does not divide" in c["error"]["line"]
    # an f16 cache works without flash attention
    c = _check("tinyQ", {"device": "none", "cache-type-k": "f16", "cache-type-v": "f16", "ctx-size": 1024})
    assert c["ok"] is True and c["host"]["context"] == _row("tinyQ")["facts"]["kv_bytes_per_token"]["f16"] * 1024


def test_models_fit_check_quantized_kv_cache():
    """q8_0 needs about half the bytes of f16 per token: the dry run of a model that supports it agrees with the facts"""
    global server
    _setup()
    _enable()
    server.start()
    if MODEL_A not in stage1._models():
        pytest.skip(f"{MODEL_A} is not in the cache")  # ty: ignore[too-many-positional-arguments]
    facts = _row(MODEL_A)["facts"]
    assert facts["d_k"] % 32 == 0 and facts["d_v"] % 32 == 0

    f16 = _check(MODEL_A, {"device": "none", "ctx-size": 1024})
    q8 = _check(MODEL_A, {"device": "none", "ctx-size": 1024, "cache-type-k": "q8_0", "cache-type-v": "q8_0", "flash-attn": "on"})
    assert f16["ok"] is True and q8["ok"] is True, (f16["error"], q8["error"])
    assert f16["host"]["context"] == facts["kv_bytes_per_token"]["f16"] * 1024
    assert q8["host"]["context"] == facts["kv_bytes_per_token"]["q8_0"] * 1024
    assert q8["host"]["model"] == f16["host"]["model"]


def test_models_fit_check_device_errors():
    global server
    _setup(_section("tinyBad", "device = CUDA42"))
    _enable(PROVISION_ADMIN_INVENTORY="file:" + _inventory(_fake_gpus()), PROVISION_ADMIN_GPU_PROBE="off")
    server.start()

    # a device of the inventory that the build cannot open: the dry run says so
    c = _check("tinyA", {"device": "CUDA6"})
    assert c["ok"] is False and c["error"]["code"] == "invalid_device"
    assert "invalid device: CUDA6" in c["error"]["line"]
    # a device that is not on this server at all: no dry run
    c = _check("tinyBad")
    assert c["ok"] is False and c["error"]["code"] == "device_missing" and "CUDA42" in c["error"]["message"]
    assert "args" not in c and "duration_ms" not in c
    # in a draft it is a bad value
    _assert_error(stage1._admin("POST", "/admin/models/settings/check", data={"model": "tinyA", "settings": {"device": "CUDA42"}}),
                  400, "invalid_request_error", "device")


class _BlockingFile:
    """a path that blocks whoever opens it for reading: a FIFO without a writer, on Windows a named pipe that never answers"""

    def __init__(self):
        self.listener = None
        if os.name == "nt":
            from multiprocessing.connection import Listener
            self.path = f"\\\\.\\pipe\\provision-admin-test-{os.getpid()}-{server.server_port}"
            self.listener = Listener(self.path, family="AF_PIPE")
        elif hasattr(os, "mkfifo"):
            self.path = str(work_dir / "template.fifo")
            os.mkfifo(self.path)
        else:
            pytest.skip("no FIFO or named pipe on this platform")  # ty: ignore[too-many-positional-arguments]

    def close(self):
        if self.listener is not None:
            self.listener.close()


def test_models_fit_check_busy_and_timeout():
    """one dry run at a time (409 check_busy, a cached result still answers), and a dry run that hangs ends at 30 s"""
    global server
    blocking = _BlockingFile()
    try:
        # the helper parses the arguments of the model like its child would: reading the template file never ends
        _setup(_section("tinyHang", f"chat-template-file = {blocking.path}"))
        _enable()
        server.start()
        cached = _check("tinyA", {"device": "none", "ctx-size": 512})
        assert cached["ok"] is True

        t0 = time.time()
        hanging = stage1._start_request(lambda: _check("tinyHang", {"device": "none"}, timeout=120))
        time.sleep(1.5)
        res = stage1._admin("POST", "/admin/models/settings/check", data={"model": "tinyB", "settings": {"device": "none"}})
        _assert_error(res, 409, "check_busy")
        assert _check("tinyA", {"device": "none", "ctx-size": 512})["cached"] is True

        c = hanging.result(120)
        elapsed = time.time() - t0
        assert c["ok"] is False and c["error"]["code"] == "timeout", c
        assert c["duration_ms"] >= 29000
        assert 29 <= elapsed < 45, elapsed

        # the helper is terminated: the next check runs (after a moment for the process to be reaped)
        deadline = time.time() + 15
        while True:
            res = stage1._admin("POST", "/admin/models/settings/check", data={"model": "tinyB", "settings": {"device": "none"}})
            if res.status_code != 409 or time.time() > deadline:
                break
            time.sleep(0.2)
        assert res.status_code == 200 and res.body["ok"] is True, res.body
    finally:
        blocking.close()


#
# GPU view: canned device inventories and vendor tool output
#


def test_gpus_shuffled_nvidia_smi_rows_are_matched_by_pci_id():
    """mixed GPUs with different memory, nvidia-smi rows not in device order: matched by PCI bus id, never by index"""
    global server
    _setup()
    _enable(PROVISION_ADMIN_INVENTORY="file:" + _inventory(_two_gpus()),
            PROVISION_ADMIN_GPU_PROBE="file:" + _fixture_file("nvidia.txt", NVIDIA_SHUFFLED))
    server.start()

    body = _gpus()
    assert body["build_devices"] == ["CUDA0", "CUDA1"]
    assert body["gpu_backend"]["state"] == "ok"
    assert body["margin"] == GiB and body["settle_ms"] == 1500
    inv = body["inventory"]
    assert inv["ran"] is True and inv["ok"] is True and inv["source"] == "file" and inv["error"] == ""
    assert [d["pci_bus_id"] for d in inv["devices"]] == ["0000:01:00.0", "0000:02:00.0"]
    assert {d["vendor"] for d in inv["devices"]} == {"nvidia"}
    assert isinstance(inv["time"], str) and inv["time_ms"] > 0
    probe = body["probe"]
    assert probe["mode"] == "file" and probe["source"] == "file" and probe["ok"] is True and probe["error"] == ""
    assert probe["mapping"] == "pci" and probe["stale"] is False
    assert [(t["tool"], t["ok"], t["rows"]) for t in probe["tools"]] == [("file", True, 2)]

    c0, c1 = _card(body, "CUDA0"), _card(body, "CUDA1")
    assert c0["mapping"] == "pci" and c0["tool"] == "nvidia-smi" and c0["index"] == 1
    assert c0["uuid"] == "GPU-aaaaaaaa-0000-0000-0000-000000000001" and c0["name"] == "NVIDIA GeForce GTX 1080 Ti"
    assert (c0["total"], c0["used"], c0["free"]) == (11264 * MiB, 2264 * MiB, 9000 * MiB)
    assert c0["memory_source"] == "vendor_tool" and c0["memory_age_ms"] >= 0
    assert c0["backend"] == "CUDA" and c0["vendor"] == "nvidia" and c0["type"] == "gpu" and c0["pci_bus_id"] == "0000:01:00.0"
    assert c0["unsupported"] is False and c0["settling"] is False and c0["same_gpu_as"] == []
    assert c1["index"] == 0 and (c1["total"], c1["free"]) == (4096 * MiB, 3096 * MiB)
    assert body["unmatched"] == [] and body["warnings"] == []

    # a model without a device is listed on every card; when it runs, its memory is an estimate (no report at this verbosity)
    tiny = _card_model(c0, "tinyA")
    assert tiny["all_gpus"] is True and tiny["memory"] is None and tiny["status"] == "unloaded"
    _save("tinyB", {"device": "CUDA1"})
    body = _gpus()
    assert "tinyB" not in [m["id"] for m in _card(body, "CUDA0")["models"]]
    assert _card_model(_card(body, "CUDA1"), "tinyB")["all_gpus"] is False
    stage1._load_model_and_wait("tinyA")
    stage1._wait_until(lambda: _card_model(_card(_gpus(), "CUDA0"), "tinyA")["memory"] is not None, 30, "an estimate for tinyA")
    memory = _card_model(_card(_gpus(), "CUDA0"), "tinyA")["memory"]
    assert memory["source"] == "estimate" and memory["by"] == "formula" and memory["bytes"] > 0


def test_gpus_hidden_gpu_is_not_matched_by_index():
    """CUDA_VISIBLE_DEVICES / container device_ids hide GPUs: fewer devices than nvidia-smi rows, renumbered"""
    global server
    _setup()
    _enable(PROVISION_ADMIN_INVENTORY="file:" + _inventory([_dev("CUDA0", "CUDA", "NVIDIA GeForce GTX 1050 Ti", "0000:02:00.0", 4294967296, 3000000000)]),
            PROVISION_ADMIN_GPU_PROBE="file:" + _fixture_file("nvidia.txt", NVIDIA_THREE))
    server.start()

    body = _gpus()
    assert [g["device"] for g in body["gpus"]] == ["CUDA0"]
    c0 = _card(body, "CUDA0")
    assert c0["mapping"] == "pci" and c0["index"] == 1 and c0["total"] == 4096 * MiB
    assert sorted(u["pci_bus_id"] for u in body["unmatched"]) == ["0000:01:00.0", "0000:03:00.0"]
    assert {u["tool"] for u in body["unmatched"]} == {"nvidia-smi"}
    assert "vendor_rows_not_matched" in body["warnings"]


def test_gpus_mixed_vendors_vulkan_and_a_device_without_pci_id():
    global server
    devices = [
        _dev("CUDA0", "CUDA", "NVIDIA GeForce GTX 1080 Ti", "0000:01:00.0", 11811160064, 11000000000),
        _dev("ROCm0", "ROCm", "AMD Instinct MI300X", "0000:c1:00.0", 206141652992, 200000000000),
        _dev("Vulkan0", "Vulkan", "NVIDIA GeForce GTX 1080 Ti", "0000:01:00.0", 11811160064, 11000000000),
        _dev("SYCL0", "SYCL", "Intel(R) Arc(TM) A770 Graphics", None, 16225243136, 15000000000),
        _dev("BLAS", "BLAS", "OpenBLAS", None, 0, 0, kind="accel"),
    ]
    _setup()
    _enable(PROVISION_ADMIN_INVENTORY="file:" + _inventory(devices),
            PROVISION_ADMIN_GPU_PROBE="file:" + _fixture_file("tools.txt", NVIDIA_AND_AMD))
    server.start()

    body = _gpus()
    # accelerators are in the inventory, but they are no GPU for a model
    assert body["build_devices"] == ["CUDA0", "ROCm0", "Vulkan0", "SYCL0"]
    assert [d["name"] for d in body["inventory"]["devices"]] == ["CUDA0", "ROCm0", "Vulkan0", "SYCL0", "BLAS"]
    assert stage1._settings()["build_devices"] == ["CUDA0", "ROCm0", "Vulkan0", "SYCL0"]
    cuda, rocm, vulkan, sycl = (_card(body, d) for d in ("CUDA0", "ROCm0", "Vulkan0", "SYCL0"))
    assert cuda["tool"] == "nvidia-smi" and cuda["mapping"] == "pci" and cuda["free"] == 9000 * MiB
    assert rocm["tool"] == "amd-smi" and rocm["mapping"] == "pci" and rocm["vendor"] == "amd"
    assert (rocm["total"], rocm["used"], rocm["free"]) == (196592 * MiB, 283 * MiB, 196309 * MiB)
    # the same physical GPU through two backends: one row, both cards say so
    assert cuda["same_gpu_as"] == ["Vulkan0"] and vulkan["same_gpu_as"] == ["CUDA0"]
    assert vulkan["mapping"] == "unknown" and vulkan["memory_source"] == "inventory" and vulkan["vendor"] == "nvidia"
    # no PCI id, no vendor tool: the memory the inventory measured, with its age
    assert sycl["pci_bus_id"] is None and sycl["vendor"] == "intel" and sycl["mapping"] == "unknown"
    assert sycl["memory_source"] == "inventory" and sycl["total"] == 16225243136 and sycl["free"] == 15000000000
    assert sycl["memory_age_ms"] >= 0
    assert body["probe"]["mapping"] == "mixed"
    assert _diagnostics()["memory_mapping"]["status"] == "warning"

    # device names of any backend, matched case-insensitively, stored in the inventory spelling
    assert _save("tinyA", {"device": "rocm0"})["model"]["overrides"] == {"device": "ROCm0"}
    _save("tinyA", {"device": "vulkan0,sycl0"})
    assert _flag("tinyA", "--device") == "Vulkan0,SYCL0"
    _assert_error(stage1._admin("POST", "/admin/models/settings", data={"model": "tinyA", "settings": {"device": "BLAS"}}),
                  400, "invalid_request_error", "device")


@pytest.mark.parametrize("tool,canned", [
    pytest.param("amd-smi", AMD_SMI, id="amd-smi"),
    pytest.param("rocm-smi", ROCM_SMI, id="rocm-smi"),
    pytest.param("amd-smi", AMD_SMI_WARNINGS, id="amd-smi_with_warnings"),
    pytest.param("rocm-smi", ROCM_SMI_WARNINGS, id="rocm-smi_with_warnings"),
])
def test_gpus_amd_tools(tool: str, canned: str):
    """amd-smi (list + metric, joined on the gpu index) and rocm-smi (JSON per card), matched by PCI id"""
    global server
    devices = [_dev("ROCm0", "ROCm", "AMD Radeon RX 7900 XTX", "0000:03:00.0", 25753026560, 25000000000),
               _dev("ROCm1", "ROCm", "AMD Radeon RX 6800", "0000:0a:00.0", 17163091968, 17000000000)]
    _setup()
    _enable(PROVISION_ADMIN_INVENTORY="file:" + _inventory(devices), PROVISION_ADMIN_GPU_PROBE="file:" + _fixture_file("amd.txt", canned))
    server.start()

    body = _gpus()
    g0, g1 = _card(body, "ROCm0"), _card(body, "ROCm1")
    assert g0["mapping"] == "pci" and g1["mapping"] == "pci" and g0["tool"] == tool and g1["tool"] == tool
    assert g0["memory_source"] == "vendor_tool" and g0["vendor"] == "amd"
    if tool == "amd-smi":
        assert g0["index"] == 0 and (g0["total"], g0["used"], g0["free"]) == (24560 * MiB, 60 * MiB, 24500 * MiB)
        assert g1["index"] == 1 and g1["free"] == 368 * MiB and g1["uuid"] == "a2"
    else:
        assert g0["index"] == 1 and (g0["total"], g0["used"]) == (25753026560, 1073741824)
        assert g0["free"] == 25753026560 - 1073741824
        assert g1["index"] == 0 and g1["free"] == 17163091968 - 12582912 and g1["name"].startswith("Navi 21")
    assert body["unmatched"] == []


def test_gpus_single_device_without_pci_id():
    global server
    _setup()
    _enable(PROVISION_ADMIN_INVENTORY="file:" + _inventory([_dev("Vulkan0", "Vulkan", "NVIDIA GeForce GTX 1050 Ti", None, 4219469824, 3500000000)]),
            PROVISION_ADMIN_GPU_PROBE="file:" + _fixture_file("nvidia.txt", "nvidia-smi\nthis line is not csv\n"
                                                              "0, 00000000:01:00.0, GPU-14f2ed89, 4096, 801, 3205, NVIDIA GeForce GTX 1050 Ti\n"))
    server.start()

    body = _gpus()
    g = _card(body, "Vulkan0")
    assert g["mapping"] == "single" and g["memory_source"] == "vendor_tool" and g["free"] == 3205 * MiB
    assert g["pci_bus_id"] == "0000:01:00.0"  # from the tool row
    assert body["probe"]["mapping"] == "single" and body["unmatched"] == []


def test_gpus_mig_rows_are_unsupported():
    global server
    _setup()
    _enable(PROVISION_ADMIN_INVENTORY="file:" + _inventory(_two_gpus()),
            PROVISION_ADMIN_GPU_PROBE="file:" + _fixture_file("nvidia.txt",
                                                              "nvidia-smi\n0, 00000000:01:00.0, MIG-GPU-1, [N/A], [N/A], [N/A], NVIDIA A100-SXM4-40GB, MIG 1g.5gb\n"))
    server.start()

    body = _gpus()
    c0, c1 = _card(body, "CUDA0"), _card(body, "CUDA1")
    assert c0["unsupported"] is True and c0["mapping"] == "pci" and c0["name"] == "NVIDIA A100-SXM4-40GB, MIG 1g.5gb"
    assert c0["memory_source"] == "inventory" and c0["total"] == 11811160064
    assert c1["mapping"] == "unknown" and c1["tool"] is None and c1["memory_source"] == "inventory"


@pytest.mark.parametrize("setting,error,mode", [
    pytest.param("off", "disabled", "off", id="off"),
    pytest.param("file:{missing}", "not_found", "file", id="missing_file"),
    pytest.param("nvidia-smi:{missing}", "not_found", "tool", id="missing_tool"),
    pytest.param("{missing}", "not_found", "tool", id="missing_path"),
    pytest.param("file:{unreadable}", "parse_error", "file", id="unreadable_output"),
    pytest.param("file:{failing}", "failed", "file", id="tool_fails"),
])
def test_gpus_probe_problems(setting: str, error: str, mode: str):
    """memory from the device inventory when there is no usable vendor tool, with the reason"""
    global server
    missing = str(work_dir / "no-such-dir" / "nvidia-smi")
    unreadable = _fixture_file("bad.txt", "nvidia-smi\nnot,csv\nhello\n")
    failing = _fixture_file("fail.txt", "nvidia-smi\n# exit 9\n"
                            "NVIDIA-SMI has failed because it couldn't communicate with the NVIDIA driver.\n")
    _setup()
    _enable(PROVISION_ADMIN_INVENTORY="file:" + _inventory(_two_gpus()),
            PROVISION_ADMIN_GPU_PROBE=setting.format(missing=missing, unreadable=unreadable, failing=failing))
    server.start()

    body = _gpus()
    probe = body["probe"]
    assert probe["ok"] is False and probe["error"] == error and probe["mode"] == mode and probe["message"] != ""
    if error == "failed":
        assert "status 9" in probe["message"]
    for card in body["gpus"]:
        assert card["memory_source"] == "inventory" and card["mapping"] == "unknown"
        assert card["free"] == {"CUDA0": 11000000000, "CUDA1": 3000000000}[card["device"]]
    assert _diagnostics()["vendor_tool"]["status"] == "warning"


def test_gpus_probe_timeout_and_stuck_probe():
    global server
    canned = "0, 00000000:01:00.0, GPU-a, 11264, 2264, 9000, NVIDIA GeForce GTX 1080 Ti\n"
    slow = _fixture_file("slow.txt", "nvidia-smi\n# sleep 8000\n" + canned)
    _setup()
    _enable(PROVISION_ADMIN_INVENTORY="file:" + _inventory(_two_gpus()), PROVISION_ADMIN_GPU_PROBE="file:" + slow)
    server.start()
    t0 = time.time()
    body = _gpus()
    elapsed = time.time() - t0
    # a caller never waits longer than 5 s, whatever the tool does
    assert body["probe"]["error"] == "timeout" and elapsed < 8, elapsed
    assert _card(body, "CUDA0")["memory_source"] == "inventory"
    # the slow run goes on (its own deadline is 15 s: nvidia-smi without persistence mode can take seconds per GPU);
    # its answer is used once it comes, without a second run
    time.sleep(max(0.0, t0 + 8.5 - time.time()))
    body = _gpus()
    assert body["probe"]["ok"] is True and body["probe"]["error"] == "", body["probe"]
    assert _card(body, "CUDA0")["memory_source"] == "vendor_tool" and _card(body, "CUDA0")["free"] == 9000 * MiB
    server.stop()

    # a tool that does not even die when it is killed (stuck in the driver): no second process while it lives
    stuck = _fixture_file("stuck.txt", "nvidia-smi\n# ignore-terminate\n# sleep 21000\n" + canned)
    stage1.env.setenv("PROVISION_ADMIN_GPU_PROBE", "file:" + stuck)
    server.start()
    t_start = time.time()
    body = _gpus()
    assert body["probe"]["error"] == "timeout"
    # past the deadline of the run (15 s) it is given up, but its process lives on
    time.sleep(max(0.0, t_start + 16 - time.time()))
    t0 = time.time()
    body = _gpus(refresh=True)
    assert body["probe"]["error"] == "probe_stuck" and time.time() - t0 < 3
    assert body["probe"]["tools"][0]["error"] == "probe_stuck" and "has not exited" in body["probe"]["tools"][0]["message"]
    # once it has exited, the next probe runs again
    with open(stuck, "w", encoding="utf-8") as f:
        f.write("nvidia-smi\n" + canned)
    while time.time() - t_start < 22:
        time.sleep(0.5)
    stage1._wait_until(lambda: _gpus(refresh=True)["probe"]["ok"], 20, "the probe to recover")
    assert _card(_gpus(), "CUDA0")["memory_source"] == "vendor_tool"


def test_gpus_settling_while_a_model_loads():
    global server
    _setup()
    _enable(PROVISION_ADMIN_INVENTORY="file:" + _inventory(_two_gpus()),
            PROVISION_ADMIN_GPU_PROBE="file:" + _fixture_file("nvidia.txt", NVIDIA_SHUFFLED))
    server.start()
    assert _card(_gpus(), "CUDA0")["settling"] is False

    loading = stage1._start_request(lambda: _load_async("tinyB"))
    seen = False
    deadline = time.time() + 20
    while time.time() < deadline and not seen:
        seen = _card(_gpus(), "CUDA0")["settling"]
        time.sleep(0.05)
    assert seen, "the card of a GPU whose model loads is never marked settling"
    loading.result(60)
    stage1._wait_for_model_status("tinyB", "loaded")
    time.sleep(2.0)  # settle_ms after the last status change
    assert _card(_gpus(), "CUDA0")["settling"] is False


def test_gpus_rescan():
    global server
    _setup()
    inventory = _inventory(_two_gpus()[:1])
    _enable(PROVISION_ADMIN_INVENTORY="file:" + inventory, PROVISION_ADMIN_GPU_PROBE="off")
    server.start()
    before = _gpus()
    assert before["build_devices"] == ["CUDA0"]
    _assert_error(stage1._admin("POST", "/admin/models/settings", data={"model": "tinyA", "settings": {"device": "CUDA1"}}),
                  400, "invalid_request_error", "device")

    # a GPU appears; the inventory takes 2 s this time
    _inventory(_two_gpus(), directives="# sleep 2000\n")
    first = stage1._start_request(lambda: stage1._admin("POST", "/admin/gpus/rescan", data={}))
    time.sleep(0.7)
    _assert_error(stage1._admin("POST", "/admin/gpus/rescan", data={}), 409, "rescan_busy")
    res = first.result(60)
    assert res.status_code == 200
    inv = res.body["inventory"]
    assert inv["ok"] is True and inv["duration_ms"] >= 1900 and inv["time_ms"] > before["inventory"]["time_ms"]
    assert res.body["build_devices"] == ["CUDA0", "CUDA1"]
    assert stage1._settings()["build_devices"] == ["CUDA0", "CUDA1"]
    _save("tinyA", {"device": "CUDA1"})

    # two at the same moment (a double click, two admins): one inventory runs, the other request is refused
    _inventory(_two_gpus(), directives="# sleep 1500\n")
    barrier = threading.Barrier(2)

    def rescan():
        barrier.wait()
        return stage1._admin("POST", "/admin/gpus/rescan", data={})

    both = [stage1._start_request(rescan), stage1._start_request(rescan)]
    results = [r.result(60) for r in both]
    assert sorted(r.status_code for r in results) == [200, 409], [r.body for r in results]
    _assert_error(next(r for r in results if r.status_code == 409), 409, "rescan_busy")


@pytest.mark.parametrize("content,error,message", [
    pytest.param("# exit 3\nsomething went wrong\n", "failed", "status 3", id="exit_status"),
    pytest.param("this is not json\n", "parse_error", "without a result", id="no_result"),
    pytest.param('{"ok": false, "error": {"code": "failed", "message": "the driver exploded"}}\n', "failed", "the driver exploded",
                 id="helper_error"),
    pytest.param('{"ok": true, "devices": "none"}\n', "failed", "devices", id="no_device_list"),
])
def test_gpus_inventory_failures(content: str, error: str, message: str):
    """a failed device inventory: the router starts anyway, says why, and uses the device names of its own backends"""
    global server
    _setup()
    server.log_path = str(work_dir / "server.log")
    _enable(PROVISION_ADMIN_INVENTORY="file:" + _fixture_file("inventory.json", content), PROVISION_ADMIN_GPU_PROBE="off")
    server.start()

    body = _gpus()
    inv = body["inventory"]
    assert inv["ran"] is True and inv["ok"] is False and inv["error"] == error and message in inv["message"]
    assert inv["devices"] == []
    assert [g["device"] for g in body["gpus"]] == body["build_devices"]
    diag = _diagnostics()["inventory"]
    assert diag["status"] == "error" and message in diag["hint"]
    server.stop()
    assert "device inventory unavailable" in stage1._read_text(server.log_path)


def test_gpus_inventory_off():
    global server
    _setup()
    _enable(PROVISION_ADMIN_INVENTORY="off", PROVISION_ADMIN_GPU_PROBE="off")
    server.start()

    inv = _gpus()["inventory"]
    assert inv["ok"] is False and inv["error"] == "disabled" and inv["source"] == "off"
    assert _diagnostics()["inventory"]["status"] == "warning"


def test_gpus_inventory_timeout():
    """an inventory helper that hangs (a GPU driver) is given up after 20 s; the router starts anyway. When its process
    does not even exit after it was stopped (stuck in the driver), Re-detect GPUs says so instead of "busy" forever,
    and so does the diagnostics report"""
    global server
    _setup()
    server.log_path = str(work_dir / "server.log")
    _enable(PROVISION_ADMIN_INVENTORY="file:" + _inventory(_two_gpus(), directives="# ignore-terminate\n# sleep 40000\n"),
            PROVISION_ADMIN_GPU_PROBE="off")
    t0 = time.time()
    server.start(timeout_seconds=90)
    assert time.time() - t0 >= 19

    inv = _gpus()["inventory"]
    assert inv["ok"] is False and inv["error"] == "timeout" and "20 s" in inv["message"]

    time.sleep(3.5)  # a stopped helper normally exits within milliseconds; this one is still there
    res = stage1._admin("POST", "/admin/gpus/rescan", data={})
    _assert_error(res, 409, "rescan_stuck")
    assert "has not exited" in res.body["error"]["message"] and "GPU driver" in res.body["error"]["message"]
    body = _admin_get("/admin/diagnostics")
    assert [i["id"] for i in body["items"]] == DIAGNOSTIC_ITEMS + ["stuck_helpers"]
    stuck = body["items"][-1]
    assert stuck["status"] == "error" and len(stuck["value"]) == 1 and "device inventory" in stuck["value"][0]
    server.stop()
    assert "device inventory unavailable" in stage1._read_text(server.log_path)


def test_gpus_cpu_only_build():
    """the real device inventory of a CPU-only build: no GPU, no vendor tool is run"""
    global server
    _setup()
    _enable()
    server.start()
    _skip_unless_cpu_only_build()

    body = _gpus()
    assert body["gpu_backend"]["state"] == "cpu_only" and body["gpu_backend"]["libraries"] == []
    assert body["inventory"]["ok"] is True and body["inventory"]["source"] == "helper"
    # an accelerator (e.g. BLAS) can be in the inventory of a CPU build, but no GPU
    assert [d for d in body["inventory"]["devices"] if d["type"] in ("gpu", "igpu")] == []
    assert body["probe"]["mode"] == "auto" and body["probe"]["error"] == "unsupported" and body["probe"]["tools"] == []
    assert body["build_devices"] == [] and body["gpus"] == [] and body["unmatched"] == [] and body["warnings"] == []
    assert stage1._settings()["build_devices"] == []
    res = stage1._admin("POST", "/admin/models/settings", data={"model": "tinyA", "settings": {"device": "CUDA0"}})
    _assert_error(res, 400, "invalid_request_error", "device")
    assert '"none"' in res.body["error"]["message"]
    _save("tinyA", {"device": "none"})
    items = _diagnostics()
    assert items["gpu_backend"]["status"] == "ok" and items["inventory"]["status"] == "ok"
    assert items["vendor_tool"]["status"] == "ok" and items["memory_mapping"]["value"] == "none"
    # without a GPU every model is CPU only: the dry run says so
    assert _check("tinyA")["verdict"] == "cpu_only"


def test_gpus_backend_library_without_devices(tmp_path_factory):
    """a GPU backend library next to the binary but no device found: "GPU backend did not initialise", not "CPU-only build" """
    global server
    original = server.server_path or os.environ.get("LLAMA_SERVER_BIN_PATH")
    if not original or not os.path.isfile(original):
        pytest.skip("LLAMA_SERVER_BIN_PATH is not set")  # ty: ignore[too-many-positional-arguments]
    bin_dir = tmp_path_factory.mktemp("bin")
    for entry in os.listdir(os.path.dirname(original)):
        src = os.path.join(os.path.dirname(original), entry)
        lower = entry.lower()
        if os.path.isfile(src) and (entry == os.path.basename(original) or lower.endswith((".dll", ".dylib")) or ".so" in lower):
            shutil.copy2(src, bin_dir / entry)
    fake = "ggml-cuda.dll" if os.name == "nt" else ("libggml-cuda.dylib" if sys.platform == "darwin" else "libggml-cuda.so")
    if (bin_dir / fake).exists():
        pytest.skip("this build has a real CUDA backend library")  # ty: ignore[too-many-positional-arguments]
    (bin_dir / fake).write_bytes(b"not a real library\n")
    server.server_path = str(bin_dir / os.path.basename(original))
    _setup()
    _enable(PROVISION_ADMIN_GPU_PROBE="off")
    server.start()

    body = _gpus()
    backend = body["gpu_backend"]
    if backend["state"] == "ok":
        pytest.skip("this build found GPU devices")  # ty: ignore[too-many-positional-arguments]
    assert backend["state"] == "no_devices" and fake in backend["libraries"]
    assert "did not initialise" in backend["message"]
    assert "gpu_backend_not_initialised" in body["warnings"]
    item = _diagnostics()["gpu_backend"]
    assert item["status"] == "error" and "driver" in item["hint"]


def test_gpus_memory_reported_by_the_model():
    """the memory breakdown a model child prints (verbosity >= 4) beats any estimate: works for every backend, also in Docker"""
    global server
    # print_info prints general.name at verbosity 4: a model whose name is a real breakdown line of a CUDA host
    named = _gguf_copy(stage1._tiny_gguf(), work_dir / "tinyR.gguf", strings={"general.name": BREAKDOWN_LINE})
    _setup(stage1._preset_section("tinyR", (work_dir / "tinyR.gguf").as_posix(), "log-verbosity = 4"))
    _enable(PROVISION_ADMIN_INVENTORY="file:" + _inventory(_two_gpus()),
            PROVISION_ADMIN_GPU_PROBE="file:" + _fixture_file("nvidia.txt", NVIDIA_SHUFFLED))
    server.start()
    assert os.path.getsize(named) > 0
    # on a GPU build the child prints breakdown lines of its own real GPUs too, for the canned names
    _skip_on_gpu_build()

    stage1._load_model_and_wait("tinyR")
    expected = {"bytes": (4829 + 3584 + 422) * MiB, "source": "reported", "by": "model"}
    stage1._wait_until(lambda: _card_model(_card(_gpus(), "CUDA1"), "tinyR")["memory"] == expected, 30, "the reported memory")
    # the report names CUDA1 only
    assert _card_model(_card(_gpus(), "CUDA0"), "tinyR")["memory"] is None


def test_gpus_memory_estimate_by_the_formula():
    """no report, no measurement and no dry run: a running model's memory on its GPUs is the GGUF formula, with the
    KV cache of the whole pool as llama.cpp sizes it (-c; else n_parallel x --kv-unified-per-slot; else the training
    context, not times the slots), split over the GPUs it may use"""
    global server
    _setup()
    _enable(PROVISION_ADMIN_INVENTORY="file:" + _inventory(_two_gpus()), PROVISION_ADMIN_GPU_PROBE="off")
    server.start()
    _save("tinyB", {"parallel": 2, "kv-unified-per-slot": 512})
    f = _row("tinyA")["facts"]
    assert f["n_ctx_train"] == 2048

    def expected(n_ctx: int) -> int:
        weights = f["output_bytes"] + sum(f["layer_bytes"])  # n-gpu-layers 99 in [*]: every layer on the GPUs
        kv = f["kv_bytes_per_token"]["f16"] * ((n_ctx + 255) // 256 * 256)
        compute = 4 * 512 * (f["n_vocab"] + 6 * f["n_embd"])  # ubatch 512, flash attention not off
        return (weights + kv + compute) // 2  # no device: both GPUs

    for model, n_ctx in (("tinyA", 2048), ("tinyB", 2 * 512), ("tinyP", 512)):
        stage1._load_model_and_wait(model)
        stage1._wait_until(lambda: _card_model(_card(_gpus(), "CUDA0"), model)["memory"] is not None, 30, f"the estimate of {model}")
        for device in ("CUDA0", "CUDA1"):
            memory = _card_model(_card(_gpus(), device), model)["memory"]
            assert memory == {"bytes": expected(n_ctx), "source": "estimate", "by": "formula"}, (model, device, memory)


@pytest.mark.skipif(not sys.platform.startswith("linux"), reason="per-process GPU memory is matched to the children through /proc")
def test_gpus_memory_measured_per_process():
    """on a Linux host: nvidia-smi --query-compute-apps rows of a model child's PID count as measured"""
    global server
    canned = _fixture_file("nvidia.txt", NVIDIA_SHUFFLED)
    _setup()
    _enable(PROVISION_ADMIN_INVENTORY="file:" + _inventory(_two_gpus()), PROVISION_ADMIN_GPU_PROBE="file:" + canned)
    server.start()
    stage1._load_model_and_wait("tinyA")
    assert server.process is not None
    children = stage1._child_pids(server.process.pid)
    if len(children) != 1:
        pytest.skip(f"cannot tell the model child among {children}")  # ty: ignore[too-many-positional-arguments]
    with open(canned, "a", encoding="utf-8") as f:
        f.write(f"nvidia-smi-compute-apps\n{children[0]}, 812, 00000000:02:00.0\n424242, 999, 00000000:01:00.0\n")

    expected = {"bytes": 812 * MiB, "source": "measured", "by": "nvidia-smi"}
    stage1._wait_until(lambda: _card_model(_card(_gpus(refresh=True), "CUDA1"), "tinyA")["memory"] == expected, 30,
                       "the measured memory")
    # a process that is no child of this router is not counted for any model
    memory = _card_model(_card(_gpus(), "CUDA0"), "tinyA")["memory"]
    assert memory is None or memory["source"] != "measured"


#
# output tail and failure reason of the model children
#


def test_models_log_failure_reasons():
    global server
    garbage = _fixture_file("garbage.gguf", "this is not a gguf file\n" * 100)
    _setup(_section("badini", "device = CUDA42") + stage1._preset_section("garbage", garbage.replace("\\", "/")))
    server.log_path = str(work_dir / "server.log")
    _enable()
    server.start()

    # models.ini values bypass the admin validation: the child refuses the device itself
    _load_async("badini")
    log = _wait_failure("badini")
    assert log["model"] == "badini" and log["status"] == "unloaded"
    reason = log["reason"]
    assert reason["code"] == "invalid_device" and "invalid device" in reason["line"]
    assert reason["message"] == "The configured device does not exist on this server" and reason["hint"] != ""
    assert any("invalid device" in line for line in log["lines"])
    assert log["instance"]["running"] is False and log["instance"]["lines_total"] >= len(log["lines"]) > 0
    failure = log["last_failure"]
    assert failure["reason"]["code"] == "invalid_device" and failure["before_loaded"] is True and failure["exit_code"] != 0
    assert failure["failures"] == 0 and failure["settings_reverted"] is False  # no admin settings were involved
    assert failure["lines"] and isinstance(failure["time"], str)
    # started_ms: the start of that instance in ms (the second of "started"), before the failure it ended in
    instance = log["instance"]
    assert isinstance(instance["started_ms"], int)
    assert instance["started_ms"] // 1000 == calendar.timegm(time.strptime(instance["started"], "%Y-%m-%dT%H:%M:%SZ"))
    assert 0 < instance["started_ms"] <= failure["time_ms"]
    row = _row("badini")
    assert row["last_failure"]["reason"]["code"] == "invalid_device" and row["last_run"]["reason"]["code"] == "invalid_device"
    assert row["last_run"]["failed"] is True

    _load_async("garbage")
    assert _wait_failure("garbage")["reason"]["code"] == "model_load_failed"

    # the log forwarding of the router is unchanged
    server.stop()
    assert "invalid device: CUDA42" in stage1._read_text(server.log_path)


def test_models_log_reason_of_an_unsupported_kv_cache_type():
    global server
    _setup()
    _enable()
    server.start()

    _save("tinyA", {"cache-type-k": "q8_0", "cache-type-v": "q8_0"})  # valid for the server, not for this model
    _load_async("tinyA")
    log = _wait_failure("tinyA")
    assert any("does not divide n_embd_head_k" in line for line in log["lines"])
    assert _row("tinyA")["phase"] == "failed"  # it counts as a failed start of these settings either way
    assert log["reason"]["code"] == "invalid_settings", log["reason"]


def test_models_log_tail_is_bounded():
    global server
    _setup(_section("tinyV", "log-verbosity = 5"))
    _enable()
    server.start()

    stage1._load_model_and_wait("tinyV")
    log = _log("tinyV")
    size = sum(len(line.encode("utf-8")) + 1 for line in log["lines"])
    assert 0 < len(log["lines"]) <= 200 and size <= 64 * 1024
    assert log["truncated"] is True and log["instance"]["lines_total"] > 200
    assert log["instance"]["running"] is True and log["last_failure"] is None
    assert not any("\x1b" in line for line in log["lines"])
    assert not any("cmd_child_to_router" in line for line in log["lines"])

    # the tail survives the end of the instance and a reload; a new instance starts a new tail
    first = log["instance"]
    res = server.make_request("POST", "/models/unload", data={"model": "tinyV"})
    assert res.status_code == 200
    stage1._wait_for_model_status("tinyV", "unloaded")
    stage1._models(reload=True)
    log = _log("tinyV")
    assert log["lines"] and log["instance"]["running"] is False and log["instance"]["started"] == first["started"]
    stage1._load_model_and_wait("tinyV")
    second = _log("tinyV")["instance"]
    assert second["running"] is True and (second["started"], second["port"]) != (first["started"], first["port"])
    assert second["started_ms"] > first["started_ms"] > 0


def test_models_log_of_unknown_and_idle_models():
    global server
    _setup()
    _enable()
    server.start()

    stage1._assert_not_found(stage1._admin("GET", "/admin/models/log?model=no-such-model"))
    _assert_error(stage1._admin("GET", "/admin/models/log"), 400, "invalid_request_error")
    log = _log("tinyB")
    assert log == {"model": "tinyB", "status": "unloaded", "instance": None, "reason": None, "lines": [], "truncated": False,
                   "last_failure": None}


#
# diagnostics
#


def test_diagnostics_report():
    global server
    _setup()
    _enable(PROVISION_ADMIN_GPU_PROBE="off")
    server.start()

    items = _diagnostics()
    for item in items.values():
        assert item["status"] in ("ok", "warning", "error", "info") and isinstance(item["label"], str) and isinstance(item["hint"], str)
    container = os.path.exists("/.dockerenv") or os.path.exists("/run/.containerenv")
    if os.name == "nt":
        assert items["deployment"]["value"] == "host"
    assert items["deployment"]["status"] == "ok"
    binary = items["binary"]["value"]
    assert binary["version"] != ""
    assert os.path.normcase(os.path.realpath(binary["path"])) == os.path.normcase(os.path.realpath(server.server_path or os.environ["LLAMA_SERVER_BIN_PATH"]))
    assert items["inventory"]["value"]["ran"] is True
    vendor = items["vendor_tool"]
    assert vendor["status"] == "warning" and vendor["value"]["mode"] == "off" and vendor["value"]["setting"] == "off"
    assert vendor["value"]["error"] == "disabled" and vendor["value"]["tools"] == []
    assert items["router_gpu_free"]["status"] == "info" and items["router_gpu_free"]["value"] == "not checked"
    state = items["state_dir"]["value"]
    assert stage1._same_path(state["path"], state_dir) and state["writable"] is True
    if not container:
        assert state["looks_persistent"] == (not state["path"].startswith(("/tmp", "/var/tmp")))
    assert items["admin_key"]["status"] == "ok" and items["admin_key"]["value"] == "env"
    assert items["proxy"]["status"] == "info" and items["proxy"]["value"] is False
    assert items["model_devices"]["value"] is None and items["model_devices"]["hint"] == "not set"
    settings = items["model_settings"]
    assert settings["status"] == "ok" and settings["value"]["entries"] == 0 and settings["value"]["read_only"] is False

    # a request that came through a reverse proxy is remembered
    _diagnostics(headers={"X-Forwarded-For": "10.0.0.1"})
    items = _diagnostics()
    assert items["proxy"]["status"] == "ok" and items["proxy"]["value"] is True
    _save("tinyA", {"ctx-size": 1024})
    assert _diagnostics()["model_settings"]["value"]["entries"] == 1


def test_diagnostics_with_canned_devices_and_tools():
    global server
    _setup()
    tools = _fixture_file("nvidia.txt", NVIDIA_SHUFFLED)
    _enable(PROVISION_ADMIN_INVENTORY="file:" + _inventory(_two_gpus()), PROVISION_ADMIN_GPU_PROBE="file:" + tools,
            MODEL_DEVICES="*=CUDA0")
    server.start()

    items = _diagnostics()
    assert items["gpu_backend"]["status"] == "ok" and items["gpu_backend"]["value"]["state"] == "ok"
    assert items["inventory"]["status"] == "ok" and "2 device(s)" in items["inventory"]["hint"]
    vendor = items["vendor_tool"]
    assert vendor["status"] == "ok" and vendor["value"]["mode"] == "file"
    assert [(t["tool"], t["ok"]) for t in vendor["value"]["tools"]] == [("file", True)]
    assert vendor["value"]["tools"][0]["version"].startswith("canned output")
    assert items["memory_mapping"]["status"] == "ok" and items["memory_mapping"]["value"] == "pci"
    assert items["model_devices"]["value"] == "*=CUDA0"


#
# existing routes
#


def test_models_existing_routes_keep_their_shape(monkeypatch, tmp_path_factory):
    """saved settings change the arguments of a model, nothing else in the public model routes"""
    global server
    # an empty cache of this test: nothing another test downloads or deletes meanwhile shows up in one run only
    monkeypatch.setenv("LLAMA_CACHE", str(tmp_path_factory.mktemp("cache")))
    _setup()
    server.start()
    plain = stage1._models()
    plain_props = stage1._props()
    server.stop()

    _enable()
    server.start()
    _save("tinyA", {"ubatch-size": 128})
    _save("tinyB", {"ctx-size": 1024, "parallel": 2})
    with_admin = stage1._models()
    assert sorted(with_admin) == sorted(plain)
    for model_id, item in with_admin.items():
        assert sorted(item) == sorted(plain[model_id]), model_id
        assert sorted(item["status"]) == sorted(plain[model_id]["status"]), model_id
    assert sorted(stage1._props()) == sorted(plain_props)
    # the arguments differ by the saved values only
    args = list(with_admin["tinyA"]["status"]["args"])
    i = args.index("--ubatch-size")
    assert args[i + 1] == "128"
    del args[i:i + 2]
    assert args == plain["tinyA"]["status"]["args"]
    assert with_admin["tinyP"]["status"]["args"] == plain["tinyP"]["status"]["args"]
    res = server.make_request("GET", "/v1/models")
    assert res.status_code == 200 and sorted(m["id"] for m in res.body["data"]) == sorted(plain)
