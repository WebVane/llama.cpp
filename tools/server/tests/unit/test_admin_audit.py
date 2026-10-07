# tests for stage 3 milestone M3 of the Provision admin layer: the audit log of admin actions
# (tools/server/server-admin-audit.cpp, the hooks in server-admin.cpp and server-admin-upload.cpp,
# docs/provision-admin.md): GET /admin/audit, the three JSON-lines files in the admin state directory, the in-memory
# rings when they cannot be written, the merge windows and caps of refused requests and of observed effects.
#
# What is recorded, and how it is checked here:
#   - change    every changing /admin route, one record per request (also when it fails), with the identity of the
#               client (TCP peer, the client named by a trusted proxy, a raw X-Forwarded-For marked unverified);
#   - refusal   the 401 / 403 answers of the /admin routes and of the pre-body hook of the chunk route, merged per
#               client + path + status + reason, capped per window, in a file of their own;
#   - server    what the server does by itself (audit.started, settings reverted, uploads expired / swept / resumed);
#   - observed  effects of the public API seen on the model event feed, actor "public API (unknown)", capped per
#               window, in a file of their own (like refusals they need no admin key: they never push a change out).
#
# Everything runs on a CPU-only build and without the network: the routers are started with --offline and
# MODEL_ENDPOINT points at a closed port, except for the one test that downloads from a stand-in for Hugging Face
# that runs inside the test (test_admin._FakeHub). Sizes, windows and write errors that cannot be caused from
# outside in reasonable time come from the test-only hooks PROVISION_ADMIN_TEST_AUDIT_* (and the upload hooks of
# stage 3 M1), read once at startup and never passed on to a child.
# The test marked slow (SLOW_TESTS=1) is the acceptance test of the design review: 10,000 refused requests from 1,000
# clients leave every change record in place.
# The routers of this file use the port block of test_admin_upload.py (ADMIN_UPLOAD_TEST_PORT_BASE), see _port().

import json
import os
import shutil
import signal
import socket
import stat
import subprocess
import sys
import threading
import time
from urllib.parse import urlencode

import pytest
from utils import *

import admin_gguf as gg

# the helpers of the stage-1 admin tests (the model cache, the error format, the fake hub) and of the M1 upload tests
# (the router of a test, requests, uploads, staging); their module globals are pointed at the router of each test
import test_admin as stage1
import test_admin_upload as m1
from test_admin import ADMIN_KEY

server: ServerProcess
env: pytest.MonkeyPatch
work_dir: Any
state_dir: str
models_dir: str

KiB = 1024
MiB = 1024 * KiB

MAIN_FILE = "admin-audit.jsonl"
REFUSALS_FILE = "admin-audit-refusals.jsonl"
OBSERVED_FILE = "admin-audit-observed.jsonl"
# a port nothing listens on: a download check or a download that is not meant to reach a stand-in fails at once
CLOSED_ENDPOINT = "http://127.0.0.1:9/"
FAKE_TOKEN = "hf_FAKEtokenAUDIT0123456789abcdefXYZ"
NO_SESSION = m1.NO_SESSION

ENTRY_KEYS = {"seq", "id", "kind", "action", "result", "ts", "time", "target", "actor", "details", "persisted", "open"}
PAGE_KEYS = {"entries", "next_before", "matched", "persisted", "audit", "warnings"}
STATUS_KEYS = {"persisted", "error", "since", "in_memory", "lost", "max_bytes", "ring_max"}
SERVER_ACTOR = {"type": "server", "label": "server"}
PUBLIC_API_ACTOR = {"type": "public_api", "label": "public API (unknown)"}
TRUSTED_PROXY_WARNING = ("PROVISION_ADMIN_TRUSTED_PROXIES without PROVISION_ADMIN_ALLOW_FROM only affects the client address "
                         "the audit log records: any address may use /admin")

# fixed constants of the store (server-admin-audit.cpp)
REFUSALS_PER_WINDOW = 60
OBSERVED_PER_WINDOW = 30
CLIENTS_MAX = 256
SAMPLE_MAX = 8
ITEMS_MAX = 16
LINE_MAX = 4096


def _port() -> int:
    """the first port of this xdist worker's block of test_admin_upload.py (m1._ports(): five per worker from 8680,
    ADMIN_UPLOAD_TEST_PORT_BASE moves them). A worker runs one test at a time and every test stops its routers, so the
    block is free whichever of the two files the test comes from, with any number of workers (tests.sh runs -n auto).
    A restart reuses the port once the old router has released it (m1._restart)."""
    return m1._ports()[0]


@pytest.fixture(autouse=True)
def create_server(monkeypatch, tmp_path):
    global server, env, work_dir, state_dir, models_dir
    env = monkeypatch
    work_dir = tmp_path
    state_dir = str(tmp_path / "admin-state")
    models_dir = str(tmp_path / "models")
    os.makedirs(models_dir)
    # the server inherits os.environ (see ServerProcess.start), so start every test from a clean slate
    for name in list(os.environ):
        upper = name.upper()
        if (upper.startswith("PROVISION_ADMIN") or upper.startswith("LLAMA_ARG_")
                or upper in ("MODEL_DEVICES", "HF_TOKEN", "MODEL_ENDPOINT", "HF_ENDPOINT")):
            monkeypatch.delenv(name)
    monkeypatch.setenv("MODEL_ENDPOINT", CLOSED_ENDPOINT)
    m1.env = monkeypatch
    m1.work_dir = tmp_path
    m1.state_dir = state_dir
    m1.models_dir = models_dir
    server = m1._router(_port(), "server.log", models_dir)
    m1.server = server
    stage1.server = server
    stage1.env = monkeypatch
    stage1.state_dir = state_dir


@pytest.fixture
def hub(monkeypatch, tmp_path_factory):
    """the router of this test talks to a fake Hugging Face endpoint and has an empty model cache of its own"""
    # not below tmp_path: the cache nests deeply and the downloader cannot go beyond MAX_PATH on Windows
    monkeypatch.setenv("LLAMA_CACHE", str(tmp_path_factory.mktemp("cache")))
    fake = stage1._FakeHub()
    monkeypatch.setenv("MODEL_ENDPOINT", fake.url)
    server.offline = False
    yield fake
    server.stop()  # before its endpoint goes away
    fake.close()


#
# helpers
#


def _enable(**variables: str | None) -> None:
    """admin layer on, key from the environment, state in a directory of this test, no GPU inventory (see m1._enable)"""
    m1._enable(**variables)


def _call(method: str, path: str, data: Any = None, key: str | None = ADMIN_KEY, headers: dict | None = None,
          timeout: float = 60) -> ServerResponse:
    return m1._call(method, path, data, key=key, headers=headers, timeout=timeout)


def _ok(res: ServerResponse, status: int = 200) -> Any:
    return m1._ok(res, status)


def _audit(key: str | None = ADMIN_KEY, headers: dict | None = None, **params) -> ServerResponse:
    query = urlencode({k: v for k, v in params.items() if v is not None})
    return _call("GET", "/admin/audit" + ("?" + query if query else ""), key=key, headers=headers)


def _page(headers: dict | None = None, **params) -> dict:
    body = _ok(_audit(headers=headers, **params))
    assert set(body) == PAGE_KEYS, body.keys()
    return body


def _download(**params) -> requests.Response:
    """GET /admin/audit?format=jsonl (the text as it is: one line would parse as a JSON object)"""
    query = urlencode({"format": "jsonl", **params})
    return requests.get(server.make_url("/admin/audit?" + query), headers={"X-Admin-Key": ADMIN_KEY}, timeout=60)


def _download_lines(**params) -> list[dict]:
    r = _download(**params)
    assert r.status_code == 200, r.text
    return [json.loads(line) for line in r.text.splitlines()]


def _entries(headers: dict | None = None, **params) -> list[dict]:
    """every entry that matches (all pages), newest first"""
    out: list[dict] = []
    before = None
    for _ in range(1000):
        page = _page(headers=headers, **{"limit": 500, **params, "before": before})
        out += page["entries"]
        before = page["next_before"]
        if before is None:
            return out
    raise AssertionError("GET /admin/audit did not come to an end")


def _top() -> int:
    """the highest seq given out so far (a merged record keeps the seq of its first line, its updates count higher)"""
    return max((max(e["seq"], e.get("updated_seq", 0)) for e in _entries()), default=0)


def _since(top: int, **params) -> list[dict]:
    """the records that started after `top`, newest first"""
    return [e for e in _entries(**params) if e["seq"] > top]


def _one(top: int) -> dict:
    """the single record that started after `top`"""
    new = _since(top)
    assert len(new) == 1, f"expected one new record, got {[(e['action'], e['result']) for e in new]}"
    return new[0]


def _path(name: str) -> str:
    return os.path.join(state_dir, name)


def _file_lines(name: str) -> list[dict]:
    path = _path(name)
    if not os.path.exists(path):
        return []
    with open(path, "rb") as f:
        return [json.loads(raw.decode("utf-8")) for raw in f.read().split(b"\n") if raw.strip()]


def _file_bytes(name: str) -> bytes:
    with open(_path(name), "rb") as f:
        return f.read()


def _audit_files() -> dict[str, int]:
    if not os.path.isdir(state_dir):
        return {}
    return {n: os.path.getsize(_path(n)) for n in sorted(os.listdir(state_dir)) if n.startswith("admin-audit")}


def _assert_lines_capped(limit: int = LINE_MAX) -> None:
    for name in _audit_files():
        longest = max((len(line) + 1 for line in _file_bytes(name).split(b"\n") if line), default=0)
        assert longest <= limit, f"{name}: a line of {longest} bytes"


def _status() -> dict:
    return _ok(_call("GET", "/admin/status"))


def _diagnostics_item(item_id: str) -> dict:
    items = [i for i in _ok(_call("GET", "/admin/diagnostics"))["items"] if i["id"] == item_id]
    assert len(items) == 1, item_id
    return items[0]


def _post_settings(data: dict, headers: dict | None = None) -> dict:
    return _ok(_call("POST", "/admin/settings", data, headers=headers))


def _wait_for(fn, timeout: float, what: str):
    """polls fn until it returns something truthy, returns that"""
    deadline = time.time() + timeout
    while True:
        value = fn()
        if value:
            return value
        if time.time() > deadline:
            raise AssertionError(f"timed out waiting for {what}")
        time.sleep(0.2)


def _wait_record(action: str, timeout: float = 60, **params) -> list[dict]:
    return _wait_for(lambda: [e for e in _entries(**params) if e["action"] == action], timeout, f"a record {action}")


def _tiny() -> str:
    return stage1._tiny_gguf()


def _inventory(*names: str) -> str:
    """a canned device inventory with fake GPUs this build cannot open (a child started on one fails at once)"""
    devices = [{"name": n, "backend": "CUDA", "description": "fake GPU", "type": "gpu", "memory_total": 4294967296,
                "memory_free": 3000000000, "device_id": f"0000:0{i + 1}:00.0"} for i, n in enumerate(names)]
    path = work_dir / "inventory.json"
    path.write_text(json.dumps({"ok": True, "backends": ["CPU"], "devices": devices}) + "\n", encoding="utf-8")
    return "file:" + str(path)


def _xff(address: str) -> dict:
    return {"X-Forwarded-For": address}


def _flood(n: int, path: str = "/admin/settings", key: str | None = "wrong-key", prefix: str = "10.77",
           clients: int | None = None, threads: int = 8) -> dict[int, int]:
    """n refused requests through the trusted proxy 127.0.0.1 (this process); request i comes from client i % clients,
    named in X-Forwarded-For as <prefix>.a.b. Returns {status: count}."""
    clients = clients or n
    codes: dict[int, int] = {}
    lock = threading.Lock()
    url = server.make_url(path)

    def worker(first: int) -> None:
        session = requests.Session()
        for i in range(first, n, threads):
            c = i % clients
            headers = _xff(f"{prefix}.{(c // 250) % 250}.{c % 250 + 1}")
            if key is not None:
                headers["X-Admin-Key"] = key
            try:
                status = session.get(url, headers=headers, timeout=30).status_code
            except requests.RequestException:
                status = -1
            with lock:
                codes[status] = codes.get(status, 0) + 1

    workers = [threading.Thread(target=worker, args=(t,)) for t in range(threads)]
    for t in workers:
        t.start()
    for t in workers:
        t.join()
    return codes


def _raw_post(path: str, body: dict, header_lines: list[str]) -> tuple[int | None, Any]:
    """a POST with the admin key and header lines written as they are (a header may come twice)"""
    data = json.dumps(body).encode()
    lines = [f"POST {path} HTTP/1.1", f"Host: {server.server_host}:{server.server_port}", f"X-Admin-Key: {ADMIN_KEY}",
             "Content-Type: application/json", f"Content-Length: {len(data)}", *header_lines]
    s = socket.create_connection((server.server_host, server.server_port), timeout=30)
    try:
        s.sendall(("\r\n".join(lines) + "\r\n\r\n").encode("latin-1") + data)
        return m1._read_response(s)
    finally:
        s.close()


def _secret_variants(path: str) -> list[bytes]:
    """a path as it may appear in a JSON file: native, with forward slashes, with JSON-escaped backslashes"""
    p = os.path.abspath(path)
    variants = {p, p.replace("\\", "/"), p.replace("\\", "\\\\")}
    return [v.encode("utf-8") for v in variants if v]


# a normal stop of the router (its shutdown code runs): SIGINT on POSIX; on Windows ServerProcess.stop() kills the
# process, so the router gets a hidden console of its own and a CTRL_C from a helper attached to that console

_CTRL_C = r'''
import ctypes, sys
k = ctypes.windll.kernel32
k.FreeConsole()
if not k.AttachConsole(int(sys.argv[1])):
    sys.exit(2)
k.SetConsoleCtrlHandler(None, True)
sys.exit(0 if k.GenerateConsoleCtrlEvent(0, 0) else 3)
'''


def _start_stoppable() -> None:
    if os.name != "nt":
        server.start()
        return
    import ctypes
    original = subprocess.Popen

    def popen(args, **kw):
        ctypes.windll.kernel32.SetConsoleCtrlHandler(None, False)  # CTRL+C not ignored: inherited by the child
        kw["creationflags"] = subprocess.CREATE_NEW_CONSOLE
        si = subprocess.STARTUPINFO()
        si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        si.wShowWindow = 0  # SW_HIDE
        kw["startupinfo"] = si
        return original(args, **kw)

    subprocess.Popen = popen  # utils.py looks it up at call time
    try:
        server.start()
    finally:
        subprocess.Popen = original


def _stop_gracefully(timeout: float = 60) -> int:
    proc = server.process
    assert proc is not None and proc.poll() is None
    if os.name == "nt":
        r = subprocess.run([sys.executable, "-c", _CTRL_C, str(proc.pid)], timeout=30)
        assert r.returncode == 0, f"cannot send CTRL_C to the router (helper exit code {r.returncode})"
    else:
        proc.send_signal(signal.SIGINT)
    code = proc.wait(timeout)
    server.stop()
    return code


#
# the store, its status and the route
#


def test_audit_started_and_reported():
    _enable()
    server.start()

    page = _page()
    assert page["persisted"] is True and page["warnings"] == [] and page["next_before"] is None and page["matched"] == 1
    [e] = page["entries"]
    assert set(e) == ENTRY_KEYS, e
    assert (e["kind"], e["action"], e["result"], e["target"], e["actor"]) == ("server", "audit.started", "ok", "", SERVER_ACTOR)
    assert e["details"]["persisted"] is True and e["details"]["max_bytes"] == 4 * MiB
    assert isinstance(e["details"]["version"], str) and e["details"]["version"] != ""
    assert e["persisted"] is True and e["open"] is False and e["id"] == str(e["seq"]) and e["seq"] == 1
    assert abs(e["ts"] / 1000 - time.time()) < 600
    assert time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(e["ts"] // 1000)) == e["time"]
    assert _audit().headers.get("Cache-Control") == "no-store"

    status = _status()
    assert "audit" in status["features"] and "audit_not_persisted" not in status["warnings"]
    assert status["audit"] == {"persisted": True, "error": None, "since": None, "in_memory": 0, "lost": 0,
                               "max_bytes": 4 * MiB, "ring_max": 1000}
    size = os.path.getsize(_path(MAIN_FILE))
    storage = _ok(_call("GET", "/admin/storage"))["audit"]
    assert storage == {"persisted": True, "files": [{"name": MAIN_FILE, "bytes": size, "store": "changes"}],
                       "total_bytes": size, "max_bytes": 4 * MiB}
    item = _diagnostics_item("audit")
    assert item["label"] == "Audit log" and item["status"] == "ok" and item["value"]["persisted"] is True
    assert item["value"]["files"] == storage["files"]

    # the file: one line, the record without the fields the API adds; the refusal file is created on the first refusal
    [line] = _file_lines(MAIN_FILE)
    assert line == {k: v for k, v in e.items() if k not in ("id", "persisted", "open")}
    assert _audit_files() == {MAIN_FILE: size}
    if os.name != "nt":
        assert stat.S_IMODE(os.stat(_path(MAIN_FILE)).st_mode) == 0o600

    # the route needs the key like every admin route; the refusal goes to the file of its own
    m1._err(_audit(key=None), 401, "admin_key_required")
    m1._err(_audit(key="wrong-key", format="jsonl"), 401, "admin_key_required")
    assert [(l["action"], l["target"]) for l in _file_lines(REFUSALS_FILE)] == [("access.refused", "/admin/audit")] * 2
    if os.name != "nt":
        assert stat.S_IMODE(os.stat(_path(REFUSALS_FILE)).st_mode) == 0o600
    assert len(_file_lines(MAIN_FILE)) == 1


def test_audit_read_only_requests_make_no_record():
    tiny = _tiny()
    m1._place(tiny, "tinyA.gguf")
    _enable(PROVISION_ADMIN_INVENTORY=_inventory("CUDA0"))
    server.start()
    sid = m1._create("pending-up", [tiny])["upload"]["id"]
    top = _top()

    for path in ("/admin/status", "/admin/settings", "/admin/storage", "/admin/models/settings", "/admin/gpus",
                 "/admin/diagnostics", "/admin/models/upload", "/admin/models/upload?id=" + sid, "/admin/audit",
                 "/admin/audit?format=jsonl"):
        assert _call("GET", path).status_code == 200, path
    _call("GET", "/admin/models/log?model=tinyA")
    _call("GET", "/admin/status", key=None)  # the no-key probe answers 200 here (no allow-list)
    _call("POST", "/admin/models/download/check", {"model": "ggml-org/none-GGUF"})  # read-only, whatever it answers
    _ok(_call("POST", "/admin/models/settings/check", {"model": "tinyA", "settings": {"ctx-size": 512}}, timeout=120))
    _ok(_call("POST", "/admin/models/upload", {"name": "dry", "dry_run": True, "files": m1._files([tiny])}))
    _ok(_call("POST", "/admin/models/delete", {"model": "tinyA", "dry_run": True}))
    # renaming an upload is not recorded either: its outcome (published / rejected) is
    _ok(_call("POST", "/admin/models/upload/update", {"id": sid, "name": "pending-up-2"}))
    # nor are chunks
    _ok(m1._chunk(sid, 0, 0, open(tiny, "rb").read()[:64 * KiB]))
    assert _since(top) == []
    assert _top() == top


#
# change records
#


def test_audit_one_record_per_changing_route():
    m1._place(_tiny(), "tinyA.gguf")
    _enable(PROVISION_ADMIN_INVENTORY=_inventory("CUDA0"))
    server.start()

    # global settings: old -> new of what changed, the keys of the request
    top = _top()
    _ok(_call("POST", "/admin/settings", {"models_max": 3}, headers={"User-Agent": "audit-test/1.0"}))
    e = _one(top)
    assert (e["kind"], e["action"], e["result"], e["target"]) == ("change", "settings.update", "ok", "settings")
    assert e["details"] == {"changed": True, "changes": {"models_max": {"from": 2, "to": 3}}, "keys": ["models_max"]}
    assert e["actor"] == {"type": "client", "peer": "127.0.0.1", "client": "127.0.0.1", "via_proxy": False,
                          "user_agent": "audit-test/1.0", "label": "127.0.0.1"}
    assert e["persisted"] is True and e["open"] is False

    top = _top()
    _post_settings({"default_device": "CUDA0", "pinned": ["tinyA"]})
    e = _one(top)
    changes = e["details"]["changes"]
    assert set(changes) == {"default_device", "pinned"} and sorted(e["details"]["keys"]) == ["default_device", "pinned"]
    assert changes["default_device"]["to"] == "CUDA0" and changes["pinned"] == {"from": [], "to": ["tinyA"]}

    # the same values again: still one record, nothing changed
    top = _top()
    _post_settings({"models_max": 3})
    e = _one(top)
    assert e["details"] == {"changed": False, "changes": {}, "keys": ["models_max"]} and e["result"] == "ok"

    # a refused change: one record with result error, the status and the error type (never the message)
    top = _top()
    m1._err(_call("POST", "/admin/settings", {"models_max": 0}), 400, "invalid_request_error")
    e = _one(top)
    assert (e["action"], e["result"], e["target"]) == ("settings.update", "error", "settings")
    assert e["details"] == {"status": 400, "code": "invalid_request_error"}

    # per-model settings: update, the same again, reset, an unknown model
    top = _top()
    _ok(_call("POST", "/admin/models/settings", {"model": "tinyA", "settings": {"ctx-size": 512}}))
    e = _one(top)
    assert (e["kind"], e["action"], e["result"], e["target"]) == ("change", "model_settings.update", "ok", "tinyA")
    assert e["details"]["changed"] is True and e["details"]["changes"] == {"ctx-size": {"from": None, "to": "512"}}
    top = _top()
    _ok(_call("POST", "/admin/models/settings", {"model": "tinyA", "settings": {"ctx-size": 512}}))
    e = _one(top)
    assert e["action"] == "model_settings.update" and e["details"]["changed"] is False
    top = _top()
    _ok(_call("POST", "/admin/models/settings", {"model": "tinyA", "reset": True}))
    e = _one(top)
    assert (e["action"], e["target"]) == ("model_settings.reset", "tinyA")
    assert e["details"]["changed"] is True and e["details"]["changes"] == {"ctx-size": {"from": "512", "to": None}}
    top = _top()
    res = _call("POST", "/admin/models/settings", {"model": "nope-model", "settings": {"ctx-size": 512}})
    assert res.status_code == 404, res.body
    e = _one(top)
    assert (e["action"], e["result"], e["target"]) == ("model_settings.update", "error", "nope-model")
    assert e["details"]["status"] == 404 and set(e["details"]) == {"status", "code"}

    # GPU re-detection
    top = _top()
    _ok(_call("POST", "/admin/gpus/rescan", {}))
    e = _one(top)
    assert (e["kind"], e["action"], e["result"], e["target"]) == ("change", "gpus.rescan", "ok", "gpus")
    assert e["details"] == {"devices": 1, "error": None}

    # every record of this test is a change record of this client, in the change file
    changes = _entries(kind="change")
    assert len(changes) == 9 and all(c["actor"]["client"] == "127.0.0.1" for c in changes)
    assert [l["seq"] for l in _file_lines(MAIN_FILE)] == list(range(1, 11))
    _assert_lines_capped()


@pytest.mark.parametrize("trusted", [None, "127.0.0.1 10.9.0.0/16"])
def test_audit_identity_of_the_client(trusted: str | None):
    """who made a change: the TCP peer, the client a trusted proxy names (the allow-list may be off), a raw
    X-Forwarded-For marked unverified unless a trusted proxy sent it, the User-Agent; header values are cut"""
    _enable(PROVISION_ADMIN_TRUSTED_PROXIES=trusted)
    server.start()
    value = [3]

    def change(headers: dict) -> dict:
        top = _top()
        value[0] = 5 - value[0]  # 2, 3, 2, ...
        _post_settings({"models_max": value[0]}, headers=headers)
        return _one(top)["actor"]

    long_agent = "agent-\xe9-" + "x" * 300  # a byte that is not printable ASCII, longer than the cap
    long_forwarded = ", ".join(f"192.0.2.{i}" for i in range(1, 30))
    if trusted is None:
        a = change({})
        assert a == {"type": "client", "peer": "127.0.0.1", "client": "127.0.0.1", "via_proxy": False,
                     "user_agent": a["user_agent"], "label": "127.0.0.1"}
        # anyone can write X-Forwarded-For: shown, never taken as the client
        a = change({**_xff("6.6.6.6"), "User-Agent": long_agent})
        assert a["client"] == "127.0.0.1" and a["via_proxy"] is False
        assert a["forwarded_for"] == "6.6.6.6" and a["forwarded_trusted"] is False
        assert a["label"] == "127.0.0.1 (X-Forwarded-For 6.6.6.6, unverified)"
        assert a["user_agent"] == ("agent-?-" + "x" * 300)[:128] + "..."
        a = change(_xff(long_forwarded))
        assert a["forwarded_for"] == long_forwarded[:128] + "..." and a["forwarded_trusted"] is False
        assert a["label"] == f"127.0.0.1 (X-Forwarded-For {long_forwarded[:64]}..., unverified)"
        return

    # through the trusted proxy (this process): the client it names, verified
    a = change(_xff("203.0.113.7"))
    assert a == {"type": "client", "peer": "127.0.0.1", "client": "203.0.113.7", "via_proxy": True,
                 "forwarded_for": "203.0.113.7", "forwarded_trusted": True, "user_agent": a["user_agent"],
                 "label": "203.0.113.7 via trusted proxy 127.0.0.1"}
    # a chain: hops of trusted proxies are skipped, a hop the client wrote left of the first untrusted one is not taken
    a = change(_xff("198.51.100.1, 203.0.113.9, 10.9.0.4"))
    assert a["client"] == "203.0.113.9" and a["via_proxy"] is True and a["forwarded_trusted"] is True
    assert a["forwarded_for"] == "198.51.100.1, 203.0.113.9, 10.9.0.4"
    assert a["label"] == "203.0.113.9 via trusted proxy 127.0.0.1"
    # what the trusted proxy sent cannot be used: the proxy itself is the client, with the reason
    a = change(_xff("not-an-address"))
    assert a["client"] == "127.0.0.1" and a["via_proxy"] is False and a["forwarded_trusted"] is True
    assert a["note"] == "X-Forwarded-For has an entry that is not an address" and a["label"] == "127.0.0.1"
    a = change({})
    assert a["client"] == "127.0.0.1" and a["note"] == "the trusted proxy sent no X-Forwarded-For"
    assert "forwarded_for" not in a and a["label"] == "127.0.0.1"
    # two spellings of the name (as from a proxy that appends a header of its own): not used for the client
    top = _top()
    status, body = _raw_post("/admin/settings", {"models_max": 3}, ["X-Forwarded-For: 203.0.113.7",
                                                                     "x-forwarded-for: 198.51.100.2"])
    assert status == 200, body
    a = _one(top)["actor"]
    assert a["client"] == "127.0.0.1" and a["via_proxy"] is False and a["note"] == "X-Forwarded-For was sent more than once"
    assert sorted(a["forwarded_for"].split(", ")) == ["198.51.100.2", "203.0.113.7"]
    assert a["forwarded_trusted"] is True and a["label"] == "127.0.0.1"
    # the trusted proxies matter for the audit even with the allow-list off: the warning says so
    assert TRUSTED_PROXY_WARNING in _diagnostics_item("ip_allowlist")["hint"]
    assert TRUSTED_PROXY_WARNING in m1._log()


def test_audit_identity_x_forwarded_for_line_sent_twice():
    """two X-Forwarded-For lines of the same spelling (server-http.cpp keeps every line in req.forwarded_for; the
    header map holds only the last one): never used for the client, and both shown in the order they came"""
    _enable(PROVISION_ADMIN_TRUSTED_PROXIES="127.0.0.1")
    server.start()
    top = _top()
    status, body = _raw_post("/admin/settings", {"models_max": 3}, ["X-Forwarded-For: 203.0.113.7",
                                                                     "X-Forwarded-For: 198.51.100.2"])
    assert status == 200, body
    a = _one(top)["actor"]
    assert a["client"] == "127.0.0.1" and a["via_proxy"] is False and a["note"] == "X-Forwarded-For was sent more than once"
    assert a["forwarded_for"] == "203.0.113.7, 198.51.100.2" and a["label"] == "127.0.0.1"

    # with the allow-list the repeated line is refused whatever its order (the peer, not on the list, is the client)
    _enable(PROVISION_ADMIN_TRUSTED_PROXIES="127.0.0.1", PROVISION_ADMIN_ALLOW_FROM="10.64.0.0/24")
    m1._restart()
    for lines in (["X-Forwarded-For: 203.0.113.9", "X-Forwarded-For: 10.64.0.7"],
                  ["X-Forwarded-For: 10.64.0.7", "X-Forwarded-For: 203.0.113.9"]):
        status, body = _raw_post("/admin/settings", {"models_max": 3}, lines)
        assert status == 403 and body["error"]["type"] == "admin_forbidden", (lines, status, body)
        assert body["error"]["message"].endswith(" 127.0.0.1"), body
    [refused] = _entries(headers=_xff("10.64.0.7"), kind="refusal")
    assert refused["count"] == 2 and refused["actor"]["client"] == "127.0.0.1"
    assert refused["actor"]["note"] == "X-Forwarded-For was sent more than once"
    assert [e["action"] for e in _entries(headers=_xff("10.64.0.7"), kind="change")] == ["settings.update"]


#
# refused requests
#


def test_audit_refusals_are_merged_per_client_path_and_reason():
    _enable(PROVISION_ADMIN_TRUSTED_PROXIES="127.0.0.1")
    server.start()
    a, b = _xff("192.0.2.50"), _xff("192.0.2.51")
    for _ in range(5):
        m1._err(_call("GET", "/admin/settings", key="nope-key", headers=a), 401, "admin_key_required")
    for _ in range(3):
        m1._err(_call("GET", "/admin/settings", key=None, headers=a), 401, "admin_key_required")
    for _ in range(2):
        m1._err(_call("GET", "/admin/storage", key="nope-key", headers=a), 401, "admin_key_required")
    m1._err(_call("GET", "/admin/settings", key="nope-key", headers=b), 401, "admin_key_required")
    # the chunk route answers before the body (pre-body hook): recorded the same way
    status, body = m1._raw(m1._chunk_target(NO_SESSION), {**m1._chunk_headers(key=None), **_xff("192.0.2.77")}, b"", 8 * MiB)
    assert status == 401 and body["error"]["type"] == "admin_key_required", body
    status, body = m1._raw(m1._chunk_target(NO_SESSION), {**m1._chunk_headers(key="nope-key"), **_xff("192.0.2.77")}, b"", 1000)
    assert status == 401, body

    refusals = _entries(kind="refusal")
    by = {(e["actor"]["client"], e["target"], e["details"]["reason"]): e for e in refusals}
    assert len(by) == len(refusals) == 6, sorted(by)
    expected = {
        ("192.0.2.50", "/admin/settings", "wrong_key"): 5,
        ("192.0.2.50", "/admin/settings", "no_key"): 3,
        ("192.0.2.50", "/admin/storage", "wrong_key"): 2,
        ("192.0.2.51", "/admin/settings", "wrong_key"): 1,
        ("192.0.2.77", "/admin/models/upload/chunk", "no_key"): 1,
        ("192.0.2.77", "/admin/models/upload/chunk", "wrong_key"): 1,
    }
    assert {k: e["count"] for k, e in by.items()} == expected
    e = by[("192.0.2.50", "/admin/settings", "wrong_key")]
    assert set(e) == ENTRY_KEYS | {"count", "updated_seq"}
    assert (e["kind"], e["action"], e["result"], e["open"], e["persisted"]) == ("refusal", "access.refused", "refused", True, True)
    assert e["details"] == {"status": 401, "code": "admin_key_required", "reason": "wrong_key", "probe": False,
                            "count": 5, "first": e["details"]["first"], "last": e["details"]["last"]}
    assert e["details"]["first"] <= e["details"]["last"]
    assert e["actor"]["label"] == "192.0.2.50 via trusted proxy 127.0.0.1" and e["actor"]["via_proxy"] is True

    # the refusal file got the first line of every window at once (count 1); the change file nothing
    lines = _file_lines(REFUSALS_FILE)
    assert len(lines) == 6 and all(l["kind"] == "refusal" and l["count"] == 1 and "group" not in l for l in lines)
    assert sorted(l["seq"] for l in lines) == sorted(e["seq"] for e in refusals)
    assert [l["action"] for l in _file_lines(MAIN_FILE)] == ["audit.started"]


def test_audit_refusal_window_ends_with_its_final_count():
    _enable(PROVISION_ADMIN_TEST_AUDIT_MERGE_MS="1500")
    server.start()
    for _ in range(4):
        m1._err(_call("GET", "/admin/settings", key=None), 401, "admin_key_required")
    [e] = _entries(kind="refusal")
    assert e["count"] == 4 and e["open"] is True

    # the window ends: the events thread writes its count as an update line of the record
    e = _wait_for(lambda: [x for x in _entries(kind="refusal") if not x["open"]], 15, "the window to end")[0]
    assert e["count"] == 4 and e["details"]["count"] == 4 and e["updated_seq"] > e["seq"]
    first, update = _file_lines(REFUSALS_FILE)
    assert first["seq"] == e["seq"] and first["count"] == 1 and "group" not in first
    assert update["seq"] == e["updated_seq"] and update["group"] == e["seq"] and update["count"] == 4

    # a refusal after the window opens a new record
    m1._err(_call("GET", "/admin/settings", key=None), 401, "admin_key_required")
    newer, older = _entries(kind="refusal")
    assert older["seq"] == e["seq"] and older["count"] == 4
    assert newer["seq"] > e["updated_seq"] and newer["count"] == 1 and newer["open"] is True
    assert [l["action"] for l in _file_lines(MAIN_FILE)] == ["audit.started"]


def test_audit_allow_list_refusals_and_the_status_probe():
    _enable(PROVISION_ADMIN_TRUSTED_PROXIES="127.0.0.1", PROVISION_ADMIN_ALLOW_FROM="10.200.0.0/16")
    server.start()
    allowed = _xff("10.200.1.1")
    refused = _xff("203.0.113.5")
    _post_settings({"models_max": 3}, headers=allowed)
    for _ in range(3):
        m1._assert_forbidden(_call("GET", "/admin/settings", headers=refused), "203.0.113.5")
    # the audit itself is protected by the allow-list (the refusal is recorded)
    m1._assert_forbidden(_audit(headers=refused), "203.0.113.5")
    # the proxy itself without X-Forwarded-For
    m1._assert_forbidden(_call("POST", "/admin/settings", {"models_max": 2}), "127.0.0.1")
    # the probe every browser of a refused address makes on every page load, with and without a key
    for i in range(20):
        m1._assert_forbidden(_call("GET", "/admin/status", key=None, headers=_xff(f"172.16.0.{i + 1}")), f"172.16.0.{i + 1}")
    m1._assert_forbidden(_call("GET", "/admin/status", headers=_xff("172.16.0.1")), "172.16.0.1")
    # the chunk route, before its body
    status, body = m1._raw(m1._chunk_target(NO_SESSION), {**m1._chunk_headers(), **_xff("203.0.113.9")}, b"", 8 * MiB)
    assert status == 403 and body["error"]["type"] == "admin_forbidden", body

    refusals = _entries(headers=allowed, kind="refusal")
    singles = {(e["actor"]["client"], e["target"]): e for e in refusals if not e["details"]["probe"]}
    assert {k: e["count"] for k, e in singles.items()} == {
        ("203.0.113.5", "/admin/settings"): 3, ("203.0.113.5", "/admin/audit"): 1,
        ("127.0.0.1", "/admin/settings"): 1, ("203.0.113.9", "/admin/models/upload/chunk"): 1,
    }
    for e in singles.values():
        assert e["details"]["status"] == 403 and e["details"]["code"] == "admin_forbidden"
        assert e["details"]["reason"] == "not_allowed" and e["result"] == "refused"
    assert singles[("127.0.0.1", "/admin/settings")]["actor"]["note"] == "the trusted proxy sent no X-Forwarded-For"
    # the probe: one record per path over every client, the clients counted and sampled
    [probe] = [e for e in refusals if e["details"]["probe"]]
    assert probe["target"] == "/admin/status" and probe["count"] == 21
    assert probe["actor"] == {"type": "various", "label": "browsers of refused addresses"}
    assert probe["details"]["clients"] == 20 and probe["details"]["clients_capped"] is False
    assert probe["details"]["sample"] == [f"172.16.0.{i + 1}" for i in range(SAMPLE_MAX)]
    # the change of the allowed client, with its address
    [change] = _entries(headers=allowed, kind="change")
    assert change["actor"]["client"] == "10.200.1.1" and change["actor"]["label"] == "10.200.1.1 via trusted proxy 127.0.0.1"


def test_audit_status_probe_is_not_eaten_by_the_cap():
    """a flood of refusals uses up the records of a window (60, then one summary): the no-key status probe of refused
    addresses still has its record of its own, with every request counted"""
    _enable(PROVISION_ADMIN_TRUSTED_PROXIES="127.0.0.1", PROVISION_ADMIN_ALLOW_FROM="10.0.0.0/8",
            PROVISION_ADMIN_TEST_AUDIT_MERGE_MS="600000")  # a window of 10 minutes: the run stays in one, almost always
    server.start()
    assert _flood(200) == {401: 200}  # allowed addresses (10.77.x.y) with a wrong key
    for i in range(30):
        m1._assert_forbidden(_call("GET", "/admin/status", key=None, headers=_xff(f"172.16.1.{i + 1}")), f"172.16.1.{i + 1}")
    # and refused addresses on another route: over the cap, into the summary
    for i in range(5):
        m1._assert_forbidden(_call("GET", "/admin/settings", headers=_xff(f"172.16.2.{i + 1}")), f"172.16.2.{i + 1}")

    refusals = _entries(headers=_xff("10.200.1.1"), kind="refusal")
    [probe] = [e for e in refusals if e["details"].get("probe")]
    assert probe["target"] == "/admin/status" and probe["count"] == 30
    assert probe["details"]["clients"] == 30 and probe["details"]["clients_capped"] is False
    assert probe["actor"] == {"type": "various", "label": "browsers of refused addresses"}
    singles = [e for e in refusals if e["action"] == "access.refused" and not e["details"]["probe"]]
    more = [e for e in refusals if e["action"] == "access.refused_more"]
    assert 1 <= len(more) <= 2 and len(singles) <= REFUSALS_PER_WINDOW * 2, (len(singles), len(more))
    if len(more) == 1 and len(singles) == REFUSALS_PER_WINDOW:  # the usual case: one window
        assert more[0]["count"] == 200 + 5 - REFUSALS_PER_WINDOW
        assert set(more[0]["details"]["items"]) == {"/admin/settings"}
    assert sum(e["count"] for e in refusals) == 200 + 30 + 5


def test_audit_locked_allow_list_refusals_are_recorded():
    """an allow-list entry that cannot be read refuses every /admin request (fail closed): the audit says why"""
    _enable(PROVISION_ADMIN_ALLOW_FROM="not-an-address")
    server.start()
    for _ in range(2):
        m1._assert_forbidden(_call("GET", "/admin/settings"), "127.0.0.1")
    m1._assert_forbidden(_call("GET", "/admin/status", key=None), "127.0.0.1")
    m1._assert_forbidden(_audit(), "127.0.0.1")
    # nobody can read the route: the file tells
    lines = {(l["target"], l["details"]["probe"]): l for l in _file_lines(REFUSALS_FILE)}
    assert set(lines) == {("/admin/settings", False), ("/admin/status", True), ("/admin/audit", False)}
    assert all(l["details"]["reason"] == "allow_list_locked" and l["details"]["status"] == 403 for l in lines.values())


def test_audit_refusal_flood_is_capped_and_never_touches_the_change_records():
    _enable(PROVISION_ADMIN_TRUSTED_PROXIES="127.0.0.1")
    server.start()
    for v in (3, 2, 3, 2):
        _post_settings({"models_max": v})
    changes_before = _entries(kind="change")
    main_before = _file_bytes(MAIN_FILE)

    n = 500
    assert _flood(n) == {401: n}
    refusals = _entries(kind="refusal")
    singles = [e for e in refusals if e["action"] == "access.refused"]
    more = [e for e in refusals if e["action"] == "access.refused_more"]
    # a window of 60 s: at most 60 records of single clients, then one summary; the flood (seconds) may cross into
    # the next window (they are counted in fixed windows), never into a third
    assert 1 <= len(more) <= 2 and len(singles) <= REFUSALS_PER_WINDOW * 2, (len(singles), len(more))
    if len(more) == 1 and len(singles) == REFUSALS_PER_WINDOW:  # the usual case: one window
        assert more[0]["count"] == n - REFUSALS_PER_WINDOW
    assert sum(e["count"] for e in refusals) == n
    assert all(e["count"] == 1 and e["actor"]["client"].startswith("10.77.") for e in singles)
    for e in more:
        # every request of the flood came from a client of its own: the summary counts them up to the cap
        d = e["details"]
        assert (e["kind"], e["result"], e["target"]) == ("refusal", "refused", "")
        assert e["actor"] == {"type": "various", "label": "several clients"}
        assert d["count"] == e["count"] and d["clients"] == min(e["count"], CLIENTS_MAX)
        assert d["clients_capped"] is (e["count"] > CLIENTS_MAX)
        assert len(d["sample"]) == min(e["count"], SAMPLE_MAX) and all(s.startswith("10.77.") for s in d["sample"])
        assert d["items"] == ["/admin/settings"] and d["items_capped"] is False
    # the refusal file holds the first lines of the windows (the counts follow when they end); the change file and
    # every change record are untouched
    assert len(singles) + len(more) <= len(_file_lines(REFUSALS_FILE)) <= len(singles) + 2 * len(more)
    assert _file_bytes(MAIN_FILE) == main_before
    assert _entries(kind="change") == changes_before
    _assert_lines_capped()
    # the admin key works right after the flood, and is recorded
    top = _top()
    _post_settings({"models_max": 3})
    assert _one(top)["details"]["changes"] == {"models_max": {"from": 2, "to": 3}}


@pytest.mark.slow
@pytest.mark.skipif(not is_slow_test_allowed(), reason="skipping slow test: 10,000 refused requests")
def test_audit_ten_thousand_refusals_from_a_thousand_clients():
    """the acceptance test of the design review: every change record stays in place, files and memory stay bounded"""
    psutil = pytest.importorskip("psutil")
    _enable(PROVISION_ADMIN_TRUSTED_PROXIES="127.0.0.1")
    server.start()
    for v in (3, 2) * 5:
        _post_settings({"models_max": v})
    changes_before = _entries(kind="change")
    main_before = _file_bytes(MAIN_FILE)
    proc = psutil.Process(server.process.pid)

    def memory() -> int:
        mi = proc.memory_info()
        return getattr(mi, "private", mi.rss)

    base = memory()
    t0 = time.time()
    assert _flood(10000, path="/admin/storage", clients=1000, prefix="10.100") == {401: 10000}
    windows = 1 + int((time.time() - t0) // 60) + 1
    refusals = _entries(kind="refusal")
    assert sum(e["count"] for e in refusals) == 10000
    assert len([e for e in refusals if e["action"] == "access.refused"]) <= REFUSALS_PER_WINDOW * windows
    assert len([e for e in refusals if e["action"] == "access.refused_more"]) <= windows
    assert len(_file_lines(REFUSALS_FILE)) <= (REFUSALS_PER_WINDOW + 1) * 2 * windows
    assert _entries(kind="change") == changes_before and _file_bytes(MAIN_FILE) == main_before
    assert memory() - base < 64 * MiB
    top = _top()
    _post_settings({"models_max": 3})
    assert _one(top)["action"] == "settings.update"


def test_audit_shutdown_writes_the_final_counts():
    """a normal stop writes the count of every window that is still open (a crash loses only those counts)"""
    _enable()
    _start_stoppable()
    for _ in range(5):
        m1._err(_call("GET", "/admin/settings", key="nope-key"), 401, "admin_key_required")
    for _ in range(3):
        m1._err(_call("GET", "/admin/storage", key=None), 401, "admin_key_required")
    firsts = {l["target"]: l for l in _file_lines(REFUSALS_FILE)}
    assert {t: l["count"] for t, l in firsts.items()} == {"/admin/settings": 1, "/admin/storage": 1}

    _stop_gracefully()
    lines = _file_lines(REFUSALS_FILE)
    updates = {l["group"]: l["count"] for l in lines if "group" in l}
    assert updates == {firsts["/admin/settings"]["seq"]: 5, firsts["/admin/storage"]["seq"]: 3}, lines

    server.start()
    refusals = {e["target"]: e for e in _entries(kind="refusal")}
    assert {t: (e["count"], e["open"]) for t, e in refusals.items()} == {"/admin/settings": (5, False), "/admin/storage": (3, False)}


#
# uploads and deletes
#


def test_audit_upload_lifecycle():
    tiny = _tiny()
    size = os.path.getsize(tiny)
    data = open(tiny, "rb").read()
    m1._place(tiny, "tinyA.gguf")
    _enable()
    server.start()

    # create and published, never a record per chunk (64 KiB chunks: about 20 of them)
    top = _top()
    sid, s = m1._upload("up-one", [tiny], chunk=64 * KiB, settings={"ctx-size": 384})
    assert s["state"] == "published", m1._brief(s)
    _wait_record("upload.published")
    published, create = _since(top)
    assert (create["kind"], create["action"], create["result"], create["target"]) == ("change", "upload.create", "ok", "up-one")
    assert create["details"] == {"id": sid, "bytes": size, "files": 1}
    assert create["actor"]["client"] == "127.0.0.1" and create["actor"]["type"] == "client"
    # published by the worker, in the name of the client that created the upload
    assert (published["kind"], published["action"], published["result"], published["target"]) == ("change", "upload.published", "ok", "up-one")
    assert published["actor"] == create["actor"]
    assert published["details"] == {"id": sid, "bytes": size, "layout": "file", "sha256": [gg.sha256_file(tiny)],
                                    "arch": "llama", "settings": {"ctx-size": "384"}}

    # another tab: other_client_active is no change; the takeover is
    sid = m1._create("up-take", [tiny])["upload"]["id"]
    _ok(m1._chunk(sid, 0, 0, data[:64 * KiB], client="tab-A"))
    top = _top()
    m1._err(m1._chunk(sid, 0, 64 * KiB, data[64 * KiB:128 * KiB], client="tab-B"), 409, "other_client_active")
    assert _since(top) == []
    _ok(m1._chunk(sid, 0, 64 * KiB, data[64 * KiB:128 * KiB], client="tab-B", takeover=True))
    e = _one(top)
    assert (e["action"], e["result"], e["target"], e["details"]) == ("upload.takeover", "ok", "up-take", {"id": sid})

    # cancel: the name and the bytes that were thrown away
    top = _top()
    m1._cancel(sid)
    e = _one(top)
    assert (e["action"], e["result"], e["target"]) == ("upload.cancel", "ok", "up-take")
    assert e["details"] == {"id": sid, "bytes": 128 * KiB}
    top = _top()
    m1._err(_call("POST", "/admin/models/upload/cancel", {"id": NO_SESSION}), 410, "upload_gone")
    e = _one(top)
    assert (e["action"], e["result"], e["target"]) == ("upload.cancel", "error", NO_SESSION)
    assert e["details"] == {"status": 410, "code": "upload_gone"}

    # a create the server refuses
    top = _top()
    res = _call("POST", "/admin/models/upload", {"name": "tinyA", "files": m1._files([tiny])})
    assert res.status_code == 409, res.body
    e = _one(top)
    assert (e["action"], e["result"], e["target"]) == ("upload.create", "error", "tinyA")
    assert e["details"] == {"status": 409, "code": res.body["error"]["type"]}

    # rejected by the checks (after the first chunk): in the name of the creator
    unknown = gg.copy(tiny, m1._fx("unknown.gguf"), strings={"general.architecture": "nonexistent-arch"})
    top = _top()
    sid = m1._create("bad-arch", [unknown])["upload"]["id"]
    assert m1._send(sid, 0, unknown, chunk=256 * KiB, start=0, first_only=True).status_code == 200
    s = m1._wait_state(sid, ("rejected",), timeout=60)
    _wait_record("upload.rejected")
    rejected, create = _since(top)
    assert create["action"] == "upload.create" and create["details"]["id"] == sid
    assert (rejected["kind"], rejected["action"], rejected["result"], rejected["target"]) == ("change", "upload.rejected", "rejected", "bad-arch")
    assert rejected["actor"] == create["actor"]
    assert rejected["details"] == {"id": sid, "bytes": s["received_bytes"], "code": "unknown_architecture", "file": 0}


def test_audit_upload_expired_and_swept_are_server_records():
    tiny = _tiny()
    abandoned = m1._staging("ab" * 16)  # a session folder without a record and without a lock holder
    os.makedirs(abandoned)
    with open(os.path.join(abandoned, "0.part"), "wb") as f:
        f.write(b"x" * 1000)
    _enable(PROVISION_ADMIN_TEST_UPLOAD_TTL_S="2", PROVISION_ADMIN_TEST_UPLOAD_SWEEP_MS="300",
            PROVISION_ADMIN_TEST_UPLOAD_ORPHAN_S="0")
    server.start()

    [swept] = _wait_record("upload.swept", 30)
    assert (swept["kind"], swept["result"], swept["target"], swept["actor"]) == ("server", "ok", "ab" * 16, SERVER_ACTOR)
    # relative to models_dir, never the path on the host
    assert swept["details"] == {"folder": ".provision-upload/" + "ab" * 16, "foreign": False, "name": None}

    sid = m1._create("will-expire", [tiny])["upload"]["id"]
    _ok(m1._chunk(sid, 0, 0, open(tiny, "rb").read()[:100000]))
    [expired] = _wait_record("upload.expired", 30)
    assert (expired["kind"], expired["result"], expired["target"], expired["actor"]) == ("server", "ok", "will-expire", SERVER_ACTOR)
    assert expired["details"] == {"id": sid, "bytes": 100000, "code": "expired"}
    assert [e for e in _entries() if e["action"] == "upload.swept"] == [swept]
    assert [e["action"] for e in _entries() if e["action"] != "upload.swept"] == ["upload.expired", "upload.create", "audit.started"]


def test_audit_sweep_of_a_folder_that_cannot_be_removed_is_recorded_once():
    """upload.swept is recorded when the removal is done: a folder that cannot be removed gives one record "failed"
    (the sweep retries it every time without another record or log line), and one "ok" once a retry removed it"""
    abandoned = m1._staging("cd" * 16)
    if os.name == "nt":
        os.makedirs(abandoned)
        part = os.path.join(abandoned, "0.part")
        with open(part, "wb") as f:
            f.write(b"x" * 1000)
        holder = open(part, "rb")  # opened without FILE_SHARE_DELETE: the file cannot be removed while it is open
        release = holder.close
    else:
        if os.geteuid() == 0:
            pytest.skip("root can remove anything")  # ty: ignore[too-many-positional-arguments]
        stuck = os.path.join(abandoned, "sub")
        os.makedirs(stuck)
        with open(os.path.join(stuck, "0.part"), "wb") as f:
            f.write(b"x" * 1000)
        os.chmod(stuck, 0o500)  # nothing in it can be removed

        def release() -> None:
            os.chmod(stuck, 0o700)
    try:
        _enable(PROVISION_ADMIN_TEST_UPLOAD_SWEEP_MS="300", PROVISION_ADMIN_TEST_UPLOAD_ORPHAN_S="0")
        server.start()
        [failed] = _wait_record("upload.swept", 30)
        assert (failed["kind"], failed["result"], failed["target"], failed["actor"]) == ("server", "failed", "cd" * 16, SERVER_ACTOR)
        assert failed["details"] == {"folder": ".provision-upload/" + "cd" * 16, "foreign": False, "name": None}
        time.sleep(3)  # about ten more sweeps, each of them tries again
        assert os.path.isdir(abandoned)
        assert [e for e in _entries() if e["action"] == "upload.swept"] == [failed]
        log = m1._log()
        assert log.count("could not remove all of .provision-upload/" + "cd" * 16) == 1, log
        assert log.count("removing the abandoned upload folder .provision-upload/" + "cd" * 16) == 1, log
    finally:
        release()
    ok = _wait_for(lambda: [e for e in _entries() if e["action"] == "upload.swept" and e["result"] == "ok"], 30,
                   "the folder to be removed")
    assert len(ok) == 1 and ok[0]["details"]["folder"] == failed["details"]["folder"]
    _wait_for(lambda: not os.path.exists(abandoned), 10, "the folder to be gone")
    assert [e["result"] for e in _entries() if e["action"] == "upload.swept"] == ["ok", "failed"]


def test_audit_upload_resumed_after_a_restart_is_still_published_by_its_creator():
    tiny = _tiny()
    _enable()
    server.start()
    sid = m1._create("will-resume", [tiny])["upload"]["id"]
    _ok(m1._chunk(sid, 0, 0, open(tiny, "rb").read()[:256 * KiB]))
    m1._restart(kill=True)

    [resumed] = _wait_record("upload.resumed_after_restart", 30)
    assert (resumed["kind"], resumed["result"], resumed["target"], resumed["actor"]) == ("server", "ok", "will-resume", SERVER_ACTOR)
    assert resumed["details"] == {"id": sid, "state": "receiving"}
    assert m1._send(sid, 0, tiny, chunk=MiB).status_code == 200
    assert m1._wait_state(sid)["state"] == "published"
    [published] = _wait_record("upload.published")
    [create] = [e for e in _entries() if e["action"] == "upload.create"]
    # the creator is kept in the session record in the state directory, so it survives the restart
    assert published["kind"] == "change" and published["actor"] == create["actor"]
    assert published["actor"]["type"] == "client" and published["actor"]["client"] == "127.0.0.1"


def test_audit_delete_and_remove_projector():
    tiny = _tiny()
    size = os.path.getsize(tiny)
    m1._place(tiny, "plain.gguf")
    m1._place(tiny, "mm/mm.gguf")
    projector = m1._place(m1._projector("mmproj-x.gguf"), "mm/mmproj-mm.gguf")
    projector_size = os.path.getsize(projector)
    m1._place(tiny, "keepfold/keepfold.gguf")
    for i in range(120):  # files the model does not reference: kept, and counted in full by the dry run
        with open(os.path.join(models_dir, "keepfold", f"note-{i:03d}.txt"), "w") as f:
            f.write("x")
    _enable()
    server.start()

    top = _top()
    dry = _ok(_call("POST", "/admin/models/delete", {"model": "keepfold", "dry_run": True}))
    assert dry["n_kept"] == 120 and len(dry["kept"]) == 100 and dry["kept_truncated"] is True
    dry = _ok(_call("POST", "/admin/models/delete", {"model": "plain", "dry_run": True}))
    assert dry["n_kept"] == 0 and dry["kept"] == [] and dry["kept_truncated"] is False
    assert _since(top) == []  # a dry run is no change

    top = _top()
    _ok(_call("POST", "/admin/models/delete", {"model": "mm", "projector_only": True}))
    e = _one(top)
    assert (e["kind"], e["action"], e["result"], e["target"]) == ("change", "model.remove_projector", "ok", "mm")
    assert e["details"] == {"bytes": projector_size, "files": 1, "paths": ["mm/mmproj-mm.gguf"], "projector_only": True,
                            "stopped": False, "settings_removed": False, "pin_removed": False, "folder_removed": False}

    _ok(_call("POST", "/admin/models/settings", {"model": "plain", "settings": {"ctx-size": 256}}))
    top = _top()
    _ok(_call("POST", "/admin/models/delete", {"model": "plain"}))
    e = _one(top)
    assert (e["action"], e["result"], e["target"]) == ("model.delete", "ok", "plain")
    assert e["details"] == {"bytes": size, "files": 1, "paths": ["plain.gguf"], "projector_only": False, "stopped": False,
                            "settings_removed": True, "pin_removed": False, "folder_removed": False}
    top = _top()
    _ok(_call("POST", "/admin/models/delete", {"model": "keepfold"}))
    e = _one(top)
    assert e["details"]["paths"] == ["keepfold/keepfold.gguf"] and e["details"]["folder_removed"] is False

    top = _top()
    m1._err(_call("POST", "/admin/models/delete", {"model": "nope"}), 409, "not_deletable")
    e = _one(top)
    assert (e["action"], e["result"], e["target"]) == ("model.delete", "error", "nope")
    assert e["details"] == {"status": 409, "code": "not_deletable"}
    # no path on the host in the audit
    blob = b"".join(_file_bytes(n) for n in _audit_files())
    for p in (models_dir, str(work_dir)):
        for variant in _secret_variants(p):
            assert variant not in blob


#
# effects of the public API, seen on the event feed
#


def test_audit_observed_download_and_removal(hub):
    model = "acme/tiny-GGUF:Q8_0"
    broken = "acme/broken-GGUF:Q8_0"
    hub.repos["acme/tiny-GGUF"] = [("tiny-Q8_0.gguf", 50000)]
    hub.repos["acme/broken-GGUF"] = [("broken-Q8_0.gguf", 50000)]
    hub.file_status["acme/broken-GGUF"] = 500
    _enable()
    server.start()

    res = server.make_request("POST", "/models", data={"model": model}, timeout=60)
    assert res.status_code == 200, res.body
    [done] = _wait_record("download.finished", 60, kind="observed")
    assert (done["kind"], done["result"], done["target"], done["actor"], done["details"]) == ("observed", "ok", model, PUBLIC_API_ACTOR, {})
    assert done["persisted"] is True and done["open"] is False

    res = server.make_request("POST", "/models", data={"model": broken}, timeout=60)
    assert res.status_code == 200, res.body
    [failed] = _wait_record("download.failed", 60, kind="observed")
    assert (failed["result"], failed["target"], failed["actor"]) == ("failed", broken, PUBLIC_API_ACTOR)

    res = server.make_request("DELETE", f"/models?model={model}", timeout=60)
    assert res.status_code == 200, res.body
    [removed] = _wait_record("model.removed", 30, kind="observed")
    assert (removed["result"], removed["target"], removed["actor"]) == ("ok", model, PUBLIC_API_ACTOR)
    # the public routes stay untouched: no change record, no identity
    assert [e["action"] for e in _entries()] == ["model.removed", "download.failed", "download.finished", "audit.started"]
    # anyone who can reach the public API causes these: a file of their own, never the one of the changes
    assert [l["action"] for l in _file_lines(OBSERVED_FILE)] == ["download.finished", "download.failed", "model.removed"]
    assert [l["action"] for l in _file_lines(MAIN_FILE)] == ["audit.started"]
    # the token-free fake hub saw no admin key either
    assert all("x-admin-key" not in headers for _, _, headers in hub.requests)


def test_audit_observed_start_failures_and_settings_reverts():
    m1._place(_tiny(), "tinyA.gguf")
    _enable(PROVISION_ADMIN_INVENTORY=_inventory("CUDA7"))
    server.start()
    _ok(_call("POST", "/admin/models/settings", {"model": "tinyA", "settings": {"device": "CUDA7"}}))

    # who starts a model is unknown (autoload, POST /models/load, a restart from the panel): observed
    assert server.make_request("POST", "/models/load", data={"model": "tinyA"}, timeout=60).status_code == 200
    _wait_record("model.start_failed", 60, kind="observed")
    assert server.make_request("POST", "/models/load", data={"model": "tinyA"}, timeout=60).status_code == 200
    # the second failure in a row: the circuit breaker reverts the settings (a server action)
    [reverted] = _wait_record("model_settings.reverted", 60, kind="server")
    assert (reverted["result"], reverted["target"], reverted["actor"]) == ("ok", "tinyA", SERVER_ACTOR)
    assert reverted["details"] == {"reason": "invalid_device", "failures": 2, "to": "models_ini"}

    def start_failures():
        found = [e for e in _entries(kind="observed") if e["action"] == "model.start_failed"]
        return found if len(found) == 2 else None

    second, first = _wait_for(start_failures, 30, "two start failures")
    for e, failures, reverts in ((first, 1, False), (second, 2, True)):
        assert (e["result"], e["target"], e["actor"]) == ("failed", "tinyA", PUBLIC_API_ACTOR)
        assert e["details"]["reason"] == "invalid_device" and e["details"]["exit_code"] != 0
        assert e["details"]["failures"] == failures and e["details"]["settings_reverted"] is reverts
    # the effects in the file of the observed records, the change and the revert in the change file
    assert [l["action"] for l in _file_lines(OBSERVED_FILE)] == ["model.start_failed"] * 2
    assert [l["action"] for l in _file_lines(MAIN_FILE)] == ["audit.started", "model_settings.update", "model_settings.reverted"]
    server.stop()

    # an entry still on trial at startup is reverted at once: a server record right after audit.started
    path = _path("admin-models.json")
    with open(path, encoding="utf-8") as f:
        doc = json.load(f)
    doc["models"]["tinyA"] = {"settings": {"ctx-size": "777"}, "last_good": {"ctx-size": "512"}, "phase": "trial", "failures": 0}
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(doc, f)
    server.start()
    newest, started = _entries()[:2]
    assert started["action"] == "audit.started"
    assert (newest["kind"], newest["action"], newest["target"], newest["actor"]) == ("server", "model_settings.reverted_at_startup", "tinyA", SERVER_ACTOR)
    assert newest["details"] == {"phase": "trial", "to": "last_good"}


def test_audit_observed_effects_are_capped(monkeypatch, tmp_path_factory):
    """anyone who can reach the public API can cause these: 30 records per window, then one summary"""
    n = 65
    cache = tmp_path_factory.mktemp("cache")
    commit = "0123456789abcdef0123456789abcdef01234567"
    source = str(work_dir / "tiny.gguf")
    shutil.copyfile(_tiny(), source)
    for i in range(n):
        repo = cache / f"models--acme--tiny-{i:02d}-GGUF"
        (repo / "refs").mkdir(parents=True)
        (repo / "refs" / "main").write_text(commit, encoding="utf-8")
        (repo / "snapshots" / commit).mkdir(parents=True)
        target = str(repo / "snapshots" / commit / "tiny-Q8_0.gguf")
        try:
            os.link(source, target)
        except OSError:
            shutil.copyfile(source, target)
    monkeypatch.setenv("LLAMA_CACHE", str(cache))
    # a window of 10 minutes (test hook): the run stays inside one window, almost always
    _enable(PROVISION_ADMIN_TEST_AUDIT_MERGE_MS="600000")
    server.start()
    _post_settings({"models_max": 3})
    change = _entries(kind="change")
    ids = sorted(m for m, v in m1._models().items() if m.startswith("acme/tiny-") and v["source"] == "cache")
    assert len(ids) == n, ids

    for model in ids:
        res = server.make_request("DELETE", f"/models?model={model}", timeout=60)
        assert res.status_code == 200, res.body

    def counted():
        obs = _entries(kind="observed")
        total = sum(e.get("count", 1) for e in obs)
        return obs if total == n else None

    observed = _wait_for(counted, 30, f"{n} removals to be counted")
    singles = [e for e in observed if e["action"] == "model.removed"]
    more = [e for e in observed if e["action"] == "observed.more"]
    # one window: 30 single records, then one summary of the rest (two when the run crossed into the next window)
    assert 1 <= len(more) <= 2 and len(singles) <= OBSERVED_PER_WINDOW * 2
    if len(more) == 1 and len(singles) == OBSERVED_PER_WINDOW:
        assert more[0]["count"] == n - OBSERVED_PER_WINDOW
    assert all(e["actor"] == PUBLIC_API_ACTOR and e["target"] in ids for e in singles)
    for e in more:
        d = e["details"]
        assert (e["kind"], e["result"], e["target"], e["actor"]) == ("observed", "ok", "", PUBLIC_API_ACTOR)
        assert d["count"] == e["count"] and len(d["items"]) == min(e["count"], ITEMS_MAX)
        assert d["items_capped"] is (e["count"] > ITEMS_MAX)
        assert all(item.startswith("model.removed acme/tiny-") for item in d["items"])
    # the change record is still there; nothing went to the refusal file, nothing of this to the change file
    assert _entries(kind="change") == change
    assert not os.path.exists(_path(REFUSALS_FILE))
    assert [l["action"] for l in _file_lines(MAIN_FILE)] == ["audit.started", "settings.update"]
    assert {l["kind"] for l in _file_lines(OBSERVED_FILE)} == {"observed"}
    storage = _ok(_call("GET", "/admin/storage"))["audit"]
    assert {f["name"]: f["store"] for f in storage["files"]} == {MAIN_FILE: "changes", OBSERVED_FILE: "observed"}


#
# the files: rotation, restart, write errors
#


def test_audit_rotation_at_the_test_size():
    _enable(PROVISION_ADMIN_TEST_AUDIT_MAX_BYTES="4096", PROVISION_ADMIN_TRUSTED_PROXIES="127.0.0.1",
            PROVISION_ADMIN_TEST_AUDIT_MERGE_MS="5000")
    server.start()
    assert _status()["audit"]["max_bytes"] == 4096
    for i in range(40):
        _post_settings({"models_max": 3 if i % 2 == 0 else 2})
    # refusals rotate in their own file (one record per client: the merge key is client + path + status + reason)
    for i in range(30):
        m1._err(_call("GET", "/admin/settings", key=None, headers=_xff(f"192.0.2.{i + 1}")), 401, "admin_key_required")

    files = _audit_files()
    assert set(files) == {MAIN_FILE, MAIN_FILE + ".1", REFUSALS_FILE, REFUSALS_FILE + ".1"}, files
    assert all(size <= 4096 for size in files.values()), files
    if os.name != "nt":  # the files created by a rotation are private too
        assert {n: stat.S_IMODE(os.stat(_path(n)).st_mode) for n in files} == {n: 0o600 for n in files}
    _assert_lines_capped()
    for name in (MAIN_FILE, REFUSALS_FILE):
        old, cur = _file_lines(name + ".1"), _file_lines(name)
        assert old and cur and old[-1]["seq"] < cur[0]["seq"]
    old, cur = _file_lines(MAIN_FILE + ".1"), _file_lines(MAIN_FILE)
    assert old[-1]["seq"] + 1 == cur[0]["seq"] and all(l["kind"] == "change" for l in old + cur)

    # the route reads both files: contiguous, newest first; the oldest records rotated away
    changes = _entries(kind="change")
    seqs = [e["seq"] for e in changes]
    assert seqs == list(range(seqs[0], seqs[0] - len(seqs), -1)) and len(changes) == len(old) + len(cur)
    assert seqs[-1] > 2 and "audit.started" not in [e["action"] for e in _entries()]
    # merge windows still counting are listed from memory, also when their first line was rotated away; once they
    # ended, what is left is what the files hold
    refusals = _entries(kind="refusal")
    assert len(refusals) == 30 and all(e["open"] for e in refusals)
    _wait_for(lambda: not [e for e in _entries(kind="refusal") if e["open"]], 20, "the merge windows to end")
    on_disk = _file_lines(REFUSALS_FILE) + _file_lines(REFUSALS_FILE + ".1")
    assert sorted(e["seq"] for e in _entries(kind="refusal")) == sorted(l["seq"] for l in on_disk if "group" not in l)
    storage = _ok(_call("GET", "/admin/storage"))["audit"]
    assert {f["name"]: f["store"] for f in storage["files"]} == {
        MAIN_FILE: "changes", MAIN_FILE + ".1": "changes", REFUSALS_FILE: "refusals", REFUSALS_FILE + ".1": "refusals"}
    assert storage["max_bytes"] == 4096 and storage["total_bytes"] == sum(f["bytes"] for f in storage["files"])

    # after a restart the numbering continues from the files
    top = _top()
    m1._restart()
    [started] = [e for e in _entries() if e["action"] == "audit.started"]
    assert started["seq"] == top + 1


def test_audit_survives_a_restart_and_a_crash():
    _enable()
    server.start()
    for v in (3, 2):
        _post_settings({"models_max": v})
    m1._err(_call("GET", "/admin/settings", key=None), 401, "admin_key_required")
    lines = _file_lines(MAIN_FILE)
    changes = _entries(kind="change")
    top = _top()

    m1._restart()
    after = _file_lines(MAIN_FILE)
    assert after[:len(lines)] == lines and after[-1]["action"] == "audit.started" and after[-1]["seq"] == top + 1
    assert _entries(kind="change") == changes
    assert [e["details"]["reason"] for e in _entries(kind="refusal")] == ["no_key"]

    # a hard kill (OOM killer, power cut of the process): every record was on disk before the answer
    _post_settings({"models_max": 3})
    changes = _entries(kind="change")
    top = _top()
    m1._restart(kill=True)
    assert _entries(kind="change") == changes
    newest = _entries()[0]
    assert newest["action"] == "audit.started" and newest["seq"] == top + 1


def test_audit_in_memory_when_the_state_dir_cannot_be_written():
    blocker = work_dir / "not-a-dir"
    blocker.write_text("x")
    _enable(PROVISION_ADMIN_STATE_DIR=str(blocker / "state"))
    server.start()

    status = _status()
    assert "audit_not_persisted" in status["warnings"]
    audit = status["audit"]
    assert set(audit) == STATUS_KEYS and audit["persisted"] is False and audit["in_memory"] == 1 and audit["lost"] == 0
    assert isinstance(audit["error"], str) and audit["error"] != "" and isinstance(audit["since"], str)
    item = _diagnostics_item("audit")
    assert item["status"] == "warning" and "memory" in item["hint"]
    assert _ok(_call("GET", "/admin/storage"))["audit"]["persisted"] is False

    # the admin actions that need no state file work, their records are kept in memory and say so; a change that
    # needs the state file fails as before (stage 1) and is recorded as such
    _ok(_call("POST", "/admin/gpus/rescan", {}))
    _ok(_call("POST", "/admin/models/upload", {"name": "mem-up", "files": m1._files([_tiny()])}), 201)
    m1._err(_call("POST", "/admin/settings", {"models_max": 3}), 500, "server_error")
    m1._err(_call("GET", "/admin/settings", key=None), 401, "admin_key_required")
    page = _page()
    assert page["persisted"] is False and page["warnings"] == ["audit_not_persisted"]
    entries = _entries()
    assert [(e["action"], e["result"]) for e in entries] == [
        ("access.refused", "refused"), ("settings.update", "error"), ("upload.create", "ok"),
        ("gpus.rescan", entries[3]["result"]), ("audit.started", "ok")]
    assert entries[1]["details"] == {"status": 500, "code": "server_error"}
    assert all(e["persisted"] is False for e in entries)
    assert entries[-1]["details"]["persisted"] is False
    assert [l["seq"] for l in _download_lines()] == sorted(e["seq"] for e in entries)
    assert _status()["audit"]["in_memory"] == 5
    assert m1._log().count("audit log is kept in memory only") == 1

    # memory only: a restart forgets them
    m1._restart()
    assert [e["action"] for e in _entries()] == ["audit.started"]


def test_audit_refusals_never_push_change_records_out_of_memory():
    """the in-memory rings are per file: a flood of refusals fills its own ring, never the one of the changes"""
    blocker = work_dir / "not-a-dir"
    blocker.write_text("x")
    # short windows (test hook) so that the flood makes more refusal records than the ring holds
    _enable(PROVISION_ADMIN_STATE_DIR=str(blocker / "state"), PROVISION_ADMIN_TRUSTED_PROXIES="127.0.0.1",
            PROVISION_ADMIN_TEST_AUDIT_MERGE_MS="50")
    server.start()
    for _ in range(10):
        _ok(_call("POST", "/admin/gpus/rescan", {}))  # needs no state file
    changes = _entries(kind="change")
    assert len(changes) == 10

    deadline = time.time() + 60
    batch = 0
    while _status()["audit"]["lost"] == 0:
        assert time.time() < deadline, "the refusal ring never filled up"
        assert _flood(2000, prefix=f"10.{100 + batch % 100}") == {401: 2000}
        batch += 1
    audit = _status()["audit"]
    assert audit["lost"] > 0 and audit["in_memory"] <= 2000
    assert _entries(kind="change") == changes
    assert [e["action"] for e in _entries(kind="server")] == ["audit.started"]


def test_audit_write_error_at_runtime_keeps_the_records_in_memory_until_it_works_again():
    # appends are counted from the start: 1 = audit.started; append 3 fails, so does the first retry (append 4)
    _enable(PROVISION_ADMIN_TEST_AUDIT_FAIL="3,2", PROVISION_ADMIN_TEST_AUDIT_RETRY_MS="1500")
    server.start()
    _post_settings({"models_max": 3})  # append 2
    _post_settings({"models_max": 2})  # append 3: fails, the action does not
    status = _status()
    assert status["audit"]["persisted"] is False and "audit_not_persisted" in status["warnings"]
    assert "simulated" in status["audit"]["error"] and status["audit"]["in_memory"] == 1
    newest, older = _entries(kind="change")
    assert newest["persisted"] is False and newest["details"]["changes"] == {"models_max": {"from": 3, "to": 2}}
    assert older["persisted"] is True
    assert [l["action"] for l in _file_lines(MAIN_FILE)] == ["audit.started", "settings.update"]

    # the retry writes the ring in order, then audit.persisted_again
    _wait_for(lambda: _status()["audit"]["persisted"], 20, "the audit file to be written again")
    status = _status()
    assert "audit_not_persisted" not in status["warnings"] and status["audit"]["in_memory"] == 0
    lines = _file_lines(MAIN_FILE)
    assert [l["action"] for l in lines] == ["audit.started", "settings.update", "settings.update", "audit.persisted_again"]
    assert [l["seq"] for l in lines] == sorted(l["seq"] for l in lines)
    again = lines[-1]
    assert again["kind"] == "server" and again["target"] == MAIN_FILE and again["actor"] == SERVER_ACTOR
    assert again["details"]["store"] == "changes" and again["details"]["lost"] == 0
    assert isinstance(again["details"]["not_persisted_since"], str)
    assert all(e["persisted"] for e in _entries())


def test_audit_read_only_file_at_runtime():
    if os.name != "nt" and os.geteuid() == 0:
        pytest.skip("root can write a read-only file")  # ty: ignore[too-many-positional-arguments]
    _enable(PROVISION_ADMIN_TEST_AUDIT_RETRY_MS="1000")
    server.start()
    path = _path(MAIN_FILE)
    os.chmod(path, stat.S_IREAD)
    try:
        t0 = time.time()
        _post_settings({"models_max": 3})
        _post_settings({"models_max": 2})
        assert time.time() - t0 < 10  # never waits for the audit
        status = _status()
        assert status["audit"]["persisted"] is False and f"cannot open {MAIN_FILE}" in status["audit"]["error"]
        assert [e["persisted"] for e in _entries(kind="change")] == [False, False]
    finally:
        os.chmod(path, stat.S_IREAD | stat.S_IWRITE)
    _wait_for(lambda: _status()["audit"]["persisted"], 20, "the audit file to be written again")
    assert [l["action"] for l in _file_lines(MAIN_FILE)][-3:] == ["settings.update", "settings.update", "audit.persisted_again"]


def test_audit_refusal_file_write_error_and_recovery():
    """the refusal file cannot be written: only the refusals go to memory (the change file goes on), the ring
    overflows, and once the file can be written again the ring goes there in order, then audit.persisted_again with
    what was lost"""
    if os.name != "nt" and os.geteuid() == 0:
        pytest.skip("root can write a read-only file")  # ty: ignore[too-many-positional-arguments]
    # short windows (test hook) so that a flood makes more refusal records than the ring holds
    _enable(PROVISION_ADMIN_TRUSTED_PROXIES="127.0.0.1", PROVISION_ADMIN_TEST_AUDIT_MERGE_MS="50",
            PROVISION_ADMIN_TEST_AUDIT_RETRY_MS="500")
    server.start()
    m1._err(_call("GET", "/admin/settings", key=None), 401, "admin_key_required")  # the refusal file is created
    path = _path(REFUSALS_FILE)
    assert os.path.isfile(path)
    os.chmod(path, stat.S_IREAD)
    try:
        deadline = time.time() + 60
        batch = 0
        while _status()["audit"]["lost"] == 0:
            assert time.time() < deadline, "the refusal ring never filled up"
            assert _flood(2000, prefix=f"10.{100 + batch % 100}") == {401: 2000}
            batch += 1
        audit = _status()["audit"]
        assert audit["persisted"] is False and f"cannot open {REFUSALS_FILE}" in audit["error"]
        # the change file is not affected: a change is written at once
        top = _top()
        _post_settings({"models_max": 3})
        [change] = [e for e in _since(top) if e["kind"] == "change"]
        assert change["persisted"] is True and _file_lines(MAIN_FILE)[-1]["seq"] == change["seq"]
    finally:
        os.chmod(path, stat.S_IREAD | stat.S_IWRITE)
    _wait_for(lambda: _status()["audit"]["persisted"], 30, "the refusal file to be written again")
    lines = _file_lines(REFUSALS_FILE)
    [again] = [l for l in lines if l["action"] == "audit.persisted_again"]
    assert again["kind"] == "server" and again["target"] == REFUSALS_FILE and again["actor"] == SERVER_ACTOR
    assert again["details"]["store"] == "refusals" and again["details"]["lost"] > 0
    assert isinstance(again["details"]["not_persisted_since"], str)
    # in order: the first refusal, what the ring still held, then persisted_again (and whatever came after it)
    seqs = [l["seq"] for l in lines]
    assert seqs == sorted(seqs) and len(set(seqs)) == len(seqs)
    i = lines.index(again)
    assert i > 1000 and all(l["kind"] == "refusal" for l in lines[:i])
    assert [l["kind"] for l in _file_lines(MAIN_FILE)].count("refusal") == 0
    assert _status()["audit"]["in_memory"] == 0 and _status()["audit"]["lost"] == 0
    _assert_lines_capped()


def test_audit_no_written_again_record_while_nothing_can_be_written():
    """the retries while the state directory is missing write nothing and record nothing; when it appears, every store
    writes its ring and then one audit.persisted_again"""
    blocker = work_dir / "not-a-dir"
    blocker.write_text("x")
    state = blocker / "state"
    _enable(PROVISION_ADMIN_STATE_DIR=str(state), PROVISION_ADMIN_TEST_AUDIT_RETRY_MS="300")
    server.start()
    time.sleep(2.5)  # several retries of every store (the events thread tries about once a second)
    m1._err(_call("GET", "/admin/settings", key=None), 401, "admin_key_required")  # a retry first, then the record
    _ok(_call("POST", "/admin/gpus/rescan", {}))
    time.sleep(1.5)
    entries = _entries()
    assert [e["action"] for e in entries] == ["gpus.rescan", "access.refused", "audit.started"], entries
    assert all(e["persisted"] is False for e in entries)
    status = _status()["audit"]
    assert status["persisted"] is False and status["in_memory"] == 3 and status["lost"] == 0

    # the directory appears: the rings go to the files, then one persisted_again per store
    blocker.unlink()
    state.mkdir(parents=True)
    _wait_for(lambda: _status()["audit"]["persisted"], 30, "the audit files to be written again")
    again = [e for e in _entries() if e["action"] == "audit.persisted_again"]
    assert sorted(e["details"]["store"] for e in again) == ["changes", "observed", "refusals"]
    assert all(e["persisted"] and e["details"]["lost"] == 0 and e["kind"] == "server" for e in again)

    def lines(name: str) -> list[dict]:  # the state directory of this test, not the usual one
        return [json.loads(raw) for raw in (state / name).read_bytes().split(b"\n") if raw.strip()]

    assert [l["action"] for l in lines(MAIN_FILE)] == ["audit.started", "gpus.rescan", "audit.persisted_again"]
    assert [l["action"] for l in lines(REFUSALS_FILE)] == ["access.refused", "audit.persisted_again"]
    assert [l["action"] for l in lines(OBSERVED_FILE)] == ["audit.persisted_again"]
    for name in (MAIN_FILE, REFUSALS_FILE, OBSERVED_FILE):
        seqs = [l["seq"] for l in lines(name)]
        assert seqs == sorted(seqs)
    every = _entries()
    assert len({e["seq"] for e in every}) == len(every)


def test_audit_a_line_cut_by_a_crash_stays_on_a_line_of_its_own():
    """a file that ends without a newline (a write cut by a crash): the next record starts a new line, the cut one is
    listed as unreadable"""
    _enable()
    server.start()
    _post_settings({"models_max": 3})
    top = _top()
    server.stop()
    cut = ('{"seq":%d,"kind":"change","action":"settings.update","result":"ok","ts":17' % (top + 1)).encode()
    with open(_path(MAIN_FILE), "ab") as f:
        f.write(cut)
    server.start()
    _post_settings({"models_max": 2})

    raw = _file_bytes(MAIN_FILE).split(b"\n")
    assert raw[-1] == b"" and cut in raw  # the cut line alone on its line
    after = [json.loads(line) for line in raw[raw.index(cut) + 1:] if line]
    assert [(l["seq"], l["action"]) for l in after] == [(top + 2, "audit.started"), (top + 3, "settings.update")]
    entries = _entries()
    assert [e["seq"] for e in entries[:3]] == [top + 3, top + 2, top + 1]
    assert entries[2]["unreadable"] is True and entries[2]["action"] == "settings.update"
    assert entries[1]["action"] == "audit.started" and entries[0]["details"]["changes"] == {"models_max": {"from": 3, "to": 2}}


def test_audit_a_long_record_is_cut_to_the_line_size():
    """a record longer than one line (4 KiB; 512 bytes here, test hook): the details are cut first (the start of their
    text is kept), then everything but the head and the time"""
    parts = m1._split("cut", 16)
    _enable(PROVISION_ADMIN_TEST_AUDIT_LINE_MAX="512")
    server.start()
    sid, s = m1._upload("cut-up", parts, chunk=MiB)
    assert s["state"] == "published", m1._brief(s)
    [published] = _wait_record("upload.published")
    assert (published["kind"], published["result"], published["target"]) == ("change", "ok", "cut-up")
    assert published["actor"]["client"] == "127.0.0.1"
    d = published["details"]
    assert set(d) == {"truncated", "text"} and d["truncated"] is True
    assert d["text"].startswith("{") and len(d["text"]) <= 128 and sid in d["text"]

    # an actor with long header values does not fit even so: the head and the time are kept
    top = _top()
    _post_settings({"models_max": 3}, headers={"User-Agent": "u" * 300, **_xff("192.0.2." + "9" * 300)})
    e = _one(top)
    assert (e["kind"], e["action"], e["result"], e["truncated"]) == ("change", "settings.update", "ok", True)
    assert "actor" not in e and "details" not in e and isinstance(e["ts"], int) and isinstance(e["time"], str)
    _assert_lines_capped(512)


#
# secrets, the route
#


def test_audit_never_stores_secrets():
    env.setenv("HF_TOKEN", FAKE_TOKEN)
    m1._place(_tiny(), "plain.gguf")
    _enable()
    server.start()
    # the key and the token where a client could put them: header values, a body key, a wrong key that holds the key
    _post_settings({"models_max": 3}, headers={"User-Agent": f"ua {ADMIN_KEY} {FAKE_TOKEN}", **_xff(FAKE_TOKEN)})
    res = _call("POST", "/admin/settings", {"models_max": 2, ADMIN_KEY: FAKE_TOKEN})
    assert res.status_code in (200, 400), res.body
    m1._err(_call("POST", "/admin/settings", {"models_max": 2}, key=ADMIN_KEY + "x",
                  headers={"Authorization": f"Bearer {FAKE_TOKEN}"}), 401, "admin_key_required")
    m1._err(_call("GET", "/admin/settings", key=None, headers={"Authorization": f"Bearer {ADMIN_KEY}"}), 401, "admin_key_required")
    _ok(_call("POST", "/admin/models/settings", {"model": "plain", "settings": {"ctx-size": 256}},
              headers={"User-Agent": ADMIN_KEY}))
    _ok(_call("POST", "/admin/models/delete", {"model": "plain"}))

    blob = b"".join(_file_bytes(n) for n in _audit_files())
    texts = {
        "files": blob,
        "download": _download().content,
        "api": json.dumps(_entries()).encode("utf-8"),
    }
    for where, text in texts.items():
        for secret in (ADMIN_KEY, FAKE_TOKEN, "Bearer"):
            assert secret.encode() not in text, f"{secret!r} in the {where}"
        for p in (models_dir, state_dir, str(work_dir)):
            for variant in _secret_variants(p):
                assert variant not in text, f"a host path in the {where}"
    assert b"[redacted]" in blob
    [first] = [e for e in _entries(kind="change") if e["actor"].get("forwarded_for") == "[redacted]"]
    assert first["actor"]["user_agent"] == "ua [redacted] [redacted]"


def test_audit_log_lines_never_show_the_key_or_the_token():
    """the log line of a record (admin_audit_record() in server-admin.cpp) gets the same secrets replaced as the record"""
    env.setenv("HF_TOKEN", FAKE_TOKEN)
    _enable()
    server.start()
    _post_settings({"models_max": 3}, headers=_xff(FAKE_TOKEN))
    _call("POST", "/admin/settings", {"models_max": 2, ADMIN_KEY: 1})
    _wait_for(lambda: m1._log().count("admin: audit: settings.update") == 2, 15, "the log lines of the two records")
    log = m1._log()
    assert FAKE_TOKEN not in log
    assert ADMIN_KEY not in log
    assert "X-Forwarded-For [redacted], unverified" in log


def test_audit_generated_key_is_a_secret_too():
    """no PROVISION_ADMIN_KEY: the key generated into the state directory is replaced like the others, also where a
    value is cut before it is recorded (X-Forwarded-For in the label at 64 bytes, a body key at 64 bytes; the generated
    key has 64) and in the log line of the record"""
    _enable(PROVISION_ADMIN_KEY=None)
    server.start()
    with open(_path("admin.key"), encoding="utf-8") as f:
        key = f.read().strip()
    assert len(key) == 64, key

    def get(path: str) -> requests.Response:
        return requests.get(server.make_url(path), headers={"X-Admin-Key": key}, timeout=60)

    res = m1._call("POST", "/admin/settings", {"models_max": 3, "y" + key: 1}, key=key,
                   headers={**_xff("x" + key), "User-Agent": "ua-" + key})
    assert res.status_code in (200, 400), res.body
    _wait_for(lambda: "admin: audit: settings.update" in m1._log(), 15, "the log line of the record")
    page = get("/admin/audit?kind=change")
    assert page.status_code == 200, page.text
    [e] = page.json()["entries"]
    assert e["actor"]["forwarded_for"] == "x[redacted]" and e["actor"]["user_agent"] == "ua-[redacted]"
    assert e["actor"]["label"] == "127.0.0.1 (X-Forwarded-For x[redacted], unverified)"
    assert "y[redacted]" in e["details"]["keys"]
    texts = {
        "files": b"".join(_file_bytes(n) for n in _audit_files()),
        "download": get("/admin/audit?format=jsonl").content,
        "api": page.content,
        "log": m1._log().encode("utf-8"),
    }
    for where, text in texts.items():
        # no part of the key either: a value cut before it was redacted would leave its first bytes
        assert key[:16].encode() not in text, f"a part of the generated key in the {where}"


def test_audit_paging_and_filters():
    tiny = _tiny()
    _enable()
    server.start()
    for v in (3, 2, 3, 2, 3, 2):
        _post_settings({"models_max": v})
    m1._err(_call("POST", "/admin/settings", {"models_max": 0}), 400, "invalid_request_error")
    m1._err(_call("GET", "/admin/settings", key=None), 401, "admin_key_required")
    m1._err(_call("GET", "/admin/settings", key="wrong-key"), 401, "admin_key_required")
    sid = m1._create("paged", [tiny])["upload"]["id"]
    m1._cancel(sid)

    everything = _entries()
    seqs = [e["seq"] for e in everything]
    assert len(everything) == 12 and seqs == sorted(seqs, reverse=True) and len(set(seqs)) == len(seqs)

    # pages: newest first, next_before continues without overlap or gap, matched counts what is left
    p1 = _page(limit=3)
    assert [e["seq"] for e in p1["entries"]] == seqs[:3] and p1["next_before"] == str(seqs[2]) and p1["matched"] == 12
    p2 = _page(limit=3, before=p1["next_before"])
    assert [e["seq"] for e in p2["entries"]] == seqs[3:6] and p2["matched"] == 9
    walked = []
    before = None
    while True:
        p = _page(limit=5, before=before)
        walked += p["entries"]
        before = p["next_before"]
        if before is None:
            break
    assert walked == everything
    assert _page(limit=12)["next_before"] is None and _page(limit=11)["next_before"] == str(seqs[10])
    assert _page(before=seqs[-1])["entries"] == [] and _page(before=seqs[-1])["matched"] == 0
    assert _page(before=10**12)["matched"] == 12
    assert len(_page(limit=500)["entries"]) == 12

    # filters
    def actions(**params) -> list:
        return [e["action"] for e in _entries(**params)]

    assert set(actions(kind="change")) == {"settings.update", "upload.create", "upload.cancel"} and len(actions(kind="change")) == 9
    assert actions(kind="server") == ["audit.started"]
    assert actions(kind="refusal,server") == ["access.refused", "access.refused", "audit.started"]
    assert actions(action="upload") == ["upload.cancel", "upload.create"] == actions(action="upload.*")
    assert actions(action="upload.create") == ["upload.create"]
    assert actions(action="settings") == ["settings.update"] * 7 == actions(action="settings.update")
    assert actions(action="sett") == [] and actions(action="upload.cre") == []
    assert actions(action="upload.cancel,audit") == ["upload.cancel", "audit.started"]
    assert actions(result="error") == ["settings.update"]
    assert actions(result="refused") == ["access.refused", "access.refused"]
    assert len(actions(result="ok,error")) == 10
    assert actions(kind="change", result="error", action="settings") == ["settings.update"]
    assert _page(kind="change", limit=2)["matched"] == 9

    # bad parameters
    for params, param in (({"limit": 0}, "limit"), ({"limit": 501}, "limit"), ({"limit": "abc"}, "limit"),
                          ({"before": "abc"}, "before"), ({"before": 0}, "before"), ({"before": -1}, "before"),
                          ({"kind": "nope"}, "kind"), ({"kind": "change,nope"}, "kind"), ({"format": "xml"}, "format")):
        m1._err(_audit(**params), 400, "invalid_request_error", param=param)


def test_audit_jsonl_download():
    _enable(PROVISION_ADMIN_TEST_AUDIT_MERGE_MS="4000")
    server.start()
    _post_settings({"models_max": 3})
    for _ in range(3):
        m1._err(_call("GET", "/admin/settings", key=None), 401, "admin_key_required")
    _wait_for(lambda: [e for e in _entries(kind="refusal") if not e["open"]], 15, "the window to end")
    for _ in range(2):  # a new window, still counting
        m1._err(_call("GET", "/admin/storage", key=None), 401, "admin_key_required")
    _post_settings({"models_max": 2})

    r = _download()
    assert r.status_code == 200
    assert r.headers["Content-Type"].startswith("application/x-ndjson")
    assert r.headers["Content-Disposition"] == 'attachment; filename="provision-admin-audit.jsonl"'
    assert r.headers.get("Cache-Control") == "no-store"
    lines = [json.loads(line) for line in r.text.splitlines()]
    # every line of both files, oldest first; the update line of the merged record is there (group = its first line)
    file_lines = _file_lines(MAIN_FILE) + _file_lines(REFUSALS_FILE)
    assert [l["seq"] for l in lines] == sorted(l["seq"] for l in file_lines)
    assert [l["action"] for l in lines] == ["audit.started", "settings.update", "access.refused", "access.refused",
                                           "access.refused", "settings.update"]
    closed_first, update, still_open = lines[2], lines[3], lines[4]
    assert closed_first["count"] == 1 and update["group"] == closed_first["seq"] and update["count"] == 3
    # the first line of a window still counting is replaced by its state now
    assert still_open["target"] == "/admin/storage" and still_open["count"] == 2
    assert [l["count"] for l in _file_lines(REFUSALS_FILE) if l["target"] == "/admin/storage"] == [1]

    # the filters and before apply, limit does not
    assert [l["action"] for l in _download_lines(kind="refusal")] == ["access.refused"] * 3
    assert [l["action"] for l in _download_lines(kind="change", limit=1)] == ["settings.update"] * 2
    assert [l["seq"] for l in _download_lines(before=still_open["seq"])] == [l["seq"] for l in lines[:4]]
    assert [l["action"] for l in _download_lines(action="audit")] == ["audit.started"]
    m1._err(_audit(format="csv"), 400, "invalid_request_error", param="format")


@pytest.mark.parametrize("value", [None, "0"])
def test_audit_admin_disabled_has_no_route(value: str | None):
    m1._place(_tiny(), "keep.gguf")
    _enable(PROVISION_ADMIN=value, PROVISION_ADMIN_TEST_AUDIT_MAX_BYTES="4096")
    server.start()
    for key in (None, ADMIN_KEY):
        for target in ("/admin/audit", "/admin/audit?format=jsonl", "/admin/status"):
            stage1._assert_not_found(_call("GET", target, key=key))
    assert "keep" in m1._models()
    assert not os.path.exists(state_dir)
    assert "admin:" not in m1._log()
