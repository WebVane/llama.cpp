# tests for stage 3 milestone M1 of the Provision admin layer (tools/server/server-admin-upload.cpp,
# docs/provision-admin.md): the GGUF upload into --models-dir (/admin/models/upload, .../chunk, .../update,
# .../publish, .../cancel), the delete of models_dir models and "Remove projector" (/admin/models/delete), the
# typed pre-check of a projector that the router reads on every reload (a crash fix that applies with the admin
# layer disabled too), the '@' in the URL of a download, and the answers of the allow-list that the "Admin" item
# of the UI relies on.
#
# Everything runs on a CPU-only build and without the network:
#   - the models are the tiny model of the shared cache (stories260K, see conftest.py) and fixtures derived from it
#     by admin_gguf.py: a split set in the layout of llama-gguf-split, projectors without tensors, broken files;
#   - what cannot be caused from outside (a full disk, a validate helper that hangs or cannot start, a file that
#     appears in the last moment of a publish) comes from the test-only hooks PROVISION_ADMIN_TEST_* of the server,
#     read once at startup and never passed on to a child;
#   - the '@' test talks to two stand-ins for Hugging Face that run inside the test.
# The test marked slow (SLOW_TESTS=1) writes and uploads a file larger than 1 GiB.
# The routers of this file listen on 8680 and up: five ports per xdist worker, see _ports().

import glob
import json
import os
import re
import shutil
import socket
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import pytest
from utils import *

import admin_gguf as gg

# the helpers of the stage-1 admin tests (the model cache, the error format)
import test_admin as stage1
from test_admin import ADMIN_KEY, MODEL_B, SHARED_CACHE_DIR

server: ServerProcess
env: pytest.MonkeyPatch
work_dir: Any
state_dir: str
models_dir: str

KiB = 1024
MiB = 1024 * KiB

LIMITS = {
    "chunk_max": 16 * MiB, "chunk_default": 8 * MiB, "chunk_min": 240 * KiB, "max_sessions": 4, "max_writers": 4,
    "max_files": 256, "ttl_h": 72, "ttl_s": 72 * 3600, "max_header_bytes": 64 * MiB,
}
FINISHED = ("published", "ready", "rejected")
RENAME_METHODS = ("renameat2", "renameatx_np", "link", "check+rename", "MoveFileEx")

SUMMARY_KEYS = {
    "id", "name", "state", "substate", "blocked_by", "total_bytes", "received_bytes", "n_files", "auto_publish",
    "created", "last_activity", "error", "blocked", "expires_at",
}
SESSION_KEYS = SUMMARY_KEYS | {
    "files", "settings", "settings_mode", "plan", "facts", "result", "warnings", "idle_ms", "last_client_idle_ms",
    "recovered", "recovered_without_record", "record_persisted", "chunk_max", "chunk_default", "progress", "checks",
}
FILE_KEYS = {
    "index", "name", "size", "offset", "durable_offset", "complete", "dropped", "role", "sha256", "sha256_expected",
    "head_crc32", "final_name", "header", "error",
}
CHUNK_KEYS = {"offset", "durable_offset", "file_complete", "state", "file", "received_bytes", "total_bytes", "file_error"}

NO_SESSION = "0" * 32

# every route of the upload and delete feature, with a body for the POST routes (a refused request never gets to it)
UPLOAD_ROUTES = [
    ("GET", "/admin/models/upload", None),
    ("GET", "/admin/models/upload?id=" + NO_SESSION, None),
    ("POST", "/admin/models/upload", {"name": "routes", "files": [{"name": "routes.gguf", "size": 1000}]}),
    ("POST", f"/admin/models/upload/chunk?id={NO_SESSION}&file=0&offset=0", {}),
    ("POST", "/admin/models/upload/update", {"id": NO_SESSION}),
    ("POST", "/admin/models/upload/publish", {"id": NO_SESSION}),
    ("POST", "/admin/models/upload/cancel", {"id": NO_SESSION}),
    ("POST", "/admin/models/delete", {"model": "routes"}),
]

# the keys the projector loader reads with a typed getter when the router asks a projector for its capabilities
# (the constructor of clip_model_loader in tools/mtmd/clip.cpp); a value of another type aborted the router
PROBE_KEYS = {
    "general.name": gg.T_STRING,
    "general.description": gg.T_STRING,
    "clip.has_vision_encoder": gg.T_BOOL,
    "clip.has_audio_encoder": gg.T_BOOL,
    "clip.has_gen_audio_encoder": gg.T_BOOL,
}
WRONG_TYPES = {gg.T_STRING: gg.val(gg.T_UINT32, 7), gg.T_BOOL: gg.val(gg.T_UINT8, 1)}


def _ports() -> list[int]:
    """five ports per xdist worker, from 8680 on (ADMIN_UPLOAD_TEST_PORT_BASE moves the range): worker n gets
    base + 5n .. base + 5n + 4, so any number of workers (tests.sh runs -n auto) never shares a port. The first is
    for the router of the test, the others for a second one."""
    worker = os.environ.get("PYTEST_XDIST_WORKER", "")
    n = int(worker[2:]) if worker.startswith("gw") and worker[2:].isdigit() else 0
    base = int(os.environ.get("ADMIN_UPLOAD_TEST_PORT_BASE", "8680")) + 5 * n
    return list(range(base, base + 5))


def _port_in_use(port: int) -> bool:
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=0.5):
            return True
    except OSError:
        return False


def _router(port: int, log_name: str, models: str | None) -> ServerProcess:
    # ServerProcess.start() returns once /health answers on the port: a router of another test (or another worker)
    # already listening there would be taken for this one (on Windows both even bind the port)
    if _port_in_use(port):
        pytest.fail(f"port {port} is already in use by another process: set ADMIN_UPLOAD_TEST_PORT_BASE to a free range")
    srv = ServerPreset.router()
    # no context size, slot count or batch size on the router command line: they would lock those settings
    srv.n_ctx = None
    srv.n_slots = None
    srv.n_batch = None
    srv.models_max = 2
    srv.server_port = port
    srv.models_dir = models
    srv.log_path = str(work_dir / log_name)
    return srv


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
    server = _router(_ports()[0], "server.log", models_dir)
    stage1.server = server
    stage1.env = monkeypatch
    stage1.state_dir = state_dir


#
# helpers
#


def _enable(**variables: str | None) -> None:
    """admin layer on, key from the environment, state in a directory of this test, no GPU inventory or vendor tool
    (not needed here). None removes a variable."""
    values = {
        "PROVISION_ADMIN": "1",
        "PROVISION_ADMIN_KEY": ADMIN_KEY,
        "PROVISION_ADMIN_STATE_DIR": state_dir,
        "PROVISION_ADMIN_INVENTORY": "off",
        "PROVISION_ADMIN_GPU_PROBE": "off",
        **variables,
    }
    for name, value in values.items():
        if value is None:
            env.delenv(name, raising=False)
        else:
            env.setenv(name, value)


def _second_router(models: str | None = None, log_name: str = "server-2.log") -> ServerProcess:
    return _router(_ports()[1], log_name, models if models is not None else models_dir)


def _response(r: requests.Response) -> ServerResponse:
    res = ServerResponse()
    res.status_code = r.status_code
    res.headers = r.headers
    try:
        res.body = r.json()
    except ValueError:
        res.body = r.text
    return res


def _call(method: str, path: str, data: Any = None, key: str | None = ADMIN_KEY, headers: dict | None = None,
          srv: ServerProcess | None = None, timeout: float = 60) -> ServerResponse:
    h = dict(headers or {})
    if key is not None:
        h["X-Admin-Key"] = key
    url = (srv or server).make_url(path)
    if method == "GET":
        return _response(requests.get(url, headers=h, timeout=timeout))
    return _response(requests.request(method, url, json=data, headers=h, timeout=timeout))


def _ok(res: ServerResponse, status: int = 200) -> Any:
    assert res.status_code == status, f"expected {status}, got {res.status_code}: {res.body}"
    return res.body


def _err(res: ServerResponse, status: int, error_type: str, **fields) -> dict:
    """the error format of the server, {"error": {"code", "message", "type", ...}}; returns the error object"""
    stage1._assert_error(res, status, error_type)
    error = res.body["error"]
    for name, value in fields.items():
        assert error.get(name) == value, f"error.{name}: expected {value!r}, got {error.get(name)!r} in {error}"
    return error


def _chunk(sid: str, index: int, offset: int, data: bytes, client: str | None = "tab-1", crc: bool = True,
           takeover: bool = False, verify: bool = False, key: str | None = ADMIN_KEY,
           content_type: str = "application/octet-stream", headers: dict | None = None,
           srv: ServerProcess | None = None, timeout: float = 120) -> ServerResponse:
    h = {"Content-Type": content_type}
    if client:
        h["X-Upload-Client"] = client
    if key is not None:
        h["X-Admin-Key"] = key
    if crc:
        h["X-Chunk-CRC32"] = gg.crc32_hex(data)
    h.update(headers or {})
    target = f"/admin/models/upload/chunk?id={sid}&file={index}&offset={offset}"
    target += "&takeover=1" if takeover else ""
    target += "&verify=1" if verify else ""
    return _response(requests.post((srv or server).make_url(target), data=data, headers=h, timeout=timeout))


def _files(paths: list, names: list | None = None, sha: list | None = None) -> list:
    out = []
    for i, path in enumerate(paths):
        f = {"name": names[i] if names else os.path.basename(path), "size": os.path.getsize(path), "last_modified": 0}
        if sha and sha[i]:
            f["sha256"] = sha[i]
        out.append(f)
    return out


def _create(name: str, paths: list, expect: int = 201, srv: ServerProcess | None = None, names: list | None = None,
            sha: list | None = None, **extra) -> dict:
    body = {"name": name, "files": _files(paths, names, sha), **extra}
    res = _call("POST", "/admin/models/upload", body, srv=srv)
    assert res.status_code == expect, f"create {name}: expected {expect}, got {res.status_code}: {res.body}"
    return res.body


def _session(sid: str, srv: ServerProcess | None = None) -> dict:
    return _ok(_call("GET", "/admin/models/upload?id=" + sid, srv=srv))


def _wait_state(sid: str, states=FINISHED, timeout: float = 120, srv: ServerProcess | None = None) -> dict:
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        res = _call("GET", "/admin/models/upload?id=" + sid, srv=srv)
        if res.status_code == 200 and res.body["state"] in states:
            return res.body
        last = res.body
        time.sleep(0.1)
    raise AssertionError(f"upload {sid} did not reach {states}; last answer: {json.dumps(last)[:1500]}")


def _send(sid: str, index: int, path: str, chunk: int = 256 * KiB, start: int | None = None, stop_at: int | None = None,
          first_only: bool = False, client: str = "tab-1", srv: ServerProcess | None = None) -> ServerResponse | None:
    """sends a file from `start` (default: where the server is) up to `stop_at` (default: its end) in chunks;
    returns the last answer (the first one that is not 200 ends the loop), None when nothing was left to send"""
    size = os.path.getsize(path)
    if start is None:
        start = _session(sid, srv)["files"][index]["offset"]
    end = size if stop_at is None else min(size, stop_at)
    last = None
    with open(path, "rb") as f:
        offset = start
        while offset < end:
            f.seek(offset)
            data = f.read(min(chunk, end - offset))
            last = _chunk(sid, index, offset, data, client=client, srv=srv)
            if last.status_code != 200:
                return last
            offset = last.body["offset"]
            if first_only:
                break
    return last


def _upload(name: str, paths: list, chunk: int = 256 * KiB, wait=FINISHED, srv: ServerProcess | None = None,
            **extra) -> tuple[str, dict]:
    """a whole upload as the UI makes it: the first chunk of every file first (the header checks start at once),
    then the rest; returns the id and the session once it reached one of `wait`"""
    sid = _create(name, paths, srv=srv, **extra)["upload"]["id"]
    for i, path in enumerate(paths):
        res = _send(sid, i, path, chunk=chunk, start=0, first_only=True, srv=srv)
        assert res is not None and res.status_code == 200, f"first chunk of file {i}: {res.status_code} {res.body}"
    for i, path in enumerate(paths):
        res = _send(sid, i, path, chunk=chunk, srv=srv)
        assert res is None or res.status_code == 200, f"file {i}: {res.status_code} {res.body}"
    return sid, _wait_state(sid, wait, srv=srv)



def _brief(s: dict) -> str:
    """the parts of a session that explain where it stopped (an assertion message pytest does not cut short)"""
    keys = ("id", "name", "state", "substate", "blocked_by", "blocked", "error", "result", "warnings", "checks")
    files = [{k: f.get(k) for k in ("index", "offset", "complete", "dropped", "role", "header", "error")} for f in s.get("files", [])]
    return json.dumps({**{k: s.get(k) for k in keys}, "files": files})


def _cancel(sid: str, srv: ServerProcess | None = None) -> dict:
    return _ok(_call("POST", "/admin/models/upload/cancel", {"id": sid}, srv=srv))


def _models(reload: bool = False, srv: ServerProcess | None = None) -> dict[str, dict]:
    r = requests.get((srv or server).make_url("/models" + ("?reload=1" if reload else "")), timeout=60)
    assert r.status_code == 200, r.text
    return {m["id"]: m for m in r.json()["data"]}


def _flag(args: list, flag: str) -> str | None:
    return args[args.index(flag) + 1] if flag in args else None


def _assert_alive(srv: ServerProcess | None = None) -> None:
    srv = srv or server
    assert srv.process is not None and srv.process.poll() is None, "the router is not running any more"
    assert requests.get(srv.make_url("/health"), timeout=10).status_code == 200


def _kill(srv: ServerProcess | None = None) -> None:
    """a hard stop of the router, like a crash or the OOM killer: no shutdown code runs"""
    srv = srv or server
    assert srv.process is not None
    srv.process.kill()
    srv.process.wait(timeout=10)
    srv.stop()


def _restart(srv: ServerProcess | None = None, kill: bool = False) -> None:
    srv = srv or server
    if kill:
        _kill(srv)
    else:
        srv.stop()
    # the port must be free again before the next instance binds it
    stage1._wait_until(lambda: not _answers(srv), 10, "the old router to release its port")
    srv.start()


def _answers(srv: ServerProcess) -> bool:
    try:
        requests.get(srv.make_url("/health"), timeout=1)
        return True
    except requests.exceptions.RequestException:
        return False


def _staging(*parts: str) -> str:
    return os.path.join(models_dir, ".provision-upload", *parts)


def _record(sid: str) -> str:
    return os.path.join(state_dir, "uploads", sid + ".json")


def _read_json(path: str) -> Any:
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def _write_json(path: str, doc: Any) -> None:
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(doc, f, indent=1)


def _models_file() -> dict:
    """the per-model settings of the admin layer (stage 2), {} when there are none"""
    path = os.path.join(state_dir, "admin-models.json")
    return _read_json(path).get("models", {}) if os.path.exists(path) else {}


def _row(model: str) -> dict:
    return _ok(_call("GET", "/admin/models/settings?model=" + model))


def _log() -> str:
    return stage1._read_text(server.log_path) if os.path.exists(server.log_path) else ""


def _tiny() -> str:
    return stage1._tiny_gguf()


def _fx(name: str) -> str:
    """a path for a fixture file of this test (outside of --models-dir)"""
    folder = work_dir / "fixtures"
    folder.mkdir(exist_ok=True)
    return str(folder / name)


def _split(prefix: str = "stories", n: int = 3) -> list:
    """the tiny model as a split set of n files, named <prefix>-0000i-of-0000n.gguf"""
    return gg.split(_tiny(), work_dir / "fixtures" / ("split-" + prefix), prefix, n)


def _projector(name: str = "mmproj-test.gguf", **kw) -> str:
    return gg.projector(_fx(name), **kw)


def _place(src: str, rel: str) -> str:
    """a copy of a file in --models-dir (rel may name a first-level folder: 'folder/file.gguf')"""
    dst = os.path.join(models_dir, *rel.split("/"))
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copyfile(src, dst)
    return dst


def _remove(path: str) -> None:
    """os.remove that waits for the router to close the file (Windows: the facts reader holds it for a moment)"""
    for _ in range(100):
        try:
            os.remove(path)
            return
        except PermissionError:
            time.sleep(0.1)
    os.remove(path)


def _write_preset(text: str) -> str:
    path = str(work_dir / "models.ini")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    return path


def _same_file(a: str, b: str) -> bool:
    return os.path.exists(a) and os.path.exists(b) and os.path.samefile(a, b)


def _assert_forbidden(res: ServerResponse, seen: str = "127.0.0.1") -> None:
    """403 admin_forbidden of the allow-list: the message ends with the address the server saw, never cached"""
    stage1._assert_error(res, 403, "admin_forbidden")
    assert res.body["error"]["message"].endswith(" " + seen), res.body
    assert res.headers.get("Cache-Control") == "no-store"


# requests written by hand: a body that is announced but never sent, a body that is held back, a connection that is
# cut in the middle of a body


def _read_response(s: socket.socket) -> tuple[int | None, Any]:
    data = b""
    try:
        while b"\r\n\r\n" not in data:
            b = s.recv(65536)
            if not b:
                return None, "closed without an answer"
            data += b
    except OSError as e:
        return None, type(e).__name__
    head, rest = data.split(b"\r\n\r\n", 1)
    lines = head.decode("latin-1").split("\r\n")
    status = int(lines[0].split()[1])
    headers = {}
    for line in lines[1:]:
        if ":" in line:
            k, v = line.split(":", 1)
            headers[k.strip().lower()] = v.strip()
    n = int(headers.get("content-length", "0"))
    try:
        while len(rest) < n:
            b = s.recv(65536)
            if not b:
                break
            rest += b
    except OSError:
        pass
    try:
        return status, json.loads(rest[:n].decode("utf-8"))
    except ValueError:
        return status, rest[:n].decode("utf-8", errors="replace")


def _open_request(target: str, headers: dict, body: bytes = b"", content_length: int | None = None,
                  srv: ServerProcess | None = None) -> socket.socket:
    """a POST whose request line, headers and `body` are sent; Content-Length may announce more than that"""
    srv = srv or server
    s = socket.create_connection((srv.server_host, srv.server_port), timeout=30)
    lines = [f"POST {target} HTTP/1.1", f"Host: {srv.server_host}:{srv.server_port}"]
    if content_length is not None:
        lines.append(f"Content-Length: {content_length}")
    lines += [f"{k}: {v}" for k, v in headers.items()]
    s.sendall(("\r\n".join(lines) + "\r\n\r\n").encode("latin-1") + body)
    return s


def _raw(target: str, headers: dict, body: bytes = b"", content_length: int | None = None,
         srv: ServerProcess | None = None) -> tuple[int | None, Any]:
    s = _open_request(target, headers, body, content_length, srv)
    try:
        return _read_response(s)
    finally:
        s.close()


def _chunk_headers(key: str | None = ADMIN_KEY, client: str = "tab-1") -> dict:
    h = {"Content-Type": "application/octet-stream", "X-Upload-Client": client}
    if key is not None:
        h["X-Admin-Key"] = key
    return h


def _chunk_target(sid: str, index: int = 0, offset: int = 0) -> str:
    return f"/admin/models/upload/chunk?id={sid}&file={index}&offset={offset}"


def _hold_chunk(sid: str, index: int, offset: int, data: bytes, sent: int, srv: ServerProcess | None = None) -> socket.socket:
    """a chunk of len(data) bytes of which only the first `sent` are sent: its body is held back"""
    return _open_request(_chunk_target(sid, index, offset), _chunk_headers(), data[:sent], len(data), srv)


#
# availability, limits, status
#


def test_upload_status_limits_and_features():
    _enable()
    server.start()

    res = _call("GET", "/admin/models/upload")
    body = _ok(res)
    assert res.headers.get("Cache-Control") == "no-store"
    assert body["available"] is True and body["reason"] is None and body["message"] is None
    assert body["limits"] == LIMITS
    assert body["limits"]["max_writers"] == body["limits"]["max_sessions"]  # one chunk in flight per session: no 429
    assert body["sessions"] == [] and body["staging_bytes"] == 0
    assert _same_file(body["models_dir"], models_dir)
    assert body["settings_writable"] is True and body["settings_error"] is None

    status = _ok(_call("GET", "/admin/status"))
    assert status["features"][-2:] == ["upload", "delete"]
    assert status["upload"] == {"available": True, "reason": None, "message": None}

    storage = _ok(_call("GET", "/admin/storage"))
    assert storage["uploads"] == []
    staging = storage["upload_staging"]
    assert os.path.basename(staging["path"]) == ".provision-upload"
    assert _same_file(os.path.dirname(staging["path"]), models_dir)
    assert staging["bytes"] == 0 and staging["trash_bytes"] == 0

    # a models dir stays as the operator left it until the first upload: no staging folder, no session records
    assert not os.path.exists(_staging())
    assert not os.path.exists(os.path.join(state_dir, "uploads"))
    # a dry run of the first upload creates the staging folder (that is where it learns it can write); no record yet
    _create("first", [_tiny()], expect=200, dry_run=True)
    assert os.path.isdir(_staging()) and os.listdir(_staging()) == []
    assert not os.path.exists(os.path.join(state_dir, "uploads"))
    # the scanner of the models dir never lists the staging folder
    assert ".provision-upload" not in _models(reload=True)


def test_upload_unavailable_without_models_dir():
    server.models_dir = None
    _enable()
    server.start()

    body = _ok(_call("GET", "/admin/models/upload"))
    assert body["available"] is False and body["reason"] == "no_models_dir"
    assert "--models-dir" in body["message"]
    assert body["models_dir"] is None and body["limits"] == LIMITS and body["sessions"] == []
    for dry_run in (False, True):
        res = _call("POST", "/admin/models/upload", {"name": "x1", "files": _files([_tiny()]), "dry_run": dry_run})
        _err(res, 409, "upload_unavailable", reason="no_models_dir")
    # the feature is there, the status says why it cannot be used
    status = _ok(_call("GET", "/admin/status"))
    assert "upload" in status["features"]
    assert status["upload"]["available"] is False and status["upload"]["reason"] == "no_models_dir"
    storage = _ok(_call("GET", "/admin/storage"))
    assert storage["uploads"] == [] and storage["upload_staging"]["path"] is None
    # a cache model is deleted with DELETE /models, never from here
    _err(_call("POST", "/admin/models/delete", {"model": MODEL_B}), 409, "not_deletable", reason="cache")


def test_upload_unavailable_while_staging_is_not_a_folder():
    """a file in the place of the staging folder: uploads are unavailable until it is gone, without a restart"""
    blocker = _staging()
    with open(blocker, "wb") as f:
        f.write(b"not a folder")
    _enable()
    server.start()

    body = _ok(_call("GET", "/admin/models/upload"))
    assert body["available"] is False and body["reason"] == "staging_unusable"
    res = _call("POST", "/admin/models/upload", {"name": "x1", "files": _files([_tiny()])})
    _err(res, 409, "upload_unavailable", reason="staging_unusable")
    assert _ok(_call("GET", "/admin/status"))["upload"]["reason"] == "staging_unusable"

    os.remove(blocker)
    assert _ok(_call("GET", "/admin/models/upload"))["available"] is True
    sid = _create("x1", [_tiny()])["upload"]["id"]
    assert os.path.isdir(_staging(sid))


@pytest.mark.skipif(os.name == "nt" or (hasattr(os, "geteuid") and os.geteuid() == 0),
                    reason="needs POSIX permissions and a user other than root")
def test_upload_unavailable_on_a_read_only_models_dir():
    os.chmod(models_dir, 0o555)
    try:
        _enable()
        server.start()
        body = _ok(_call("GET", "/admin/models/upload"))
        assert body["available"] is False and body["reason"] == "not_writable"
        res = _call("POST", "/admin/models/upload", {"name": "x1", "files": _files([_tiny()])})
        _err(res, 409, "upload_unavailable", reason="not_writable")
    finally:
        os.chmod(models_dir, 0o755)


#
# happy paths
#


def test_upload_single_file_in_small_chunks():
    tiny = _tiny()
    size = os.path.getsize(tiny)
    sha = gg.sha256_file(tiny)
    data = open(tiny, "rb").read()
    _enable()
    server.start()

    dry = _create("tiny-up", [tiny], expect=200, dry_run=True)
    assert dry["upload"] == {
        "id": None, "name": "tiny-up", "chunk_max": LIMITS["chunk_max"], "chunk_default": LIMITS["chunk_default"],
        "files": [{"index": 0, "name": os.path.basename(tiny), "size": size}], "settings": {},
    }
    assert set(dry["disk"]) == {"free_bytes", "total_bytes", "needed_bytes", "other_uploads_bytes", "shares_disk_with"}
    assert dry["disk"]["needed_bytes"] == size and dry["disk"]["other_uploads_bytes"] == 0
    assert "not_enough_space" not in dry["warnings"] and "larger_than_disk" not in dry["warnings"]

    created = _create("tiny-up", [tiny])
    up = created["upload"]
    sid = up["id"]
    assert re.fullmatch(r"[0-9a-f]{32}", sid)
    assert set(up) == SESSION_KEYS
    assert up["state"] == "receiving" and up["substate"] is None and up["blocked_by"] is None
    assert up["total_bytes"] == size and up["received_bytes"] == 0 and up["n_files"] == 1
    assert up["auto_publish"] is True and up["settings"] == {} and up["settings_mode"] == "replace"
    assert isinstance(up["expires_at"], str) and up["expires_at"] != ""
    assert up["chunk_max"] == LIMITS["chunk_max"] and up["chunk_default"] == LIMITS["chunk_default"]
    assert up["recovered"] is False and up["record_persisted"] is True
    assert up["files"] == [{
        "index": 0, "name": os.path.basename(tiny), "size": size, "offset": 0, "durable_offset": 0, "complete": False,
        "dropped": False, "role": None, "sha256": None, "sha256_expected": None, "head_crc32": None,
        "final_name": None, "header": {"state": "none"}, "error": None,
    }]
    assert set(created) == {"upload", "disk", "warnings", "locks"}

    # the record in the protected state dir is the authority; the staging folder holds the data and an offsets hint
    record = _read_json(_record(sid))
    assert record["id"] == sid and record["name"] == "tiny-up" and record["state"] == "receiving"
    hint = _read_json(_staging(sid, "session.json"))
    assert set(hint) == {"version", "id", "name", "note", "files"}
    assert set(hint["files"][0]) == {"index", "name", "size", "durable_offset", "dropped"}

    # 64 KiB chunks with a CRC32 each
    offset = 0
    head_crc = None
    answer = None
    while offset < size:
        piece = data[offset:offset + 64 * KiB]
        answer = _ok(_chunk(sid, 0, offset, piece))
        assert set(answer) == CHUNK_KEYS
        assert answer["offset"] == offset + len(piece) and answer["durable_offset"] <= answer["offset"]
        assert answer["file"] == 0 and answer["total_bytes"] == size and answer["file_error"] is None
        assert answer["received_bytes"] == answer["offset"]
        offset = answer["offset"]
        if head_crc is None and offset >= MiB:
            head_crc = _session(sid)["files"][0]["head_crc32"]
    assert answer is not None
    assert answer["file_complete"] is True and answer["durable_offset"] == size and answer["state"] == "checking"
    assert head_crc == gg.head_crc32(tiny)  # the resume identity: CRC32 of the first MiB

    s = _wait_state(sid)
    assert s["state"] == "published", _brief(s)
    f0 = s["files"][0]
    assert f0["sha256"] == sha and f0["complete"] is True and f0["role"] == "model" and f0["final_name"] == "tiny-up.gguf"
    assert f0["header"]["state"] == "ok" and f0["header"]["arch"] == "llama"
    result = s["result"]
    assert result["model"] == "tiny-up" and result["layout"] == "file" and result["target"] == "tiny-up.gguf"
    assert result["rename"] in RENAME_METHODS and result["reload_error"] is None and result["settings_saved"] is False
    assert result["files"] == [{"name": "tiny-up.gguf", "role": "model", "size": size, "sha256": sha}]
    assert s["plan"]["layout"] == "file" and s["plan"]["id"] == "tiny-up"
    assert s["facts"]["arch"] == "llama" and s["facts"]["n_vocab"] > 0 and s["facts"]["n_layer"] > 0
    assert s["expires_at"] is None and s["error"] is None and s["blocked"] is None

    published = os.path.join(models_dir, "tiny-up.gguf")
    assert gg.sha256_file(published) == sha
    model = _models()["tiny-up"]
    assert model["status"]["value"] == "unloaded" and model["source"] == "models_dir"  # a reload never starts it
    stage1._wait_until(lambda: not os.path.exists(_staging(sid)), 15, "the staging folder of the upload to be removed")
    storage = _ok(_call("GET", "/admin/storage"))
    assert storage["uploads"] == [] and storage["upload_staging"]["bytes"] == 0
    # finished sessions stay listed (24 h) for the UI, with their record
    listed = {x["id"]: x for x in _ok(_call("GET", "/admin/models/upload"))["sessions"]}
    assert set(listed[sid]) == SUMMARY_KEYS and listed[sid]["state"] == "published"
    assert os.path.isfile(_record(sid))
    _assert_alive()


def test_upload_never_visible_before_publish():
    """the model list, rescanned all the time (?reload=1), never shows an upload before it is published"""
    _enable(PROVISION_ADMIN_TEST_CHECK_DELAY_MS="1500")  # a longer checking phase
    server.start()
    uploads = [("early-single", [_tiny()]), ("sp-early", _split("early"))]
    names = {name for name, _ in uploads}
    seen: list = []  # (request sent, answer received, the upload ids in the answer)
    stop = threading.Event()

    def poll():
        while not stop.is_set():
            t0 = time.time()
            try:
                ids = set(_models(reload=True).keys())
            except Exception:
                continue
            seen.append((t0, time.time(), ids & names))
            time.sleep(0.02)

    poller = threading.Thread(target=poll, daemon=True)
    poller.start()
    try:
        for name, paths in uploads:
            sid = _create(name, paths)["upload"]["id"]
            for i, path in enumerate(paths):
                _send(sid, i, path, start=0, first_only=True)
            for i, path in enumerate(paths):
                _send(sid, i, path)
            last_before = 0.0  # when the session was last seen receiving or checking (asked at that time)
            deadline = time.time() + 120
            while True:
                assert time.time() < deadline, f"{name} was not published"
                t0 = time.time()
                s = _session(sid)
                if s["state"] in ("receiving", "checking"):
                    last_before = t0
                elif s["state"] == "published":
                    break
                else:
                    assert s["state"] == "publishing", _brief(s)
                time.sleep(0.05)
            early = [x for x in seen if name in x[2] and x[1] < last_before]
            assert not early, f"{name} was listed before it was published: {early[:3]}"
            stage1._wait_until(lambda: any(name in x[2] for x in seen), 10, f"{name} in the model list")
    finally:
        stop.set()
        poller.join(10)
    assert len(seen) > 20  # the list was really asked all the time


def test_upload_split_set_published_as_one_folder():
    parts = _split("stories", 3)
    _enable()
    server.start()

    sid, s = _upload("split-up", parts, chunk=128 * KiB)
    assert s["state"] == "published", _brief(s)
    folder = os.path.join(models_dir, "split-up")
    want = ["split-up-%05d-of-00003.gguf" % i for i in (1, 2, 3)]
    assert sorted(os.listdir(folder)) == want  # the loader's own shard names
    for i, name in enumerate(want):
        assert gg.sha256_file(os.path.join(folder, name)) == gg.sha256_file(parts[i])
    assert s["result"]["layout"] == "folder" and s["result"]["target"] == "split-up"
    assert [f["name"] for f in s["result"]["files"]] == ["split-up/" + n for n in want]
    assert [f["header"].get("split") for f in s["files"]] == [{"no": i, "count": 3} for i in range(3)]
    assert s["plan"]["files"] == [{"index": i, "final_name": want[i], "role": "model", "split_no": i} for i in range(3)]
    assert s["facts"]["arch"] == "llama"

    # it really loads: a request through the router starts a child on the split set
    res = server.make_request("POST", "/tokenize", data={"model": "split-up", "content": "hello world"}, timeout=180)
    assert res.status_code == 200 and len(res.body["tokens"]) > 0, res.body
    assert server.make_request("POST", "/models/unload", data={"model": "split-up"}).status_code == 200
    stage1._wait_until(lambda: _models()["split-up"]["status"]["value"] == "unloaded", 60, "split-up to stop")


def test_upload_long_name_whose_published_path_fits():
    """the longest names whose published path stays within 259 characters (MAX_PATH on Windows): staging adds about 55
    characters to it. A single file and a model with its projector publish as they are (the staging uses long paths).
    A split model is loaded by llama.cpp only from a path of at most 259 characters (it builds the shard paths in a
    buffer of PATH_MAX), so on Windows the upload waits with the longest name that works, and publishes under it
    without anything being uploaded again"""
    parts = _split("long", 3)
    proj = _projector("mmproj-long.gguf")
    root = len(os.path.abspath(models_dir))
    length = min(128, (259 - root - len("\\\\-00001-of-00003.gguf")) // 2)
    single = min(128, 259 - root - len("\\.gguf"))
    if length < 8:
        pytest.skip("the temporary directory of this test is too deep")
    _enable()
    server.start()

    name = "one-" + "x" * (single - 4)
    assert len(os.path.join(os.path.abspath(models_dir), name + ".gguf")) <= 259
    sid, s = _upload(name, [_tiny()])
    assert s["state"] == "published", _brief(s)
    assert os.path.isfile(os.path.join(models_dir, name + ".gguf")) and name in _models()

    pair = min(128, (259 - root - len("\\\\mmproj-.gguf")) // 2)
    name = "pair-" + "x" * (pair - 5)
    sid, s = _upload(name, [_tiny(), proj])
    assert s["state"] == "published", _brief(s)
    assert sorted(os.listdir(os.path.join(models_dir, name))) == sorted([name + ".gguf", "mmproj-" + name + ".gguf"])

    name = "long-" + "x" * (length - 5)
    assert len(os.path.join(os.path.abspath(models_dir), name, name + "-00001-of-00003.gguf")) <= 259
    sid, s = _upload(name, parts, chunk=512 * KiB, wait=("published", "ready", "rejected"))
    if os.name == "nt" and s["state"] == "ready":
        assert s["blocked_by"] == "check_failed" and s["blocked"]["code"] == "path_too_long", _brief(s)
        shorter = s["blocked"]["max_name_length"]
        assert 0 < shorter < len(name) and str(shorter) in s["blocked"]["message"]
        name = name[:shorter]
        _ok(_call("POST", "/admin/models/upload/update", {"id": sid, "name": name}))
        assert _call("POST", "/admin/models/upload/publish", {"id": sid}).status_code == 202  # the Retry
        s = _wait_state(sid, ("published", "rejected", "ready"))
    assert s["state"] == "published", _brief(s)
    assert sorted(os.listdir(os.path.join(models_dir, name))) == [f"{name}-{i:05d}-of-00003.gguf" for i in (1, 2, 3)]
    assert name in _models()


def test_upload_model_with_projector_and_placement():
    """the projector picked first (it often sorts first): the server classifies by content, the placement chosen at
    upload time is saved before the files become visible and reaches the arguments of the model"""
    proj = _projector("mmproj-pair.gguf")
    tiny = _tiny()
    _enable()
    server.start()

    sid, s = _upload("pair-up", [proj, tiny], settings={"ctx-size": 512})
    assert s["state"] == "published", _brief(s)
    folder = os.path.join(models_dir, "pair-up")
    assert sorted(os.listdir(folder)) == ["mmproj-pair-up.gguf", "pair-up.gguf"]
    assert [f["role"] for f in s["files"]] == ["projector", "model"]
    assert s["files"][0]["header"]["arch"] == "clip"
    assert s["result"]["layout"] == "folder" and s["result"]["target"] == "pair-up" and s["result"]["settings_saved"] is True
    assert s["facts"]["vision"] is True and s["facts"]["audio"] is False and s["facts"]["arch"] == "llama"
    assert s["settings"] == {"ctx-size": "512"}

    model = _models()["pair-up"]
    assert model["architecture"]["input_modalities"] == ["text", "image"]
    args = model["status"]["args"]
    assert _flag(args, "--ctx-size") == "512"
    assert _same_file(_flag(args, "--mmproj") or "", os.path.join(folder, "mmproj-pair-up.gguf"))
    row = _row("pair-up")
    assert row["overrides"] == {"ctx-size": "512"} and row["phase"] == "pending"
    entry = _models_file()["pair-up"]
    assert entry["origin"] == "upload" and entry["phase"] == "pending" and entry["settings"] == {"ctx-size": "512"}


def test_upload_placement_is_used_at_the_first_start():
    _enable()
    server.start()
    sid, s = _upload("placed", [_tiny()], settings={"ctx-size": 384})
    assert s["state"] == "published", _brief(s)
    assert _flag(_models()["placed"]["status"]["args"], "--ctx-size") == "384"

    res = server.make_request("POST", "/tokenize", data={"model": "placed", "content": "hello"}, timeout=180)
    assert res.status_code == 200, res.body
    # the two-phase commit of stage 2: a placement that started once is committed
    stage1._wait_until(lambda: _row("placed")["phase"] == "committed", 60, "the placement to be committed")
    assert server.make_request("POST", "/models/unload", data={"model": "placed"}).status_code == 200
    stage1._wait_until(lambda: _models()["placed"]["status"]["value"] == "unloaded", 60, "placed to stop")


@pytest.mark.skipif(os.name == "nt" or not hasattr(os, "geteuid") or os.geteuid() != 0,
                    reason="needs a router running as root (Docker) on POSIX")
def test_upload_as_root_gives_the_files_to_the_owner_of_the_models_dir():
    """Docker runs the router as root: what it creates in the models dir belongs to the owner of the mount, and the
    staging folder stays listable for the group (another router sharing the models dir must be able to scan it)"""
    os.chown(models_dir, 1000, 1000)
    _enable()
    server.start()
    # the lock of an open session too: another router on the models dir (as uid 1000) must be able to probe it
    open_sid = _create("lock-owner", [_tiny()])["upload"]["id"]
    st = os.stat(_staging(open_sid, ".lock"))
    assert (st.st_uid, st.st_gid) == (1000, 1000) and st.st_mode & 0o777 == 0o660, oct(st.st_mode)
    _cancel(open_sid)
    sid, s = _upload("owned", [_projector("mmproj-owned.gguf"), _tiny()])
    assert s["state"] == "published", _brief(s)
    folder = os.path.join(models_dir, "owned")
    for path in [_staging(), folder] + [os.path.join(folder, n) for n in os.listdir(folder)]:
        st = os.stat(path)
        assert (st.st_uid, st.st_gid) == (1000, 1000), path
        # folders 0770 (never 0700: the group lists them), files 0660
        assert st.st_mode & 0o777 == (0o770 if os.path.isdir(path) else 0o660), (path, oct(st.st_mode))


def test_upload_real_model_and_projector():
    """the tinygemma3 model and its projector from the shared cache (conftest.py downloads them)"""
    found = sorted(glob.glob(os.path.join(SHARED_CACHE_DIR, "models--ggml-org--tinygemma3-GGUF", "snapshots", "*", "*.gguf")))
    mmproj = [p for p in found if os.path.basename(p).startswith("mmproj")]
    model = [p for p in found if not os.path.basename(p).startswith("mmproj")]
    if not mmproj or not model:
        pytest.skip("tinygemma3 is not in the model cache")
    _enable()
    server.start()

    sid, s = _upload("gemma-up", [mmproj[0], model[0]], chunk=8 * MiB)
    assert s["state"] == "published", _brief(s)
    assert sorted(os.listdir(os.path.join(models_dir, "gemma-up"))) == ["gemma-up.gguf", "mmproj-gemma-up.gguf"]
    assert s["facts"]["arch"] == "gemma3" and s["facts"]["vision"] is True
    assert _models()["gemma-up"]["architecture"]["input_modalities"] == ["text", "image"]


#
# the chunk rules
#


def test_upload_chunk_rules():
    tiny = _tiny()
    size = os.path.getsize(tiny)
    data = open(tiny, "rb").read()
    _enable()
    server.start()
    sid = _create("rules", [tiny])["upload"]["id"]

    # append, a duplicate (a retry), an overlap that matches with a new tail
    assert _ok(_chunk(sid, 0, 0, data[:100000]))["offset"] == 100000
    assert _ok(_chunk(sid, 0, 0, data[:100000]))["offset"] == 100000
    assert _ok(_chunk(sid, 0, 50000, data[50000:150000]))["offset"] == 150000
    # an overlap that differs: not the same file
    bad = bytearray(data[100000:200000])
    bad[10] ^= 0xFF
    e = _err(_chunk(sid, 0, 100000, bytes(bad)), 409, "content_mismatch")
    assert e["offset"] == 100000
    # a gap; a chunk beyond the declared size
    _err(_chunk(sid, 0, 200000, data[200000:210000]), 409, "offset_mismatch", offset=150000)
    _err(_chunk(sid, 0, 150000, data[150000:] + b"xx"), 400, "beyond_declared_size")
    # the CRC32 header: damaged on the way, not 8 hex digits; it is optional
    _err(_chunk(sid, 0, 150000, data[150000:160000], crc=False, headers={"X-Chunk-CRC32": "00000000"}), 400, "crc_mismatch")
    _err(_chunk(sid, 0, 150000, data[150000:160000], crc=False, headers={"X-Chunk-CRC32": "xyz"}),
         400, "invalid_request_error", param="X-Chunk-CRC32")
    # a body that is not raw octets (a multipart or JSON body never reaches the file)
    _err(_chunk(sid, 0, 150000, data[150000:160000], content_type="application/json"), 415, "unsupported_media_type")
    # parameters
    res = _response(requests.post(server.make_url(f"/admin/models/upload/chunk?id={sid}&file=7&offset=150000"),
                                  data=data[150000:160000], headers=_chunk_headers()))
    _err(res, 400, "invalid_request_error", param="file")
    res = _response(requests.post(server.make_url(f"/admin/models/upload/chunk?id={sid}&file=x&offset=150000"),
                                  data=data[150000:160000], headers=_chunk_headers()))
    _err(res, 400, "invalid_request_error", param="file")
    res = _response(requests.post(server.make_url(f"/admin/models/upload/chunk?id={sid}&file=0&offset=-1"),
                                  data=data[150000:160000], headers=_chunk_headers()))
    _err(res, 400, "invalid_request_error", param="offset")
    _err(_chunk("zz", 0, 0, data[:1000]), 400, "invalid_request_error", param="id")
    _err(_chunk(NO_SESSION, 0, 0, data[:1000]), 410, "upload_gone", state=None)
    assert _session(sid)["files"][0]["offset"] == 150000  # none of the refused chunks changed anything

    # a verify-only probe compares without writing (the resume identity after a re-pick of the file)
    v = _ok(_chunk(sid, 0, 0, data[:100000], verify=True))
    assert v == {"verified": True, "verified_bytes": 100000, "offset": 150000, "durable_offset": v["durable_offset"],
                 "file_complete": False, "state": "receiving"}
    assert _ok(_chunk(sid, 0, 100000, data[100000:200000], verify=True))["verified_bytes"] == 50000
    bad = bytearray(data[:1000])
    bad[0] ^= 1
    _err(_chunk(sid, 0, 0, bytes(bad), verify=True), 409, "content_mismatch", offset=0)
    assert _session(sid)["files"][0]["offset"] == 150000

    # without the CRC header, then the rest
    assert _ok(_chunk(sid, 0, 150000, data[150000:400000], crc=False))["offset"] == 400000
    assert _send(sid, 0, tiny).status_code == 200
    s = _wait_state(sid)
    assert s["state"] == "published" and s["files"][0]["sha256"] == gg.sha256_file(tiny), _brief(s)
    assert os.path.getsize(os.path.join(models_dir, "rules.gguf")) == size


def test_upload_resent_chunk_inside_the_stored_bytes_is_idempotent():
    tiny = _tiny()
    data = open(tiny, "rb").read()
    _enable()
    server.start()
    sid = _create("late-retry", [tiny])["upload"]["id"]
    assert _ok(_chunk(sid, 0, 0, data[:100000]))["offset"] == 100000
    assert _ok(_chunk(sid, 0, 100000, data[100000:200000]))["offset"] == 200000

    # the answer to the first chunk was lost, the client sends it again after the second one went through
    res = _chunk(sid, 0, 0, data[:100000])
    assert res.status_code == 200, res.body
    assert res.body["offset"] == 200000
    # a resume whose overlap (here all 200000 bytes) is longer than the chunk (here 64 KiB; the UI resends up to 1 MiB
    # and steps down to 240 KiB chunks behind a proxy that limits the body size)
    assert _send(sid, 0, tiny, chunk=64 * KiB, start=0, stop_at=200000).status_code == 200
    assert _session(sid)["files"][0]["offset"] == 200000


def test_upload_chunk_size_limits():
    """chunk_max is accepted; a larger body, or one without a length, is answered before it is read"""
    big = _fx("big17.gguf")
    gg.big_copy(_tiny(), big, 16 * MiB)  # a model of about 17 MiB
    _enable()
    server.start()
    sid = _create("sized", [big])["upload"]["id"]

    status, body = _raw(_chunk_target(sid), _chunk_headers(), b"", LIMITS["chunk_max"] + 1)
    assert status == 413, body
    assert body["error"]["type"] == "chunk_too_large" and body["error"]["chunk_max"] == LIMITS["chunk_max"]
    status, body = _raw(_chunk_target(sid), {**_chunk_headers(), "Transfer-Encoding": "chunked"})
    assert status == 411 and body["error"]["type"] == "length_required", body
    # the length decides what is admitted, so it must be the real framing: "Content-Length: 0" next to a chunked body
    # (httplib would read the chunked body), no length at all (read until the connection closes), a content coding
    status, body = _raw(_chunk_target(sid), {**_chunk_headers(), "Transfer-Encoding": "chunked"}, b"5\r\nhello\r\n0\r\n\r\n", 0)
    assert status == 411 and body["error"]["type"] == "length_required", body
    status, body = _raw(_chunk_target(sid), {**_chunk_headers(), "Connection": "close"}, b"hello")
    assert status == 411 and body["error"]["type"] == "length_required", body
    status, body = _raw(_chunk_target(sid), {**_chunk_headers(), "Content-Encoding": "gzip"}, b"", 0)
    assert status == 415 and body["error"]["type"] == "unsupported_media_type", body
    assert _session(sid)["files"][0]["offset"] == 0

    with open(big, "rb") as f:
        first = f.read(LIMITS["chunk_max"])
    assert _ok(_chunk(sid, 0, 0, first))["offset"] == LIMITS["chunk_max"]
    del first
    assert _send(sid, 0, big, chunk=8 * MiB).status_code == 200
    s = _wait_state(sid)
    assert s["state"] == "published" and s["files"][0]["sha256"] == gg.sha256_file(big), _brief(s)


def test_upload_cut_off_body_changes_nothing():
    tiny = _tiny()
    data = open(tiny, "rb").read()
    _enable(PROVISION_ADMIN_TEST_CHECKPOINT_BYTES=str(64 * KiB))
    server.start()
    sid = _create("cut", [tiny])["upload"]["id"]
    assert _ok(_chunk(sid, 0, 0, data[:200000]))["offset"] == 200000

    # the connection drops after 150000 of 400000 bytes: the handler never runs, nothing of it is stored
    s = _hold_chunk(sid, 0, 200000, data[200000:600000], 150000)
    time.sleep(0.3)
    s.close()
    time.sleep(0.5)
    f0 = _session(sid)["files"][0]
    assert f0["offset"] == 200000
    assert os.path.getsize(_staging(sid, "0.part")) == 200000
    # and the session takes its next chunk at once (the admission of the cut body was released)
    assert _ok(_chunk(sid, 0, 200000, data[200000:600000]))["offset"] == 600000
    assert _send(sid, 0, tiny).status_code == 200
    s = _wait_state(sid)
    assert s["state"] == "published" and s["files"][0]["sha256"] == gg.sha256_file(tiny), _brief(s)


def test_upload_one_chunk_at_a_time_per_session():
    tiny = _tiny()
    data = open(tiny, "rb").read()
    _enable()
    server.start()
    sid = _create("one-at-a-time", [tiny])["upload"]["id"]

    held = _hold_chunk(sid, 0, 0, data[:200000], 1000)
    try:
        time.sleep(0.5)
        # refused before its body is read, the held chunk is not disturbed
        _err(_chunk(sid, 0, 0, data[:1000]), 409, "chunk_in_progress")
        held.sendall(data[1000:200000])
        status, body = _read_response(held)
    finally:
        held.close()
    assert status == 200 and body["offset"] == 200000, body
    assert _ok(_chunk(sid, 0, 200000, data[200000:300000]))["offset"] == 300000


def test_upload_writers_are_bounded_by_the_sessions():
    """max_writers = max_sessions: with one chunk in flight in every open session no chunk gets a 429 after its body
    was read (the memory bound is one chunk per session); a second chunk of a session is refused before its body"""
    tiny = _tiny()
    data = open(tiny, "rb").read()
    _enable()
    server.start()
    sids = [_create(f"writer-{i}", [tiny])["upload"]["id"] for i in range(LIMITS["max_sessions"])]
    held = [_hold_chunk(sid, 0, 0, data[:200000], 1000) for sid in sids]
    try:
        time.sleep(0.5)
        _err(_chunk(sids[0], 0, 0, data[:1000]), 409, "chunk_in_progress")
        answers = []
        for s in held:
            s.sendall(data[1000:200000])
            answers.append(_read_response(s))
    finally:
        for s in held:
            s.close()
    assert [a[0] for a in answers] == [200] * len(sids), answers
    assert all(a[1]["offset"] == 200000 for a in answers)


def test_upload_other_client_and_takeover():
    tiny = _tiny()
    data = open(tiny, "rb").read()
    _enable()
    server.start()
    sid = _create("tabs", [tiny])["upload"]["id"]

    assert _ok(_chunk(sid, 0, 0, data[:100000], client="tab-1"))["offset"] == 100000
    # another tab within 30 s of the last chunk
    e = _err(_chunk(sid, 0, 100000, data[100000:200000], client="tab-2"), 409, "other_client_active")
    assert isinstance(e["idle_ms"], int) and 0 <= e["idle_ms"] < 30000
    s = _session(sid)
    assert s["files"][0]["offset"] == 100000 and isinstance(s["last_client_idle_ms"], int)
    # an explicit takeover; the old tab stops at its next chunk
    assert _ok(_chunk(sid, 0, 100000, data[100000:200000], client="tab-2", takeover=True))["offset"] == 200000
    _err(_chunk(sid, 0, 200000, data[200000:300000], client="tab-1"), 409, "other_client_active")
    # the same tab after a reload keeps its id (sessionStorage) and simply continues
    assert _ok(_chunk(sid, 0, 200000, data[200000:300000], client="tab-2"))["offset"] == 300000


def test_upload_session_limit():
    tiny = _tiny()
    _enable()
    server.start()
    sids = [_create(f"open-{i}", [tiny])["upload"]["id"] for i in range(LIMITS["max_sessions"])]

    res = _call("POST", "/admin/models/upload", {"name": "open-x", "files": _files([tiny])})
    e = _err(res, 409, "too_many_uploads")
    assert sorted(x["id"] for x in e["sessions"]) == sorted(sids)
    assert all(set(x) == SUMMARY_KEYS for x in e["sessions"])
    # a dry run still answers (the dialog can show the name and the disk check)
    _create("open-x", [tiny], expect=200, dry_run=True)
    _cancel(sids[0])
    _create("open-x", [tiny])


def test_upload_cancel():
    tiny = _tiny()
    data = open(tiny, "rb").read()
    _enable()
    server.start()
    sid = _create("cancelled", [tiny])["upload"]["id"]
    _ok(_chunk(sid, 0, 0, data[:160000]))

    body = _cancel(sid)
    assert body["cancelled"] is True and body["freed_bytes"] == 160000 and body["upload"]["state"] == "cancelled"
    stage1._wait_until(lambda: not os.path.exists(_staging(sid)), 15, "the staging folder of the cancelled upload to go")
    _err(_chunk(sid, 0, 160000, data[160000:170000]), 410, "upload_gone", state="cancelled")
    _err(_call("POST", "/admin/models/upload/update", {"id": sid, "name": "other"}), 410, "upload_gone", state="cancelled")
    _err(_call("POST", "/admin/models/upload/publish", {"id": sid}), 410, "upload_gone", state="cancelled")
    # cancelling again changes nothing
    again = _cancel(sid)
    assert again["cancelled"] is True and again["freed_bytes"] == 0
    s = _session(sid)
    assert s["state"] == "cancelled" and s["expires_at"] is None
    assert _ok(_call("GET", "/admin/storage"))["uploads"] == []
    # the name is free again
    _create("cancelled", [tiny], expect=200, dry_run=True)


def test_upload_ids_are_checked_and_never_404():
    """an unknown session answers 410 on every route (upstream rewrites the body of a 404), a malformed id 400"""
    _enable()
    server.start()
    for sid, status, error_type in ((NO_SESSION, 410, "upload_gone"), ("zz", 400, "invalid_request_error"),
                                    ("../" + "0" * 29, 400, "invalid_request_error")):
        _err(_call("GET", "/admin/models/upload?id=" + sid), status, error_type)
        for path in ("/admin/models/upload/update", "/admin/models/upload/publish", "/admin/models/upload/cancel"):
            _err(_call("POST", path, {"id": sid}), status, error_type)
        _err(_chunk(sid, 0, 0, b"GGUF" + b"\0" * 100), status, error_type)
    _err(_call("POST", "/admin/models/upload/cancel", {}), 400, "invalid_request_error", param="id")


#
# what the server refuses, and the router stays alive
#


def test_upload_not_gguf_marks_only_that_file():
    tiny = _tiny()
    junk = _fx("junk.gguf")
    with open(junk, "wb") as f:
        f.write(os.urandom(300000))
    _enable()
    server.start()

    # alone: 422 on the first chunk, the session stays so the file can be replaced by cancel and a new upload
    sid = _create("junk", [junk])["upload"]["id"]
    _err(_chunk(sid, 0, 0, open(junk, "rb").read()[:65536]), 422, "not_gguf", file=0, reason="not_gguf")
    s = _session(sid)
    assert s["state"] == "receiving" and s["files"][0]["offset"] == 0
    assert s["files"][0]["error"]["code"] == "not_gguf" and s["files"][0]["header"]["state"] == "error"
    _err(_chunk(sid, 0, 0, open(junk, "rb").read()[:65536]), 422, "not_gguf")
    _err(_call("POST", "/admin/models/upload/update", {"id": sid, "drop_files": [0]}), 400, "invalid_request_error")
    _cancel(sid)

    # next to a model: drop the wrong file, the model is published
    sid = _create("with-junk", [tiny, junk])["upload"]["id"]
    _err(_send(sid, 1, junk, chunk=65536, start=0, first_only=True), 422, "not_gguf", file=1)
    assert _send(sid, 0, tiny).status_code == 200
    assert _session(sid)["state"] == "receiving"  # waits for the user: drop it or cancel
    upd = _ok(_call("POST", "/admin/models/upload/update", {"id": sid, "drop_files": [1]}))
    assert upd["upload"]["files"][1]["dropped"] is True
    assert not os.path.exists(_staging(sid, "1.part"))
    s = _wait_state(sid)
    assert s["state"] == "published" and os.path.isfile(os.path.join(models_dir, "with-junk.gguf")), _brief(s)
    assert s["result"]["layout"] == "file" and len(s["result"]["files"]) == 1
    _err(_chunk(sid, 1, 0, b"GGUF"), 410, "upload_gone", state="published")
    _assert_alive()


def test_upload_unknown_architecture_rejected_after_the_first_chunk():
    unknown = gg.copy(_tiny(), _fx("unknown.gguf"), strings={"general.architecture": "nonexistent-arch"})
    _enable()
    server.start()
    sid = _create("unknown-arch", [unknown])["upload"]["id"]

    t0 = time.time()
    assert _send(sid, 0, unknown, chunk=256 * KiB, start=0, first_only=True).status_code == 200
    s = _wait_state(sid, ("rejected",), timeout=60)
    assert s["error"]["code"] == "unknown_architecture" and s["error"]["file"] == 0, s["error"]
    assert "nonexistent-arch" in s["error"]["message"]
    assert s["received_bytes"] <= 256 * KiB and time.time() - t0 < 60  # the rest of the file is never sent
    stage1._wait_until(lambda: not os.path.exists(_staging(sid)), 15, "the data of the rejected upload to be deleted")
    _err(_chunk(sid, 0, 256 * KiB, b"x" * 1000), 410, "upload_gone", state="rejected")
    _assert_alive()


@pytest.mark.parametrize("kind", ["adapter", "imatrix"])
def test_upload_adapter_and_imatrix_are_refused(kind: str):
    path = gg.copy(_tiny(), _fx(kind + ".gguf"), strings={"general.type": kind})
    _enable()
    server.start()
    sid = _create("is-" + kind, [path])["upload"]["id"]

    assert _send(sid, 0, path, chunk=256 * KiB, start=0, first_only=True).status_code == 200
    stage1._wait_until(lambda: _session(sid)["files"][0]["error"] is not None, 30, "the header check")
    s = _session(sid)
    assert s["files"][0]["error"]["code"] == kind and s["state"] == "receiving"
    data = open(path, "rb").read()
    _err(_chunk(sid, 0, 256 * KiB, data[256 * KiB:512 * KiB]), 422, "file_rejected", file=0, reason=kind)
    _assert_alive()


def _set_rule_case(case: str) -> tuple[list, str]:
    parts = _split("rule", 3)
    tiny = _tiny()
    if case == "split_incomplete":
        return [parts[0], parts[1]], case
    if case == "split_mismatch":
        return [parts[0], parts[0], parts[2]], case  # two files claim to be the first one
    if case == "two_models":
        return [tiny, gg.copy(tiny, _fx("tiny-copy.gguf"))], case
    if case == "two_projectors":
        return [tiny, _projector("mmproj-a.gguf"), _projector("mmproj-b.gguf")], case
    if case == "projector_without_model":  # a projector uploaded alone: CLIP is never taken as the model
        return [_projector("mmproj-alone.gguf")], case
    raise AssertionError(case)


@pytest.mark.parametrize("case", ["split_incomplete", "split_mismatch", "two_models", "two_projectors",
                                  "projector_without_model"])
def test_upload_wrong_set_of_files_rejected_after_the_headers(case: str):
    paths, code = _set_rule_case(case)
    _enable()
    server.start()
    sid = _create("set-" + case.replace("_", "-"), paths)["upload"]["id"]

    for i, path in enumerate(paths):  # only the headers: the verdict comes before the bulk data
        assert _send(sid, i, path, chunk=128 * KiB, start=0, first_only=True).status_code == 200
    s = _wait_state(sid, ("rejected",), timeout=60)
    assert s["error"]["code"] == code, s["error"]
    assert s["received_bytes"] <= len(paths) * 128 * KiB
    _assert_alive()


def test_upload_truncated_model_rejected_by_the_load_check():
    truncated = gg.copy(_tiny(), _fx("truncated.gguf"), truncate=5000)  # the last tensor ends after the end of the file
    _enable()
    server.start()
    sid, s = _upload("truncated", [truncated])
    assert s["state"] == "rejected" and s["error"]["code"] == "truncated", _brief(s)
    assert "not within the file bounds" in s["error"]["message"]
    assert "truncated" not in _models(reload=True)
    _assert_alive()


@pytest.mark.parametrize("variant,code", [
    ("uint8_has_vision_encoder", "projector_bad_metadata"),
    ("no_modality", "projector_without_modality"),
])
def test_upload_unusable_projector_can_be_dropped(variant: str, code: str):
    """the uint8 has_vision_encoder projector crashed the router on every reload; here only that file is refused"""
    if variant == "uint8_has_vision_encoder":
        proj = _projector("mmproj-bad.gguf", typed={"clip.has_vision_encoder": gg.val(gg.T_UINT8, 1)})
    else:
        proj = _projector("mmproj-none.gguf", vision=False, audio=False)
    tiny = _tiny()
    _enable()
    server.start()
    sid = _create("with-bad-proj", [tiny, proj])["upload"]["id"]

    assert _send(sid, 1, proj, start=0, first_only=True).status_code == 200
    stage1._wait_until(lambda: _session(sid)["files"][1]["error"] is not None, 30, "the header check of the projector")
    s = _session(sid)
    assert s["files"][1]["error"]["code"] == code and s["files"][1]["header"]["state"] == "error"
    assert _send(sid, 0, tiny).status_code == 200
    _ok(_call("POST", "/admin/models/upload/update", {"id": sid, "drop_files": [1]}))
    s = _wait_state(sid)
    assert s["state"] == "published", _brief(s)
    assert os.path.isfile(os.path.join(models_dir, "with-bad-proj.gguf"))  # alone: a top-level file
    assert _models()["with-bad-proj"]["architecture"]["input_modalities"] == ["text"]
    _assert_alive()


def test_upload_metadata_size_guard():
    """the router reads at most 64 MiB of GGUF metadata, without allocating per value, before any parser runs"""
    huge = _fx("huge-meta.gguf")
    # an array that declares 65 MiB of uint8 in a file of a few bytes
    array = gg.kv("x.huge", b"\x09\x00\x00\x00" + b"\x00\x00\x00\x00" + (65 * MiB).to_bytes(8, "little"))
    gg.write(huge, 3, [gg.kv("general.architecture", gg.val(gg.T_STRING, "llama")), array], [])
    dims = _fx("bad-dims.gguf")
    # a tensor with 7 dimensions
    gg.write(dims, 3, [gg.kv("general.architecture", gg.val(gg.T_STRING, "llama"))],
             [(gg.enc_str("t") + (7).to_bytes(4, "little") + (1).to_bytes(8, "little") * 7 + (0).to_bytes(4, "little"), b"\0" * 32)])
    _enable()
    server.start()

    for name, path, reason in (("huge-meta", huge, "header_too_large"), ("bad-dims", dims, "malformed")):
        sid = _create(name, [_tiny(), path])["upload"]["id"]
        _err(_send(sid, 1, path, start=0, first_only=True), 422, "file_rejected", file=1, reason=reason)
        assert _session(sid)["files"][1]["error"]["code"] == reason
        _cancel(sid)
    _assert_alive()


def test_upload_sha256_mismatch_keeps_the_data():
    tiny = _tiny()
    sha = gg.sha256_file(tiny)
    _enable()
    server.start()

    # a wrong expected value (a hash copied from another quant): ready, the data is kept
    sid, s = _upload("sha-wrong", [tiny], sha=["0" * 64])
    assert s["state"] == "ready" and s["blocked_by"] == "sha256_mismatch", _brief(s)
    assert s["blocked"]["code"] == "sha256_mismatch" and s["blocked"]["file"] == 0
    assert s["blocked"]["expected"] == "0" * 64 and s["blocked"]["actual"] == sha
    assert s["files"][0]["sha256"] == sha and s["files"][0]["sha256_expected"] == "0" * 64
    assert os.path.isdir(_staging(sid)) and isinstance(s["expires_at"], str)
    # Retry without a fix: checked again, still blocked
    assert _call("POST", "/admin/models/upload/publish", {"id": sid}).status_code == 202
    s = _wait_state(sid, ("ready", "published", "rejected"))
    stage1._wait_until(lambda: _session(sid)["state"] == "ready", 30, "the check to run again")
    assert _session(sid)["blocked_by"] == "sha256_mismatch"
    # the right value (upper case is fine), then publish: nothing is uploaded again
    upd = _ok(_call("POST", "/admin/models/upload/update", {"id": sid, "files": [{"index": 0, "sha256": sha.upper()}]}))
    assert upd["upload"]["files"][0]["sha256_expected"] == sha
    assert _call("POST", "/admin/models/upload/publish", {"id": sid}).status_code == 202
    s = _wait_state(sid, ("published", "rejected"))
    assert s["state"] == "published", _brief(s)

    # publish anyway
    sid, s = _upload("sha-anyway", [tiny], sha=["1" * 64], wait=("ready",))
    assert _call("POST", "/admin/models/upload/publish", {"id": sid, "ignore_sha256_mismatch": True}).status_code == 202
    assert _wait_state(sid, ("published", "rejected"))["state"] == "published"

    # a value that is not a SHA-256 is refused at create
    res = _call("POST", "/admin/models/upload", {"name": "sha-bad", "files": _files([tiny], sha=["abc"])})
    _err(res, 400, "invalid_request_error", param="files")


@pytest.mark.parametrize("mode", ["exit", "hang", "spawn"])
def test_upload_helper_failure_keeps_the_data(mode: str):
    """a validate helper that crashes, hangs or cannot start is not a verdict about the file: ready, Retry"""
    _enable(PROVISION_ADMIN_TEST_VALIDATE=mode + ",2", PROVISION_ADMIN_TEST_HELPER_TIMEOUT_MS="3000")
    server.start()
    sid, s = _upload("helper-" + mode, [_tiny()], wait=("ready", "published", "rejected"))
    assert s["state"] == "ready" and s["blocked_by"] == "check_failed", _brief(s)
    assert os.path.isdir(_staging(sid))
    assert _call("POST", "/admin/models/upload/publish", {"id": sid}).status_code == 202  # the Retry
    s = _wait_state(sid, ("published", "rejected"))
    assert s["state"] == "published", _brief(s)
    _assert_alive()


def test_upload_staging_failure_is_undone_and_retried():
    """a move into the final layout that fails (here the second of two, an injected EIO) is undone: every file is back
    under its staged name, so a Retry starts from a clean state and publishes without anything being uploaded again"""
    tiny = _tiny()
    proj = _projector("mmproj-stage.gguf")
    _enable(PROVISION_ADMIN_TEST_STAGE_FAIL_AT="1")
    server.start()
    sid, s = _upload("stage-fail", [tiny, proj], wait=("ready", "published", "rejected"))
    assert s["state"] == "ready" and s["blocked_by"] == "check_failed", _brief(s)
    assert s["blocked"]["code"] == "staging_failed" and "nothing was changed" in s["blocked"]["message"], s["blocked"]
    assert os.path.isfile(_staging(sid, "0.part")) and os.path.isfile(_staging(sid, "1.part"))
    assert not os.path.exists(_staging(sid, "out", "stage-fail"))
    assert _call("POST", "/admin/models/upload/publish", {"id": sid}).status_code == 202  # the Retry
    s = _wait_state(sid, ("published", "rejected"))
    assert s["state"] == "published", _brief(s)
    folder = os.path.join(models_dir, "stage-fail")
    assert sorted(os.listdir(folder)) == ["mmproj-stage-fail.gguf", "stage-fail.gguf"]
    assert gg.sha256_file(os.path.join(folder, "stage-fail.gguf")) == gg.sha256_file(tiny)


def test_upload_drop_after_the_files_were_arranged_is_refused():
    """once the checks passed and the files carry their final names, a dropped file would still be published with the
    others: the drop is refused (cancel and upload again without it), and nothing changes"""
    tiny = _tiny()
    proj = _projector("mmproj-late.gguf")
    _enable()
    server.start()
    sid, s = _upload("no-late-drop", [tiny, proj], auto_publish=False, wait=("ready", "published", "rejected"))
    assert s["state"] == "ready" and s["blocked_by"] == "manual", _brief(s)
    _err(_call("POST", "/admin/models/upload/update", {"id": sid, "drop_files": [1]}), 409, "upload_busy", staged=True)
    s = _session(sid)
    assert s["files"][1]["dropped"] is False and s["state"] == "ready" and s["blocked_by"] == "manual"
    assert _call("POST", "/admin/models/upload/publish", {"id": sid}).status_code == 202
    s = _wait_state(sid, ("published", "rejected"))
    assert s["state"] == "published", _brief(s)
    names = ["mmproj-no-late-drop.gguf", "no-late-drop.gguf"]
    assert sorted(f["name"] for f in s["result"]["files"]) == ["no-late-drop/" + n for n in names]
    assert sorted(os.listdir(os.path.join(models_dir, "no-late-drop"))) == names


#
# restarts of the router
#


def test_upload_resumes_after_a_router_crash():
    parts = _split("crash", 3)
    _enable(PROVISION_ADMIN_TEST_CHECKPOINT_BYTES=str(128 * KiB))
    server.start()
    sid = _create("sp-crash", parts, settings={"ctx-size": 384})["upload"]["id"]
    for i, path in enumerate(parts):
        _send(sid, i, path, chunk=64 * KiB, start=0, first_only=True)
    _send(sid, 0, parts[0], chunk=64 * KiB, stop_at=450000)
    before = _session(sid)["files"][0]
    assert before["offset"] == 450000 and 0 < before["durable_offset"] <= 450000

    _restart(kill=True)
    s = _session(sid)
    f0 = s["files"][0]
    assert s["state"] == "receiving" and s["recovered"] is True and s["recovered_without_record"] is False
    # never more than the durable offset of the record: bytes after it may not have reached the disk
    assert f0["offset"] == f0["durable_offset"] and before["durable_offset"] <= f0["offset"] <= before["offset"]
    assert os.path.getsize(_staging(sid, "0.part")) == f0["offset"]
    assert s["settings"] == {"ctx-size": "384"}  # the settings came back from the record
    assert "continues after the restart" in _log()

    # resume with an overlap (it proves the same file), the rest of the files after it; the overlap is shorter than
    # the chunk, see test_upload_resent_chunk_inside_the_stored_bytes_is_idempotent for a longer one
    assert _send(sid, 0, parts[0], chunk=64 * KiB, start=max(0, f0["offset"] - 32 * KiB)).status_code == 200
    for i in (1, 2):
        assert _send(sid, i, parts[i], chunk=64 * KiB).status_code == 200
    s = _wait_state(sid)
    assert s["state"] == "published", _brief(s)
    folder = os.path.join(models_dir, "sp-crash")
    names = sorted(os.listdir(folder))
    assert [gg.sha256_file(os.path.join(folder, n)) for n in names] == [gg.sha256_file(p) for p in parts]
    assert [f["sha256"] for f in s["files"]] == [gg.sha256_file(p) for p in parts]  # read again from disk
    assert _row("sp-crash")["overrides"] == {"ctx-size": "384"}


def test_upload_checks_run_again_after_a_restart():
    tiny = _tiny()
    _enable(PROVISION_ADMIN_TEST_CHECK_DELAY_MS="5000")
    server.start()
    sid = _create("restart-check", [tiny])["upload"]["id"]
    assert _send(sid, 0, tiny).status_code == 200
    assert _session(sid)["state"] == "checking"

    env.delenv("PROVISION_ADMIN_TEST_CHECK_DELAY_MS")
    _restart(kill=True)
    s = _wait_state(sid)
    assert s["state"] == "published" and s["recovered"] is True, _brief(s)
    assert s["files"][0]["sha256"] == gg.sha256_file(tiny)


def test_upload_staged_file_refused_after_a_restart_waits_in_ready():
    """a router that stops during the load check finds the files already under their final names; when one of them is
    refused then (damaged on disk while the router was down, or a stricter build), it cannot be dropped any more: the
    upload waits in ready (Retry, or cancel) instead of running its checks again and again"""
    tiny = _tiny()
    _enable()
    server.start()
    sid, s = _upload("staged-bad", [tiny], auto_publish=False, wait=("ready", "published", "rejected"))
    assert s["state"] == "ready" and s["blocked_by"] == "manual", _brief(s)
    server.stop()
    # the record of a router that died in the load check: checking, the files staged; the staged file is damaged
    rec = _read_json(_record(sid))
    assert rec["staged"] is True
    rec.update({"state": "checking", "blocked_by": "", "blocked": None})
    _write_json(_record(sid), rec)
    with open(_staging(sid, "out", "staged-bad.gguf"), "r+b") as f:
        f.write(b"XXXX")

    _restart()
    s = _wait_state(sid, ("ready", "published", "rejected"), timeout=60)
    assert s["state"] == "ready" and s["blocked_by"] == "check_failed", _brief(s)
    assert s["blocked"]["code"] == "not_gguf" and s["blocked"]["file"] == 0, s["blocked"]
    assert s["files"][0]["error"] is None  # nothing to drop: the choice is Retry or cancel
    # it rests: the record is not rewritten over and over (a loop wrote it about 86 times a second)
    changes, last, t0 = 0, os.path.getmtime(_record(sid)), time.time()
    while time.time() - t0 < 2:
        m = os.path.getmtime(_record(sid))
        if m != last:
            changes, last = changes + 1, m
        time.sleep(0.02)
    assert changes <= 2, changes
    # a Retry checks the file again: the same answer, the same rest
    assert _call("POST", "/admin/models/upload/publish", {"id": sid}).status_code == 202
    stage1._wait_until(lambda: _session(sid)["state"] == "ready", 30, "the check to run again")
    assert _session(sid)["blocked"]["code"] == "not_gguf"
    _cancel(sid)
    stage1._wait_until(lambda: not os.path.exists(_staging(sid)), 15, "the data of the cancelled upload to be removed")
    _assert_alive()


def test_upload_interrupted_staging_is_repaired_after_a_restart():
    """a router that stops between the moves of a staging (a crash, the OOM killer) left one file under its final name:
    after the restart it is moved back to its staged name, so nothing is re-created empty or uploaded again"""
    tiny = _tiny()
    proj = _projector("mmproj-repair.gguf")
    _enable(PROVISION_ADMIN_TEST_CHECK_DELAY_MS="600000")
    server.start()
    sid = _create("repaired", [tiny, proj])["upload"]["id"]
    for i, path in enumerate([tiny, proj]):
        assert _send(sid, i, path, start=0, first_only=True).status_code == 200
    for i, path in enumerate([tiny, proj]):
        res = _send(sid, i, path)
        assert res is None or res.status_code == 200
    stage1._wait_until(lambda: _session(sid)["plan"] is not None, 30, "the plan of the upload")
    s = _session(sid)
    assert s["state"] == "checking", _brief(s)
    final = s["files"][0]["final_name"]
    assert final == "repaired.gguf"
    server.stop()
    # what a staging that stopped after its first move leaves behind: the model under its final name in the folder
    # that the record names (a staging records its names and its folder before anything moves)
    rec = _read_json(_record(sid))
    rec["staged_id"] = "repaired"
    _write_json(_record(sid), rec)
    os.makedirs(_staging(sid, "out", "repaired"))
    os.replace(_staging(sid, "0.part"), _staging(sid, "out", "repaired", final))

    env.delenv("PROVISION_ADMIN_TEST_CHECK_DELAY_MS")
    _restart()
    s = _wait_state(sid, ("published", "rejected", "ready"))
    assert s["state"] == "published" and s["recovered"] is True, _brief(s)
    folder = os.path.join(models_dir, "repaired")
    assert sorted(os.listdir(folder)) == ["mmproj-repaired.gguf", "repaired.gguf"]
    assert gg.sha256_file(os.path.join(folder, "repaired.gguf")) == gg.sha256_file(tiny)
    assert "moved back" in _log()


def test_upload_rebuilt_from_the_staging_hint_without_a_record():
    """the record in the state dir is the authority; without one (state dir not writable at the time) a session is
    rebuilt from the offsets hint in staging, checked against the files, with no settings and no automatic publish"""
    tiny = _tiny()
    data = open(tiny, "rb").read()
    _enable(PROVISION_ADMIN_TEST_CHECKPOINT_BYTES=str(64 * KiB))
    server.start()
    sid = _create("hinted", [tiny], settings={"ctx-size": 320})["upload"]["id"]
    assert _send(sid, 0, tiny, chunk=64 * KiB, stop_at=400000).status_code == 200
    other = _create("forged", [tiny])["upload"]["id"]
    _ok(_chunk(other, 0, 0, data[:100000]))
    server.stop()

    os.remove(_record(sid))
    os.remove(_record(other))
    on_disk = os.path.getsize(_staging(sid, "0.part"))
    # a hint that claims more than there is on disk: the file length wins
    hint = _read_json(_staging(sid, "session.json"))
    hint["files"][0]["durable_offset"] = os.path.getsize(tiny)
    _write_json(_staging(sid, "session.json"), hint)
    # a hint with a name that is not a valid id is not taken at all
    forged = _read_json(_staging(other, "session.json"))
    forged["name"] = "../outside"
    _write_json(_staging(other, "session.json"), forged)

    _restart()
    s = _session(sid)
    assert s["recovered"] is True and s["recovered_without_record"] is True
    assert "recovered_without_record" in s["warnings"]
    assert s["files"][0]["offset"] == on_disk and s["state"] == "receiving"
    assert s["settings"] == {} and s["auto_publish"] is False  # nothing from the hint is trusted beyond offsets
    _err(_call("GET", "/admin/models/upload?id=" + other), 410, "upload_gone")

    assert _send(sid, 0, tiny).status_code == 200
    s = _wait_state(sid)
    assert s["state"] == "ready" and s["blocked_by"] == "manual", _brief(s)
    assert _call("POST", "/admin/models/upload/publish", {"id": sid}).status_code == 202
    s = _wait_state(sid, ("published", "rejected"))
    assert s["state"] == "published" and s["files"][0]["sha256"] == gg.sha256_file(tiny), _brief(s)
    assert os.path.isfile(_record(sid))  # written again once the state dir works


def test_upload_head_crc32_survives_a_restart():
    tiny = _tiny()
    _enable(PROVISION_ADMIN_TEST_CHECKPOINT_BYTES=str(256 * KiB))
    server.start()
    sid = _create("crc-restart", [tiny])["upload"]["id"]
    assert _send(sid, 0, tiny, chunk=256 * KiB, stop_at=1100000).status_code == 200
    f0 = _session(sid)["files"][0]
    assert f0["durable_offset"] >= MiB and f0["head_crc32"] == gg.head_crc32(tiny)

    _restart(kill=True)
    f0 = _session(sid)["files"][0]
    assert f0["offset"] >= MiB
    assert f0["head_crc32"] == gg.head_crc32(tiny)


#
# expiry, sweep, two routers on one models dir
#


def test_upload_expiry_counts_only_while_the_router_runs():
    tiny = _tiny()
    data = open(tiny, "rb").read()
    _enable(PROVISION_ADMIN_TEST_UPLOAD_TTL_S="5", PROVISION_ADMIN_TEST_UPLOAD_SWEEP_MS="300")
    server.start()
    assert _ok(_call("GET", "/admin/models/upload"))["limits"]["ttl_s"] == 5
    sid = _create("ttl", [tiny])["upload"]["id"]
    _ok(_chunk(sid, 0, 0, data[:100000]))
    server.stop()
    time.sleep(7)  # longer than the TTL while the router is down

    _restart()
    s = _session(sid)
    assert s["state"] == "receiving" and isinstance(s["expires_at"], str), s  # not expired at boot
    s = _wait_state(sid, ("expired",), timeout=30)
    assert s["error"]["code"] == "expired"
    stage1._wait_until(lambda: not os.path.exists(_staging(sid)), 15, "the data of the expired upload to be removed")


def test_upload_sweep_removes_leftovers_and_old_records():
    tiny = _tiny()
    data = open(tiny, "rb").read()
    abandoned = _staging("f" * 32)  # a session folder without a record and without a lock holder
    os.makedirs(abandoned)
    with open(os.path.join(abandoned, "0.part"), "wb") as f:
        f.write(b"x" * 1000)
    trash = _staging("trash-0123456789abcdef")
    os.makedirs(trash)
    with open(os.path.join(trash, "x.gguf"), "wb") as f:
        f.write(b"x" * 1000)
    _enable(PROVISION_ADMIN_TEST_UPLOAD_TTL_S="3", PROVISION_ADMIN_TEST_UPLOAD_SWEEP_MS="300",
            PROVISION_ADMIN_TEST_UPLOAD_ORPHAN_S="0", PROVISION_ADMIN_TEST_UPLOAD_KEEP_S="4")
    server.start()

    sid = _create("will-expire", [tiny])["upload"]["id"]
    _ok(_chunk(sid, 0, 0, data[:100000]))
    s = _wait_state(sid, ("expired",), timeout=30)
    assert s["state"] == "expired"
    stage1._wait_until(lambda: os.listdir(_staging()) == [], 15, "the staging folder to be swept clean")
    # the record of a finished session is kept for a while for the UI, then forgotten
    stage1._wait_until(lambda: _call("GET", "/admin/models/upload?id=" + sid).status_code == 410, 30,
                       "the finished record to be forgotten")
    assert not os.path.exists(_record(sid))


def test_upload_two_routers_share_a_models_dir():
    """a second router on the same models dir (several nodes on one share) never touches a live session of the first"""
    tiny = _tiny()
    data = open(tiny, "rb").read()
    _enable()
    server.start()
    sid = _create("on-a", [tiny])["upload"]["id"]
    _ok(_chunk(sid, 0, 0, data[:300000]))
    size_before = os.path.getsize(_staging(sid, "0.part"))

    _enable(PROVISION_ADMIN_STATE_DIR=state_dir + "-b", PROVISION_ADMIN_TEST_UPLOAD_SWEEP_MS="300",
            PROVISION_ADMIN_TEST_UPLOAD_ORPHAN_S="0")
    other = _second_router()
    other.start()
    time.sleep(2)  # several sweeps of router B
    sessions_b = _ok(_call("GET", "/admin/models/upload", srv=other))["sessions"]
    assert all(x["id"] != sid for x in sessions_b)
    assert os.path.isdir(_staging(sid)) and os.path.getsize(_staging(sid, "0.part")) == size_before
    # B uploads at the same time into its own session
    sid_b, s_b = _upload("on-b", [tiny], srv=other)
    assert s_b["state"] == "published", _brief(s_b)

    assert _send(sid, 0, tiny).status_code == 200
    s = _wait_state(sid)
    assert s["state"] == "published" and s["files"][0]["sha256"] == gg.sha256_file(tiny), _brief(s)
    assert {"on-a", "on-b"} <= set(_models(reload=True)) and {"on-a", "on-b"} <= set(_models(reload=True, srv=other))
    other.stop()


def test_upload_a_second_router_never_takes_over_a_stopped_routers_session():
    """router A stops with an unfinished upload; router B on the same models dir (another node on a shared folder)
    neither sweeps it nor rebuilds it from its hint, however short its orphan time: the upload waits for A, with its
    settings. Only once A's own TTL could have run out (plus a margin) does B remove the folder."""
    tiny = _tiny()
    data = open(tiny, "rb").read()
    _enable(PROVISION_ADMIN_TEST_CHECKPOINT_BYTES=str(64 * KiB))
    server.start()
    sid = _create("owned-by-a", [tiny], settings={"ctx-size": 320})["upload"]["id"]
    _ok(_chunk(sid, 0, 0, data[:300000]))
    hint = _read_json(_staging(sid, "session.json"))
    assert re.fullmatch(r"[0-9a-f]{32}", hint["owner"]) and isinstance(hint["idle_ms"], int), hint
    server.stop()
    size_before = os.path.getsize(_staging(sid, "0.part"))
    assert size_before == 300000

    _enable(PROVISION_ADMIN_STATE_DIR=state_dir + "-b", PROVISION_ADMIN_TEST_UPLOAD_SWEEP_MS="300",
            PROVISION_ADMIN_TEST_UPLOAD_ORPHAN_S="0")
    other = _second_router()
    other.start()
    time.sleep(2)  # several sweeps of B
    _err(_call("GET", "/admin/models/upload?id=" + sid, srv=other), 410, "upload_gone")
    assert os.path.getsize(_staging(sid, "0.part")) == size_before
    _restart(other)  # B's recovery reads the hint again: still A's
    time.sleep(1)
    _err(_call("GET", "/admin/models/upload?id=" + sid, srv=other), 410, "upload_gone")
    assert "belongs to another router" in stage1._read_text(other.log_path)
    assert os.path.getsize(_staging(sid, "0.part")) == size_before
    other.stop()

    # A comes back: its upload continues where it was, with its settings
    _enable(PROVISION_ADMIN_TEST_CHECKPOINT_BYTES=str(64 * KiB))
    _restart()
    s = _session(sid)
    assert s["state"] == "receiving" and s["files"][0]["offset"] == size_before, _brief(s)
    assert s["settings"] == {"ctx-size": "320"} and s["recovered_without_record"] is False
    assert _send(sid, 0, tiny).status_code == 200
    s = _wait_state(sid)
    assert s["state"] == "published" and s["files"][0]["sha256"] == gg.sha256_file(tiny), _brief(s)
    assert _row("owned-by-a")["overrides"] == {"ctx-size": "320"}

    # an upload whose router never comes back: B removes it once that router's TTL could have run out (2 s here,
    # no margin)
    sid2 = _create("left-by-a", [tiny])["upload"]["id"]
    _ok(_chunk(sid2, 0, 0, data[:100000]))
    server.stop()
    _enable(PROVISION_ADMIN_STATE_DIR=state_dir + "-b", PROVISION_ADMIN_TEST_UPLOAD_SWEEP_MS="300",
            PROVISION_ADMIN_TEST_UPLOAD_ORPHAN_S="0", PROVISION_ADMIN_TEST_UPLOAD_TTL_S="2",
            PROVISION_ADMIN_TEST_UPLOAD_FOREIGN_MARGIN_S="0")
    _restart(other)
    stage1._wait_until(lambda: not os.path.exists(_staging(sid2)), 20, "B to remove the folder of the gone router")
    assert "of another router" in stage1._read_text(other.log_path)
    other.stop()


#
# names
#


INVALID_NAMES = [
    ("", "empty"), ("x" * 129, "too_long"),
    ("-x", "charset"), ("a b", "charset"), (".provision-upload", "charset"), ("a/b", "charset"), ("a\\b", "charset"),
    ("café", "charset"), ("x:y", "charset"),
    ("my-mmproj", "mmproj"), ("MMPROJ-x", "mmproj"),
    ("a.gguf.b", "gguf"), ("x.GGUF", "gguf"),
    ("mtp-x", "draft_prefix"), ("dspark-x", "draft_prefix"), ("dflash-x", "draft_prefix"), ("Eagle3-x", "draft_prefix"),
    ("m-00001-of-00002", "shard_suffix"), ("trail.", "trailing_dot"),
    ("CON", "reserved"), ("nul.txt", "reserved"), ("com1", "reserved"), ("Lpt9", "reserved"),
]


def test_upload_name_rules():
    _enable()
    server.start()
    wrong = []
    for name, reason in INVALID_NAMES:
        res = _call("POST", "/admin/models/upload", {"name": name, "files": _files([_tiny()]), "dry_run": True})
        error = res.body.get("error", {}) if isinstance(res.body, dict) else {}
        if not (res.status_code == 400 and error.get("type") == "invalid_name" and error.get("reason") == reason
                and error.get("param") == "name"):
            wrong.append((name[:24], reason, res.status_code, res.body))
    assert not wrong, wrong
    for name in ("x" * 128, "Qwen2.5-7B-Instruct_Q4_K_M", "a", "0-model.v2", "console"):
        _create(name, [_tiny()], expect=200, dry_run=True)


def test_upload_name_collisions():
    tiny = _tiny()
    _place(tiny, "existing.gguf")
    _place(tiny, "fold/fold.gguf")
    preset_model = gg.copy(tiny, _fx("preset-tiny.gguf"))
    server.models_preset = _write_preset(f"[presetA]\nmodel = {preset_model.replace(os.sep, '/')}\nalias = aliasA\n")
    _enable()
    server.start()
    # a file dropped in by hand after the last scan: the scanner's own id rule (every ".gguf" removed)
    _place(tiny, "late.gguf.gguf")
    held = _create("held", [tiny])["upload"]["id"]

    cases = [
        ("existing", "model", False), ("EXISTING", "model", True), ("fold", "model", False),
        ("presetA", "preset", False), ("aliasa", "alias", True), ("late", "file", False), ("HELD", "upload", True),
    ]
    for name, by, case_variant in cases:
        res = _call("POST", "/admin/models/upload", {"name": name, "files": _files([tiny]), "dry_run": True})
        e = _err(res, 409, "name_taken", by=by, case_variant=case_variant)
        assert e["param"] == "name" and e["other"].lower() == name.lower()
    # the same rule when the name changes later
    sid = _create("free-name", [tiny])["upload"]["id"]
    _err(_call("POST", "/admin/models/upload/update", {"id": sid, "name": "Existing"}), 409, "name_taken", by="model")
    _err(_call("POST", "/admin/models/upload/update", {"id": sid, "name": "held"}), 409, "name_taken", by="upload")
    assert _ok(_call("POST", "/admin/models/upload/update", {"id": sid, "name": "Free-Name"}))["upload"]["name"] == "Free-Name"
    _cancel(held)
    _create("held", [tiny], expect=200, dry_run=True)


def test_upload_name_taken_before_publish_then_renamed():
    tiny = _tiny()
    size = os.path.getsize(tiny)
    _enable()
    server.start()
    sid, s = _upload("appears", [tiny], auto_publish=False, wait=("ready",))
    assert s["blocked_by"] == "manual"

    # a model with this name is dropped in by hand meanwhile
    _place(tiny, "appears.gguf")
    assert _call("POST", "/admin/models/upload/publish", {"id": sid}).status_code == 202
    stage1._wait_until(lambda: _session(sid)["blocked_by"] == "name_taken", 30, "the publish to stop at the name")
    s = _session(sid)
    assert s["state"] == "ready" and s["blocked"]["code"] == "name_taken"
    # a new name, then publish: nothing is uploaded again
    _ok(_call("POST", "/admin/models/upload/update", {"id": sid, "name": "appears-2"}))
    assert _call("POST", "/admin/models/upload/publish", {"id": sid}).status_code == 202
    s = _wait_state(sid, ("published", "rejected"))
    assert s["state"] == "published" and s["result"]["model"] == "appears-2" and s["received_bytes"] == size, _brief(s)
    assert os.path.isfile(os.path.join(models_dir, "appears-2.gguf"))
    assert gg.sha256_file(os.path.join(models_dir, "appears.gguf")) == gg.sha256_file(tiny)  # the other one untouched
    # publish of a published upload: 200, nothing happens
    assert _call("POST", "/admin/models/upload/publish", {"id": sid}).status_code == 200


def test_upload_rename_conflict_at_publish_rolls_back_the_entry():
    """the target appears between the last check and the rename: never replaced, the settings entry written before the
    rename is rolled back"""
    _enable(PROVISION_ADMIN_TEST_RENAME_ERRNO="EEXIST")
    server.start()
    sid, s = _upload("conflict", [_tiny()], settings={"ctx-size": 384}, wait=("ready", "published", "rejected"))
    assert s["state"] == "ready" and s["blocked_by"] == "name_taken", _brief(s)
    assert s["blocked"]["by"] == "file" and s["blocked"]["other"] == "conflict.gguf"
    assert "conflict" not in _models_file()
    unattached = _ok(_call("GET", "/admin/models/settings")).get("unattached", [])
    assert all(u["id"] != "conflict" for u in unattached)
    assert not os.path.exists(os.path.join(models_dir, "conflict.gguf"))


@pytest.mark.skipif(os.name == "nt", reason="the no-replace rename chain renameat2 -> link -> check+rename is POSIX only")
@pytest.mark.parametrize("rename_errno,link_errno,layout,method", [
    ("EINVAL", None, "file", "link"),
    ("EINVAL", "EPERM", "file", "check+rename"),
    ("EINVAL", None, "folder", "check+rename"),  # a folder is never linked
])
def test_upload_rename_fallbacks(rename_errno: str, link_errno: str | None, layout: str, method: str):
    _enable(PROVISION_ADMIN_TEST_RENAME_ERRNO=rename_errno, PROVISION_ADMIN_TEST_LINK_ERRNO=link_errno)
    server.start()
    paths = [_tiny()] if layout == "file" else _split("fallback", 2)
    sid, s = _upload("fallback", paths)
    assert s["state"] == "published" and s["result"]["rename"] == method and s["result"]["layout"] == layout, _brief(s)
    target = os.path.join(models_dir, "fallback.gguf" if layout == "file" else "fallback")
    assert os.path.exists(target)
    stage1._wait_until(lambda: not os.path.exists(_staging(sid)), 15, "the staging folder to be removed")


@pytest.mark.skipif(os.name == "nt", reason="the injected rename errno is POSIX only")
def test_upload_rename_error_blocks_with_publish_error():
    _enable(PROVISION_ADMIN_TEST_RENAME_ERRNO="EACCES")
    server.start()
    sid, s = _upload("rename-fails", [_tiny()], settings={"ctx-size": 256}, wait=("ready", "published", "rejected"))
    assert s["state"] == "ready" and s["blocked_by"] == "publish_error" and s["blocked"]["code"] == "publish_failed", _brief(s)
    assert s["blocked"]["errno"] != 0 and "rename-fails" not in _models_file()  # the entry written before is rolled back
    assert os.path.isdir(_staging(sid)) and not os.path.exists(os.path.join(models_dir, "rename-fails.gguf"))
    # a Retry runs the publish again (the hook fails every attempt here): blocked again, still nothing visible
    assert _call("POST", "/admin/models/upload/publish", {"id": sid}).status_code == 202
    stage1._wait_until(lambda: _session(sid)["blocked_by"] == "publish_error", 30, "the publish to be tried again")
    assert "rename-fails" not in _models(reload=True) and os.path.isdir(_staging(sid))


def _concurrent_creates(bodies: list) -> list:
    barrier = threading.Barrier(len(bodies))
    results: list = [None] * len(bodies)

    def one(i: int):
        barrier.wait()
        results[i] = _call("POST", "/admin/models/upload", bodies[i])

    threads = [threading.Thread(target=one, args=(i,)) for i in range(len(bodies))]
    for t in threads:
        t.start()
    for t in threads:
        t.join(60)
    return results


def test_upload_concurrent_creates_reserve_the_name():
    _enable()
    server.start()
    counts = []
    for round_ in range(3):
        results = _concurrent_creates([{"name": f"race-{round_}", "files": _files([_tiny()])}] * 4)
        created = [r for r in results if r.status_code == 201]
        for r in created:
            _cancel(r.body["upload"]["id"])
        counts.append(len(created))
        for r in results:
            if r.status_code != 201:
                _err(r, 409, "name_taken", by="upload")
    assert counts == [1, 1, 1]


def test_upload_concurrent_creates_respect_the_session_limit():
    _enable()
    server.start()
    counts = []
    for round_ in range(3):
        results = _concurrent_creates([{"name": f"lim-{round_}-{i}", "files": _files([_tiny()])} for i in range(16)])
        created = [r for r in results if r.status_code == 201]
        for r in created:
            _cancel(r.body["upload"]["id"])
        counts.append(len(created))
        for r in results:
            if r.status_code != 201:
                _err(r, 409, "too_many_uploads")
    # the name check, the count and the insert are one step: exactly max_sessions of them get in, every time
    assert counts == [LIMITS["max_sessions"]] * 3, counts


def test_upload_create_validation():
    tiny = _tiny()
    _enable()
    server.start()
    good = _files([tiny])

    def create(body: dict) -> ServerResponse:
        return _call("POST", "/admin/models/upload", body)

    _err(create({"name": "v", "files": []}), 400, "invalid_request_error", param="files")
    _err(create({"name": "v"}), 400, "invalid_request_error", param="files")
    _err(create({"name": "v", "files": good * (LIMITS["max_files"] + 1)}), 400, "invalid_request_error", param="files")
    _err(create({"name": "v", "files": [{"name": "x.gguf", "size": 23}]}), 400, "invalid_request_error", param="files")
    _err(create({"name": "v", "files": [{"name": "x.gguf"}]}), 400, "invalid_request_error", param="files")
    _err(create({"name": "v", "files": [{"name": "x.gguf", "size": "100"}]}), 400, "invalid_request_error", param="files")
    _err(create({"name": "v", "files": [{"name": "../x.gguf", "size": 1000}]}), 400, "invalid_request_error", param="files")
    _err(create({"name": "v", "files": [{"name": "", "size": 1000}]}), 400, "invalid_request_error", param="files")
    _err(create({"name": "v", "files": ["x.gguf"]}), 400, "invalid_request_error", param="files")
    # draft models (mtp-, dspark-, dflash-, eagle3-) are not supported yet
    e = _err(create({"name": "v", "files": [{"name": "mtp-model.gguf", "size": 1000}]}), 400, "unsupported_file")
    assert e["reason"] == "draft" and e["file"] == 0
    _err(create({"name": "v", "files": good, "settings_mode": "merge"}), 400, "invalid_request_error", param="settings_mode")
    _err(create({"name": "v", "files": good, "auto_publish": "yes"}), 400, "invalid_request_error", param="auto_publish")
    _err(create({"name": "v", "files": good, "dry_run": 1}), 400, "invalid_request_error", param="dry_run")
    _err(create({"name": "v", "files": good, "settings": ["ctx-size"]}), 400, "invalid_request_error", param="settings")
    _err(create({"files": good}), 400, "invalid_name", reason="empty")
    _err(_call("POST", "/admin/models/upload", None), 400, "invalid_request_error")
    assert not os.path.exists(os.path.join(state_dir, "uploads"))  # nothing was created


def test_upload_update_and_publish_rules():
    tiny = _tiny()
    data = open(tiny, "rb").read()
    _enable(PROVISION_ADMIN_TEST_CHECK_DELAY_MS="3000")
    server.start()
    sid = _create("upd", [tiny], settings={"ctx-size": 256}, auto_publish=False)["upload"]["id"]

    up = _ok(_call("POST", "/admin/models/upload/update", {"id": sid, "settings": {"ctx-size": 320}}))["upload"]
    assert up["settings"] == {"ctx-size": "320"}
    up = _ok(_call("POST", "/admin/models/upload/update", {"id": sid, "settings": None}))["upload"]
    assert up["settings"] == {}
    up = _ok(_call("POST", "/admin/models/upload/update", {"id": sid, "name": "upd-2", "settings": {"parallel": 2}}))["upload"]
    assert up["name"] == "upd-2" and up["settings"] == {"parallel": "2"}
    _err(_call("POST", "/admin/models/upload/update", {"id": sid, "name": "bad name"}), 400, "invalid_name", reason="charset")
    _err(_call("POST", "/admin/models/upload/update", {"id": sid, "files": [{"index": 0, "sha256": "zz"}]}),
         400, "invalid_request_error", param="files")
    _err(_call("POST", "/admin/models/upload/update", {"id": sid, "files": [{"index": 3, "sha256": ""}]}),
         400, "invalid_request_error", param="files")
    _err(_call("POST", "/admin/models/upload/update", {"id": sid, "drop_files": [5]}), 400, "invalid_request_error",
         param="drop_files")
    _err(_call("POST", "/admin/models/upload/update", {"id": sid, "settings_mode": "x"}), 400, "invalid_request_error",
         param="settings_mode")
    _err(_call("POST", "/admin/models/upload/update", {"id": sid, "settings": {"ctx-size": "lots"}}), 400,
         "invalid_request_error", param="ctx-size")
    # publish before every byte is there
    _err(_call("POST", "/admin/models/upload/publish", {"id": sid}), 409, "upload_busy", state="receiving")

    assert _send(sid, 0, tiny).status_code == 200
    assert _session(sid)["state"] == "checking"
    _err(_call("POST", "/admin/models/upload/update", {"id": sid, "name": "upd-3"}), 409, "upload_busy", state="checking")
    _err(_chunk(sid, 0, 0, data[:1000]), 409, "upload_busy", state="checking")
    s = _wait_state(sid, ("ready",))
    assert s["blocked_by"] == "manual" and s["name"] == "upd-2"
    # in ready, the name and the settings can still change
    _ok(_call("POST", "/admin/models/upload/update", {"id": sid, "name": "upd-final", "settings": {"ctx-size": 448}}))
    assert _call("POST", "/admin/models/upload/publish", {"id": sid}).status_code == 202
    s = _wait_state(sid, ("published", "rejected"))
    assert s["state"] == "published" and s["result"]["model"] == "upd-final", _brief(s)
    assert _row("upd-final")["overrides"] == {"ctx-size": "448"}
    _err(_call("POST", "/admin/models/upload/update", {"id": sid, "name": "again"}), 410, "upload_gone", state="published")

    # a cancel while checking stops the checks
    sid = _create("cancel-checking", [tiny])["upload"]["id"]
    assert _send(sid, 0, tiny).status_code == 200
    assert _cancel(sid)["upload"]["state"] == "cancelled"
    time.sleep(4)
    assert _session(sid)["state"] == "cancelled"
    assert "cancel-checking" not in _models(reload=True)


#
# placement at upload time
#


def test_upload_settings_follow_the_locks():
    """MODEL_DEVICES and the [*] section of the preset file lock the device of an id that is not listed yet"""
    tiny = _tiny()
    server.models_preset = _write_preset("[*]\ndevice = none\n")
    _enable()
    env.setenv("MODEL_DEVICES", "*=none")
    server.start()

    res = _call("POST", "/admin/models/upload", {"name": "locked", "files": _files([tiny]), "settings": {"device": "none"},
                                                 "dry_run": True})
    _err(res, 409, "field_locked", param="device")
    _err(_call("POST", "/admin/models/upload", {"name": "locked", "files": _files([tiny]), "settings": {"device": "none"}}),
         409, "field_locked", param="device")
    dry = _create("locked", [tiny], expect=200, dry_run=True, settings={"ctx-size": 256})
    assert dry["locks"]["device"]["by"] == "model_devices"
    assert dry["upload"]["settings"] == {"ctx-size": "256"}
    res = _call("POST", "/admin/models/upload", {"name": "locked", "files": _files([tiny]), "settings": {"ctx-size": "lots"},
                                                 "dry_run": True})
    _err(res, 400, "invalid_request_error", param="ctx-size")
    res = _call("POST", "/admin/models/upload", {"name": "locked", "files": _files([tiny]), "settings": {"no-such-key": 1},
                                                 "dry_run": True})
    assert res.status_code == 400, res.body


def test_upload_existing_settings_keep_or_start_fresh():
    tiny = _tiny()
    _place(tiny, "again.gguf")
    _enable()
    server.start()

    def orphan(ctx_size: int) -> None:
        _ok(_call("POST", "/admin/models/settings", {"model": "again", "settings": {"ctx-size": ctx_size}}))
        _remove(os.path.join(models_dir, "again.gguf"))
        assert "again" not in _models(reload=True)

    # the settings of an earlier model with this name are shown; "keep" attaches them again
    orphan(320)
    dry = _create("again", [tiny], expect=200, dry_run=True)
    assert dry["existing_settings"] == {"ctx-size": "320"}
    sid, s = _upload("again", [tiny], settings_mode="keep")
    assert s["state"] == "published" and s["settings_mode"] == "keep", _brief(s)
    assert _row("again")["overrides"] == {"ctx-size": "320"}
    # "replace" (the default) starts fresh: the old entry is removed
    orphan(330)
    sid, s = _upload("again", [tiny])
    assert s["state"] == "published", _brief(s)
    assert _row("again")["overrides"] == {}
    assert "again" not in _models_file()
    # "keep" with a new value on top
    orphan(340)
    sid, s = _upload("again", [tiny], settings_mode="keep", settings={"parallel": 2})
    assert _row("again")["overrides"] == {"ctx-size": "340", "parallel": "2"}


def test_upload_keep_starts_the_trial_of_the_placement_again():
    """settings_mode keep with the values of an entry that was committed for earlier files: the entry starts again as
    pending, so the first start of the new files is a trial (and repeated failures revert it), as for any upload"""
    _enable()
    server.start()
    sid, s = _upload("trial-again", [_tiny()], settings={"ctx-size": 384})
    assert s["state"] == "published", _brief(s)
    res = server.make_request("POST", "/tokenize", data={"model": "trial-again", "content": "hello"}, timeout=180)
    assert res.status_code == 200, res.body
    stage1._wait_until(lambda: _row("trial-again")["phase"] == "committed", 60, "the placement to be committed")
    _ok(_call("POST", "/admin/models/delete", {"model": "trial-again", "stop": True, "remove_settings": False}))
    assert _models_file()["trial-again"]["phase"] == "committed"

    sid, s = _upload("trial-again", [_tiny()], settings_mode="keep")
    assert s["state"] == "published" and s["result"]["settings_saved"] is True, _brief(s)
    row = _row("trial-again")
    assert row["overrides"] == {"ctx-size": "384"} and row["phase"] == "pending", row


def test_upload_settings_that_cannot_be_saved_block_the_publish():
    """the per-model settings file cannot be written: the placement would be lost, so the publish stops and asks"""
    os.makedirs(state_dir)
    with open(os.path.join(state_dir, "admin-models.json"), "w") as f:
        f.write("{ this is not json")
    _enable()
    server.start()
    tiny = _tiny()

    body = _ok(_call("GET", "/admin/models/upload"))
    assert body["settings_writable"] is False and body["settings_error"]
    dry = _create("ro-one", [tiny], expect=200, dry_run=True, settings={"ctx-size": 512})
    assert "state_not_writable" in dry["warnings"]
    sid, s = _upload("ro-one", [tiny], settings={"ctx-size": 512}, wait=("ready", "published", "rejected"))
    assert s["state"] == "ready" and s["blocked_by"] == "settings_not_saved", _brief(s)
    assert "ro-one" not in _models(reload=True)  # nothing visible
    assert _call("POST", "/admin/models/upload/publish", {"id": sid, "without_settings": True}).status_code == 202
    s = _wait_state(sid, ("published", "rejected"))
    assert s["state"] == "published" and s["result"]["settings_saved"] is False, _brief(s)
    # without settings at all there is nothing to save: published at once
    sid, s = _upload("ro-two", [tiny])
    assert s["state"] == "published", _brief(s)


#
# disk
#


def test_upload_disk_warnings():
    tiny = _tiny()
    _enable()
    server.start()
    base = _create("disk", [tiny], expect=200, dry_run=True)["disk"]
    assert base["free_bytes"] > 0 and base["total_bytes"] >= base["free_bytes"]

    # warn only: a prediction never refuses
    res = _call("POST", "/admin/models/upload", {"name": "huge", "files": [{"name": "huge.gguf", "size": 1 << 61}], "dry_run": True})
    assert "larger_than_disk" in _ok(res)["warnings"]
    free = base["free_bytes"]
    if free + 1 <= base["total_bytes"]:
        res = _call("POST", "/admin/models/upload", {"name": "short", "files": [{"name": "s.gguf", "size": free + 1}], "dry_run": True})
        assert "not_enough_space" in _ok(res)["warnings"]
    # the bytes other open uploads still need count against the free space
    sid = _create("open", [tiny])["upload"]["id"]
    dry = _create("next", [tiny], expect=200, dry_run=True)
    assert dry["disk"]["other_uploads_bytes"] == os.path.getsize(tiny)
    if os.name != "nt":  # the admin state shares the file system of the models dir here
        assert dry["disk"]["shares_disk_with"] == ["admin_state"] and "shares_disk_with_state" in dry["warnings"]
    _cancel(sid)


@pytest.mark.parametrize("hook,status,error_type", [
    ("700000,1,ENOSPC", 507, "disk_full"),
    ("700000,1,ENOSPC,sync", 507, "disk_full"),  # a file system that reports it only at fdatasync (NFS, thin LVM)
    pytest.param("700000,1,EDQUOT", 507, "disk_full",
                 marks=pytest.mark.skipif(os.name == "nt", reason="no EDQUOT on Windows")),
    ("700000,1,EIO", 500, "write_failed"),
])
def test_upload_write_failure_keeps_the_session(hook: str, status: int, error_type: str):
    tiny = _tiny()
    _enable(PROVISION_ADMIN_TEST_FAIL_WRITE_AT=hook, PROVISION_ADMIN_TEST_CHECKPOINT_BYTES=str(256 * KiB))
    server.start()
    sid = _create("write-fails", [tiny])["upload"]["id"]

    e = _err(_send(sid, 0, tiny, chunk=128 * KiB), status, error_type)
    if status == 507:
        assert isinstance(e["free_bytes"], int) and e["offset"] == e["durable_offset"]
    # back to the durable offset: nothing after it is trusted
    s = _session(sid)
    assert s["state"] == "receiving" and s["files"][0]["offset"] == e["offset"] == s["files"][0]["durable_offset"]
    assert e["offset"] < 700000 and os.path.getsize(_staging(sid, "0.part")) == e["offset"]
    # the client resumes (the hook fails once): the SHA-256 is read again from disk
    assert _send(sid, 0, tiny, chunk=128 * KiB).status_code == 200
    s = _wait_state(sid)
    assert s["state"] == "published" and s["files"][0]["sha256"] == gg.sha256_file(tiny), _brief(s)


#
# delete and "Remove projector"
#


def test_delete_top_level_file():
    tiny = _tiny()
    size = os.path.getsize(tiny)
    _place(tiny, "topx.gguf")
    _enable()
    server.start()
    _ok(_call("POST", "/admin/models/settings", {"model": "topx", "settings": {"ctx-size": 256}}))
    _ok(_call("POST", "/admin/settings", {"pinned": ["topx"]}))

    dry = _ok(_call("POST", "/admin/models/delete", {"model": "topx", "dry_run": True}))
    assert dry == {
        "model": "topx", "source": "models_dir", "projector_only": False, "total_bytes": size, "n_files": 1, "n_kept": 0,
        "kept_bytes": 0, "running": False, "status": "unloaded", "pinned": True, "has_settings": True, "folder": None,
        "folder_removed": False, "files": [{"path": "topx.gguf", "bytes": size, "role": "model"}], "kept": [],
        "warnings": [], "dry_run": True,
    }
    assert os.path.isfile(os.path.join(models_dir, "topx.gguf"))  # a dry run changes nothing

    body = _ok(_call("POST", "/admin/models/delete", {"model": "topx"}))
    assert body["deleted"] is True and body["freed_bytes"] == size and body["folder_removed"] is False
    assert body["settings_removed"] is True and body["pin_removed"] is True and body["reload_error"] is None
    assert body["files"] == dry["files"] and body["kept"] == []
    assert not os.path.exists(os.path.join(models_dir, "topx.gguf"))
    assert "topx" not in _models() and "topx" not in _models(reload=True)
    assert "topx" not in _models_file()
    assert _ok(_call("GET", "/admin/settings"))["pinned"] == []
    stage1._wait_until(lambda: not [x for x in os.listdir(_staging()) if x.startswith("trash-")], 15,
                       "the trash to be emptied")
    assert _ok(_call("GET", "/admin/storage"))["upload_staging"]["trash_bytes"] == 0
    _err(_call("POST", "/admin/models/delete", {"model": "topx"}), 409, "not_deletable", reason="not_found")


def test_delete_keeps_what_the_model_does_not_reference():
    """several quants in one folder: only the file the model points at goes; the folder and the rest stay"""
    tiny = _tiny()
    _place(tiny, "fold/a-quant.gguf")
    _place(tiny, "fold/b-quant.gguf")
    with open(os.path.join(models_dir, "fold", "README.md"), "w") as f:
        f.write("not part of the model")
    os.makedirs(os.path.join(models_dir, "fold", "sub"))
    with open(os.path.join(models_dir, "fold", "sub", "x.safetensors"), "w") as f:
        f.write("not part of the model")
    _enable()
    server.start()

    dry = _ok(_call("POST", "/admin/models/delete", {"model": "fold", "dry_run": True}))
    assert dry["folder"] == "fold" and dry["folder_removed"] is False and dry["n_files"] == 1
    assert dry["files"][0]["path"] in ("fold/a-quant.gguf", "fold/b-quant.gguf")
    kept = {k["path"]: k for k in dry["kept"]}
    other = ({"fold/a-quant.gguf", "fold/b-quant.gguf"} - {dry["files"][0]["path"]}).pop()
    assert set(kept) == {other, "fold/README.md", "fold/sub"}
    assert kept["fold/sub"]["kind"] == "dir" and kept["fold/sub"]["bytes"] is None and kept["fold/README.md"]["kind"] == "file"
    assert dry["n_kept"] == 3 and "folder_keeps_other_model" in dry["warnings"]

    body = _ok(_call("POST", "/admin/models/delete", {"model": "fold"}))
    assert body["deleted"] is True and body["folder_removed"] is False
    left = sorted(os.listdir(os.path.join(models_dir, "fold")))
    assert left == sorted([os.path.basename(other), "README.md", "sub"])
    assert os.path.isfile(os.path.join(models_dir, "fold", "sub", "x.safetensors"))


def test_delete_keeps_a_projector_and_a_draft_every_model_shares():
    """a draft on the router command line and a projector in the [*] section of the preset file are in the preset of
    every model, but the models directory gave them to no one model: deleting one model (or its projector) never takes
    them from the others. A folder model's own projector is still part of it."""
    tiny = _tiny()
    size = os.path.getsize(tiny)
    _place(tiny, "one.gguf")
    _place(tiny, "two.gguf")
    draft = _place(tiny, "mtp-shared.gguf")  # a draft: never a model of its own
    shared_proj = _place(_projector("mmproj-shared-src.gguf"), "mmproj-shared.gguf")
    _place(tiny, "fm/fm.gguf")
    _place(_projector("mmproj-fm-src.gguf"), "fm/mmproj-fm.gguf")
    server.model_draft = draft
    server.models_preset = _write_preset(f"[*]\nmmproj = {shared_proj.replace(os.sep, '/')}\n")
    _enable()
    server.start()
    assert {"one", "two", "fm"} <= set(_models()) and "mtp-shared" not in _models()

    dry = _ok(_call("POST", "/admin/models/delete", {"model": "one", "dry_run": True}))
    assert dry["files"] == [{"path": "one.gguf", "bytes": size, "role": "model"}], dry
    kept = {k["path"]: k for k in dry["kept"]}
    assert set(kept) == {"mtp-shared.gguf", "mmproj-shared.gguf"} and all(k["shared"] is True for k in kept.values()), dry
    _err(_call("POST", "/admin/models/delete", {"model": "one", "projector_only": True}), 409, "not_deletable",
         reason="no_projector")
    body = _ok(_call("POST", "/admin/models/delete", {"model": "one"}))
    assert body["deleted"] is True and body["freed_bytes"] == size
    for rel in ("mtp-shared.gguf", "mmproj-shared.gguf", "two.gguf"):
        assert os.path.isfile(os.path.join(models_dir, rel)), rel
    assert not os.path.exists(os.path.join(models_dir, "one.gguf"))

    dry = _ok(_call("POST", "/admin/models/delete", {"model": "fm", "dry_run": True}))
    assert {f["path"]: f["role"] for f in dry["files"]} == {"fm/fm.gguf": "model", "fm/mmproj-fm.gguf": "projector"}, dry
    assert [k["path"] for k in dry["kept"] if k.get("shared")] == ["mtp-shared.gguf"], dry
    _assert_alive()


def test_delete_folder_model_with_shards_and_projector():
    parts = _split("sharded", 3)
    for p in parts:
        _place(p, "sharded/" + os.path.basename(p))
    _place(_projector("mmproj-x.gguf"), "sharded/mmproj-sharded.gguf")
    _enable()
    server.start()
    assert _models()["sharded"]["architecture"]["input_modalities"] == ["text", "image"]

    dry = _ok(_call("POST", "/admin/models/delete", {"model": "sharded", "dry_run": True}))
    assert [(f["path"], f["role"]) for f in dry["files"]] == [
        ("sharded/sharded-00001-of-00003.gguf", "model"), ("sharded/sharded-00002-of-00003.gguf", "shard"),
        ("sharded/sharded-00003-of-00003.gguf", "shard"), ("sharded/mmproj-sharded.gguf", "projector"),
    ]
    assert dry["kept"] == [] and dry["folder_removed"] is True
    assert dry["total_bytes"] == sum(f["bytes"] for f in dry["files"])
    body = _ok(_call("POST", "/admin/models/delete", {"model": "sharded"}))
    assert body["folder_removed"] is True and body["freed_bytes"] == dry["total_bytes"]
    assert not os.path.exists(os.path.join(models_dir, "sharded"))
    assert "sharded" not in _models(reload=True)


def test_delete_refusals():
    tiny = _tiny()
    preset_model = gg.copy(tiny, _fx("preset-tiny.gguf"))
    _place(tiny, "plain.gguf")
    _place(tiny, "merged.gguf")
    # a models.ini section that merges over a local model makes it a preset model
    server.models_preset = _write_preset(f"[presetB]\nmodel = {preset_model.replace(os.sep, '/')}\n\n[merged]\nctx-size = 256\n")
    linked = False
    try:
        os.symlink(os.path.join(models_dir, "plain.gguf"), os.path.join(models_dir, "linked.gguf"))
        linked = True
    except (OSError, NotImplementedError, AttributeError):
        pass  # Windows without the symlink privilege
    _enable()
    server.start()

    _err(_call("POST", "/admin/models/delete", {"model": "presetB"}), 409, "not_deletable", reason="preset")
    _err(_call("POST", "/admin/models/delete", {"model": "merged"}), 409, "not_deletable", reason="preset")
    _err(_call("POST", "/admin/models/delete", {"model": MODEL_B}), 409, "not_deletable", reason="cache")
    _err(_call("POST", "/admin/models/delete", {"model": "nope"}), 409, "not_deletable", reason="not_found")
    _err(_call("POST", "/admin/models/delete", {"model": "plain", "projector_only": True}), 409, "not_deletable",
         reason="no_projector")
    _err(_call("POST", "/admin/models/delete", {}), 400, "invalid_request_error", param="model")
    _err(_call("POST", "/admin/models/delete", {"model": "plain", "stop": "yes"}), 400, "invalid_request_error", param="stop")
    if linked:
        _err(_call("POST", "/admin/models/delete", {"model": "linked"}), 409, "not_deletable", reason="symlink")
        assert os.path.isfile(os.path.join(models_dir, "plain.gguf"))
    for name in ("plain.gguf", "merged.gguf"):
        assert os.path.isfile(os.path.join(models_dir, name))
    assert os.path.isfile(preset_model)


def test_delete_running_model_needs_stop():
    _place(_tiny(), "busy.gguf")
    _enable()
    server.start()
    res = server.make_request("POST", "/models/load", data={"model": "busy"}, timeout=120)
    assert res.status_code == 200, res.body
    stage1._wait_until(lambda: _models()["busy"]["status"]["value"] == "loaded", 120, "busy to load")

    dry = _ok(_call("POST", "/admin/models/delete", {"model": "busy", "dry_run": True}))
    assert dry["running"] is True and dry["status"] == "loaded" and "running" in dry["warnings"]
    _err(_call("POST", "/admin/models/delete", {"model": "busy"}), 409, "model_running")
    assert os.path.isfile(os.path.join(models_dir, "busy.gguf"))
    body = _ok(_call("POST", "/admin/models/delete", {"model": "busy", "stop": True}, timeout=120))
    assert body["deleted"] is True
    assert "busy" not in _models()
    stage1._wait_until(lambda: not os.path.exists(os.path.join(models_dir, "busy.gguf")), 15, "the file to go")


def test_delete_can_keep_the_settings():
    tiny = _tiny()
    _place(tiny, "keepme.gguf")
    _enable()
    server.start()
    _ok(_call("POST", "/admin/models/settings", {"model": "keepme", "settings": {"ctx-size": 256}}))

    body = _ok(_call("POST", "/admin/models/delete", {"model": "keepme", "remove_settings": False}))
    assert body["deleted"] is True and body["settings_removed"] is False
    assert _models_file()["keepme"]["settings"] == {"ctx-size": "256"}
    unattached = {u["id"]: u for u in _ok(_call("GET", "/admin/models/settings")).get("unattached", [])}
    assert "keepme" in unattached
    # an upload under the same name is offered those settings
    assert _create("keepme", [tiny], expect=200, dry_run=True)["existing_settings"] == {"ctx-size": "256"}


def test_remove_projector():
    tiny = _tiny()
    _place(tiny, "mm/mm.gguf")
    _place(_projector("mmproj-x.gguf"), "mm/mmproj-mm.gguf")
    _enable()
    server.start()
    _ok(_call("POST", "/admin/models/settings", {"model": "mm", "settings": {"ctx-size": 256}}))
    before = _models()["mm"]
    assert before["architecture"]["input_modalities"] == ["text", "image"] and "--mmproj" in before["status"]["args"]

    dry = _ok(_call("POST", "/admin/models/delete", {"model": "mm", "projector_only": True, "dry_run": True}))
    assert dry["projector_only"] is True and dry["folder"] == "mm"
    assert [(f["path"], f["role"]) for f in dry["files"]] == [("mm/mmproj-mm.gguf", "projector")]
    body = _ok(_call("POST", "/admin/models/delete", {"model": "mm", "projector_only": True}))
    assert body["projector_removed"] is True and "deleted" not in body
    assert body["settings_removed"] is False and body["folder_removed"] is False
    assert not os.path.exists(os.path.join(models_dir, "mm", "mmproj-mm.gguf"))
    assert os.path.isfile(os.path.join(models_dir, "mm", "mm.gguf"))
    after = _models()["mm"]
    assert after["architecture"]["input_modalities"] == ["text"] and "--mmproj" not in after["status"]["args"]
    assert _row("mm")["overrides"] == {"ctx-size": "256"}  # the settings stay
    _err(_call("POST", "/admin/models/delete", {"model": "mm", "projector_only": True}), 409, "not_deletable",
         reason="no_projector")


def test_delete_a_published_upload():
    tiny = _tiny()
    _enable()
    server.start()
    sid, s = _upload("up-then-del", [tiny], settings={"ctx-size": 256})
    assert s["state"] == "published", _brief(s)
    body = _ok(_call("POST", "/admin/models/delete", {"model": "up-then-del"}))
    assert body["deleted"] is True and body["settings_removed"] is True
    assert not os.path.exists(os.path.join(models_dir, "up-then-del.gguf"))
    assert "up-then-del" not in _models() and "up-then-del" not in _models_file()
    # the name is free again
    _create("up-then-del", [tiny], expect=200, dry_run=True)


#
# crash fixes that apply with the admin layer disabled too
#


@pytest.mark.parametrize("key", list(PROBE_KEYS))
def test_projector_with_a_wrongly_typed_key_does_not_crash_the_router(key: str):
    """the router asks every projector of the models dir for its capabilities on every reload; a value of another type
    aborted the whole router (at startup: a crash loop). Now that projector is not used and the model stays."""
    tiny = _tiny()
    _place(tiny, "badmm/badmm.gguf")
    _place(tiny, "goodmm/goodmm.gguf")
    _place(_projector("mmproj-good.gguf"), "goodmm/mmproj-goodmm.gguf")
    bad = _projector("mmproj-bad.gguf", typed={key: WRONG_TYPES[PROBE_KEYS[key]]})
    server.start()  # the admin layer is off

    assert _models()["badmm"]["architecture"]["input_modalities"] == ["text"]
    # a reload that finds the bad projector
    _place(bad, "badmm/mmproj-badmm.gguf")
    models = _models(reload=True)
    _assert_alive()
    assert models["badmm"]["architecture"]["input_modalities"] == ["text"]
    assert models["goodmm"]["architecture"]["input_modalities"] == ["text", "image"]
    # a start with it present
    _restart()
    models = _models()
    _assert_alive()
    assert models["badmm"]["architecture"]["input_modalities"] == ["text"]
    assert models["goodmm"]["architecture"]["input_modalities"] == ["text", "image"]
    log = _log()
    assert "mmproj-badmm.gguf is not used" in log and key in log
    assert "admin:" not in log


def test_projector_probe_keys_match_the_clip_loader():
    """the pre-check knows the keys the projector loader reads with a typed getter during the capability probe; when an
    upstream merge adds one, this test fails (update PROBE_KEYS here and the key list in server-admin-upload.cpp)"""
    mtmd = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "mtmd")
    clip_cpp = os.path.join(mtmd, "clip.cpp")
    clip_impl = os.path.join(mtmd, "clip-impl.h")
    if not (os.path.isfile(clip_cpp) and os.path.isfile(clip_impl)):
        pytest.skip("the sources of tools/mtmd are not next to the tests")
    source = stage1._read_text(clip_cpp)
    start = source.index("clip_model_loader(const char * fname")
    end = source.index("void load_hparams", start)
    names = dict(re.findall(r'#define\s+(KEY_\w+)\s+"([^"]+)"', stage1._read_text(clip_impl)))
    getter_type = {"string": gg.T_STRING, "bool": gg.T_BOOL}
    found = {}
    for getter, key_name in re.findall(r"\bget_(\w+)\(\s*(KEY_\w+)", source[start:end]):
        assert key_name in names, key_name
        found[names[key_name]] = getter_type.get(getter, getter)
    assert found == PROBE_KEYS


class _Hub:
    """a minimal stand-in for Hugging Face (refs, tree, file download); records every request with its headers"""

    COMMIT = "0123456789abcdef0123456789abcdef01234567"

    def __init__(self):
        self.files: dict = {}  # path in the repository -> size
        self.requests: list = []
        hub = self

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                hub.handle(self)

            def do_HEAD(self):
                hub.handle(self)

            def log_message(self, format, *args):
                pass

        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.httpd.daemon_threads = True
        self.port = self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def close(self) -> None:
        self.httpd.shutdown()
        self.httpd.server_close()

    def _json(self, req, status: int, body) -> None:
        data = json.dumps(body).encode()
        req.send_response(status)
        req.send_header("Content-Type", "application/json")
        req.send_header("Content-Length", str(len(data)))
        req.end_headers()
        if req.command != "HEAD":
            req.wfile.write(data)

    def handle(self, req) -> None:
        self.requests.append((req.command, req.path, {k.lower(): v for k, v in req.headers.items()}))
        if req.path == "/api/models/org/atrepo/refs":
            return self._json(req, 200, {"branches": [{"name": "main", "ref": "refs/heads/main", "targetCommit": self.COMMIT}],
                                         "converts": [], "tags": []})
        if req.path.startswith("/api/models/org/atrepo/tree/" + self.COMMIT):
            tree = [{"type": "file", "path": p, "oid": "%040x" % i, "size": size,
                     "lfs": {"oid": "%064x" % i, "size": size, "pointerSize": 134}} for i, (p, size) in enumerate(self.files.items())]
            return self._json(req, 200, tree)
        prefix = "/org/atrepo/resolve/" + self.COMMIT + "/"
        if req.path.startswith(prefix) and req.path[len(prefix):] in self.files:
            size = self.files[req.path[len(prefix):]]
            req.send_response(200)
            req.send_header("Content-Type", "application/octet-stream")
            req.send_header("Content-Length", str(size))
            req.send_header("ETag", '"%040x"' % size)
            req.end_headers()
            if req.command == "GET":
                req.wfile.write(b"\0" * size)
            return
        return self._json(req, 404, {"error": "Not found"})


def test_download_with_an_at_sign_in_a_file_name_keeps_the_token():
    """common_http_parse_url took the user info from the first '@' anywhere after '://': a repository file named
    'x@host:port/...' sent the download, with 'Authorization: Bearer <HF_TOKEN>', to that host"""
    evil = _Hub()
    hub = _Hub()
    try:
        hub.files = {f"x@127.0.0.1:{evil.port}/model-Q4_0.gguf": 4096}
        server.offline = False
        env.setenv("MODEL_ENDPOINT", f"http://127.0.0.1:{hub.port}/")
        env.setenv("HF_TOKEN", "hf_SECRET_TOKEN_must_not_leak")
        env.setenv("LLAMA_CACHE", str(work_dir / "cache"))  # the download must not land in the shared cache
        server.start()  # the admin layer is off

        res = server.make_request("POST", "/models", data={"model": "org/atrepo:Q4_0"}, timeout=60)
        assert res.status_code == 200, res.body
        stage1._wait_until(lambda: evil.requests or any("/resolve/" in p for _, p, _ in hub.requests), 30,
                           "the download to start")
        time.sleep(1)
        assert evil.requests == [], f"requests reached the other host: {evil.requests}"
        files = [p for _, p, _ in hub.requests if "/resolve/" in p]
        assert files and all(p.endswith(f"/x@127.0.0.1:{evil.port}/model-Q4_0.gguf") for p in files), files
        _assert_alive()
    finally:
        evil.close()
        hub.close()


#
# the key, the allow-list and the "Admin" item of the UI
#


def test_upload_routes_need_the_admin_key():
    _enable()
    server.start()
    for method, path, data in UPLOAD_ROUTES:
        for key in (None, "", "wrong-key", ADMIN_KEY.lower()):
            res = _call(method, path, data, key=key)
            _err(res, 401, "admin_key_required")
            assert res.headers.get("Cache-Control") == "no-store", (method, path)
        res = _call(method, path, data, key=None, headers={"Authorization": f"Bearer {ADMIN_KEY}"})
        _err(res, 401, "admin_key_required")
    # a chunk without the key is refused before its body is read
    status, body = _raw(_chunk_target(NO_SESSION), _chunk_headers(key=None), b"", 8 * MiB)
    assert status == 401 and body["error"]["type"] == "admin_key_required", body
    # a refused request changes nothing
    assert not os.path.exists(_staging()) and not os.path.exists(os.path.join(state_dir, "uploads"))


def test_upload_refused_address_gets_403_everywhere():
    """PROVISION_ADMIN_ALLOW_FROM without this address: the probe the UI makes (GET /admin/status) answers 403, so the
    sidebar never shows the Admin item, and every upload or delete route answers the same 403 - a chunk before its body"""
    _place(_tiny(), "keep.gguf")
    _enable(PROVISION_ADMIN_ALLOW_FROM="10.0.0.0/8")
    server.start()

    for key in (None, "wrong-key", ADMIN_KEY):
        _assert_forbidden(_call("GET", "/admin/status", key=key))
        for method, path, data in UPLOAD_ROUTES:
            _assert_forbidden(_call(method, path, data, key=key))
    status, body = _raw(_chunk_target(NO_SESSION), _chunk_headers(), b"", 8 * MiB)  # never sent: answered first
    assert status == 403 and body["error"]["type"] == "admin_forbidden" and body["error"]["message"].endswith(" 127.0.0.1")
    # the other routes as always; nothing was deleted or created
    for path in ("/health", "/models", "/v1/models", "/props"):
        assert server.make_request("GET", path).status_code == 200
    assert "keep" in _models() and os.path.isfile(os.path.join(models_dir, "keep.gguf"))
    assert not os.path.exists(_staging())
    assert ADMIN_KEY not in _log()


@pytest.mark.parametrize("allow_from", [None, "127.0.0.1", "::1 127.0.0.0/8"])
def test_upload_allowed_address_sees_the_admin_item(allow_from: str | None):
    """no allow-list, or one with this address: the probe answers 200 without a key (the Admin item is shown) and the
    upload routes work with the key; a missing key is 401, never 403"""
    _enable(PROVISION_ADMIN_ALLOW_FROM=allow_from)
    server.start()
    res = _call("GET", "/admin/status", key=None)
    assert res.status_code == 200 and res.body == {"admin": True, "authorized": False, "key_configured": True}
    status = _ok(_call("GET", "/admin/status"))
    assert status["authorized"] is True and status["ip_allowlist"] is (allow_from is not None)
    assert _ok(_call("GET", "/admin/models/upload"))["available"] is True
    _err(_call("GET", "/admin/models/upload", key=None), 401, "admin_key_required")
    status, body = _raw(_chunk_target(NO_SESSION), _chunk_headers(key=None), b"", 1000)
    assert status == 401, body
    sid, s = _upload("allowed", [_tiny()])
    assert s["state"] == "published", _brief(s)


def test_upload_chunk_refusal_through_a_trusted_proxy():
    _enable(PROVISION_ADMIN_ALLOW_FROM="10.64.0.0/24", PROVISION_ADMIN_TRUSTED_PROXIES="127.0.0.1")
    server.start()
    tiny = _tiny()
    data = open(tiny, "rb").read()
    allowed = {"X-Forwarded-For": "10.64.0.7"}
    res = _call("POST", "/admin/models/upload", {"name": "proxied", "files": _files([tiny])}, headers=allowed)
    sid = _ok(res, 201)["upload"]["id"]

    _assert_forbidden(_chunk(sid, 0, 0, data[:1000], headers={"X-Forwarded-For": "203.0.113.9"}), "203.0.113.9")
    _assert_forbidden(_chunk(sid, 0, 0, data[:1000]), "127.0.0.1")  # the proxy itself, without X-Forwarded-For
    status, body = _raw(_chunk_target(sid), {**_chunk_headers(), "X-Forwarded-For": "203.0.113.9"}, b"", 8 * MiB)
    assert status == 403 and body["error"]["message"].endswith(" 203.0.113.9"), body
    assert _ok(_chunk(sid, 0, 0, data[:1000], headers=allowed))["offset"] == 1000


def test_upload_chunk_refusals_do_not_buffer_the_body():
    """refusals of a chunk are answered before httplib reads the body: an unauthenticated 90 MiB body costs nothing"""
    psutil = pytest.importorskip("psutil")
    _enable()
    server.start()
    sid = _create("mem", [_tiny()])["upload"]["id"]
    proc = psutil.Process(server.process.pid)

    def memory() -> int:
        mi = proc.memory_info()
        return getattr(mi, "private", mi.rss)

    cases = [
        ("no key", _chunk_target(sid), _chunk_headers(key=None)),
        ("larger than chunk_max", _chunk_target(sid), _chunk_headers()),
        ("unknown session", _chunk_target(NO_SESSION), _chunk_headers()),
    ]
    piece = b"\0" * MiB
    for name, target, headers in cases:
        base = memory()
        peak = [base]
        done = threading.Event()

        def sample():
            while not done.is_set():
                peak[0] = max(peak[0], memory())
                time.sleep(0.01)

        sampler = threading.Thread(target=sample, daemon=True)
        sampler.start()
        s = _open_request(target, headers, b"", 90 * MiB)
        try:
            for _ in range(90):
                s.sendall(piece)
        except OSError:
            pass  # the server answered and closed while the body was still being sent
        finally:
            s.close()
        time.sleep(0.5)
        done.set()
        sampler.join()
        assert peak[0] - base < 32 * MiB, f"{name}: the router grew by {(peak[0] - base) // MiB} MiB"
    _assert_alive()


#
# isolation, admin layer disabled
#


def test_upload_routes_not_reachable_through_predict_route():
    """the Vertex AI compat route dispatches to handlers by name: the upload and delete handlers are not among them"""
    _place(_tiny(), "keep.gguf")
    _enable()
    server.gcp_compat = True
    server.start()
    formats = [
        "/admin/models/upload", "adminModelsUpload", "/admin/models/upload/chunk", "/admin/models/upload/update",
        "/admin/models/upload/publish", "/admin/models/upload/cancel", "/admin/models/delete", "adminModelsDelete",
    ]
    instances = [{"@requestFormat": fmt, "name": "p", "files": _files([_tiny()]), "model": "keep", "id": NO_SESSION}
                 for fmt in formats]
    instances.append({"@requestFormat": "/props"})
    res = _call("POST", "/predict", {"instances": instances})
    predictions = _ok(res)["predictions"]
    assert len(predictions) == len(instances)
    for fmt, prediction in zip(formats, predictions):
        assert set(prediction) == {"error"}, f"{fmt} was dispatched: {prediction}"
        assert "no handler registered" in prediction["error"]["message"]
    assert predictions[-1]["role"] == "router"
    assert os.path.isfile(os.path.join(models_dir, "keep.gguf")) and not os.path.exists(_staging())


def test_upload_admin_disabled_has_no_upload_routes():
    _place(_tiny(), "keep.gguf")
    server.start()  # PROVISION_ADMIN unset
    for method, path, data in UPLOAD_ROUTES:
        for key in (None, ADMIN_KEY):
            stage1._assert_not_found(_call(method, path, data, key=key))
    assert "keep" in _models() and os.path.isfile(os.path.join(models_dir, "keep.gguf"))
    assert os.listdir(models_dir) == ["keep.gguf"] and not os.path.exists(state_dir)
    assert "admin:" not in _log()


#
# a file larger than 1 GiB (lane C / SLOW_TESTS=1)
#


@pytest.mark.slow
@pytest.mark.skipif(not is_slow_test_allowed(), reason="skipping slow test: writes and uploads a file of 1.1 GiB")
def test_upload_larger_than_1_gib():
    big = str(work_dir / "big-source.gguf")
    published = os.path.join(models_dir, "big-one.gguf")
    try:
        sha = gg.big_copy(_tiny(), big, 1100 * MiB)
        size = os.path.getsize(big)
        assert size > 1024 * MiB
        _enable()
        server.start()
        psutil = None
        try:
            import psutil  # optional: the memory bound is checked where it is there
        except ImportError:
            pass
        peak = [0]
        done = threading.Event()
        base = 0
        if psutil is not None:
            proc = psutil.Process(server.process.pid)

            def memory() -> int:
                mi = proc.memory_info()
                return getattr(mi, "private", mi.rss)

            base = memory()

            def sample():
                while not done.is_set():
                    peak[0] = max(peak[0], memory())
                    time.sleep(0.05)

            threading.Thread(target=sample, daemon=True).start()
        sid, s = _upload("big-one", [big], chunk=LIMITS["chunk_max"], wait=FINISHED)
        done.set()
        assert s["state"] == "published" and s["files"][0]["sha256"] == sha, _brief(s)
        assert os.path.getsize(published) == size and gg.sha256_file(published) == sha
        if psutil is not None:
            assert peak[0] - base < 200 * MiB, f"the router grew by {(peak[0] - base) // MiB} MiB"
        body = _ok(_call("POST", "/admin/models/delete", {"model": "big-one"}))
        assert body["freed_bytes"] == size
        stage1._wait_until(lambda: not [x for x in os.listdir(_staging()) if x.startswith("trash-")], 60,
                           "the trash to be emptied")
    finally:
        # whatever happened: no 1.1 GiB file stays behind (the source, the published file, a staged .part or a
        # trash folder of a run that failed half-way); the router first, so it holds no file open; never raises,
        # so a cleanup problem cannot hide the failure of the test
        try:
            server.stop()
        except Exception:
            pass
        for path in (big, published):
            for _ in range(100):
                try:
                    if os.path.exists(path):
                        os.remove(path)
                    break
                except OSError:
                    time.sleep(0.1)
        shutil.rmtree(models_dir, ignore_errors=True)
