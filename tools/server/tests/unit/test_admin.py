# tests for the Provision admin layer of the router: the /admin/* routes and the hooks behind them
# (tools/server/server-admin.cpp, docs/provision-admin.md). Everything is driven through the environment
# of the router process; the existing routes are only used to observe the effect of the settings.
#
# Only the two tests marked slow need the network (SLOW_TESTS=1): everywhere else the download pre-flight check
# talks to a stand-in for the Hugging Face endpoint that runs inside the test (_FakeHub, set through MODEL_ENDPOINT).
# Reading the environment of a model instance needs /proc or psutil; where neither is there, that test is skipped.

import glob
import hashlib
import http.client
import shutil
import signal
import stat
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import pytest
from utils import *

server: ServerProcess
env: pytest.MonkeyPatch
state_dir: str

# mixed case on purpose: the key is compared as is
ADMIN_KEY = "Test-Admin-Key-0123456789abcdef"

# models that conftest.py puts into the cache (same ids as test_router.py)
MODEL_A = "ggml-org/tinygemma3-GGUF:Q8_0"
MODEL_B = "ggml-org/test-model-stories260K:F32"
MODEL_C = "ggml-org/test-model-stories260K-infill:F32"

# the model cache every test server shares (same default as ServerProcess.start)
SHARED_CACHE_DIR = os.environ.get("LLAMA_CACHE", "tmp")

PROTECTED_ROUTES = [
    ("GET", "/admin/settings", None),
    ("POST", "/admin/settings", {"models_max": 5}),
    ("GET", "/admin/storage", None),
    ("POST", "/admin/models/download/check", {"model": "user/repo"}),
]
ALL_ROUTES = [("GET", "/admin/status", None)] + PROTECTED_ROUTES


@pytest.fixture(autouse=True)
def create_server(monkeypatch, tmp_path):
    global server, env, state_dir
    server = ServerPreset.router()
    env = monkeypatch
    state_dir = str(tmp_path / "admin-state")
    # the server inherits os.environ (see ServerProcess.start), so start every test from a clean slate
    for name in list(os.environ):
        if name.upper().startswith("PROVISION_ADMIN") or name.upper() == "MODEL_DEVICES":
            monkeypatch.delenv(name)


def _enable_admin(**variables: str | None) -> None:
    """admin layer on, key from the environment, state in a directory of this test. None removes a variable."""
    values = {
        "PROVISION_ADMIN": "1",
        "PROVISION_ADMIN_KEY": ADMIN_KEY,
        "PROVISION_ADMIN_STATE_DIR": state_dir,
        **variables,
    }
    for name, value in values.items():
        if value is None:
            env.delenv(name, raising=False)
        else:
            env.setenv(name, value)


def _admin(method: str, path: str, data: dict | None = None, key: str | None = ADMIN_KEY,
           headers: dict | None = None, timeout: float = 60) -> ServerResponse:
    headers = dict(headers or {})
    if key is not None:
        headers["X-Admin-Key"] = key
    return server.make_request(method, path, data=data, headers=headers, timeout=timeout)


def _assert_error(res: ServerResponse, status: int, error_type: str) -> None:
    """the error format of the server: {"error": {"code", "message", "type"}}"""
    assert res.status_code == status, f"expected {status} {error_type}, got {res.status_code}: {res.body}"
    error = res.body["error"]
    assert error["type"] == error_type
    assert error["code"] == status
    assert isinstance(error["message"], str) and error["message"] != ""


def _assert_not_found(res: ServerResponse) -> None:
    assert res.status_code == 404, f"expected 404, got {res.status_code}: {res.body}"
    assert res.body["error"]["type"] == "not_found_error"


def _admin_status() -> dict:
    res = _admin("GET", "/admin/status")
    assert res.status_code == 200
    assert res.body["authorized"] is True
    return res.body


def _settings() -> dict:
    res = _admin("GET", "/admin/settings")
    assert res.status_code == 200, f"GET /admin/settings failed: {res.body}"
    return res.body


def _post_settings(data: dict) -> dict:
    res = _admin("POST", "/admin/settings", data=data)
    assert res.status_code == 200, f"POST /admin/settings {data} failed: {res.body}"
    return res.body


def _models(reload: bool = False) -> dict[str, dict]:
    res = server.make_request("GET", "/models" + ("?reload=1" if reload else ""), timeout=60)
    assert res.status_code == 200, f"GET /models failed: {res.body}"
    return {item["id"]: item for item in res.body["data"]}


def _model_status(model_id: str) -> str:
    return _models()[model_id]["status"]["value"]


def _wait_for_model_status(model_id: str, desired: str, timeout: int = 60) -> None:
    deadline = time.time() + timeout
    last_status = None
    while time.time() < deadline:
        last_status = _model_status(model_id)
        if last_status == desired:
            return
        time.sleep(0.1)
    raise AssertionError(f"Timed out waiting for {model_id} to reach {desired}, last status: {last_status}")


def _load_model_and_wait(model_id: str, timeout: int = 120) -> None:
    res = server.make_request("POST", "/models/load", data={"model": model_id}, timeout=timeout)
    assert res.status_code == 200, f"loading {model_id} failed: {res.body}"
    assert res.body.get("success") is True
    _wait_for_model_status(model_id, "loaded", timeout=timeout)


def _tokenize(model_id: str, timeout: float = 180) -> ServerResponse:
    # a proxied request: loads the model first when it is not running (autoload)
    return server.make_request("POST", "/tokenize", data={"model": model_id, "content": "hello world"}, timeout=timeout)


def _device_arg(model_id: str) -> str | None:
    """value of --device in the arguments the router starts this model with, None when there is none"""
    args = _models()[model_id]["status"]["args"]
    return args[args.index("--device") + 1] if "--device" in args else None


def _props() -> dict:
    res = server.make_request("GET", "/props")
    assert res.status_code == 200
    return res.body


def _same_path(a: str, b: str) -> bool:
    return os.path.normcase(os.path.abspath(a)) == os.path.normcase(os.path.abspath(b))


def _read_text(path: str) -> str:
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def _write_text(path, text: str) -> str:
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    return str(path)


def _state_file(name: str = "admin-state.json") -> str:
    return os.path.join(state_dir, name)


def _tiny_gguf() -> str:
    """the stories260K model file in the cache (conftest.py downloads it before the first test)"""
    found = glob.glob(os.path.join(SHARED_CACHE_DIR, "models--ggml-org--test-model-stories260K", "snapshots", "*", "*.gguf"))
    assert found, f"stories260K model file not found in the cache directory {SHARED_CACHE_DIR}"
    return found[0]


def _make_models_dir(tmp_path, *names: str) -> str:
    """a --models-dir directory with one copy of the tiny model per name"""
    models_dir = tmp_path / "models"
    models_dir.mkdir()
    for name in names:
        shutil.copyfile(_tiny_gguf(), models_dir / f"{name}.gguf")
    return str(models_dir)


def _preset_section(name: str, model_path: str, extra: str = "") -> str:
    """a models.ini section for a model given by its local path"""
    return f"[{name}]\nmodel = {model_path}\n{extra}\n"


def _make_preset_model(tmp_path, name: str = "preset-tiny") -> str:
    """a copy of the tiny model outside of the cache and of --models-dir, to be used by a models.ini section"""
    path = tmp_path / f"{name}.gguf"
    shutil.copyfile(_tiny_gguf(), path)
    return path.as_posix()


def _restart_server() -> None:
    server.stop()
    # the port must be free again before the next instance binds it
    deadline = time.time() + 10
    while time.time() < deadline:
        try:
            requests.get(server.make_url("/health"), timeout=1)
        except requests.exceptions.RequestException:
            break
        time.sleep(0.1)
    server.start()


def _raw_get(target: str, key: str | None = None) -> tuple[int, str]:
    """GET with the request target exactly as written (requests would tidy up the path first)"""
    conn = http.client.HTTPConnection(server.server_host, server.server_port, timeout=30)
    try:
        conn.request("GET", target, headers={"X-Admin-Key": key} if key is not None else {})
        res = conn.getresponse()
        return res.status, res.read().decode("utf-8", errors="replace")
    finally:
        conn.close()


def _start_request(fn):
    """runs one request in a thread; .result(timeout) gives its response or raises what it raised"""
    executor = ThreadPoolExecutor(max_workers=1)
    future = executor.submit(fn)
    executor.shutdown(wait=False)
    return future


def _wait_until(predicate, timeout: float, what: str) -> None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return
        time.sleep(0.05)
    raise AssertionError(f"Timed out waiting for {what}")


def _listen_sse(collected: list, stop: threading.Event, ready: threading.Event) -> None:
    """collect /models/sse events until `stop` is set (same approach as test_router.py)"""
    try:
        with requests.get(server.make_url("/models/sse"), stream=True, timeout=120) as resp:
            ready.set()
            for line_bytes in resp.iter_lines():
                if stop.is_set():
                    break
                line = line_bytes.decode("utf-8")
                if line.startswith("data: "):
                    collected.append(json.loads(line[6:]))
    except Exception:
        pass


def _start_sse_listener() -> tuple[list, threading.Event]:
    events: list = []
    stop = threading.Event()
    ready = threading.Event()
    threading.Thread(target=_listen_sse, args=(events, stop, ready), daemon=True).start()
    # subscribe before triggering anything, a one-shot event is lost otherwise
    assert ready.wait(10), "SSE client failed to connect"
    return events, stop


def _has_sse_event(events: list, event_type: str, model: str) -> bool:
    return any(e.get("event") == event_type and e.get("model") == model for e in events)


def _child_pids(pid: int) -> list[int]:
    """direct children of a process that run the same binary. Empty when they cannot be listed on this platform."""
    try:
        import psutil  # optional
        parent = psutil.Process(pid)
        return [p.pid for p in parent.children() if p.name() == parent.name()]
    except ImportError:
        pass
    try:
        if os.path.isdir("/proc"):
            children = []
            for entry in os.listdir("/proc"):
                if not entry.isdigit():
                    continue
                try:
                    with open(f"/proc/{entry}/stat") as f:
                        # "pid (comm) state ppid ...", comm may contain spaces and parentheses
                        fields = f.read().rsplit(")", 1)[1].split()
                except OSError:
                    continue
                if int(fields[1]) == pid:
                    children.append(int(entry))
            return children
        if os.name == "nt":
            name = os.path.basename(server.server_path or os.environ.get("LLAMA_SERVER_BIN_PATH", ""))
            query = f"ParentProcessId={pid}" + (f" AND Name='{name}'" if name else "")
            out = subprocess.run(
                ["powershell", "-NoProfile", "-Command", f"(Get-CimInstance Win32_Process -Filter \"{query}\").ProcessId"],
                capture_output=True, text=True, timeout=60,
            ).stdout
            return [int(line) for line in out.split() if line.isdigit()]
    except Exception as e:
        print(f"cannot list the child processes: {e}")
    return []


def _windows_acl(path: str) -> tuple[list[tuple[str, bool]], str]:
    """(SID, inherited) of every entry in the access control list of a file or directory, and the SID of this account"""
    script = (
        "(Get-Acl -LiteralPath $env:ACL_PATH).Access | ForEach-Object { "
        "$_.IdentityReference.Translate([System.Security.Principal.SecurityIdentifier]).Value + ' ' + $_.IsInherited }; "
        "'me ' + [System.Security.Principal.WindowsIdentity]::GetCurrent().User.Value"
    )
    out = subprocess.run(["powershell", "-NoProfile", "-Command", script], capture_output=True, text=True,
                         timeout=60, env={**os.environ, "ACL_PATH": path}).stdout
    entries = []
    me = ""
    for line in out.splitlines():
        parts = line.split()
        if len(parts) != 2:
            continue
        if parts[0] == "me":
            me = parts[1]
        else:
            entries.append((parts[0], parts[1] == "True"))
    assert me and entries, f"cannot read the access control list of {path}: {out}"
    return entries, me


def _process_environ(pid: int) -> dict[str, str] | None:
    """environment a process was started with, None when it cannot be read on this platform"""
    try:
        import psutil  # optional
        return psutil.Process(pid).environ()
    except ImportError:
        pass
    try:
        with open(f"/proc/{pid}/environ", "rb") as f:
            entries = f.read().decode("utf-8", errors="replace").split("\0")
        return dict(entry.split("=", 1) for entry in entries if "=" in entry)
    except OSError:
        return None


class _FakeHub:
    """Stand-in for the Hugging Face endpoint (MODEL_ENDPOINT). It answers the three kinds of requests
    the server makes (see common/hf-cache.cpp): the refs of a repository, its file tree and the files."""

    COMMIT = "0123456789abcdef0123456789abcdef01234567"

    def __init__(self):
        self.repos: dict[str, list[tuple[str, int | None]]] = {}  # repo id -> [(path, size or None = not reported)]
        self.refs_status: dict[str, int] = {}  # repo id -> HTTP status forced on the refs request
        self.file_status: dict[str, int] = {}  # repo id -> HTTP status forced on the file requests (gated repository)
        self.redirects: dict[str, str] = {}    # request path -> Location
        self.responses: dict[str, tuple[int, Any]] = {}  # request path -> (status, JSON body) instead of the regular answer
        self.hang = False                      # take the request and never answer
        self.stall_files = False               # send the first bytes of a file and then nothing
        self.release = threading.Event()       # ends hanging / stalled requests
        self.requests: list[tuple[str, str, dict]] = []  # (method, path, headers) in arrival order
        self.closed = False

        hub = self

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                hub._handle(self)

            def do_HEAD(self):
                hub._handle(self)

            def log_message(self, format, *args):
                pass

        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.httpd.daemon_threads = True
        self.url = f"http://127.0.0.1:{self.httpd.server_address[1]}"
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def close(self) -> None:
        if not self.closed:
            self.closed = True
            self.release.set()
            self.httpd.shutdown()
            self.httpd.server_close()

    def file_requests(self, method: str) -> list[str]:
        """paths inside the repository of the files requested with this method"""
        return [path.split("/resolve/", 1)[1].split("/", 1)[1] for m, path, _ in self.requests if m == method and "/resolve/" in path]

    def _send_json(self, req: BaseHTTPRequestHandler, status: int, body) -> None:
        data = json.dumps(body).encode()
        req.send_response(status)
        req.send_header("Content-Type", "application/json")
        req.send_header("Content-Length", str(len(data)))
        req.end_headers()
        if req.command != "HEAD":
            req.wfile.write(data)

    def _tree(self, repo: str) -> list[dict]:
        tree = []
        for path, size in self.repos[repo]:
            item: dict = {"type": "file", "path": path, "oid": hashlib.sha1(path.encode()).hexdigest()}
            if size is not None:
                item["size"] = size
                if path.endswith(".gguf"):
                    item["lfs"] = {"oid": hashlib.sha256(path.encode()).hexdigest(), "size": size, "pointerSize": 134}
            tree.append(item)
        return tree

    def _handle(self, req: BaseHTTPRequestHandler) -> None:
        self.requests.append((req.command, req.path, {name.lower(): value for name, value in req.headers.items()}))
        if self.hang:
            self.release.wait(120)
            return
        if req.path in self.redirects:
            req.send_response(307)
            req.send_header("Location", self.redirects[req.path])
            req.send_header("Content-Length", "0")
            req.end_headers()
            return
        if req.path in self.responses:
            status, body = self.responses[req.path]
            return self._send_json(req, status, body)

        m = re.fullmatch(r"/api/models/([^/]+/[^/]+)/refs", req.path)
        if m:
            repo = m.group(1)
            status = self.refs_status.get(repo, 200 if repo in self.repos else 404)
            if status != 200:
                return self._send_json(req, status, {"error": "Repository not found"})
            return self._send_json(req, 200, {
                "branches": [{"name": "main", "ref": "refs/heads/main", "targetCommit": self.COMMIT}],
                "converts": [],
                "tags": [],
            })

        m = re.fullmatch(r"/api/models/([^/]+/[^/]+)/tree/" + self.COMMIT + r"\?recursive=true", req.path)
        if m and m.group(1) in self.repos:
            return self._send_json(req, 200, self._tree(m.group(1)))

        m = re.fullmatch(r"/([^/]+/[^/]+)/resolve/" + self.COMMIT + r"/(.+)", req.path)
        if m and m.group(1) in self.repos:
            repo, path = m.group(1), m.group(2)
            sizes = dict(self.repos[repo])
            if repo in self.file_status or path not in sizes:
                return self._send_json(req, self.file_status.get(repo, 404), {"error": "Access to this file is restricted"})
            size = sizes[path] or 0
            req.send_response(200)
            req.send_header("Content-Type", "application/octet-stream")
            req.send_header("Content-Length", str(size))
            req.send_header("ETag", f'"{hashlib.sha1(path.encode()).hexdigest()}"')
            req.end_headers()
            if req.command == "GET":
                if self.stall_files:
                    req.wfile.write(b"\0" * min(size, 1024))
                    req.wfile.flush()
                    self.release.wait(120)
                else:
                    req.wfile.write(b"\0" * size)
            return

        return self._send_json(req, 404, {"error": "Not found"})


@pytest.fixture
def private_cache(monkeypatch, tmp_path_factory) -> str:
    """an empty model cache for this test alone"""
    # not below tmp_path: the cache nests deeply and the downloader cannot go beyond MAX_PATH on Windows
    cache_dir = str(tmp_path_factory.mktemp("cache"))
    monkeypatch.setenv("LLAMA_CACHE", cache_dir)
    return cache_dir


@pytest.fixture
def hub(monkeypatch, private_cache):
    """the router of this test talks to a fake Hugging Face endpoint and has an empty model cache of its own"""
    fake = _FakeHub()
    monkeypatch.setenv("MODEL_ENDPOINT", fake.url)
    monkeypatch.delenv("HF_TOKEN", raising=False)
    server.offline = False
    yield fake
    server.stop()  # before its endpoint goes away
    fake.close()


#
# admin layer disabled: no routes, hooks inert
#


@pytest.mark.parametrize("value", [None, "", "0", "false", "off", "yes"])
def test_admin_disabled_has_no_routes(value: str | None):
    global server
    # everything else is configured, only the switch decides
    _enable_admin(PROVISION_ADMIN=value, PROVISION_ADMIN_DEFAULT_DEVICE="CUDA0")
    server.start()

    for method, path, data in ALL_ROUTES:
        _assert_not_found(_admin(method, path, data, key=None))
        _assert_not_found(_admin(method, path, data))

    # nothing is created on disk and the model presets are not touched
    assert not os.path.exists(state_dir)
    assert _device_arg(MODEL_B) is None


def test_admin_disabled_ignores_saved_settings():
    """without PROVISION_ADMIN the router behaves as before, whatever is in the environment or in the state directory"""
    global server
    state = '{"version": 1, "settings": {"models_max": 5, "default_device": "CUDA1", "pinned": ["%s"]}}\n' % MODEL_B
    os.makedirs(state_dir)
    _write_text(_state_file(), state)
    _enable_admin(PROVISION_ADMIN=None, PROVISION_ADMIN_DEFAULT_DEVICE="CUDA0", PROVISION_ADMIN_PINNED=MODEL_B)
    server.models_max = 1
    server.start()

    assert _props()["max_instances"] == 1
    assert _device_arg(MODEL_B) is None

    # "pinned" MODEL_B is evicted like any other model
    _load_model_and_wait(MODEL_B)
    _load_model_and_wait(MODEL_C)
    assert _model_status(MODEL_B) == "unloaded"

    assert _read_text(_state_file()) == state
    assert os.listdir(state_dir) == ["admin-state.json"]


def test_admin_only_in_router_mode():
    """a server that is not a router has no admin layer, even when it is asked for"""
    global server
    server = ServerPreset.tinyllama2()
    _enable_admin()
    server.start()

    for method, path, data in ALL_ROUTES:
        _assert_not_found(_admin(method, path, data))
    assert not os.path.exists(state_dir)


@pytest.mark.parametrize("value", ["1", "true", "on", "TRUE", "On"])
def test_admin_enabled_values(value: str):
    global server
    _enable_admin(PROVISION_ADMIN=value)
    server.start()

    res = _admin("GET", "/admin/status", key=None)
    assert res.status_code == 200
    assert res.body["admin"] is True


#
# status probe and the admin key
#


def test_admin_status_without_key():
    global server
    _enable_admin()
    server.start()

    # the probe needs no key and tells nothing but the three flags, a wrong key included
    for key in (None, "", "wrong-key", ADMIN_KEY + "x"):
        res = _admin("GET", "/admin/status", key=key)
        assert res.status_code == 200
        assert res.body == {"admin": True, "authorized": False, "key_configured": True}
        assert res.headers["Content-Type"].startswith("application/json")


def test_admin_status_with_key(tmp_path):
    global server
    preset_path = _write_text(tmp_path / "models.ini", _preset_section("preset-tiny", _make_preset_model(tmp_path)))
    _enable_admin()
    server.models_max = 3
    server.models_dir = _make_models_dir(tmp_path, "local-tiny")
    server.models_preset = preset_path
    server.start()

    res = _admin("GET", "/admin/status")
    assert res.status_code == 200
    assert res.headers["Content-Type"].startswith("application/json")
    body = res.body
    assert body["admin"] is True
    assert body["authorized"] is True
    assert body["key_configured"] is True
    assert body["version"] == 1
    assert body["key_source"] == "env"
    assert body["models"] == {"max": 3, "running": 0, "total": len(_models())}
    assert _same_path(body["state"]["dir"], state_dir)
    assert body["state"]["writable"] is True
    assert body["state"]["error"] == ""
    # the test router runs with --offline, so it cannot download
    assert body["download"] == {"available": False, "offline": True}
    assert _same_path(body["dirs"]["cache"], SHARED_CACHE_DIR)
    assert _same_path(body["dirs"]["models_dir"], server.models_dir)
    assert _same_path(body["dirs"]["preset"], preset_path)
    assert isinstance(body["license_build"], bool)


def test_admin_status_counts_running_models():
    global server
    _enable_admin()
    server.models_max = 2
    server.start()

    assert _admin_status()["models"]["running"] == 0
    _load_model_and_wait(MODEL_B)
    status = _admin_status()
    assert status["models"]["running"] == 1
    assert status["models"]["max"] == 2

    settings_models = {m["id"]: m["status"] for m in _settings()["models"]}
    assert settings_models[MODEL_B] == "loaded"
    assert settings_models[MODEL_C] == "unloaded"

    # the model instance is a plain server: no admin routes on its own port
    args = _models()[MODEL_B]["status"]["args"]
    child_port = int(args[args.index("--port") + 1])
    assert child_port not in (0, server.server_port)
    res = requests.get(f"http://127.0.0.1:{child_port}/admin/status", headers={"X-Admin-Key": ADMIN_KEY}, timeout=10)
    assert res.status_code == 404


@pytest.mark.parametrize("method,path,data", PROTECTED_ROUTES)
def test_admin_key_required(method: str, path: str, data: dict | None):
    global server
    _enable_admin()
    server.start()

    wrong_keys = [
        None, "", "wrong-key", ADMIN_KEY[:-1], ADMIN_KEY + "x", f"x{ADMIN_KEY}", ADMIN_KEY + ADMIN_KEY,
        ADMIN_KEY.lower(), ADMIN_KEY.upper(),
    ]
    for key in wrong_keys:
        _assert_error(_admin(method, path, data, key=key), 401, "admin_key_required")

    # the key in another header or as an API key is not an admin key
    _assert_error(_admin(method, path, data, key=None, headers={"Authorization": f"Bearer {ADMIN_KEY}"}), 401, "admin_key_required")
    _assert_error(_admin(method, path, data, key=None, headers={"X-Api-Key": ADMIN_KEY}), 401, "admin_key_required")

    # a rejected request changes nothing
    assert not os.path.exists(_state_file())
    assert _settings()["models_max"] != 5

    res = _admin(method, path, data)
    assert res.status_code == 200
    assert "error" not in res.body


@pytest.mark.parametrize("header", ["X-Admin-Key", "x-admin-key", "X-ADMIN-KEY", "x-AdMiN-kEy"])
def test_admin_key_header_name_is_case_insensitive(header: str):
    global server
    _enable_admin()
    server.start()

    res = _admin("GET", "/admin/settings", key=None, headers={header: ADMIN_KEY})
    assert res.status_code == 200
    res = _admin("GET", "/admin/status", key=None, headers={header: ADMIN_KEY})
    assert res.body["authorized"] is True
    _assert_error(_admin("GET", "/admin/settings", key=None, headers={header: "wrong-key"}), 401, "admin_key_required")


def test_admin_key_from_env(tmp_path):
    global server
    server.log_path = str(tmp_path / "server.log")
    _enable_admin()
    server.start()

    assert _admin_status()["key_source"] == "env"
    _assert_error(_admin("GET", "/admin/settings", key="wrong-key-do-not-log"), 401, "admin_key_required")
    assert _admin("GET", "/admin/settings").status_code == 200
    # nothing is generated when a key is given
    assert not os.path.exists(_state_file("admin.key"))

    # the log tells where the key comes from and that a request was rejected, never a key
    server.stop()
    log = _read_text(server.log_path)
    assert "admin: enabled" in log
    assert "admin key source: env" in log
    assert "admin: rejected /admin/settings" in log
    assert ADMIN_KEY not in log
    assert "wrong-key-do-not-log" not in log


def test_admin_key_from_file(tmp_path):
    global server
    file_key = "key-from-a-file-0123456789"
    key_file = tmp_path / "admin-key.txt"
    # only the first line counts, surrounding whitespace and the line ending are not part of the key
    key_file.write_bytes(f"  {file_key}  \r\nsecond-line\n".encode())
    server.log_path = str(tmp_path / "server.log")
    _enable_admin(PROVISION_ADMIN_KEY=None, PROVISION_ADMIN_KEY_FILE=str(key_file))
    server.start()

    res = _admin("GET", "/admin/status", key=file_key)
    assert res.body["authorized"] is True
    assert res.body["key_source"] == "file"
    assert _admin("GET", "/admin/settings", key=file_key).status_code == 200
    for wrong_key in ("second-line", f"{file_key}second-line", ADMIN_KEY):
        _assert_error(_admin("GET", "/admin/settings", key=wrong_key), 401, "admin_key_required")
    assert not os.path.exists(_state_file("admin.key"))

    server.stop()
    log = _read_text(server.log_path)
    assert "admin key source: file" in log
    assert file_key not in log


def test_admin_key_env_wins_over_file(tmp_path):
    global server
    file_key = "key-from-a-file-0123456789"
    key_file = _write_text(tmp_path / "admin-key.txt", file_key + "\n")
    _enable_admin(PROVISION_ADMIN_KEY_FILE=key_file)
    server.start()

    assert _admin_status()["key_source"] == "env"
    _assert_error(_admin("GET", "/admin/settings", key=file_key), 401, "admin_key_required")


def test_admin_key_generated(tmp_path):
    global server
    server.log_path = str(tmp_path / "server.log")
    _enable_admin(PROVISION_ADMIN_KEY=None)
    server.start()

    # no key was given: one is generated into the state directory
    res = _admin("GET", "/admin/status", key=None)
    assert res.body == {"admin": True, "authorized": False, "key_configured": True}
    key_path = _state_file("admin.key")
    assert os.path.isfile(key_path)
    generated = _read_text(key_path).strip()
    assert re.fullmatch(r"[0-9a-f]{64}", generated), "expected 32 random bytes in hex"
    if os.name != "nt":
        # nobody but the owner can read the key
        assert stat.S_IMODE(os.stat(key_path).st_mode) & 0o077 == 0
        assert stat.S_IMODE(os.stat(state_dir).st_mode) & 0o077 == 0
    else:
        # the directory the server created does not take the access rules of its parent (below %ProgramData% every
        # local user could read the key): SYSTEM, the Administrators and the account of the server only
        for path, inherited in ((state_dir, False), (key_path, True)):
            entries, me = _windows_acl(path)
            assert {sid for sid, _ in entries} == {"S-1-5-18", "S-1-5-32-544", me}, f"{path}: {entries}"
            assert all(inh == inherited for _, inh in entries), f"{path}: {entries}"

    res = _admin("GET", "/admin/status", key=generated)
    assert res.body["authorized"] is True
    assert res.body["key_source"] == "generated"
    assert _admin("GET", "/admin/settings", key=generated).status_code == 200
    _assert_error(_admin("GET", "/admin/settings", key=ADMIN_KEY), 401, "admin_key_required")

    # the log names the file, never the key
    server.stop()
    log = _read_text(server.log_path)
    assert "admin.key" in log
    assert "admin key source: generated" in log
    assert generated not in log

    # the same key is used by the next start
    server.start()
    assert _read_text(key_path).strip() == generated
    res = _admin("GET", "/admin/status", key=generated)
    assert res.body["authorized"] is True
    assert res.body["key_source"] == "generated"
    assert _admin("GET", "/admin/settings", key=generated).status_code == 200
    server.stop()
    assert generated not in _read_text(server.log_path)


@pytest.mark.parametrize("value", ["", "   "])
def test_admin_key_empty_counts_as_unset(tmp_path, value: str):
    """a deployment may pass PROVISION_ADMIN_KEY="${VAR:-}": an empty value is no key, the next source is used"""
    global server
    file_key = "key-from-a-file-0123456789"
    key_file = _write_text(tmp_path / "admin-key.txt", file_key + "\n")
    _enable_admin(PROVISION_ADMIN_KEY=value, PROVISION_ADMIN_KEY_FILE=key_file)
    server.start()

    res = _admin("GET", "/admin/status", key=file_key)
    assert res.body["authorized"] is True
    assert res.body["key_source"] == "file"
    # and an empty key never opens anything
    _assert_error(_admin("GET", "/admin/settings", key=""), 401, "admin_key_required")


@pytest.mark.parametrize("content", [
    pytest.param(None, id="missing_file"),
    pytest.param("", id="empty_file"),
    pytest.param("\n\nkey-on-the-third-line\n", id="empty_first_line"),
])
def test_admin_key_file_unusable_falls_back_to_generated(tmp_path, content: str | None):
    global server
    key_file = tmp_path / "admin-key.txt"
    if content is not None:
        _write_text(key_file, content)
    _enable_admin(PROVISION_ADMIN_KEY=None, PROVISION_ADMIN_KEY_FILE=str(key_file))
    server.start()

    generated = _read_text(_state_file("admin.key")).strip()
    res = _admin("GET", "/admin/status", key=generated)
    assert res.body["authorized"] is True
    assert res.body["key_source"] == "generated"
    # only the first line of the file could have been the key
    _assert_error(_admin("GET", "/admin/settings", key="key-on-the-third-line"), 401, "admin_key_required")


@pytest.mark.parametrize("content,usable", [
    pytest.param("﻿key-from-a-file-0123456789\r\n".encode("utf-8"), True, id="utf8_bom"),
    # what Windows PowerShell 5.1 writes with '>': UTF-16 LE with a BOM
    pytest.param("key-from-a-file-0123456789\r\n".encode("utf-16"), False, id="utf16_bom"),
    pytest.param("key-from-a-file-0123456789\r\n".encode("utf-16-be"), False, id="utf16_be_no_bom"),
    pytest.param("ключ-0123456789\n".encode("utf-8"), False, id="not_ascii"),
])
def test_admin_key_file_encoding(tmp_path, content: bytes, usable: bool):
    """a UTF-8 BOM is not part of the key; a key that no client could send is reported instead of being used"""
    global server
    file_key = "key-from-a-file-0123456789"
    key_file = tmp_path / "admin-key.txt"
    key_file.write_bytes(content)
    server.log_path = str(tmp_path / "server.log")
    _enable_admin(PROVISION_ADMIN_KEY=None, PROVISION_ADMIN_KEY_FILE=str(key_file))
    server.start()

    if usable:
        res = _admin("GET", "/admin/status", key=file_key)
        assert res.body["authorized"] is True
        assert res.body["key_source"] == "file"
        assert not os.path.exists(_state_file("admin.key"))
        return

    # the next source takes over, the file is not read as garbage
    generated = _read_text(_state_file("admin.key")).strip()
    res = _admin("GET", "/admin/status", key=generated)
    assert res.body["authorized"] is True
    assert res.body["key_source"] == "generated"
    _assert_error(_admin("GET", "/admin/settings", key=file_key), 401, "admin_key_required")
    # and the log says why, with the path of the file
    server.stop()
    log = _read_text(server.log_path)
    assert f"PROVISION_ADMIN_KEY_FILE ({key_file}) is not usable" in log
    assert ("UTF-16" in log) == (b"\0" in content)


def test_admin_key_stored_as_utf16_is_replaced(tmp_path):
    """an admin.key in the state directory that no client could use is replaced by a generated key"""
    global server
    os.makedirs(state_dir)
    with open(_state_file("admin.key"), "wb") as f:
        f.write("my-own-key-0123456789\r\n".encode("utf-16"))
    server.log_path = str(tmp_path / "server.log")
    _enable_admin(PROVISION_ADMIN_KEY=None)
    server.start()

    generated = _read_text(_state_file("admin.key")).strip()
    assert re.fullmatch(r"[0-9a-f]{64}", generated)
    res = _admin("GET", "/admin/status", key=generated)
    assert res.body["authorized"] is True
    assert res.body["key_source"] == "generated"
    server.stop()
    log = _read_text(server.log_path)
    assert "is not usable: it is UTF-16 text" in log
    assert generated not in log


def test_admin_non_ascii_paths(tmp_path):
    """the state directory and the key file may have any name the file system takes"""
    global server, state_dir
    state_dir = str(tmp_path / "состояние ü")
    file_key = "key-from-a-file-0123456789"
    key_file = _write_text(tmp_path / "ключ.txt", file_key + "\n")
    _enable_admin(PROVISION_ADMIN_KEY=None, PROVISION_ADMIN_KEY_FILE=key_file)
    server.start()

    res = _admin("GET", "/admin/status", key=file_key)
    assert res.body["authorized"] is True
    assert res.body["key_source"] == "file"
    assert _same_path(res.body["state"]["dir"], state_dir)
    assert res.body["state"]["writable"] is True

    res = _admin("POST", "/admin/settings", data={"models_max": 3}, key=file_key)
    assert res.status_code == 200
    assert json.loads(_read_text(_state_file()))["settings"] == {"models_max": 3}
    state_entry = _admin("GET", "/admin/storage", key=file_key).body["dirs"][-1]
    assert _same_path(state_entry["path"], state_dir)
    assert state_entry["exists"] is True


def test_admin_key_unavailable(tmp_path):
    """no key given and the state directory cannot be created: the protected routes answer 503, the probe still works"""
    global server
    blocker = _write_text(tmp_path / "not-a-directory", "x")
    _enable_admin(PROVISION_ADMIN_KEY=None, PROVISION_ADMIN_STATE_DIR=os.path.join(blocker, "state"))
    server.start()

    res = _admin("GET", "/admin/status", key=None)
    assert res.status_code == 200
    assert res.body == {"admin": True, "authorized": False, "key_configured": False}
    res = _admin("GET", "/admin/status", key="any-key")
    assert res.body == {"admin": True, "authorized": False, "key_configured": False}

    for method, path, data in PROTECTED_ROUTES:
        for key in (None, "", "any-key"):
            _assert_error(_admin(method, path, data, key=key), 503, "admin_key_unavailable")

    # the existing routes do not depend on it
    assert MODEL_B in _models()


def test_admin_state_dir_not_writable(tmp_path):
    """with a key from the environment the layer works without a state directory, but settings cannot be saved"""
    global server
    blocker = _write_text(tmp_path / "not-a-directory", "x")
    _enable_admin(PROVISION_ADMIN_STATE_DIR=os.path.join(blocker, "state"))
    server.models_max = 2
    server.start()

    status = _admin_status()
    assert status["key_source"] == "env"
    assert status["state"]["writable"] is False

    before = _settings()
    _assert_error(_admin("POST", "/admin/settings", data={"models_max": 4}), 500, "server_error")
    # nothing was applied
    assert _settings() == before
    assert _props()["max_instances"] == 2

    state_entry = _admin("GET", "/admin/storage").body["dirs"][-1]
    assert state_entry["kind"] == "state"
    assert state_entry["exists"] is False
    assert state_entry["writable"] is False


def test_admin_with_api_key():
    """--api-key keeps protecting everything, the admin key comes on top of it"""
    global server
    server.api_key = "sk-router-secret"
    _enable_admin()
    server.start()
    api_headers = {"Authorization": f"Bearer {server.api_key}"}

    # the API key middleware comes first, for the probe as well
    for method, path, data in ALL_ROUTES:
        _assert_error(_admin(method, path, data, key=None), 401, "authentication_error")
        _assert_error(_admin(method, path, data), 401, "authentication_error")

    res = _admin("GET", "/admin/status", key=None, headers=api_headers)
    assert res.status_code == 200
    assert res.body == {"admin": True, "authorized": False, "key_configured": True}

    for method, path, data in PROTECTED_ROUTES:
        _assert_error(_admin(method, path, data, key=None, headers=api_headers), 401, "admin_key_required")
        # the API key is not an admin key
        _assert_error(_admin(method, path, data, key=server.api_key, headers=api_headers), 401, "admin_key_required")
        assert _admin(method, path, data, headers=api_headers).status_code == 200


def test_admin_unknown_paths():
    global server
    _enable_admin()
    server.start()

    _assert_not_found(_admin("GET", "/admin/nope"))
    _assert_not_found(_admin("GET", "/admin/settings/extra"))
    # wrong method
    _assert_not_found(_admin("POST", "/admin/status", data={}))
    _assert_not_found(_admin("POST", "/admin/storage", data={}))
    _assert_not_found(_admin("GET", "/admin/models/download/check"))
    _assert_not_found(_admin("DELETE", "/admin/settings"))


def test_admin_path_spellings_need_the_key():
    """however an admin path is spelled, the server either does not know it or asks for the key"""
    global server
    _enable_admin()
    server.start()

    spellings = [
        "/admin/settings/", "//admin/settings", "/admin//settings", "/./admin/settings", "/admin/./settings",
        "/x/../admin/settings", "/admin/x/../settings", "/ADMIN/settings", "/Admin/Settings", "/admin/settings;x=1",
        "/admin/settings.json", "/admin/settings%20", "/admin/settings%2F", "/admin/settings%00", "/admin\\settings",
        "/admin/settings?x=1", "/admin/settings?X-Admin-Key=" + ADMIN_KEY, "/admin/settings?key=" + ADMIN_KEY,
        "/admin/%73ettings", "/%61dmin/settings", "/admin%2Fsettings",
    ]
    for target in spellings:
        status, body = _raw_get(target)
        assert status in (400, 401, 404), f"{target} answered {status} without the key: {body}"
        assert "models_max" not in body

    # a spelling the server understands is the same route with the same protection
    # (percent-encoding is decoded before routing: a proxy rule for /admin has to decode it as well, nginx does)
    for target in ("/admin/settings?x=1", "/admin/%73ettings"):
        assert _raw_get(target)[0] == 401
        assert _raw_get(target, key="wrong-key")[0] == 401
        status, body = _raw_get(target, key=ADMIN_KEY)
        assert status == 200
        assert "models_max" in body


def test_admin_responses_are_not_cacheable():
    """admin answers carry settings and paths of the host: nothing between the server and the page may keep them"""
    global server
    _enable_admin()
    server.start()

    def cache_control(res: ServerResponse) -> str | None:
        return {name.lower(): value for name, value in res.headers.items()}.get("cache-control")

    for method, path, data in ALL_ROUTES:
        for key, expected in ((ADMIN_KEY, 200), (None, 200 if path == "/admin/status" else 401)):
            res = _admin(method, path, data, key=key)
            assert res.status_code == expected
            assert cache_control(res) == "no-store", f"{method} {path} -> {res.status_code}"
    res = _admin("POST", "/admin/settings", data={"models_max": 0})
    assert res.status_code == 400
    assert cache_control(res) == "no-store"


def test_admin_not_reachable_through_predict_route():
    """the Vertex AI compat route dispatches to handlers by name, the admin handlers must not be among them"""
    global server
    _enable_admin()
    server.gcp_compat = True
    server.start()
    assert _admin("GET", "/admin/settings").status_code == 200

    formats = [
        "/admin/status", "adminStatus",
        "/admin/settings", "adminSettings",
        "/admin/storage", "adminStorage",
        "/admin/models/download/check", "adminModelsDownloadCheck",
    ]
    instances = [{"@requestFormat": fmt, "model": "user/repo"} for fmt in formats]
    # a route of the router that is there, to prove the dispatch itself works
    instances.append({"@requestFormat": "/props"})
    res = _admin("POST", "/predict", data={"instances": instances})
    assert res.status_code == 200
    predictions = res.body["predictions"]
    assert len(predictions) == len(instances)
    for fmt, prediction in zip(formats, predictions):
        assert set(prediction) == {"error"}, f"{fmt} was dispatched: {prediction}"
        assert "no handler registered" in prediction["error"]["message"]
    assert predictions[-1]["role"] == "router"


@pytest.mark.parametrize("enabled", [True, False])
def test_admin_environment_not_passed_to_children(enabled: bool):
    """the PROVISION_ADMIN* variables (the key among them) never reach a model instance"""
    global server
    model_devices = "some-other-model=CUDA0"
    _enable_admin(PROVISION_ADMIN="1" if enabled else None, PROVISION_ADMIN_PINNED=MODEL_C, MODEL_DEVICES=model_devices)
    server.models_max = 2
    server.start()
    _load_model_and_wait(MODEL_B)

    assert server.process is not None
    children = _child_pids(server.process.pid)
    if not children:
        pytest.skip("cannot list the child processes on this platform")  # ty: ignore[too-many-positional-arguments]
    for pid in children:
        child_env = _process_environ(pid) or {}
        if not child_env:
            pytest.skip("cannot read the environment of a process on this platform (psutil is not installed)")  # ty: ignore[too-many-positional-arguments]
        # make sure this is the environment of a router child
        assert "LLAMA_SERVER_ROUTER_PORT" in child_env
        leaked = [name for name in child_env if name.upper().startswith("PROVISION_ADMIN")]
        assert leaked == []
        assert ADMIN_KEY not in child_env.values()
        # MODEL_DEVICES belongs to the deployment, it is only read
        assert child_env.get("MODEL_DEVICES") == model_devices


#
# settings
#


def test_admin_settings_defaults(tmp_path):
    global server
    preset_path = _write_text(tmp_path / "models.ini", _preset_section("preset-tiny", _make_preset_model(tmp_path)))
    _enable_admin()
    server.models_max = 2
    server.models_dir = _make_models_dir(tmp_path, "local-tiny")
    server.models_preset = preset_path
    server.start()

    res = _admin("GET", "/admin/settings")
    assert res.status_code == 200
    assert res.headers["Content-Type"].startswith("application/json")
    body = res.body
    assert body["models_max"] == 2  # --models-max
    assert body["default_device"] == ""
    assert body["default_device_locked"] is False
    assert body["pinned"] == []
    assert body["limits"] == {"models_max_min": 1, "models_max_max": 16}
    assert body["known_devices"] == []

    # the same models, with the same source, as the existing listing
    listed = _models()
    assert {m["id"]: m["source"] for m in body["models"]} == {name: m["source"] for name, m in listed.items()}
    by_id = {m["id"]: m for m in body["models"]}
    assert by_id[MODEL_B] == {"id": MODEL_B, "source": "cache", "status": "unloaded", "pinned": False}
    assert by_id["local-tiny"] == {"id": "local-tiny", "source": "models_dir", "status": "unloaded", "pinned": False}
    assert by_id["preset-tiny"] == {"id": "preset-tiny", "source": "preset", "status": "unloaded", "pinned": False}

    # reading the settings saves nothing
    assert not os.path.exists(_state_file())


def test_admin_settings_without_any_model(private_cache):
    """a fresh installation: nothing in the cache, no preset file, no models directory"""
    global server
    _enable_admin()
    server.models_max = 2
    server.start()

    assert _models() == {}
    assert _admin_status()["models"] == {"max": 2, "running": 0, "total": 0}
    body = _settings()
    assert body["models"] == []
    assert body["pinned"] == []
    assert body["known_devices"] == []

    # the settings work all the same, the device is there for the first model that arrives
    body = _post_settings({"models_max": 3, "default_device": "CUDA0"})
    assert body["models_max"] == 3
    assert body["default_device"] == "CUDA0"
    assert body["known_devices"] == ["CUDA0"]
    assert body["models"] == []
    _assert_error(_admin("POST", "/admin/settings", data={"pinned": [MODEL_B]}), 400, "invalid_request_error")
    assert _models(reload=True) == {}


def test_admin_settings_post(tmp_path):
    global server
    preset_path = _write_text(tmp_path / "models.ini", _preset_section("preset-tiny", _make_preset_model(tmp_path)))
    _enable_admin()
    server.models_max = 2
    server.models_preset = preset_path
    server.start()

    # all three at once; duplicates of pinned ids are dropped, the order is kept
    body = _post_settings({
        "models_max": 3,
        "default_device": "CUDA1;CUDA0",
        "pinned": [MODEL_B, "preset-tiny", MODEL_B],
    })
    assert body["models_max"] == 3
    assert body["default_device"] == "CUDA1,CUDA0"
    assert body["default_device_locked"] is False
    assert body["pinned"] == [MODEL_B, "preset-tiny"]
    assert body["limits"] == {"models_max_min": 1, "models_max_max": 16}
    assert body["known_devices"] == ["CUDA0", "CUDA1"]
    assert {m["id"] for m in body["models"] if m["pinned"]} == {MODEL_B, "preset-tiny"}
    # the response is the new state
    assert _settings() == body

    # persisted, in the documented format
    assert json.loads(_read_text(_state_file())) == {
        "version": 1,
        "settings": {"models_max": 3, "default_device": "CUDA1,CUDA0", "pinned": [MODEL_B, "preset-tiny"]},
    }
    first_save = _read_text(_state_file())
    assert not os.path.exists(_state_file("admin-state.json.tmp"))

    # a subset changes only what it names
    body = _post_settings({"pinned": [MODEL_C]})
    assert body["models_max"] == 3
    assert body["default_device"] == "CUDA1,CUDA0"
    assert body["pinned"] == [MODEL_C]
    assert json.loads(_read_text(_state_file()))["settings"] == {
        "models_max": 3, "default_device": "CUDA1,CUDA0", "pinned": [MODEL_C],
    }
    # the file before this save is kept
    assert _read_text(_state_file("admin-state.json.bak")) == first_save
    assert not os.path.exists(_state_file("admin-state.json.tmp"))

    # an empty device and an empty list clear the values
    body = _post_settings({"default_device": "", "pinned": []})
    assert body["default_device"] == ""
    assert body["pinned"] == []
    assert body["known_devices"] == []
    assert body["models_max"] == 3

    # an empty change is fine
    assert _post_settings({}) == body
    assert _settings() == body


INVALID_SETTINGS = [
    pytest.param({"models_max": 0}, id="models_max_below_min"),
    pytest.param({"models_max": -1}, id="models_max_negative"),
    pytest.param({"models_max": 17}, id="models_max_above_max"),
    pytest.param({"models_max": "3"}, id="models_max_string"),
    pytest.param({"models_max": 2.5}, id="models_max_float"),
    pytest.param({"models_max": True}, id="models_max_bool"),
    pytest.param({"models_max": [3]}, id="models_max_array"),
    pytest.param({"models_max": 4294967297}, id="models_max_wraps_to_1_as_int32"),
    pytest.param({"models_max": 18446744073709551615}, id="models_max_uint64"),
    pytest.param({"default_device": "CUDA"}, id="device_without_index"),
    pytest.param({"default_device": "0CUDA"}, id="device_starts_with_digit"),
    pytest.param({"default_device": "CUDA-0"}, id="device_bad_character"),
    pytest.param({"default_device": "CUDA 0"}, id="device_with_space"),
    pytest.param({"default_device": "CUDA0 CUDA1"}, id="device_bad_separator"),
    pytest.param({"default_device": "CUDA0,"}, id="device_trailing_separator"),
    pytest.param({"default_device": ";CUDA0"}, id="device_leading_separator"),
    pytest.param({"default_device": "CUDA0,,CUDA1"}, id="device_empty_item"),
    pytest.param({"default_device": "none,CUDA0"}, id="device_none_in_list"),
    pytest.param({"default_device": "CUDA0;rm -rf /"}, id="device_injection"),
    pytest.param({"default_device": 0}, id="device_number"),
    pytest.param({"default_device": ["CUDA0"]}, id="device_array"),
    pytest.param({"pinned": MODEL_B}, id="pinned_string"),
    pytest.param({"pinned": {MODEL_B: True}}, id="pinned_object"),
    pytest.param({"pinned": [1]}, id="pinned_number_item"),
    pytest.param({"pinned": [""]}, id="pinned_empty_id"),
    pytest.param({"pinned": [MODEL_B, None]}, id="pinned_null_item"),
    pytest.param({"pinned": [MODEL_B, "no-such-model"]}, id="pinned_unknown_id"),
    pytest.param({"pinned": [MODEL_B, MODEL_C]}, id="pinned_fills_every_slot"),
    pytest.param({"models_max": 1, "pinned": [MODEL_B]}, id="pinned_with_one_slot"),
    pytest.param({"models_max": 4, "default_device": "bad device"}, id="valid_and_invalid_field"),
    pytest.param({"models_max": 4, "pinned": ["no-such-model"]}, id="valid_and_unknown_pinned"),
    pytest.param("[1, 2]", id="body_array"),
    pytest.param('"models_max"', id="body_string"),
    pytest.param('{"models_max": ', id="body_broken_json"),
    pytest.param("", id="body_empty"),
]


def _post_raw(path: str, body: str) -> ServerResponse:
    """POST a body as it is, for the ones that are not a JSON object"""
    raw = requests.post(server.make_url(path), data=body, timeout=60,
                        headers={"X-Admin-Key": ADMIN_KEY, "Content-Type": "application/json"})
    res = ServerResponse()
    res.headers = dict(raw.headers)
    res.status_code = raw.status_code
    res.body = raw.json()
    return res


@pytest.mark.parametrize("body", INVALID_SETTINGS)
def test_admin_settings_post_invalid(body):
    global server
    _enable_admin()
    server.models_max = 2
    server.start()
    before = _settings()

    if isinstance(body, str):
        res = _post_raw("/admin/settings", body)
    else:
        res = _admin("POST", "/admin/settings", data=body)
    _assert_error(res, 400, "invalid_request_error")

    # nothing is saved and nothing is applied, not even the valid part of the request
    assert not os.path.exists(_state_file())
    assert _settings() == before
    assert _props()["max_instances"] == 2


def test_admin_settings_post_ignores_unknown_keys_and_null():
    global server
    _enable_admin()
    server.models_max = 2
    server.start()
    before = _settings()

    # a client may send back what it got from GET, read-only fields included
    body = _post_settings({**before, "models_max": None, "default_device": None, "pinned": None, "something_else": 1})
    assert body == before
    body = _post_settings({**before, "models_max": 3})
    assert body["models_max"] == 3


def test_admin_settings_models_max_limits():
    global server
    _enable_admin()
    server.models_max = 2
    server.start()

    limits = _settings()["limits"]
    assert _post_settings({"models_max": limits["models_max_max"]})["models_max"] == limits["models_max_max"]
    assert _props()["max_instances"] == limits["models_max_max"]
    assert _post_settings({"models_max": limits["models_max_min"]})["models_max"] == limits["models_max_min"]
    assert _props()["max_instances"] == limits["models_max_min"]
    _assert_error(_admin("POST", "/admin/settings", data={"models_max": limits["models_max_max"] + 1}), 400, "invalid_request_error")
    _assert_error(_admin("POST", "/admin/settings", data={"models_max": limits["models_max_min"] - 1}), 400, "invalid_request_error")
    assert _settings()["models_max"] == limits["models_max_min"]


def test_admin_settings_one_slot_stays_evictable():
    """invariant on the resulting settings: fewer pinned models than models_max"""
    global server
    _enable_admin()
    server.models_max = 2
    server.start()

    assert _post_settings({"pinned": [MODEL_B]})["pinned"] == [MODEL_B]

    # a second pinned model needs a third slot
    _assert_error(_admin("POST", "/admin/settings", data={"pinned": [MODEL_B, MODEL_C]}), 400, "invalid_request_error")
    body = _post_settings({"models_max": 3, "pinned": [MODEL_B, MODEL_C]})
    assert body["models_max"] == 3
    assert body["pinned"] == [MODEL_B, MODEL_C]

    # the limit cannot go below what the pinned models need ...
    res = _admin("POST", "/admin/settings", data={"models_max": 2})
    _assert_error(res, 400, "invalid_request_error")
    assert "evictable" in res.body["error"]["message"]
    _assert_error(_admin("POST", "/admin/settings", data={"models_max": 1}), 400, "invalid_request_error")
    # ... and one more pinned model does not fit
    _assert_error(_admin("POST", "/admin/settings", data={"pinned": [MODEL_A, MODEL_B, MODEL_C]}), 400, "invalid_request_error")
    body = _settings()
    assert body["models_max"] == 3
    assert body["pinned"] == [MODEL_B, MODEL_C]

    # ... unless the same request unpins
    body = _post_settings({"models_max": 2, "pinned": [MODEL_C]})
    assert body["models_max"] == 2
    assert body["pinned"] == [MODEL_C]
    body = _post_settings({"models_max": 1, "pinned": []})
    assert body["models_max"] == 1
    assert body["pinned"] == []


def test_admin_settings_concurrent_posts():
    """settings changes are applied one at a time: the result is one of the requests as a whole, everywhere"""
    global server
    _enable_admin()
    server.models_max = 2
    server.start()

    values = [3, 4, 5, 6, 7, 8, 9, 10]
    results = parallel_function_calls([
        (lambda v: _admin("POST", "/admin/settings", data={"models_max": v, "default_device": f"CUDA{v}"}), (v,))
        for v in values
    ])
    for res in results:
        assert res is not None and res.status_code == 200
        assert res.body["default_device"] == f"CUDA{res.body['models_max']}"

    final = _settings()
    assert final["models_max"] in values
    assert final["default_device"] == f"CUDA{final['models_max']}"
    assert _props()["max_instances"] == final["models_max"]
    assert _device_arg(MODEL_B) == final["default_device"]
    assert json.loads(_read_text(_state_file()))["settings"] == {
        "models_max": final["models_max"], "default_device": final["default_device"],
    }
    assert not os.path.exists(_state_file("admin-state.json.tmp"))


def test_admin_settings_state_dir_removed_while_running():
    """the state directory disappears under the running server (a volume was wiped): the next save puts it back"""
    global server
    _enable_admin()
    server.models_max = 2
    server.start()
    _post_settings({"models_max": 3})

    shutil.rmtree(state_dir)
    assert _admin_status()["state"]["writable"] is False
    assert _settings()["models_max"] == 3

    assert _post_settings({"models_max": 4})["models_max"] == 4
    assert json.loads(_read_text(_state_file()))["settings"] == {"models_max": 4}
    assert _admin_status()["state"]["writable"] is True


def test_admin_settings_unlimited_models_max():
    """--models-max 0 (no limit) is reported as it is; a limit can be set from here, "no limit" cannot"""
    global server
    _enable_admin()
    server.models_max = 0
    server.start()

    body = _settings()
    assert body["models_max"] == 0
    assert body["limits"] == {"models_max_min": 1, "models_max_max": 16}
    assert _admin_status()["models"]["max"] == 0

    # nothing is ever evicted without a limit, so any number of models can be pinned
    assert _post_settings({"pinned": [MODEL_A, MODEL_B, MODEL_C]})["pinned"] == [MODEL_A, MODEL_B, MODEL_C]
    _assert_error(_admin("POST", "/admin/settings", data={"models_max": 0}), 400, "invalid_request_error")
    # a limit has to leave one slot evictable again
    _assert_error(_admin("POST", "/admin/settings", data={"models_max": 3}), 400, "invalid_request_error")
    body = _post_settings({"models_max": 4})
    assert body["models_max"] == 4
    assert body["pinned"] == [MODEL_A, MODEL_B, MODEL_C]
    assert _props()["max_instances"] == 4


def test_admin_settings_hidden_models_are_not_listed(tmp_path):
    """a cache model hidden by dedup-cache-models is not a model of its own here either"""
    global server
    preset = "[model-dedup]\nhf-repo = ggml-org/test-model-stories260K\ndedup-cache-models = 1\n"
    _enable_admin()
    server.models_max = 3
    server.models_preset = _write_text(tmp_path / "models.ini", preset)
    server.start()

    listed = _models()
    assert "model-dedup" in listed
    assert MODEL_B not in listed

    body = _settings()
    assert {m["id"] for m in body["models"]} == set(listed)
    assert _admin_status()["models"]["total"] == len(listed)
    _assert_error(_admin("POST", "/admin/settings", data={"pinned": [MODEL_B]}), 400, "invalid_request_error")
    assert _post_settings({"pinned": ["model-dedup"]})["pinned"] == ["model-dedup"]


def test_admin_settings_persist_across_restart():
    global server

    def comparable(settings: dict) -> dict:
        # other tests may add a model to the shared cache meanwhile: compare the models of this test only
        known = (MODEL_A, MODEL_B, MODEL_C)
        return {**settings, "models": [m for m in settings["models"] if m["id"] in known]}

    # startup values that the saved settings replace
    _enable_admin(PROVISION_ADMIN_DEFAULT_DEVICE="CUDA0", PROVISION_ADMIN_PINNED=MODEL_C)
    server.models_max = 2
    server.start()

    saved = _post_settings({"models_max": 4, "default_device": "CUDA1", "pinned": [MODEL_A, MODEL_B]})
    assert saved["models_max"] == 4
    assert _device_arg(MODEL_B) == "CUDA1"

    _restart_server()

    body = _settings()
    assert body["models_max"] == 4, "the saved value replaces --models-max"
    assert body["default_device"] == "CUDA1"
    assert body["pinned"] == [MODEL_A, MODEL_B]
    assert comparable(body) == comparable(saved)
    assert _props()["max_instances"] == 4
    assert _admin_status()["models"]["max"] == 4
    assert _admin_status()["state"]["error"] == ""
    assert _device_arg(MODEL_B) == "CUDA1"

    # a cleared value stays cleared, it does not fall back to the environment again
    _post_settings({"default_device": "", "pinned": []})
    _restart_server()
    body = _settings()
    assert body["default_device"] == ""
    assert body["pinned"] == []
    assert body["models_max"] == 4
    assert _device_arg(MODEL_B) is None


def test_admin_settings_startup_values_from_env():
    """PROVISION_ADMIN_DEFAULT_DEVICE / PROVISION_ADMIN_PINNED are used until a value is saved"""
    global server
    _enable_admin(PROVISION_ADMIN_DEFAULT_DEVICE="CUDA0", PROVISION_ADMIN_PINNED=f" {MODEL_B} , {MODEL_B},no-such-model")
    server.models_max = 3
    server.start()

    body = _settings()
    assert body["models_max"] == 3
    assert body["default_device"] == "CUDA0"
    assert body["pinned"] == [MODEL_B]
    assert body["pinned_unknown"] == ["no-such-model"]
    assert body["known_devices"] == ["CUDA0"]
    assert _device_arg(MODEL_B) == "CUDA0"
    # startup values are not written to the state file
    assert not os.path.exists(_state_file())

    # saving one setting leaves the others on their startup values
    body = _post_settings({"models_max": 4})
    assert body["default_device"] == "CUDA0"
    assert body["pinned"] == [MODEL_B]
    assert json.loads(_read_text(_state_file()))["settings"] == {"models_max": 4}

    _restart_server()
    body = _settings()
    assert body["models_max"] == 4
    assert body["default_device"] == "CUDA0"
    assert body["pinned"] == [MODEL_B]


@pytest.mark.parametrize("source", ["env", "state_file"])
def test_admin_settings_pinned_at_startup_keeps_one_slot(source: str):
    """a pinned list from the environment or from an older state file that would fill every slot is cut, not rejected"""
    global server
    if source == "env":
        _enable_admin(PROVISION_ADMIN_PINNED=f"{MODEL_B},{MODEL_C}")
    else:
        os.makedirs(state_dir)
        _write_text(_state_file(), json.dumps({"version": 1, "settings": {"pinned": [MODEL_B, MODEL_C]}}))
        _enable_admin()
    server.models_max = 2
    server.start()

    assert _admin_status()["state"]["error"] == ""
    body = _settings()
    assert body["pinned"] == [MODEL_B]
    assert body["pinned_ignored"] == [MODEL_C]
    assert {m["id"] for m in body["models"] if m["pinned"]} == {MODEL_B}
    # what GET returns can be sent back unchanged
    assert _post_settings({"models_max": body["models_max"], "pinned": body["pinned"]})["pinned"] == [MODEL_B]


def test_admin_settings_models_max_only_with_cut_startup_pins():
    """a POST without "pinned" is checked against the pinned models in effect, not against the whole startup list"""
    global server
    _enable_admin(PROVISION_ADMIN_PINNED=f"{MODEL_B},{MODEL_C},{MODEL_A}")
    server.models_max = 2
    server.start()

    body = _settings()
    assert body["pinned"] == [MODEL_B]
    assert body["pinned_ignored"] == [MODEL_C, MODEL_A]
    # what the settings page sends when only the limit changed, or nothing at all
    assert _post_settings({"models_max": 2})["pinned"] == [MODEL_B]
    body = _post_settings({"models_max": 3})
    # a startup list is cut to models_max - 1: more room, more of it in effect
    assert body["pinned"] == [MODEL_B, MODEL_C]
    assert body["pinned_ignored"] == [MODEL_A]
    assert not json.loads(_read_text(_state_file()))["settings"].get("pinned")
    # the pinned models in effect do count
    res = _admin("POST", "/admin/settings", data={"models_max": 2})
    _assert_error(res, 400, "invalid_request_error")
    assert "evictable" in res.body["error"]["message"]
    assert _settings()["models_max"] == 3


def test_admin_settings_invalid_env_device_is_ignored():
    global server
    _enable_admin(PROVISION_ADMIN_DEFAULT_DEVICE="not a device")
    server.start()

    assert _settings()["default_device"] == ""
    assert _device_arg(MODEL_B) is None


def test_admin_settings_pinned_model_leaves_the_list(tmp_path):
    """a pinned id that is no longer in the model list is not reported as pinned"""
    global server
    preset_path = tmp_path / "models.ini"
    section_a = _preset_section("preset-a", _make_preset_model(tmp_path, "preset-a"))
    section_b = _preset_section("preset-b", _make_preset_model(tmp_path, "preset-b"))
    _write_text(preset_path, section_a + section_b)
    _enable_admin()
    server.models_max = 3
    server.models_preset = str(preset_path)
    server.start()

    assert _post_settings({"pinned": ["preset-a", "preset-b"]})["pinned"] == ["preset-a", "preset-b"]

    _write_text(preset_path, section_b)
    assert "preset-a" not in _models(reload=True)
    body = _settings()
    assert body["pinned"] == ["preset-b"]
    assert body["pinned_unknown"] == ["preset-a"]
    assert "preset-a" not in {m["id"] for m in body["models"]}

    # it is pinned again when it comes back
    _write_text(preset_path, section_a + section_b)
    assert "preset-a" in _models(reload=True)
    assert _settings()["pinned"] == ["preset-a", "preset-b"]


CORRUPT_STATE_FILES = [
    pytest.param("{ this is not json", id="not_json"),
    pytest.param("", id="empty_file"),
    pytest.param("[1, 2, 3]", id="json_array"),
    pytest.param('{"version": 99, "settings": {"models_max": 5}}', id="unsupported_version"),
    pytest.param('{"version": 1, "settings": [5]}', id="settings_not_an_object"),
    pytest.param('{"version": 1, "settings": {"models_max": 99}}', id="value_out_of_range"),
]


@pytest.mark.parametrize("content", CORRUPT_STATE_FILES)
def test_admin_corrupt_state_file(content: str):
    """a state file that cannot be used does not stop the server, and it is kept until settings are saved"""
    global server
    os.makedirs(state_dir)
    _write_text(_state_file(), content)
    _enable_admin(PROVISION_ADMIN_DEFAULT_DEVICE="CUDA0")
    server.models_max = 2
    server.start()

    # the startup values are in use and the problem is reported
    status = _admin_status()
    assert status["state"]["error"] != ""
    assert status["models"]["max"] == 2
    body = _settings()
    assert body["models_max"] == 2
    assert body["default_device"] == "CUDA0"
    assert body["pinned"] == []
    assert _props()["max_instances"] == 2

    # reading and a rejected change leave the file alone
    _assert_error(_admin("POST", "/admin/settings", data={"models_max": 0}), 400, "invalid_request_error")
    assert _read_text(_state_file()) == content
    assert [n for n in os.listdir(state_dir) if not n.startswith("admin-audit")] == ["admin-state.json"]  # + the audit log

    # a successful save replaces it
    assert _post_settings({"models_max": 3})["models_max"] == 3
    assert json.loads(_read_text(_state_file())) == {"version": 1, "settings": {"models_max": 3}}
    assert _read_text(_state_file("admin-state.json.bak")) == content
    assert _admin_status()["state"]["error"] == ""

    _restart_server()
    assert _settings()["models_max"] == 3
    assert _admin_status()["state"]["error"] == ""


def test_admin_state_file_partial_settings():
    """every field of the state file is optional"""
    global server
    os.makedirs(state_dir)
    _write_text(_state_file(), '{"version": 1, "settings": {"default_device": "CUDA1"}}')
    _enable_admin(PROVISION_ADMIN_DEFAULT_DEVICE="CUDA0", PROVISION_ADMIN_PINNED=MODEL_B)
    server.models_max = 2
    server.start()

    body = _settings()
    assert body["models_max"] == 2            # --models-max
    assert body["default_device"] == "CUDA1"  # state file
    assert body["pinned"] == [MODEL_B]        # environment
    assert _admin_status()["state"]["error"] == ""


TOLERATED_STATE_FILES = [
    pytest.param('{"settings": {"models_max": 3}}', 3, id="no_version"),
    pytest.param('{"version": 1}', 2, id="no_settings"),
    pytest.param('{"version": 1, "settings": {}}', 2, id="empty_settings"),
    pytest.param('{"version": 1, "settings": {"models_max": null, "default_device": null, "pinned": null}}', 2, id="null_values"),
    pytest.param('{"version": 1, "settings": {"models_max": 3, "added_later": true}, "also_added_later": {}}', 3, id="unknown_keys"),
]


@pytest.mark.parametrize("content,models_max", TOLERATED_STATE_FILES)
def test_admin_state_file_tolerated(content: str, models_max: int):
    """a state file that says less or more than this version expects is not an error"""
    global server
    os.makedirs(state_dir)
    _write_text(_state_file(), content)
    _enable_admin(PROVISION_ADMIN_DEFAULT_DEVICE="CUDA0")
    server.models_max = 2
    server.start()

    assert _admin_status()["state"]["error"] == ""
    body = _settings()
    assert body["models_max"] == models_max
    assert body["default_device"] == "CUDA0"
    assert body["pinned"] == []
    assert _read_text(_state_file()) == content


def test_admin_state_file_invalid_field_keeps_the_others():
    """one unusable value in the state file costs that setting only, and the problem is reported"""
    global server
    os.makedirs(state_dir)
    content = '{"version": 1, "settings": {"models_max": 99, "default_device": "CUDA1", "pinned": "not-a-list"}}'
    _write_text(_state_file(), content)
    _enable_admin(PROVISION_ADMIN_DEFAULT_DEVICE="CUDA0", PROVISION_ADMIN_PINNED=MODEL_B)
    server.models_max = 2
    server.start()

    error = _admin_status()["state"]["error"]
    assert "models_max" in error and "pinned" in error
    body = _settings()
    assert body["models_max"] == 2            # --models-max
    assert body["default_device"] == "CUDA1"  # state file
    assert body["pinned"] == [MODEL_B]        # environment
    assert _read_text(_state_file()) == content


def test_admin_models_max_below_startup_models(tmp_path):
    """models_max can never go below the number of load-on-startup models: the router would not start again"""
    global server
    section_a = _preset_section("startup-a", _make_preset_model(tmp_path, "startup-a"), "load-on-startup = true")
    section_b = _preset_section("startup-b", _make_preset_model(tmp_path, "startup-b"), "load-on-startup = true")
    _enable_admin()
    server.models_max = 3
    server.models_preset = _write_text(tmp_path / "models.ini", section_a + section_b)
    server.start()

    res = _admin("POST", "/admin/settings", data={"models_max": 1})
    _assert_error(res, 400, "invalid_request_error")
    assert "load-on-startup" in res.body["error"]["message"]
    assert _post_settings({"models_max": 2})["models_max"] == 2

    # a saved value that became too small (the preset file changed meanwhile) is ignored for this run
    server.stop()
    _write_text(_state_file(), '{"version": 1, "settings": {"models_max": 1}}')
    _restart_server()
    assert _settings()["models_max"] == 3
    assert "load-on-startup" in _admin_status()["state"]["error"]
    _wait_for_model_status("startup-a", "loaded")
    _wait_for_model_status("startup-b", "loaded")


#
# settings applied to the router
#


def test_admin_models_max_applies_at_runtime(tmp_path):
    global server
    _enable_admin()
    server.models_max = 1
    server.models_dir = _make_models_dir(tmp_path, "local-x")
    server.start()
    _load_model_and_wait(MODEL_B)

    # more slots: in effect at once, the next load evicts nothing
    assert _post_settings({"models_max": 2})["models_max"] == 2
    assert _props()["max_instances"] == 2
    _load_model_and_wait(MODEL_C)
    assert _model_status(MODEL_B) == "loaded"
    assert _admin_status()["models"]["running"] == 2

    # fewer slots: the change itself unloads nothing ...
    assert _post_settings({"models_max": 1})["models_max"] == 1
    assert _props()["max_instances"] == 1
    time.sleep(1)
    assert _model_status(MODEL_B) == "loaded"
    assert _model_status(MODEL_C) == "loaded"

    # ... the next request for another model makes room down to the new limit
    res = _tokenize("local-x")
    assert res.status_code == 200
    assert _model_status("local-x") == "loaded"
    _wait_for_model_status(MODEL_B, "unloaded")
    _wait_for_model_status(MODEL_C, "unloaded")
    assert _admin_status()["models"]["running"] == 1


def test_admin_models_max_increase_serves_queued_request():
    """a request that waits for a slot gets the one a higher limit adds, nothing is evicted for it"""
    global server
    _enable_admin()
    server.models_max = 1
    server.start()
    _load_model_and_wait(MODEL_B)

    # (with LLAMA_SERVER_DEBUG_FAKE_TIMING a request keeps its model busy for 2 seconds, see test_router.py)
    busy = _start_request(lambda: _tokenize(MODEL_B))
    time.sleep(0.5)  # let the request reach the child and take the only slot
    queued = _start_request(lambda: _tokenize(MODEL_C))  # no slot free and MODEL_B is busy: it waits
    time.sleep(0.3)
    assert _model_status(MODEL_C) == "unloaded"

    assert _post_settings({"models_max": 2})["models_max"] == 2

    assert busy.result(timeout=180).status_code == 200
    assert queued.result(timeout=180).status_code == 200
    assert _model_status(MODEL_C) == "loaded"
    # with one slot MODEL_B would have been given up as soon as it went idle
    assert _model_status(MODEL_B) == "loaded"


def test_admin_models_max_decrease_then_explicit_load(tmp_path):
    """after the limit was lowered below the number of running models an explicit load makes room like any other"""
    global server
    _enable_admin()
    server.models_max = 2
    server.models_dir = _make_models_dir(tmp_path, "local-x")
    server.start()
    _load_model_and_wait(MODEL_B)
    _load_model_and_wait(MODEL_C)
    assert _post_settings({"models_max": 1})["models_max"] == 1

    # one request, no "model limit reached": both models over the limit go for it
    res = server.make_request("POST", "/models/load", data={"model": "local-x"}, timeout=120)
    assert res.status_code == 200, f"explicit load failed: {res.body}"
    _wait_for_model_status("local-x", "loaded", timeout=120)
    assert _model_status(MODEL_B) == "unloaded"
    assert _model_status(MODEL_C) == "unloaded"
    assert _admin_status()["models"]["running"] == 1


def test_admin_models_max_decrease_explicit_load_spares_pinned(tmp_path):
    """the models over the limit go least recently used first, a pinned one never, and no more than needed"""
    global server
    _enable_admin()
    server.models_max = 4
    server.models_dir = _make_models_dir(tmp_path, "local-a", "local-b", "local-c", "local-x")
    server.start()
    for name in ("local-a", "local-b", "local-c"):  # local-a is the least recently used
        _load_model_and_wait(name)
    body = _post_settings({"models_max": 2, "pinned": ["local-a"]})
    assert body["models_max"] == 2
    assert body["pinned"] == ["local-a"]

    res = server.make_request("POST", "/models/load", data={"model": "local-x"}, timeout=120)
    assert res.status_code == 200, f"explicit load failed: {res.body}"
    _wait_for_model_status("local-x", "loaded")
    assert _model_status("local-a") == "loaded"
    assert _model_status("local-b") == "unloaded"
    assert _model_status("local-c") == "unloaded"
    assert _admin_status()["models"]["running"] == 2


def test_admin_pinned_model_is_not_evicted(tmp_path):
    global server
    _enable_admin()
    server.models_max = 2
    server.models_dir = _make_models_dir(tmp_path, "local-x")
    server.start()

    assert _post_settings({"pinned": ["local-x"]})["pinned"] == ["local-x"]

    # local-x is the least recently used model, without the pin it would be the one to go
    _load_model_and_wait("local-x")
    _load_model_and_wait(MODEL_B)
    _load_model_and_wait(MODEL_C)
    assert _model_status("local-x") == "loaded"
    assert _model_status(MODEL_B) == "unloaded"
    assert _model_status(MODEL_C) == "loaded"

    # the same for a request that loads its model on demand
    res = _tokenize(MODEL_B)
    assert res.status_code == 200
    assert _model_status("local-x") == "loaded"
    assert _model_status(MODEL_B) == "loaded"
    _wait_for_model_status(MODEL_C, "unloaded")

    # unpinned, it is evicted again as the least recently used model
    assert _post_settings({"pinned": []})["pinned"] == []
    _load_model_and_wait(MODEL_C)
    assert _model_status("local-x") == "unloaded"
    assert _model_status(MODEL_B) == "loaded"


def test_admin_unpin_serves_queued_request(tmp_path):
    """a request that waits because the only idle model is pinned goes ahead as soon as the pin is removed"""
    global server
    _enable_admin()
    server.models_max = 2
    server.models_dir = _make_models_dir(tmp_path, "local-x")
    server.start()
    assert _post_settings({"pinned": [MODEL_B]})["pinned"] == [MODEL_B]
    _load_model_and_wait(MODEL_B)
    _load_model_and_wait(MODEL_C)

    busy = _start_request(lambda: _tokenize(MODEL_C))
    time.sleep(0.5)  # let the request reach the child
    queued = _start_request(lambda: _tokenize("local-x"))  # MODEL_C is busy and MODEL_B is pinned: it waits
    time.sleep(0.3)
    assert _model_status("local-x") == "unloaded"
    assert _model_status(MODEL_B) == "loaded"

    assert _post_settings({"pinned": []})["pinned"] == []

    assert busy.result(timeout=180).status_code == 200
    assert queued.result(timeout=180).status_code == 200
    assert _model_status("local-x") == "loaded"
    # the idle model went at once; still pinned, MODEL_C would have been given up after its request
    assert _model_status(MODEL_B) == "unloaded"
    assert _model_status(MODEL_C) == "loaded"


def test_admin_pinned_from_env_is_not_evicted(tmp_path):
    global server
    _enable_admin(PROVISION_ADMIN_PINNED=MODEL_B)
    server.models_max = 2
    server.models_dir = _make_models_dir(tmp_path, "local-x")
    server.start()

    # nobody asked for the settings yet: the pin is in place from the start
    _load_model_and_wait(MODEL_B)
    _load_model_and_wait(MODEL_C)
    _load_model_and_wait("local-x")
    assert _model_status(MODEL_B) == "loaded"
    assert _model_status(MODEL_C) == "unloaded"
    assert _model_status("local-x") == "loaded"


@pytest.mark.parametrize("admin_request", [False, True])
def test_admin_pinned_model_that_appears_later_is_not_evicted(tmp_path, admin_request: bool):
    """a model named in the pinned list is protected from the moment it is in the model list"""
    global server
    _enable_admin(PROVISION_ADMIN_PINNED="local-z")
    server.models_max = 2
    server.models_dir = _make_models_dir(tmp_path, "local-a", "local-b")
    server.start()

    # the model arrives while the router is running and is picked up by the existing rescan
    shutil.copyfile(_tiny_gguf(), os.path.join(server.models_dir, "local-z.gguf"))
    assert "local-z" in _models(reload=True)
    if admin_request:
        assert _settings()["pinned"] == ["local-z"]

    _load_model_and_wait("local-z")
    _load_model_and_wait("local-a")
    _load_model_and_wait("local-b")
    # local-z is the least recently used model, without the pin it is the one to go
    assert _model_status("local-z") == "loaded"
    assert _model_status("local-a") == "unloaded"
    assert _model_status("local-b") == "loaded"


def test_admin_default_device_overlay(tmp_path):
    """the default device goes to models that are not defined in the preset file and have no device of their own"""
    global server
    preset = (
        _preset_section("preset-plain", _make_preset_model(tmp_path, "preset-plain"))
        + _preset_section("preset-device", _make_preset_model(tmp_path, "preset-device"), "device = CUDA7,CUDA5")
        # a model of the cache that the preset file configures: it belongs to the preset file from then on
        + f"[{MODEL_C}]\nctx-size = 512\n"
    )
    _enable_admin(PROVISION_ADMIN_DEFAULT_DEVICE="CUDA0")
    server.models_dir = _make_models_dir(tmp_path, "local-tiny")
    server.models_preset = _write_text(tmp_path / "models.ini", preset)
    server.start()

    listed = _models()
    assert listed[MODEL_B]["source"] == "cache"
    assert listed[MODEL_C]["source"] == "preset"
    assert _device_arg(MODEL_C) is None

    def devices() -> dict:
        return {name: _device_arg(name) for name in (MODEL_B, "local-tiny", "preset-plain", "preset-device")}

    # from the environment
    assert devices() == {MODEL_B: "CUDA0", "local-tiny": "CUDA0", "preset-plain": None, "preset-device": "CUDA7,CUDA5"}
    assert _settings()["known_devices"] == ["CUDA0", "CUDA5", "CUDA7"]

    # changed at runtime: the presets are rebuilt at once
    body = _post_settings({"default_device": "CUDA1;CUDA2"})
    assert body["default_device"] == "CUDA1,CUDA2"
    assert body["known_devices"] == ["CUDA1", "CUDA2", "CUDA5", "CUDA7"]
    assert devices() == {MODEL_B: "CUDA1,CUDA2", "local-tiny": "CUDA1,CUDA2", "preset-plain": None, "preset-device": "CUDA7,CUDA5"}

    # still there after a rescan
    _models(reload=True)
    assert devices()[MODEL_B] == "CUDA1,CUDA2"

    # "none" is a device value as well
    assert _post_settings({"default_device": "none"})["default_device"] == "none"
    assert devices() == {MODEL_B: "none", "local-tiny": "none", "preset-plain": None, "preset-device": "CUDA7,CUDA5"}
    assert _device_arg(MODEL_C) is None

    # cleared: no device argument anymore
    assert _post_settings({"default_device": ""})["default_device"] == ""
    assert devices() == {MODEL_B: None, "local-tiny": None, "preset-plain": None, "preset-device": "CUDA7,CUDA5"}


@pytest.mark.parametrize("value,expected", [
    ("CUDA0", "CUDA0"),
    ("Vulkan12", "Vulkan12"),
    ("sycl_gpu_0", "sycl_gpu_0"),
    ("CUDA0,CUDA1", "CUDA0,CUDA1"),
    ("CUDA0;CUDA1", "CUDA0,CUDA1"),
    (" CUDA0 ; CUDA1 ", "CUDA0,CUDA1"),
    ("none", "none"),
])
def test_admin_default_device_formats(value: str, expected: str):
    global server
    _enable_admin()
    server.start()

    assert _post_settings({"default_device": value})["default_device"] == expected
    assert _device_arg(MODEL_B) == expected
    assert json.loads(_read_text(_state_file()))["settings"]["default_device"] == expected


def test_admin_default_device_global_preset_wins(tmp_path):
    """a device in the [*] section of the preset file applies to every model, the admin default never replaces it"""
    global server
    preset = "[*]\ndevice = CUDA7\n\n" + _preset_section("preset-plain", _make_preset_model(tmp_path, "preset-plain"))
    _enable_admin(PROVISION_ADMIN_DEFAULT_DEVICE="CUDA0")
    server.models_preset = _write_text(tmp_path / "models.ini", preset)
    server.start()

    assert _device_arg(MODEL_B) == "CUDA7"
    assert _device_arg("preset-plain") == "CUDA7"
    _post_settings({"default_device": "CUDA1"})
    assert _device_arg(MODEL_B) == "CUDA7"
    assert _device_arg("preset-plain") == "CUDA7"


def test_admin_default_device_change_reloads_running_models(tmp_path):
    """a new default device changes the preset of the affected models: the running ones are unloaded, the others stay"""
    global server
    _enable_admin()
    server.models_max = 3
    server.models_preset = _write_text(tmp_path / "models.ini", _preset_section("preset-tiny", _make_preset_model(tmp_path)))
    server.start()

    _load_model_and_wait(MODEL_B)
    _load_model_and_wait("preset-tiny")

    # settings that do not touch the presets leave everything running
    _post_settings({"models_max": 2, "pinned": [MODEL_B]})
    assert _model_status(MODEL_B) == "loaded"
    assert _model_status("preset-tiny") == "loaded"

    _post_settings({"default_device": "CUDA0"})
    _wait_for_model_status(MODEL_B, "unloaded")
    assert _device_arg(MODEL_B) == "CUDA0"
    # a model from the preset file is never touched
    assert _model_status("preset-tiny") == "loaded"
    assert _device_arg("preset-tiny") is None


def test_admin_default_device_saved_when_reload_fails(tmp_path):
    """the model list cannot be rebuilt (broken preset file): the setting is saved all the same and the answer says so"""
    global server
    preset_path = tmp_path / "models.ini"
    preset = _preset_section("preset-tiny", _make_preset_model(tmp_path))
    _write_text(preset_path, preset)
    _enable_admin()
    server.models_max = 2
    server.models_preset = str(preset_path)
    server.start()

    _write_text(preset_path, preset + "no-such-option = 1\n")
    res = _admin("POST", "/admin/settings", data={"models_max": 3, "default_device": "CUDA1"})
    assert res.status_code == 200
    assert isinstance(res.body["reload_error"], str) and res.body["reload_error"] != ""
    assert res.body["models_max"] == 3
    assert res.body["default_device"] == "CUDA1"
    assert json.loads(_read_text(_state_file()))["settings"] == {"models_max": 3, "default_device": "CUDA1"}
    # the previous model list stays in use
    assert _props()["max_instances"] == 3
    assert _device_arg(MODEL_B) is None
    assert "preset-tiny" in _models()

    # a change that needs no reload does not mention it
    assert "reload_error" not in _post_settings({"models_max": 4})

    # the device arrives with the next reload that works
    _write_text(preset_path, preset)
    _models(reload=True)
    assert _device_arg(MODEL_B) == "CUDA1"
    assert _device_arg("preset-tiny") is None


def test_admin_model_devices_lock(tmp_path):
    """MODEL_DEVICES with an entry for * decides the device of every model: the setting is locked"""
    global server
    preset = _preset_section("preset-tiny", _make_preset_model(tmp_path))
    _enable_admin(MODEL_DEVICES="preset-tiny=CUDA1, *=CUDA0;CUDA1", PROVISION_ADMIN_DEFAULT_DEVICE="CUDA5")
    server.models_max = 2
    server.models_preset = _write_text(tmp_path / "models.ini", preset)
    server.start()

    body = _settings()
    assert body["default_device_locked"] is True
    assert body["default_device"] == "CUDA0,CUDA1"
    assert "CUDA0" in body["known_devices"] and "CUDA1" in body["known_devices"]
    # the device comes from the preset file that the deployment writes, the overlay is off
    assert _device_arg(MODEL_B) is None

    for value in ("CUDA1", "CUDA5", "none", ""):
        _assert_error(_admin("POST", "/admin/settings", data={"default_device": value}), 409, "default_device_locked")
    # nothing of a rejected request is applied
    _assert_error(_admin("POST", "/admin/settings", data={"models_max": 4, "default_device": "CUDA1"}), 409, "default_device_locked")
    assert _settings() == body
    assert not os.path.exists(_state_file())

    # the other settings still work, also when the client sends the locked value back
    saved = _post_settings({"models_max": 3, "pinned": [MODEL_B], "default_device": body["default_device"]})
    assert saved["models_max"] == 3
    assert saved["pinned"] == [MODEL_B]
    assert saved["default_device"] == "CUDA0,CUDA1"
    assert saved["default_device_locked"] is True
    assert _device_arg(MODEL_B) is None


def test_admin_model_devices_without_star_does_not_lock():
    global server
    _enable_admin(MODEL_DEVICES="some-model=CUDA1,other-model=CUDA0;CUDA1")
    server.start()

    body = _settings()
    assert body["default_device_locked"] is False
    assert body["default_device"] == ""
    assert _post_settings({"default_device": "CUDA1"})["default_device"] == "CUDA1"
    assert _device_arg(MODEL_B) == "CUDA1"


#
# storage
#


def _assert_storage_dir(entry: dict, kind: str, path: str) -> None:
    assert entry["kind"] == kind
    assert _same_path(entry["path"], path)
    assert entry["exists"] is True
    assert entry["writable"] is True
    assert isinstance(entry["total_bytes"], int) and isinstance(entry["free_bytes"], int)
    assert 0 < entry["free_bytes"] <= entry["total_bytes"]
    assert entry["total_bytes"] == shutil.disk_usage(path).total


def test_admin_storage(tmp_path, private_cache):
    global server
    blobs_dir = os.path.join(private_cache, "models--acme--tiny-GGUF", "blobs")
    os.makedirs(blobs_dir)
    partial = os.path.join(blobs_dir, "0" * 64 + ".downloadInProgress")
    with open(partial, "wb") as f:
        f.write(b"x" * 123)
    with open(os.path.join(blobs_dir, "1" * 64), "wb") as f:
        f.write(b"x" * 456)
    _enable_admin()
    server.models_dir = _make_models_dir(tmp_path)
    server.start()

    res = _admin("GET", "/admin/storage")
    assert res.status_code == 200
    body = res.body
    assert [d["kind"] for d in body["dirs"]] == ["cache", "models_dir", "state"]
    _assert_storage_dir(body["dirs"][0], "cache", private_cache)
    _assert_storage_dir(body["dirs"][1], "models_dir", server.models_dir)
    _assert_storage_dir(body["dirs"][2], "state", state_dir)

    # unfinished downloads in the cache
    assert len(body["partial_downloads"]) == 1
    assert _same_path(body["partial_downloads"][0]["path"], partial)
    assert body["partial_downloads"][0]["bytes"] == 123

    os.remove(partial)
    assert _admin("GET", "/admin/storage").body["partial_downloads"] == []


def test_admin_storage_without_models_dir():
    global server
    _enable_admin()
    server.start()

    body = _admin("GET", "/admin/storage").body
    assert [d["kind"] for d in body["dirs"]] == ["cache", "state"]
    _assert_storage_dir(body["dirs"][0], "cache", SHARED_CACHE_DIR)
    _assert_storage_dir(body["dirs"][1], "state", state_dir)
    assert isinstance(body["partial_downloads"], list)

    dirs = _admin_status()["dirs"]
    assert _same_path(dirs["cache"], SHARED_CACHE_DIR)
    assert dirs["models_dir"] == ""
    assert dirs["preset"] == ""


#
# download pre-flight check
#


def _check(model: str, timeout: float = 60) -> dict:
    res = _admin("POST", "/admin/models/download/check", data={"model": model}, timeout=timeout)
    assert res.status_code == 200, f"check of {model} failed: {res.body}"
    body = res.body
    assert body["model"] == model
    assert body["repo"] == model.split(":")[0]
    assert body["quant"] == (model.split(":")[1] if ":" in model else "")
    for name in ("hf_reachable", "exists", "already_present", "enough_space"):
        assert isinstance(body[name], bool)
    # the parts of the answer that follow from each other
    assert body["total_bytes"] == sum(f["bytes"] for f in body["files"])
    assert body["enough_space"] == (body["total_bytes"] <= body["free_bytes"])
    assert ("not_enough_space" in body["warnings"]) == (not body["enough_space"])
    assert ("already_present" in body["warnings"]) == body["already_present"]
    # a preset repository is fetched as a whole, whatever the quant
    assert ("quant_not_specified" in body["warnings"]) == (":" not in model and "preset_repository" not in body["warnings"])
    assert len(set(body["warnings"])) == len(body["warnings"])
    return body


def _check_files(body: dict) -> dict[str, int]:
    return {f["name"]: f["bytes"] for f in body["files"]}


INVALID_DOWNLOAD_CHECKS = [
    pytest.param({}, id="no_model"),
    pytest.param({"model": None}, id="model_null"),
    pytest.param({"model": 5}, id="model_number"),
    pytest.param({"model": ["user/repo"]}, id="model_array"),
    pytest.param({"model": ""}, id="model_empty"),
    pytest.param({"model": "repo"}, id="no_user"),
    pytest.param({"model": "user/"}, id="no_repo"),
    pytest.param({"model": "/repo"}, id="empty_user"),
    pytest.param({"model": "user/repo/file.gguf"}, id="three_parts"),
    pytest.param({"model": "user/repo:"}, id="empty_quant"),
    pytest.param({"model": "user/repo:Q4_K_M:x"}, id="two_quants"),
    pytest.param({"model": "user/repo:Q4 K"}, id="quant_with_space"),
    pytest.param({"model": "us er/repo"}, id="user_with_space"),
    pytest.param({"model": "-user/repo"}, id="user_starts_with_dash"),
    pytest.param({"model": "user/.repo"}, id="repo_starts_with_dot"),
    pytest.param({"model": "user/repo?x=1"}, id="query_string"),
    pytest.param({"model": "../../etc/passwd"}, id="path_traversal"),
    pytest.param({"model": "https://huggingface.co/user/repo"}, id="url"),
    pytest.param({"model": "user/repo\n"}, id="trailing_newline"),
    pytest.param({"model": "us\u00e9r/repo"}, id="non_ascii"),
    pytest.param({"model": " user/repo"}, id="leading_space"),
    pytest.param("[]", id="body_array"),
    pytest.param('{"model": ', id="body_broken_json"),
    pytest.param("", id="body_empty"),
]


@pytest.mark.parametrize("body", INVALID_DOWNLOAD_CHECKS)
def test_admin_download_check_invalid(hub, body):
    global server
    _enable_admin()
    server.start()

    if isinstance(body, str):
        res = _post_raw("/admin/models/download/check", body)
    else:
        res = _admin("POST", "/admin/models/download/check", data=body)
    _assert_error(res, 400, "invalid_request_error")
    # a malformed name is never sent anywhere
    assert hub.requests == []


def test_admin_download_check_offline(hub):
    """a router in --offline mode makes no request at all"""
    global server
    _enable_admin()
    server.offline = True
    server.start()

    body = _check("acme/tiny-GGUF:Q8_0")
    assert body["hf_reachable"] is False
    assert body["exists"] is False
    assert body["already_present"] is False
    assert body["files"] == []
    assert body["total_bytes"] == 0
    assert body["free_bytes"] > 0
    assert body["warnings"] == ["offline_mode"]
    assert hub.requests == []
    assert _admin_status()["download"] == {"available": False, "offline": True}


def test_admin_download_check_offline_model_in_cache():
    global server
    _enable_admin()
    server.start()

    body = _check(MODEL_B)
    assert body["already_present"] is True
    assert set(body["warnings"]) == {"offline_mode", "already_present"}


def test_admin_download_check_file_selection(hub, private_cache):
    global server
    hub.repos["acme/multi-GGUF"] = [
        (".gitattributes", 1519),
        ("README.md", 2048),
        ("multi-F16.gguf", 1600),
        ("multi-Q8_0.gguf", 800),
        ("multi-Q4_K_M.gguf", 400),
        ("mmproj-multi-F16.gguf", 50),
    ]
    hub.repos["acme/q8-GGUF"] = [("q8-F16.gguf", 1600), ("q8-Q8_0.gguf", 800), ("q8-Q5_K_M.gguf", 500)]
    hub.repos["acme/f16-GGUF"] = [("README.md", 2048), ("f16-BF16.gguf", 1601), ("f16-F16.gguf", 1600)]
    hub.repos["acme/split-GGUF"] = [
        ("split-Q4_K_M-00001-of-00003.gguf", 10),
        ("split-Q4_K_M-00002-of-00003.gguf", 20),
        ("split-Q4_K_M-00003-of-00003.gguf", 30),
        ("split-Q8_0-00001-of-00002.gguf", 100),
        ("split-Q8_0-00002-of-00002.gguf", 200),
    ]
    hub.repos["acme/no-gguf"] = [("README.md", 2048), ("model.safetensors", 4096)]
    _enable_admin()
    server.start()

    # the quant that was asked for, and the mmproj that comes with the model
    body = _check("acme/multi-GGUF:Q8_0")
    assert body["hf_reachable"] is True
    assert body["exists"] is True
    assert body["already_present"] is False
    assert _check_files(body) == {"multi-Q8_0.gguf": 800, "mmproj-multi-F16.gguf": 50}
    assert body["total_bytes"] == 850
    # free space of the filesystem that holds the cache
    assert abs(body["free_bytes"] - shutil.disk_usage(private_cache).free) < 1024 ** 3
    assert body["enough_space"] is True
    assert body["warnings"] == []

    # the quant is matched without regard to case
    assert _check_files(_check("acme/multi-GGUF:q8_0")) == {"multi-Q8_0.gguf": 800, "mmproj-multi-F16.gguf": 50}
    assert _check_files(_check("acme/multi-GGUF:f16")) == {"multi-F16.gguf": 1600, "mmproj-multi-F16.gguf": 50}

    # no quant given: Q4_K_M, else Q8_0, else the first GGUF file; the answer names the pick
    body = _check("acme/multi-GGUF")
    assert _check_files(body) == {"multi-Q4_K_M.gguf": 400, "mmproj-multi-F16.gguf": 50}
    assert body["warnings"] == ["quant_not_specified"]
    assert body["resolved_quant"] == "Q4_K_M"
    body = _check("acme/q8-GGUF")
    assert _check_files(body) == {"q8-Q8_0.gguf": 800}
    assert body["resolved_quant"] == "Q8_0"
    body = _check("acme/f16-GGUF")
    assert _check_files(body) == {"f16-BF16.gguf": 1601}
    assert body["resolved_quant"] == "BF16"

    # every part of a split model
    body = _check("acme/split-GGUF:Q4_K_M")
    assert _check_files(body) == {
        "split-Q4_K_M-00001-of-00003.gguf": 10,
        "split-Q4_K_M-00002-of-00003.gguf": 20,
        "split-Q4_K_M-00003-of-00003.gguf": 30,
    }
    assert body["total_bytes"] == 60
    assert _check_files(_check("acme/split-GGUF:Q8_0")) == {
        "split-Q8_0-00001-of-00002.gguf": 100,
        "split-Q8_0-00002-of-00002.gguf": 200,
    }

    # the repository is there, the quant is not
    body = _check("acme/multi-GGUF:Q2_K")
    assert body["exists"] is True
    assert body["files"] == []
    assert body["total_bytes"] == 0
    assert body["warnings"] == ["no_gguf_for_quant"]
    body = _check("acme/no-gguf")
    assert body["exists"] is True
    assert body["files"] == []
    assert set(body["warnings"]) == {"quant_not_specified", "no_gguf_for_quant"}

    # no such repository
    body = _check("acme/missing:Q8_0")
    assert body["hf_reachable"] is True
    assert body["exists"] is False
    assert body["files"] == []
    assert body["warnings"] == ["repo_not_found"]

    # the check is advisory: it downloads nothing
    assert hub.file_requests("GET") == []
    assert _models() == {}


def test_admin_download_check_preset_repository(hub):
    """a repository with a preset.ini is a set of presets: that file is all a download fetches"""
    global server
    hub.repos["acme/presets"] = [("README.md", 2048), ("preset.ini", 120), ("tiny-Q8_0.gguf", 800)]
    _enable_admin()
    server.start()

    body = _check("acme/presets:Q8_0")
    assert body["exists"] is True
    assert _check_files(body) == {"preset.ini": 120}
    assert body["total_bytes"] == 120
    assert body["already_present"] is False
    assert body["resolved_quant"] == ""
    # no model file comes out of such a download, whatever the repository holds besides: the answer warns about it
    assert body["warnings"] == ["preset_repository"]
    # the quant plays no part in what is fetched, so leaving it out is no problem either
    body = _check("acme/presets")
    assert _check_files(body) == {"preset.ini": 120}
    assert body["warnings"] == ["preset_repository"]
    # access is checked on the file the download would fetch
    assert hub.file_requests("HEAD") == ["preset.ini", "preset.ini"]
    assert hub.file_requests("GET") == []

    # a gated preset repository
    hub.repos["acme/gated-presets"] = [("preset.ini", 120), ("tiny-Q8_0.gguf", 800)]
    hub.file_status["acme/gated-presets"] = 403
    body = _check("acme/gated-presets:Q8_0")
    assert body["exists"] is False
    assert body["files"] == []
    assert body["warnings"] == ["repo_not_found"]


def test_admin_download_check_listing_problems(hub):
    """the repository is there, but its file list cannot be had: the size is unknown, nothing is made up"""
    global server
    files: list[tuple[str, int | None]] = [("tiny-Q8_0.gguf", 800)]
    tree_path = "/api/models/%s/tree/" + hub.COMMIT + "?recursive=true"
    hub.repos["acme/no-branch"] = files
    hub.responses["/api/models/acme/no-branch/refs"] = (200, {"branches": [], "converts": [], "tags": []})
    hub.repos["acme/odd-tree"] = files
    hub.responses[tree_path % "acme/odd-tree"] = (200, {"error": "not a list"})
    hub.repos["acme/tree-down"] = files
    hub.responses[tree_path % "acme/tree-down"] = (503, {"error": "Service Unavailable"})
    hub.repos["acme/tree-gone"] = files
    hub.responses[tree_path % "acme/tree-gone"] = (404, {"error": "Not found"})
    _enable_admin()
    server.start()

    for repo in ("acme/no-branch", "acme/odd-tree"):
        body = _check(f"{repo}:Q8_0")
        assert body["hf_reachable"] is True
        assert body["exists"] is True
        assert body["files"] == []
        assert body["total_bytes"] == 0
        assert body["warnings"] == ["size_unknown"]

    body = _check("acme/tree-down:Q8_0")
    assert body["hf_reachable"] is False
    assert body["files"] == []
    assert "hf_unreachable" in body["warnings"]

    body = _check("acme/tree-gone:Q8_0")
    assert body["hf_reachable"] is True
    assert body["exists"] is False
    assert body["files"] == []
    assert body["warnings"] == ["repo_not_found"]

    assert hub.file_requests("GET") == []
    assert hub.file_requests("HEAD") == []


def test_admin_download_check_space_and_unknown_size(hub):
    global server
    huge = 10 ** 18  # no disk has an exabyte free
    hub.repos["acme/huge-GGUF"] = [("huge-Q8_0.gguf", huge), ("mmproj-huge-F16.gguf", 1000)]
    hub.repos["acme/nosize-GGUF"] = [("nosize-Q8_0.gguf", None)]
    _enable_admin()
    server.start()

    # a warning only, there is no quota
    body = _check("acme/huge-GGUF:Q8_0")
    assert body["total_bytes"] == huge + 1000
    assert 0 < body["free_bytes"] < huge
    assert body["enough_space"] is False
    assert body["warnings"] == ["not_enough_space"]

    body = _check("acme/nosize-GGUF:Q8_0")
    assert _check_files(body) == {"nosize-Q8_0.gguf": 0}
    assert body["enough_space"] is True
    assert body["warnings"] == ["size_unknown"]


def test_admin_download_check_endpoint_errors(hub):
    global server
    files: list[tuple[str, int | None]] = [("tiny-Q8_0.gguf", 800)]
    for status in (401, 403, 404, 429, 500, 503):
        hub.repos[f"acme/refs-{status}"] = files
        hub.refs_status[f"acme/refs-{status}"] = status
    hub.repos["acme/gated-GGUF"] = files
    hub.file_status["acme/gated-GGUF"] = 403
    hub.repos["acme/login-GGUF"] = files
    hub.file_status["acme/login-GGUF"] = 401
    _enable_admin()
    server.start()

    # private, gated without access or missing: all the same for the caller
    for status in (401, 403, 404):
        body = _check(f"acme/refs-{status}:Q8_0")
        assert body["hf_reachable"] is True
        assert body["exists"] is False
        assert body["files"] == []
        assert body["warnings"] == ["repo_not_found"]

    # the endpoint does not answer properly
    for status in (429, 500, 503):
        body = _check(f"acme/refs-{status}:Q8_0")
        assert body["hf_reachable"] is False
        assert body["exists"] is False
        assert body["files"] == []
        assert body["warnings"] == ["hf_unreachable"]

    # a gated repository lists its files to everybody, but does not hand them out
    assert hub.file_requests("HEAD") == []
    for repo in ("acme/gated-GGUF", "acme/login-GGUF"):
        body = _check(f"{repo}:Q8_0")
        assert body["hf_reachable"] is True
        assert body["exists"] is False
        assert body["files"] == []
        assert body["warnings"] == ["repo_not_found"]
    assert hub.file_requests("HEAD") == ["tiny-Q8_0.gguf", "tiny-Q8_0.gguf"]
    assert hub.file_requests("GET") == []


def test_admin_download_check_endpoint_down(hub):
    global server
    hub.close()  # nothing listens on the endpoint address anymore
    _enable_admin()
    server.start()

    started = time.time()
    body = _check("acme/tiny-GGUF:Q8_0")
    assert time.time() - started < 12
    assert body["hf_reachable"] is False
    assert body["exists"] is False
    assert body["files"] == []
    assert body["warnings"] == ["hf_unreachable"]
    assert body["free_bytes"] > 0


def test_admin_download_check_endpoint_hangs(hub):
    """the check gives up within its network budget (10 seconds at most)"""
    global server
    hub.hang = True
    _enable_admin()
    server.start()

    started = time.time()
    check = _start_request(lambda: _check("acme/tiny-GGUF:Q8_0"))
    _wait_until(lambda: hub.requests != [], 10, "the check to reach the endpoint")
    # while the check waits for the endpoint, the router and the admin routes are not blocked (no admin lock is held)
    res = _admin("GET", "/admin/settings", timeout=3)
    assert res.status_code == 200
    assert _admin("GET", "/admin/status", timeout=3).body["authorized"] is True
    assert not check.done(), "the check did not wait for the endpoint"

    body = check.result(timeout=30)
    elapsed = time.time() - started
    assert elapsed < 12, f"the check took {elapsed:.1f} seconds"
    assert body["hf_reachable"] is False
    assert body["warnings"] == ["hf_unreachable"]


def test_admin_download_check_token_and_redirects(hub):
    """the HF token of the router goes to the configured endpoint and nowhere else"""
    global server
    token = "hf_" + "a1B2c3D4e5" * 4
    other = _FakeHub()
    try:
        hub.repos["acme/tiny-GGUF"] = [("tiny-Q8_0.gguf", 800)]
        # a renamed repository: the endpoint redirects to itself
        hub.redirects["/api/models/acme/old-name/refs"] = "/api/models/acme/tiny-GGUF/refs"
        hub.redirects[f"/api/models/acme/old-name/tree/{hub.COMMIT}?recursive=true"] = f"/api/models/acme/tiny-GGUF/tree/{hub.COMMIT}?recursive=true"
        # a redirect to somewhere else, written in full and relative to the scheme
        other.repos["acme/elsewhere"] = [("elsewhere-Q8_0.gguf", 800)]
        hub.redirects["/api/models/acme/elsewhere/refs"] = other.url + "/api/models/acme/elsewhere/refs"
        hub.redirects["/api/models/acme/elsewhere-too/refs"] = other.url.removeprefix("http:") + "/api/models/acme/elsewhere/refs"
        env.setenv("HF_TOKEN", token)
        _enable_admin()
        server.start()

        body = _check("acme/tiny-GGUF:Q8_0")
        assert _check_files(body) == {"tiny-Q8_0.gguf": 800}
        assert len(hub.requests) == 3  # refs, tree, HEAD of the model file
        for method, path, headers in hub.requests:
            assert headers.get("authorization") == f"Bearer {token}", f"no token on {method} {path}"

        # followed on the same origin
        body = _check("acme/old-name:Q8_0")
        assert body["exists"] is True
        assert _check_files(body) == {"tiny-Q8_0.gguf": 800}

        # never followed to another origin
        for repo in ("acme/elsewhere", "acme/elsewhere-too"):
            body = _check(f"{repo}:Q8_0")
            assert body["exists"] is False
            assert body["files"] == []

        # an '@' in a file name or in a redirect target is no user name in front of another host
        other_port = other.httpd.server_address[1]
        hub.repos["acme/at-GGUF"] = [(f"x@127.0.0.1:{other_port}/at-Q8_0.gguf", 800)]
        body = _check("acme/at-GGUF:Q8_0")
        assert _check_files(body) == {f"x@127.0.0.1:{other_port}/at-Q8_0.gguf": 800}
        assert hub.file_requests("HEAD")[-1] == f"x%40127.0.0.1%3A{other_port}/at-Q8_0.gguf"
        hub.repos["acme/at-redirect"] = [("tiny-Q8_0.gguf", 800)]
        hub.redirects["/api/models/acme/at-redirect/refs"] = f"/x@127.0.0.1:{other_port}/stolen"
        body = _check("acme/at-redirect:Q8_0")
        assert body["exists"] is False
        assert body["files"] == []

        assert other.requests == []
    finally:
        server.stop()
        other.close()


def test_admin_download_check_matches_download(hub):
    """the files the check announces are the files the download fetches"""
    global server
    model = "acme/tiny-GGUF:Q8_0"
    hub.repos["acme/tiny-GGUF"] = [
        ("README.md", 2048),
        ("tiny-Q4_K_M.gguf", 30000),
        ("tiny-Q8_0.gguf", 50000),
        ("mmproj-tiny-F16.gguf", 7000),
    ]
    _enable_admin()
    server.start()

    status = _admin_status()
    assert status["download"]["offline"] is False
    assert status["models"]["total"] == 0
    assert _post_settings({"default_device": "CUDA1"})["default_device"] == "CUDA1"

    before = _check(model)
    assert before["already_present"] is False
    assert _check_files(before) == {"tiny-Q8_0.gguf": 50000, "mmproj-tiny-F16.gguf": 7000}
    assert hub.file_requests("GET") == []

    # download through the existing route
    events, stop = _start_sse_listener()
    res = server.make_request("POST", "/models", data={"model": model}, timeout=60)
    assert res.status_code == 200
    assert res.body.get("success") is True
    _wait_until(lambda: _has_sse_event(events, "download_finished", model) or _has_sse_event(events, "download_failed", model),
                60, "the download to end")
    stop.set()
    assert _has_sse_event(events, "download_finished", model)

    assert sorted(hub.file_requests("GET")) == sorted(_check_files(before))
    listed = _models()
    assert listed[model]["source"] == "cache"
    # the model that just arrived gets the device for new models
    assert _device_arg(model) == "CUDA1"

    after = _check(model)
    assert after["already_present"] is True
    assert after["warnings"] == ["already_present"]
    assert _check_files(after) == _check_files(before)
    # another quant of the same repository is not
    other = _check("acme/tiny-GGUF:Q4_K_M")
    assert other["already_present"] is False
    assert _check_files(other) == {"tiny-Q4_K_M.gguf": 30000, "mmproj-tiny-F16.gguf": 7000}

    # the new model is known to the admin layer as well, and nothing unfinished is left behind
    assert model in {m["id"] for m in _settings()["models"]}
    assert _admin_status()["models"]["total"] == 1
    assert _admin("GET", "/admin/storage").body["partial_downloads"] == []
    assert _post_settings({"models_max": 2, "pinned": [model]})["pinned"] == [model]

    # deleted through the existing route: gone for the admin layer, too
    res = server.make_request("DELETE", f"/models?model={model}", timeout=60)
    assert res.status_code == 200
    assert res.body.get("success") is True
    assert model not in _models()
    body = _settings()
    assert body["models"] == []
    assert body["pinned"] == []
    assert body["pinned_unknown"] == [model]
    assert _admin_status()["models"]["total"] == 0
    again = _check(model)
    assert again["already_present"] is False
    assert _check_files(again) == _check_files(before)


@pytest.mark.slow
@pytest.mark.skipif(not is_slow_test_allowed(), reason="skipping slow test")
def test_admin_download_check_real_endpoint(private_cache):
    """the same check against the real Hugging Face endpoint (needs network access)"""
    global server
    _enable_admin()
    server.offline = False
    server.start()

    status = _admin_status()
    assert status["download"] == {"available": True, "offline": False}

    # an existing repository and quant
    body = _check("ggml-org/test-model-stories260K:F32", timeout=30)
    assert body["hf_reachable"] is True
    assert body["exists"] is True
    assert body["already_present"] is False
    assert len(body["files"]) == 1
    assert body["files"][0]["name"].lower().endswith("f32.gguf")
    assert 100_000 < body["total_bytes"] < 100_000_000
    assert body["free_bytes"] > 0
    assert body["enough_space"] is True
    assert body["warnings"] == []

    # the mmproj file is part of what gets downloaded
    body = _check("ggml-org/tinygemma3-GGUF:Q8_0", timeout=30)
    assert sorted(_check_files(body)) == ["mmproj-tinygemma3.gguf", "tinygemma3-Q8_0.gguf"]

    # no quant given
    body = _check("ggml-org/test-model-stories260K", timeout=30)
    assert body["exists"] is True
    assert len(body["files"]) == 1
    assert body["warnings"] == ["quant_not_specified"]

    # a missing quant
    body = _check("ggml-org/test-model-stories260K:Q2_K", timeout=30)
    assert body["hf_reachable"] is True
    assert body["exists"] is True
    assert body["files"] == []
    assert body["total_bytes"] == 0
    assert body["warnings"] == ["no_gguf_for_quant"]

    # a missing repository
    body = _check("ggml-org/this-repository-does-not-exist-0a1b2c3d:Q4_K_M", timeout=30)
    assert body["hf_reachable"] is True
    assert body["exists"] is False
    assert body["files"] == []
    assert body["warnings"] == ["repo_not_found"]


@pytest.mark.slow
@pytest.mark.skipif(not is_slow_test_allowed(), reason="skipping slow test")
def test_admin_download_check_real_endpoint_model_in_cache():
    """a model that the real downloader put into the cache is recognised (needs network access)"""
    global server
    _enable_admin()
    server.offline = False
    server.start()

    body = _check(MODEL_B, timeout=30)
    assert body["hf_reachable"] is True
    assert body["exists"] is True
    assert body["already_present"] is True
    assert len(body["files"]) == 1
    assert body["warnings"] == ["already_present"]


#
# the two hang fixes (they do not depend on the admin layer being enabled)
#


@pytest.mark.parametrize("admin", [False, True])
def test_admin_failed_reload_does_not_block_loading(tmp_path, admin: bool):
    """a reload that throws must not leave the router in the reloading state: every later load would wait forever"""
    global server
    preset_path = tmp_path / "models.ini"
    model_path = _make_preset_model(tmp_path)
    _write_text(preset_path, _preset_section("preset-tiny", model_path))
    if admin:
        _enable_admin()
    server.models_max = 2
    server.models_dir = _make_models_dir(tmp_path, "local-a")
    server.models_preset = str(preset_path)
    server.start()
    assert set(_models(reload=True)) >= {"preset-tiny", "local-a"}

    # the preset model takes the alias "local-z" while a new model of that name shows up in the models directory:
    # the reload fails half way through
    conflict = os.path.join(server.models_dir, "local-z.gguf")
    _write_text(preset_path, _preset_section("preset-tiny", model_path, "alias = local-z"))
    shutil.copyfile(_tiny_gguf(), conflict)
    events, stop = _start_sse_listener()
    res = server.make_request("GET", "/models?reload=1", timeout=30)
    assert res.status_code == 500
    assert "error" in res.body
    # what the reload changed before it failed stays: clients are told to fetch the list again
    _wait_until(lambda: _has_sse_event(events, "models_reload", "*"), 10, "the models_reload event")
    stop.set()

    # loading still works, explicitly and on demand (a timeout here is the hang)
    res = server.make_request("POST", "/models/load", data={"model": "local-a"}, timeout=30)
    assert res.status_code == 200
    _wait_for_model_status("local-a", "loaded")
    res = server.make_request("POST", "/tokenize", data={"model": "preset-tiny", "content": "hello"}, timeout=60)
    assert res.status_code == 200

    # and so does the next reload, once the configuration is repaired
    os.remove(conflict)
    _write_text(preset_path, _preset_section("preset-tiny", model_path))
    listed = _models(reload=True)
    assert "local-z" not in listed
    assert listed["local-a"]["status"]["value"] == "loaded"


def test_admin_failed_lazy_reload_does_not_fail_requests(hub, tmp_path):
    """the reload that follows a download runs inside the next request, whatever it is: its failure is not that request's"""
    global server
    model = "acme/tiny-GGUF:Q8_0"
    hub.repos["acme/tiny-GGUF"] = [("tiny-Q8_0.gguf", 50000)]
    preset_path = tmp_path / "models.ini"
    preset = _preset_section("preset-tiny", _make_preset_model(tmp_path))
    _write_text(preset_path, preset)
    server.models_max = 2
    server.models_preset = str(preset_path)
    server.start()

    # the preset file breaks while the router is running, then a download ends and asks for a reload
    _write_text(preset_path, preset + "no-such-option = 1\n")
    events, stop = _start_sse_listener()
    res = server.make_request("POST", "/models", data={"model": model}, timeout=60)
    assert res.status_code == 200
    _wait_until(lambda: _has_sse_event(events, "download_finished", model) or _has_sse_event(events, "download_failed", model),
                60, "the download to end")
    stop.set()
    assert _has_sse_event(events, "download_finished", model)

    # unrelated requests are served from the previous list
    res = server.make_request("POST", "/models/load", data={"model": "preset-tiny"}, timeout=30)
    assert res.status_code == 200, f"an unrelated request failed: {res.body}"
    _wait_for_model_status("preset-tiny", "loaded")
    for _ in range(2):
        res = server.make_request("GET", "/models", timeout=30)
        assert res.status_code == 200
        assert "preset-tiny" in {m["id"] for m in res.body["data"]}

    # the explicit reload still reports the problem ...
    res = server.make_request("GET", "/models?reload=1", timeout=30)
    assert res.status_code >= 400
    # ... and picks up the downloaded model once the file is repaired
    _write_text(preset_path, preset)
    listed = _models(reload=True)
    assert listed[model]["source"] == "cache"
    assert listed["preset-tiny"]["status"]["value"] == "loaded"


@pytest.mark.parametrize("admin", [False, True])
def test_admin_killed_download_is_reported_as_failed(hub, admin: bool):
    """a download child that dies without reporting a result must not stay "downloading" forever"""
    global server
    model = "acme/tiny-GGUF:Q8_0"
    hub.repos["acme/tiny-GGUF"] = [("tiny-Q8_0.gguf", 4_000_000)]
    hub.stall_files = True
    if admin:
        _enable_admin()
    server.start()

    events, stop = _start_sse_listener()
    res = server.make_request("POST", "/models", data={"model": model}, timeout=60)
    assert res.status_code == 200
    # the child is in the middle of the file now, and stays there
    _wait_until(lambda: hub.file_requests("GET") != [], 60, "the download to start")
    assert _model_status(model) == "downloading"

    if admin:
        # the unfinished file is visible, the placeholder of the download is not counted as a model
        partial = _admin("GET", "/admin/storage").body["partial_downloads"]
        assert len(partial) == 1
        assert partial[0]["path"].endswith(".downloadInProgress")
        assert _settings()["models"] == []
        assert _admin_status()["models"]["total"] == 0

    assert server.process is not None
    children = _child_pids(server.process.pid)
    if not children:
        pytest.skip("cannot list the child processes on this platform")  # ty: ignore[too-many-positional-arguments]
    for pid in children:
        os.kill(pid, getattr(signal, "SIGKILL", signal.SIGTERM))

    _wait_until(lambda: _has_sse_event(events, "download_failed", model), 30, "the download_failed event")
    stop.set()

    # none of these may wait for the dead download (a timeout here is the hang)
    res = server.make_request("GET", "/models", timeout=30)
    assert res.status_code == 200
    assert all(m["status"]["value"] != "downloading" for m in res.body["data"])
    res = server.make_request("POST", "/models/unload", data={"model": model}, timeout=30)
    assert res.status_code in (200, 400)
    res = server.make_request("POST", "/models", data={"model": model}, timeout=60)
    assert res.status_code == 200, f"the download could not be started again: {res.body}"
    assert _model_status(model) == "downloading"


#
# the existing routes with the admin layer enabled
#


def _walk_existing_routes(model_a: str, model_b: str, model_c: str) -> list:
    """one pass over the model routes of the router. Whatever differs between two runs by nature is left out."""
    seen: list = []

    def note(label: str, res: ServerResponse, keep=lambda body: body) -> None:
        seen.append((label, res.status_code, keep(res.body)))

    def listing(body: dict) -> list:
        return sorted(
            (m["id"], m["source"], m["status"]["value"], m["can_remove"], m["status"]["args"][1:], sorted(m))
            for m in body["data"]
        )

    def statuses(body: dict) -> list:
        return sorted((m["id"], m["status"]["value"]) for m in body["data"])

    note("GET /models", server.make_request("GET", "/models"), listing)
    note("GET /v1/models", server.make_request("GET", "/v1/models"), listing)
    note("GET /props", server.make_request("GET", "/props"))
    note("load unknown", server.make_request("POST", "/models/load", data={"model": "no-such-model"}))
    note("unload unknown", server.make_request("POST", "/models/unload", data={"model": "no-such-model"}))
    note("unload not running", server.make_request("POST", "/models/unload", data={"model": model_b}))
    note("delete unknown", server.make_request("DELETE", "/models?model=no-such-model"))
    note("download without a name", server.make_request("POST", "/models", data={"model": ""}))

    note("load B", server.make_request("POST", "/models/load", data={"model": model_b}))
    _wait_for_model_status(model_b, "loaded")
    note("load B again", server.make_request("POST", "/models/load", data={"model": model_b}))
    note("tokenize B", _tokenize(model_b))
    note("load C", server.make_request("POST", "/models/load", data={"model": model_c}))
    _wait_for_model_status(model_c, "loaded")
    note("GET /models, two loaded", server.make_request("GET", "/models"), statuses)

    # models_max is 2: the least recently used model goes
    note("load A", server.make_request("POST", "/models/load", data={"model": model_a}))
    _wait_for_model_status(model_a, "loaded", timeout=120)
    note("GET /models, after eviction", server.make_request("GET", "/models"), statuses)

    note("tokenize C (on demand)", _tokenize(model_c))
    note("unload C", server.make_request("POST", "/models/unload", data={"model": model_c}))
    _wait_for_model_status(model_c, "unloaded")
    note("GET /models?reload=1", server.make_request("GET", "/models?reload=1"), statuses)
    note("GET /v1/models?reload=1", server.make_request("GET", "/v1/models?reload=1"), statuses)
    return seen


def test_admin_enabled_existing_routes_unchanged(tmp_path, private_cache):
    """the same requests against a router without and with the admin layer give the same answers"""
    global server
    # whole listings are compared: the models come from a directory of this test and the cache is empty,
    # so nothing another test downloads or deletes meanwhile shows up
    server.models_max = 2
    server.models_dir = _make_models_dir(tmp_path, "m-a", "m-b", "m-c")
    server.start()
    without_admin = _walk_existing_routes("m-a", "m-b", "m-c")
    server.stop()

    _enable_admin()
    server.start()
    assert _admin("GET", "/admin/status").body["authorized"] is True
    with_admin = _walk_existing_routes("m-a", "m-b", "m-c")

    for expected, actual in zip(without_admin, with_admin):
        assert actual == expected
    assert len(with_admin) == len(without_admin)

    # sanity: the walk saw what it was meant to see
    seen = {label: (status, body) for label, status, body in with_admin}
    assert seen["GET /models"][0] == 200
    assert seen["load unknown"][0] == 404
    assert seen["unload not running"][0] == 400
    assert seen["load B"] == (200, {"success": True})
    assert seen["load B again"][0] == 400
    assert seen["tokenize B"][0] == 200
    assert dict(seen["GET /models, two loaded"][1])["m-b"] == "loaded"
    assert dict(seen["GET /models, after eviction"][1])["m-a"] == "loaded"
    assert dict(seen["GET /models, after eviction"][1])["m-b"] == "unloaded"
    assert seen["unload C"] == (200, {"success": True})


def test_admin_enabled_existing_routes_need_no_admin_key():
    """loading, unloading and listing stay public by design, only /admin is protected by the admin key"""
    global server
    _enable_admin()
    server.models_max = 2
    server.start()

    for path in ("/models", "/v1/models", "/models?reload=1", "/props", "/health"):
        assert server.make_request("GET", path).status_code == 200
        # an admin key, right or wrong, makes no difference there
        assert server.make_request("GET", path, headers={"X-Admin-Key": "wrong-key"}).status_code == 200

    _load_model_and_wait(MODEL_B)
    res = server.make_request("POST", "/models/unload", data={"model": MODEL_B})
    assert res.status_code == 200
    _wait_for_model_status(MODEL_B, "unloaded")
