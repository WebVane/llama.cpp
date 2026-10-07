# tests for the optional address allow-list of the admin layer: PROVISION_ADMIN_ALLOW_FROM and
# PROVISION_ADMIN_TRUSTED_PROXIES (tools/server/server-admin.cpp, docs/provision-admin.md section 4.3).
#
# Every request is made over the loopback interface. The X-Forwarded-For cases use a router that trusts
# 127.0.0.1 as a reverse proxy, so that any client address (IPv4, IPv6, ranges, boundaries) can be
# tested from here. Reading the environment of a model instance needs /proc or psutil; where neither is
# there, that test is skipped.

import http.client
import socket

import pytest
from utils import *

# the helpers of the stage-1 admin tests; they work on the module globals bound in create_server() below
import test_admin as stage1
from test_admin import ADMIN_KEY, MODEL_B

server: ServerProcess
log_path: str

# every route of the admin layer, with a body for the POST routes (a refused request never gets to it)
ADMIN_ROUTES = [
    ("GET", "/admin/status", None),
    ("GET", "/admin/settings", None),
    ("POST", "/admin/settings", {"models_max": 5}),
    ("GET", "/admin/storage", None),
    ("POST", "/admin/models/download/check", {"model": "user/repo"}),
    ("GET", "/admin/models/settings", None),
    ("POST", "/admin/models/settings", {"model": MODEL_B, "settings": {"ctx-size": 512}}),
    ("POST", "/admin/models/settings/check", {"model": MODEL_B, "settings": {}}),
    ("GET", "/admin/models/log?model=" + MODEL_B, None),
    ("GET", "/admin/gpus", None),
    ("POST", "/admin/gpus/rescan", {}),
    ("GET", "/admin/diagnostics", None),
]
PROTECTED_ROUTES = [route for route in ADMIN_ROUTES if route[1] != "/admin/status"]

# existing routes: the allow-list never applies to them
PUBLIC_ROUTES = ["/health", "/models", "/v1/models", "/props"]


@pytest.fixture(autouse=True)
def create_server(monkeypatch, tmp_path):
    global server, log_path
    server = ServerPreset.router()
    log_path = str(tmp_path / "server.log")
    server.log_path = log_path
    stage1.server = server
    stage1.env = monkeypatch
    stage1.state_dir = str(tmp_path / "admin-state")
    # the server inherits os.environ (see ServerProcess.start), so start every test from a clean slate
    for name in list(os.environ):
        if name.upper().startswith("PROVISION_ADMIN") or name.upper() == "MODEL_DEVICES":
            monkeypatch.delenv(name)


def _enable(**variables: str | None) -> None:
    """admin layer on (see test_admin._enable_admin), without the GPU inventory and the vendor tool: not needed here"""
    stage1._enable_admin(PROVISION_ADMIN_INVENTORY="off", PROVISION_ADMIN_GPU_PROBE="off", **variables)


def _request(method: str, path: str, data: dict | None = None, key: str | None = None,
             headers: dict | None = None, host: str | None = None) -> ServerResponse:
    headers = dict(headers or {})
    if key is not None:
        headers["X-Admin-Key"] = key
    return server.make_request(method, path, data=data, headers=headers, timeout=60, host=host)


def _assert_forbidden(res: ServerResponse, seen: str, locked: bool = False) -> None:
    """403 admin_forbidden in the server's error format, the message ends with the address the server saw"""
    stage1._assert_error(res, 403, "admin_forbidden")
    message = res.body["error"]["message"]
    assert message.endswith(" " + seen), f"expected the address {seen} at the end of: {message}"
    assert ("locked" in message) == locked, message
    assert res.headers.get("Cache-Control") == "no-store"


def _diagnostic(item_id: str = "ip_allowlist", headers: dict | None = None, host: str | None = None) -> dict:
    res = _request("GET", "/admin/diagnostics", key=ADMIN_KEY, headers=headers, host=host)
    assert res.status_code == 200, res.body
    items = {item["id"]: item for item in res.body["items"]}
    return items[item_id]


def _log_text() -> str:
    return stage1._read_text(log_path)


def _wait_for_log(predicate, what: str, timeout: float = 10) -> str:
    """the router writes its log from a thread of its own: give a line a moment to arrive"""
    deadline = time.time() + timeout
    while True:
        text = _log_text()
        if predicate(text) or time.time() > deadline:
            assert predicate(text), f"not in the server log: {what}"
            return text
        time.sleep(0.1)


def _refused_lines(text: str) -> list[str]:
    return [line for line in text.splitlines() if "admin: refused" in line]


def _assert_public_routes_work(host: str | None = None) -> None:
    for path in PUBLIC_ROUTES:
        res = server.make_request("GET", path, timeout=60, host=host)
        assert res.status_code == 200, f"GET {path}: {res.status_code} {res.body}"


def _raw_status(headers: list[tuple[str, str]]) -> tuple[int, dict]:
    """GET /admin/status with the headers exactly as given (requests merges header names that differ in case)"""
    conn = http.client.HTTPConnection("127.0.0.1", server.server_port, timeout=30)
    try:
        conn.putrequest("GET", "/admin/status")
        for name, value in headers:
            conn.putheader(name, value)
        conn.endheaders()
        res = conn.getresponse()
        return res.status, json.loads(res.read().decode("utf-8"))
    finally:
        conn.close()


def _has_ipv6_loopback() -> bool:
    try:
        with socket.socket(socket.AF_INET6, socket.SOCK_STREAM) as s:
            s.bind(("::1", 0))
        return True
    except OSError:
        return False


#
# off by default
#


def test_access_off_by_default():
    """without PROVISION_ADMIN_ALLOW_FROM any address may use /admin; X-Forwarded-For changes nothing"""
    _enable()
    server.start()

    for headers in (None, {"X-Forwarded-For": "203.0.113.9"}):
        res = _request("GET", "/admin/status", headers=headers)
        assert res.status_code == 200
        assert res.body == {"admin": True, "authorized": False, "key_configured": True}
        stage1._assert_error(_request("GET", "/admin/settings", headers=headers), 401, "admin_key_required")
        assert _request("GET", "/admin/settings", key=ADMIN_KEY, headers=headers).status_code == 200

    status = _request("GET", "/admin/status", key=ADMIN_KEY).body
    assert status["authorized"] is True and status["ip_allowlist"] is False

    item = _diagnostic(headers={"X-Forwarded-For": "203.0.113.9"})
    assert item["status"] == "info" and "PROVISION_ADMIN_ALLOW_FROM" in item["hint"]
    assert item["value"] == {
        "enabled": False, "entries": [], "trusted_proxies": [],
        "seen_from": "127.0.0.1", "peer": "127.0.0.1", "via_forwarded_for": False, "note": "",
    }
    assert _refused_lines(_log_text()) == []


@pytest.mark.parametrize("allow_from", ["10.0.0.0/8", "localhost"])
def test_access_admin_disabled_ignores_the_allow_list(allow_from: str):
    """without PROVISION_ADMIN the two variables change nothing: no /admin routes, the other routes as always"""
    _enable(PROVISION_ADMIN=None, PROVISION_ADMIN_ALLOW_FROM=allow_from, PROVISION_ADMIN_TRUSTED_PROXIES="127.0.0.1")
    server.start()

    for method, path, data in ADMIN_ROUTES:
        for key in (None, ADMIN_KEY):
            assert _request(method, path, data, key=key).status_code == 404
    _assert_public_routes_work()
    assert not os.path.exists(stage1.state_dir)

    server.stop()
    text = _log_text()
    assert "PROVISION_ADMIN_ALLOW_FROM" not in text and "allow-list" not in text
    assert _refused_lines(text) == []


#
# the allow-list on
#


def test_access_allowed_address_keeps_the_key_check():
    """an allowed address gets the admin API as before: the probe, 401 without the key, 200 with it"""
    # separators and spellings: commas, blanks, empty entries, upper case, host bits, IPv4-mapped, long IPv6 form
    _enable(PROVISION_ADMIN_ALLOW_FROM=" 127.0.0.1 10.64.0.5/24,,FD00:0:0::/8\t::ffff:192.0.2.1, 0:0:0:0:0:0:0:1 ")
    server.start()

    res = _request("GET", "/admin/status")
    assert res.status_code == 200 and res.body == {"admin": True, "authorized": False, "key_configured": True}
    for method, path, data in PROTECTED_ROUTES:
        stage1._assert_error(_request(method, path, data), 401, "admin_key_required")
        stage1._assert_error(_request(method, path, data, key="wrong-key"), 401, "admin_key_required")
    assert _request("GET", "/admin/settings", key=ADMIN_KEY).status_code == 200

    status = _request("GET", "/admin/status", key=ADMIN_KEY).body
    assert status["authorized"] is True and status["ip_allowlist"] is True

    item = _diagnostic()
    # the entry with host bits set is reported, in the log and in Diagnostics
    assert item["status"] == "warning", item
    assert item["hint"] == "PROVISION_ADMIN_ALLOW_FROM: '10.64.0.5/24' has host bits set, it is read as 10.64.0.0/24"
    assert item["value"] == {
        "enabled": True, "entries": ["127.0.0.1", "10.64.0.0/24", "fd00::/8", "192.0.2.1", "::1"], "trusted_proxies": [],
        "seen_from": "127.0.0.1", "peer": "127.0.0.1", "via_forwarded_for": False, "note": "",
    }
    text = _log_text()
    assert "'10.64.0.5/24' has host bits set, it is read as 10.64.0.0/24" in text
    assert "/admin is limited to 127.0.0.1, 10.64.0.0/24, fd00::/8, 192.0.2.1, ::1 (PROVISION_ADMIN_ALLOW_FROM)" in text
    assert _refused_lines(text) == []


def test_access_refused_on_every_admin_route():
    """an address that is not on the list gets 403 admin_forbidden on every /admin route, the probe and a valid key included"""
    _enable(PROVISION_ADMIN_ALLOW_FROM="10.0.0.0/8")
    server.models_max = 2
    server.start()

    n_requests = 0
    for method, path, data in ADMIN_ROUTES:
        for key in (None, "wrong-key", ADMIN_KEY):
            _assert_forbidden(_request(method, path, data, key=key), "127.0.0.1")
            n_requests += 1

    # the existing routes are not affected, and the refused POST /admin/settings changed nothing
    _assert_public_routes_work()
    assert stage1._props()["max_instances"] == 2

    # one warning per refused request, without the key
    text = _wait_for_log(lambda t: len(_refused_lines(t)) >= n_requests, f"{n_requests} refused requests")
    refused = _refused_lines(text)
    assert len(refused) == n_requests
    assert all(" from 127.0.0.1: not on PROVISION_ADMIN_ALLOW_FROM" in line for line in refused)
    assert ADMIN_KEY not in text


def test_access_ipv6_loopback():
    if not _has_ipv6_loopback():
        pytest.skip("no IPv6 loopback on this machine")  # ty: ignore[too-many-positional-arguments]
    server.server_host = "127.0.0.1,::1"
    _enable(PROVISION_ADMIN_ALLOW_FROM="::1")
    server.start()

    res = _request("GET", "/admin/status", host="[::1]")
    assert res.status_code == 200 and res.body["authorized"] is False
    assert _request("GET", "/admin/status", key=ADMIN_KEY, host="[::1]").body["ip_allowlist"] is True
    item = _diagnostic(host="[::1]")
    assert item["value"]["seen_from"] == "::1" and item["value"]["entries"] == ["::1"]

    # 127.0.0.1 is another address
    _assert_forbidden(_request("GET", "/admin/status"), "127.0.0.1")
    _assert_public_routes_work()
    _assert_public_routes_work(host="[::1]")


@pytest.mark.parametrize("allow_from, allowed", [
    ("127.0.0.1", True),
    ("::ffff:127.0.0.0/104", True),
    ("10.0.0.0/8", False),
    ("::1", False),
])
def test_access_ipv4_mapped_peer(allow_from: str, allowed: bool):
    """a dual-stack listener reports an IPv4 client as ::ffff:a.b.c.d: it matches the IPv4 rules, numerically"""
    # an IPv6 socket bound to the IPv4-mapped loopback address takes IPv4 connections to 127.0.0.1
    server.server_host = "::ffff:127.0.0.1"
    server.make_url = lambda path, host=None: f"http://{host or '127.0.0.1'}:{server.server_port}{path}"
    _enable(PROVISION_ADMIN_ALLOW_FROM=allow_from)
    server.start()

    res = _request("GET", "/admin/status")
    if not allowed:
        _assert_forbidden(res, "127.0.0.1")
        return
    assert res.status_code == 200
    value = _diagnostic()["value"]
    assert value["peer"] == "::ffff:127.0.0.1" and value["seen_from"] == "127.0.0.1"


@pytest.mark.parametrize("allow_from, trusted, bad_entry", [
    ("127.0.0.1, localhost", None, "localhost"),
    ("127.0.0.1 10.0.0.0/33", None, "10.0.0.0/33"),
    ("127.0.0.1,127.0.0.01", None, "127.0.0.01"),
    ("127.0.0.1 [::1]", None, "[::1]"),
    ("127.0.0.1 fe80::1%eth0", None, "fe80::1%eth0"),
    ("127.0.0.1 ::1/129", None, "::1/129"),
    ("127.0.0.1 10.0.0.0/", None, "10.0.0.0/"),
    ("127.0.0.1", "127.0.0.1 proxy.example", "proxy.example"),
    # an IPv4-mapped entry with an IPv4-style prefix would be ::/8, every IPv4 address: refused, not widened
    ("127.0.0.1 ::ffff:10.0.0.0/8", None, "::ffff:10.0.0.0/8"),
    ("127.0.0.1", "::ffff:127.0.0.0/95", "::ffff:127.0.0.0/95"),
])
def test_access_invalid_entry_fails_closed(allow_from: str, trusted: str | None, bad_entry: str):
    """an entry the server cannot read locks /admin for everybody, the addresses that are on the list included"""
    _enable(PROVISION_ADMIN_ALLOW_FROM=allow_from, PROVISION_ADMIN_TRUSTED_PROXIES=trusted)
    server.start()

    for method, path, data in ADMIN_ROUTES:
        for key in (None, ADMIN_KEY):
            _assert_forbidden(_request(method, path, data, key=key), "127.0.0.1", locked=True)
    _assert_public_routes_work()

    text = _log_text()
    assert f"invalid entry '{bad_entry}'" in text
    assert "every /admin request is refused with 403" in text
    if bad_entry == "::ffff:10.0.0.0/8":
        assert "an IPv4-mapped address takes a prefix length from 96 to 128; for the IPv4 range write 10.0.0.0/8" in text


#
# X-Forwarded-For
#


@pytest.mark.parametrize("allow_from, allowed", [("10.64.0.0/24", False), ("127.0.0.1", True)])
def test_access_forwarded_for_ignored_without_trusted_proxies(allow_from: str, allowed: bool):
    """without PROVISION_ADMIN_TRUSTED_PROXIES X-Forwarded-For is client-controlled and ignored: the TCP peer counts"""
    _enable(PROVISION_ADMIN_ALLOW_FROM=allow_from)
    server.start()

    for forwarded in ("10.64.0.7", "203.0.113.9", "10.64.0.7, 10.64.0.8", "garbage"):
        res = _request("GET", "/admin/status", headers={"X-Forwarded-For": forwarded})
        if allowed:
            assert res.status_code == 200
        else:
            _assert_forbidden(res, "127.0.0.1")
    if allowed:
        value = _diagnostic(headers={"X-Forwarded-For": "203.0.113.9"})["value"]
        assert value["seen_from"] == "127.0.0.1" and value["via_forwarded_for"] is False


def test_access_forwarded_for_from_a_trusted_proxy():
    """from a trusted proxy the client is the first address of X-Forwarded-For, from the right, that is not a trusted proxy"""
    _enable(PROVISION_ADMIN_ALLOW_FROM="10.64.0.0/24 2001:db8::/32",
            PROVISION_ADMIN_TRUSTED_PROXIES="127.0.0.1, 192.168.100.0/24")
    server.start()

    cases = [
        # (X-Forwarded-For, allowed, the address the server sees)
        ("10.64.0.7", True, "10.64.0.7"),
        ("10.64.0.0", True, "10.64.0.0"),
        ("10.64.0.255", True, "10.64.0.255"),
        ("10.64.1.7", False, "10.64.1.7"),
        ("10.63.255.255", False, "10.63.255.255"),
        # multi-hop: a second trusted proxy is skipped
        ("10.64.0.7, 192.168.100.5", True, "10.64.0.7"),
        ("10.64.0.7,192.168.100.5 ,  192.168.100.6", True, "10.64.0.7"),
        # a spoofed leftmost entry: the proxy appended the real client address, which counts
        ("10.64.0.7, 203.0.113.9", False, "203.0.113.9"),
        ("10.64.0.7, 203.0.113.9, 192.168.100.5", False, "203.0.113.9"),
        # entries left of the client are never looked at
        ("garbage, 10.64.0.8", True, "10.64.0.8"),
        # only trusted hops: the leftmost one
        ("192.168.100.5", False, "192.168.100.5"),
        ("192.168.100.6, 192.168.100.5", False, "192.168.100.6"),
        # an entry that has to be read but is not an address: the direct peer counts
        ("10.64.0.7, garbage", False, "127.0.0.1"),
        ("unknown", False, "127.0.0.1"),
        ("10.64.0.07", False, "127.0.0.1"),
        ("10.64.0.7, ", False, "127.0.0.1"),
        ("10.64.0.7, 192.168.100.5:", False, "127.0.0.1"),
        # ports, IPv4-mapped IPv6, IPv6 compared as numbers, not as text
        ("10.64.0.8:51234", True, "10.64.0.8"),
        ("::ffff:10.64.0.9", True, "10.64.0.9"),
        ("2001:DB8:0:0::1", True, "2001:db8::1"),
        ("[2001:db8::5]:443", True, "2001:db8::5"),
        ("2001:db8:ffff:ffff:ffff:ffff:ffff:ffff", True, "2001:db8:ffff:ffff:ffff:ffff:ffff:ffff"),
        ("2001:db9::1", False, "2001:db9::1"),
        ("::ffff:10.65.0.1", False, "10.65.0.1"),
    ]
    n_refused = 0
    for forwarded, allowed, seen in cases:
        res = _request("GET", "/admin/status", headers={"X-Forwarded-For": forwarded})
        if allowed:
            assert res.status_code == 200, f"X-Forwarded-For: {forwarded} -> {res.status_code} {res.body}"
        else:
            _assert_forbidden(res, seen)
            n_refused += 1

    # no X-Forwarded-For: the request comes from the proxy itself
    _assert_forbidden(_request("GET", "/admin/status"), "127.0.0.1")
    n_refused += 1
    # sent twice (two spellings of the name, as from a proxy that appends a header of its own): not used
    status, body = _raw_status([("X-Forwarded-For", "10.64.0.7"), ("x-forwarded-for", "10.64.0.8")])
    assert status == 403 and body["error"]["type"] == "admin_forbidden"
    assert body["error"]["message"].endswith(" 127.0.0.1")
    n_refused += 1
    # ... and twice with the same spelling, in either order (a client line before or after the proxy's own line)
    for lines in ([("X-Forwarded-For", "203.0.113.9"), ("X-Forwarded-For", "10.64.0.7")],
                  [("X-Forwarded-For", "10.64.0.7"), ("X-Forwarded-For", "203.0.113.9")]):
        status, body = _raw_status(lines)
        assert status == 403 and body["error"]["type"] == "admin_forbidden", (lines, status, body)
        assert body["error"]["message"].endswith(" 127.0.0.1")
        n_refused += 1

    # with the key: status and diagnostics report what the server sees
    res = _request("GET", "/admin/status", key=ADMIN_KEY, headers={"X-Forwarded-For": "10.64.0.7"})
    assert res.status_code == 200 and res.body["authorized"] is True and res.body["ip_allowlist"] is True
    item = _diagnostic(headers={"X-Forwarded-For": "203.0.113.9, 10.64.0.7"})
    assert item["status"] == "ok", item
    assert item["value"] == {
        "enabled": True, "entries": ["10.64.0.0/24", "2001:db8::/32"], "trusted_proxies": ["127.0.0.1", "192.168.100.0/24"],
        "seen_from": "10.64.0.7", "peer": "127.0.0.1", "via_forwarded_for": True, "note": "",
    }
    stage1._assert_error(_request("GET", "/admin/settings", headers={"X-Forwarded-For": "10.64.0.7"}), 401, "admin_key_required")

    text = _wait_for_log(lambda t: len(_refused_lines(t)) >= n_refused, f"{n_refused} refused requests")
    refused = _refused_lines(text)
    assert len(refused) == n_refused
    assert any("from 203.0.113.9 (X-Forwarded-For of the trusted proxy 127.0.0.1)" in line for line in refused)
    assert any("from 127.0.0.1 (X-Forwarded-For has an entry that is not an address)" in line for line in refused)
    assert any("from 127.0.0.1 (the trusted proxy sent no X-Forwarded-For)" in line for line in refused)
    assert any("from 127.0.0.1 (X-Forwarded-For was sent more than once)" in line for line in refused)


def test_access_configuration_warnings():
    """a trusted proxy that is also allowed, and the CORS proxy, are reported (log and diagnostics)"""
    _enable(PROVISION_ADMIN_ALLOW_FROM="127.0.0.1 10.64.0.0/24", PROVISION_ADMIN_TRUSTED_PROXIES="10.64.0.1")
    server.ui_mcp_proxy = True
    server.start()

    assert _request("GET", "/admin/status").status_code == 200
    item = _diagnostic()
    assert item["status"] == "warning"
    assert "the trusted proxy 10.64.0.1 is also on the allow-list" in item["hint"]
    assert "the CORS proxy (--ui-mcp-proxy / --agent) is on" in item["hint"]
    text = _log_text()
    assert "the trusted proxy 10.64.0.1 is also on the allow-list" in text and "the CORS proxy" in text


def test_access_wide_trusted_ranges_are_reported():
    """every address of a trusted proxy can claim any client address: a wide trusted range, and host bits, are reported"""
    _enable(PROVISION_ADMIN_ALLOW_FROM="10.64.0.0/24",
            PROVISION_ADMIN_TRUSTED_PROXIES="127.0.0.1 192.168.0.0/16 192.168.100.0/24 2001:db8::/48 2001:db8:1::/64 "
                                            "::/80 192.168.200.9/24")
    server.start()

    # a request from a trusted address names an allowed client; the server cannot tell who wrote the header
    assert _request("GET", "/admin/status", headers={"X-Forwarded-For": "10.64.0.7"}).status_code == 200
    item = _diagnostic(headers={"X-Forwarded-For": "10.64.0.7"})
    assert item["status"] == "warning"
    hint = item["hint"]
    assert "PROVISION_ADMIN_TRUSTED_PROXIES: '192.168.200.9/24' has host bits set, it is read as 192.168.200.0/24" in hint
    for wide in ("192.168.0.0/16", "2001:db8::/48", "::/80"):
        assert f"the trusted proxy range {wide} is wide: every address in it can claim any client address" in hint, hint
    for narrow in ("127.0.0.1 ", "192.168.100.0/24 ", "2001:db8:1::/64 ", "192.168.200.0/24 "):
        assert f"the trusted proxy range {narrow}is wide" not in hint, hint
    assert "the trusted proxy range 192.168.0.0/16 is wide" in _log_text()


def test_access_trusted_proxies_without_allow_list():
    """PROVISION_ADMIN_TRUSTED_PROXIES alone switches no restriction on: a warning, any address may use /admin
    (the trusted proxies still name the client the audit log records)"""
    _enable(PROVISION_ADMIN_TRUSTED_PROXIES="127.0.0.1")
    server.start()

    assert _request("GET", "/admin/status", headers={"X-Forwarded-For": "203.0.113.9"}).status_code == 200
    item = _diagnostic()
    warning = ("PROVISION_ADMIN_TRUSTED_PROXIES without PROVISION_ADMIN_ALLOW_FROM only affects the client address the "
               "audit log records: any address may use /admin")
    assert item["status"] == "warning" and warning in item["hint"]
    assert item["value"]["enabled"] is False and item["value"]["trusted_proxies"] == ["127.0.0.1"]
    assert warning in _log_text()


def test_access_environment_not_passed_to_children():
    """the two variables are removed from the environment like the other PROVISION_ADMIN* variables"""
    _enable(PROVISION_ADMIN_ALLOW_FROM="127.0.0.1, 10.77.0.0/16", PROVISION_ADMIN_TRUSTED_PROXIES="192.168.177.0/24")
    server.start()
    stage1._load_model_and_wait(MODEL_B)

    assert server.process is not None
    children = stage1._child_pids(server.process.pid)
    if not children:
        pytest.skip("cannot list the child processes on this platform")  # ty: ignore[too-many-positional-arguments]
    for pid in children:
        child_env = stage1._process_environ(pid) or {}
        if not child_env:
            pytest.skip("cannot read the environment of a process on this platform (psutil is not installed)")  # ty: ignore[too-many-positional-arguments]
        assert "LLAMA_SERVER_ROUTER_PORT" in child_env
        assert [name for name in child_env if name.upper().startswith("PROVISION_ADMIN")] == []
        assert not any("10.77.0.0/16" in value or "192.168.177.0/24" in value for value in child_env.values())
