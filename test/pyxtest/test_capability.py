# SPDX-License-Identifier: MIT
#
# Tests for XPrism capability checks (dix/capability.c), with Xnamespace as
# the policy provider. Most cases happen inside one namespace, where
# Xnamespace's own rules allow everything, so only the capability check
# decides.

import re
import shutil
import struct
import tempfile
from pathlib import Path

import pytest
from proto import xi, xtest
from xclient import Extension, RawX11Connection, X11Error

COOKIE_PROTO = b"MIT-MAGIC-COOKIE-1"
ROOT_COOKIE = bytes.fromhex("11111111111111111111111111111111")
SANDBOX_COOKIE = bytes.fromhex("22222222222222222222222222222222")
GRANTED_COOKIE = bytes.fromhex("33333333333333333333333333333333")
COMPOSITOR_COOKIE = bytes.fromhex("44444444444444444444444444444444")

X_GET_WINDOW_ATTRIBUTES = 3
X_QUERY_TREE = 15
X_SET_SELECTION_OWNER = 22
X_GET_INPUT_FOCUS = 43
BAD_ACCESS = 10


@pytest.fixture
def capmode():
    return "audit"


@pytest.fixture
def xserver_args(tmp_path, capmode):
    conf = tmp_path / "ns.conf"
    conf.write_text(
        f"auth {COOKIE_PROTO.decode()} {ROOT_COOKIE.hex()}\n"
        f"namespace sandbox root\n"
        f"  auth {COOKIE_PROTO.decode()} {SANDBOX_COOKIE.hex()}\n"
        f"  allow xinput\n"
        f"namespace granted root\n"
        f"  auth {COOKIE_PROTO.decode()} {GRANTED_COOKIE.hex()}\n"
        f"  grant window.inspect window.enumerate\n"
        f"namespace compositor\n"
        f"  auth {COOKIE_PROTO.decode()} {COMPOSITOR_COOKIE.hex()}\n"
        f"  grant compositor.legacy\n"
    )
    return ["-namespace", str(conf), "-capmode", capmode]


def _connect(xserver, cookie):
    return RawX11Connection(xserver.display_num, auth=(COOKIE_PROTO, cookie))


def _errors_of(conn, request):
    """Send *request*, round-trip, return the error codes it caused."""
    conn.send_request(request)
    seq = conn.send_request(struct.pack("<BxH", X_GET_INPUT_FOCUS, 1))
    errors = []
    while True:
        r = conn.recv_response(timeout=5.0)
        assert r is not None, "no answer from server"
        if isinstance(r, X11Error):
            errors.append(r.error_code)
        elif r.response_type == 1 and r.sequence == seq:
            return errors


def _inspect_other_window(xserver, cookie):
    """One client inspects a window of another client in the same namespace."""
    owner = _connect(xserver, cookie)
    wid = owner.create_window()
    spy = _connect(xserver, cookie)
    return _errors_of(spy, struct.pack("<BxHI", X_GET_WINDOW_ATTRIBUTES, 2, wid))


def _list_other_window(xserver, cookie):
    """One client lists the children of another client's window."""
    owner = _connect(xserver, cookie)
    wid = owner.create_window()
    owner.create_window(parent=wid)
    spy = _connect(xserver, cookie)
    return _errors_of(spy, struct.pack("<BxHI", X_QUERY_TREE, 2, wid))


def _take_clipboard(conn):
    wid = conn.create_window()
    clipboard = conn.intern_atom("CLIPBOARD")
    return _errors_of(conn, struct.pack("<BxHIII", X_SET_SELECTION_OWNER, 4,
                                        wid, clipboard, 0))


def _cap_log(xserver):
    return [line for line in xserver._read_stderr().splitlines()
            if "capability:" in line]


class TestCapabilityAudit:
    """Default mode: log what would be denied, deny nothing."""

    def test_missing_capability_is_logged_not_denied(self, xserver):
        assert _inspect_other_window(xserver, SANDBOX_COOKIE) == []
        log = _cap_log(xserver)
        assert len(log) == 1, log
        assert re.search(r'would deny window\.inspect to client \d+ '
                         r'\(namespace "sandbox", pid \d+\) for '
                         r'(GetWindowAttributes|request 3\.0) '
                         r'on 0x[0-9a-f]+ \[audit\]', log[0]), log[0]

    def test_logged_once_per_client_and_capability(self, xserver):
        owner = _connect(xserver, SANDBOX_COOKIE)
        wid = owner.create_window()
        spy = _connect(xserver, SANDBOX_COOKIE)
        for _ in range(3):
            assert _errors_of(spy, struct.pack("<BxHI", X_GET_WINDOW_ATTRIBUTES, 2, wid)) == []
        log = _cap_log(xserver)
        assert len(log) == 1 and "window.inspect" in log[0], log

    def test_private_namespace_clipboard_needs_no_capability(self, xserver):
        """A confined client's CLIPBOARD is its namespace's private copy:
        clipboard.write is about the session clipboard only."""
        assert _take_clipboard(_connect(xserver, SANDBOX_COOKIE)) == []
        assert _cap_log(xserver) == []

    def test_granted_capability_is_not_logged(self, xserver):
        assert _inspect_other_window(xserver, GRANTED_COOKIE) == []
        assert _list_other_window(xserver, GRANTED_COOKIE) == []
        assert _cap_log(xserver) == []

    def test_root_namespace_is_unconfined(self, xserver):
        assert _inspect_other_window(xserver, ROOT_COOKIE) == []
        assert _list_other_window(xserver, ROOT_COOKIE) == []
        assert _take_clipboard(_connect(xserver, ROOT_COOKIE)) == []
        assert _cap_log(xserver) == []

    def test_own_window_needs_no_capability(self, xserver):
        conn = _connect(xserver, SANDBOX_COOKIE)
        wid = conn.create_window()
        assert _errors_of(conn, struct.pack("<BxHI", X_GET_WINDOW_ATTRIBUTES, 2, wid)) == []
        assert _cap_log(xserver) == []


class TestCapabilityEnforce:
    @pytest.fixture
    def capmode(self):
        return "enforce"

    def test_missing_capability_is_denied(self, xserver):
        assert _inspect_other_window(xserver, SANDBOX_COOKIE) == [BAD_ACCESS]
        assert _list_other_window(xserver, SANDBOX_COOKIE) == [BAD_ACCESS]
        log = _cap_log(xserver)
        assert any("denied window.inspect" in line for line in log), log
        assert any("denied window.enumerate" in line for line in log), log
        assert not any("[audit]" in line for line in log), log

    def test_granted_capability_is_allowed(self, xserver):
        assert _inspect_other_window(xserver, GRANTED_COOKIE) == []
        assert _list_other_window(xserver, GRANTED_COOKIE) == []

    def test_root_namespace_is_unconfined(self, xserver):
        assert _inspect_other_window(xserver, ROOT_COOKIE) == []
        assert _list_other_window(xserver, ROOT_COOKIE) == []
        assert _take_clipboard(_connect(xserver, ROOT_COOKIE)) == []


class TestCapabilityOff:
    @pytest.fixture
    def capmode(self):
        return "off"

    def test_no_checks(self, xserver):
        assert _inspect_other_window(xserver, SANDBOX_COOKIE) == []
        assert _cap_log(xserver) == []


X_CHANGE_WINDOW_ATTRIBUTES = 2
X_GET_IMAGE = 73
CW_EVENT_MASK = 1 << 11
SUBSTRUCTURE_REDIRECT = 1 << 20
Z_PIXMAP = 2
RR_SET_SCREEN_SIZE = 7
RECORD_CREATE_CONTEXT = 1
XI_CHANGE_HIERARCHY = 43
XI_ADD_MASTER = 1
KEYCODE_A = 38


def _opcode(xserver, name):
    """Major opcode of an extension (asked as root: it may be hidden from
    the sandbox, but the opcode is the same)."""
    ext = _connect(xserver, ROOT_COOKIE).query_extension(name)
    assert ext, f"{name} not available"
    return ext.opcode


def _get_image(conn, drawable):
    return _errors_of(conn, struct.pack("<BBHIhhHHI", X_GET_IMAGE, Z_PIXMAP, 5,
                                        drawable, 0, 0, 1, 1, 0xFFFFFFFF))


def _assert_checked(xserver, cap):
    log = _cap_log(xserver)
    assert any(f"would deny {cap} " in line for line in log), \
        f"no {cap} check in log: {log}"


class TestCapabilityCoverage:
    """Each capability's check fires where the protocol does the thing.
    Across namespaces Xnamespace denies most of these anyway, so the audit
    log is what shows the capability check ran."""

    def test_capture_screen(self, xserver):
        conn = _connect(xserver, SANDBOX_COOKIE)
        _get_image(conn, conn.root_window)
        _assert_checked(xserver, "capture.screen")

    def test_capture_window(self, xserver):
        owner = _connect(xserver, SANDBOX_COOKIE)
        wid = owner.create_window()
        _get_image(_connect(xserver, SANDBOX_COOKIE), wid)
        _assert_checked(xserver, "capture.window")

    def test_input_inject(self, xserver):
        opcode = _opcode(xserver, Extension.XTEST)
        conn = _connect(xserver, SANDBOX_COOKIE)
        _errors_of(conn, xtest.FakeInputRequest(opcode=opcode, type=xtest.KeyPress,
                                                detail=KEYCODE_A))
        _assert_checked(xserver, "input.inject")

    def test_input_monitor_record(self, xserver):
        opcode = _opcode(xserver, Extension.RECORD)
        conn = _connect(xserver, SANDBOX_COOKIE)
        # CreateContext: context, element header, 0 clients, 0 ranges
        _errors_of(conn, struct.pack("<BBHIBxxxII", opcode, RECORD_CREATE_CONTEXT, 5,
                                     conn.alloc_id(), 0, 0, 0))
        _assert_checked(xserver, "input.monitor")

    def test_input_monitor_raw_keys(self, xserver):
        spy = _connect(xserver, SANDBOX_COOKIE)
        ext = spy.query_extension(Extension.XI)
        assert ext
        spy.send_request(xi.XIQueryVersionRequest(opcode=ext.opcode))
        spy.recv_response(timeout=5.0)
        _errors_of(spy, xi.XISelectEventsRequest(
            opcode=ext.opcode, window=spy.root_window, deviceid=xi.XIAllDevices,
            events=[xi.XI_RawKeyPress, xi.XI_RawKeyRelease]))

        injector = _connect(xserver, ROOT_COOKIE)
        opcode = _opcode(xserver, Extension.XTEST)
        for evtype in (xtest.KeyPress, xtest.KeyRelease):
            _errors_of(injector, xtest.FakeInputRequest(opcode=opcode, type=evtype,
                                                        detail=KEYCODE_A))
        spy.flush_responses(timeout=0.5)
        _assert_checked(xserver, "input.monitor")

    def test_input_virtual_seat(self, xserver):
        conn = _connect(xserver, SANDBOX_COOKIE)
        ext = conn.query_extension(Extension.XI)
        assert ext
        conn.send_request(xi.XIQueryVersionRequest(opcode=ext.opcode))
        conn.recv_response(timeout=5.0)
        name = b"spy seat"
        info = struct.pack("<HHHBB", XI_ADD_MASTER, (8 + len(name) + 3) // 4,
                           len(name), 1, 1) + name + b"\0" * (-len(name) % 4)
        req = struct.pack("<BBHBxxx", ext.opcode, XI_CHANGE_HIERARCHY,
                          (8 + len(info)) // 4, 1) + info
        _errors_of(conn, req)
        _assert_checked(xserver, "input.virtual_seat")

    def test_window_manage(self, xserver):
        """Selecting SubstructureRedirect on another client's window means
        managing its children (what a window manager does on the root;
        a confined client is refused the real root by Xnamespace already)."""
        owner = _connect(xserver, SANDBOX_COOKIE)
        wid = owner.create_window()
        wm = _connect(xserver, SANDBOX_COOKIE)
        _errors_of(wm, struct.pack("<BxHIII", X_CHANGE_WINDOW_ATTRIBUTES, 4,
                                   wid, CW_EVENT_MASK, SUBSTRUCTURE_REDIRECT))
        _assert_checked(xserver, "window.manage")

    def test_display_configure(self, xserver):
        opcode = _opcode(xserver, Extension.RANDR)
        conn = _connect(xserver, SANDBOX_COOKIE)
        _errors_of(conn, struct.pack("<BBHIHHII", opcode, RR_SET_SCREEN_SIZE, 5,
                                     conn.root_window, 640, 480, 169, 127))
        _assert_checked(xserver, "display.configure")

    def test_compositor_legacy_implies_capture(self, xserver):
        """A compositor sees every window: holding compositor.legacy
        covers capture.screen (Xnamespace still keeps the namespace off
        the real root, so only the log shows the capability decision)."""
        for cookie in (COMPOSITOR_COOKIE, SANDBOX_COOKIE):
            conn = _connect(xserver, cookie)
            _get_image(conn, conn.root_window)
        log = [l for l in _cap_log(xserver) if "would deny capture.screen" in l]
        assert any("sandbox" in l for l in log), log
        assert not any("compositor" in l for l in log), log

    def test_display_query_needs_no_capability(self, xserver):
        """Reading the configuration is not configuring it."""
        opcode = _opcode(xserver, Extension.RANDR)
        conn = _connect(xserver, SANDBOX_COOKIE)
        _errors_of(conn, struct.pack("<BBHI", opcode, 8, 2, conn.root_window))  # GetScreenResources
        assert not any("display.configure" in line for line in _cap_log(xserver))


class TestPlainSession:
    """Without a policy provider every client is unconfined: plain X11
    sessions behave exactly as before, even in enforce mode."""

    @pytest.fixture
    def xserver_args(self):
        return ["-capmode", "enforce"]

    def test_everything_allowed_nothing_logged(self, xserver):
        a = RawX11Connection(xserver.display_num)
        b = RawX11Connection(xserver.display_num)
        wid = a.create_window()
        assert _errors_of(b, struct.pack("<BxHI", X_GET_WINDOW_ATTRIBUTES, 2, wid)) == []
        assert _errors_of(b, struct.pack("<BxHI", X_QUERY_TREE, 2, b.root_window)) == []
        assert _get_image(b, b.root_window) == []
        assert _take_clipboard(b) == []
        assert _cap_log(xserver) == []


# --- runtime grants and listener ceilings (X-NAMESPACE 1.1) -------------

from test_namespace_listener import Listener, _add_listener, _pad  # noqa: E402

X_XNS_QUERY_NAMESPACE = 4
X_XNS_GET_CLIENT_INFO = 12
X_XNS_SET_NAMESPACE_GRANTS = 13
GRANT_WINDOW_INSPECT = 1 << 6
GRANT_ALL = 0xFFF
BAD_VALUE = 2


def _manager(xserver):
    conn = _connect(xserver, ROOT_COOKIE)
    ext = conn.query_extension("X-NAMESPACE")
    assert ext
    return conn, ext.opcode


def _set_grants(manager, opcode, ns, grants):
    name = ns.encode()
    body = struct.pack("<IHH", grants, len(name), 0) + _pad(name)
    manager.send_request(struct.pack("<BBH", opcode, X_XNS_SET_NAMESPACE_GRANTS,
                                     1 + len(body) // 4) + body)
    return manager.recv_response(timeout=5.0)


def _query_grants(manager, opcode, ns):
    name = ns.encode()
    body = struct.pack("<HH", len(name), 0) + _pad(name)
    manager.send_request(struct.pack("<BBH", opcode, X_XNS_QUERY_NAMESPACE,
                                     1 + len(body) // 4) + body)
    r = manager.recv_response(timeout=5.0)
    assert not isinstance(r, X11Error), r
    return struct.unpack_from("<I", r.data, 24)[0]


def _grants_of(manager, opcode, client):
    """(grants, ceiling) GetClientInfo reports for *client*."""
    pixmap = client.create_pixmap(1, 1)
    client.flush_responses(timeout=0.2)
    manager.send_request(struct.pack("<BBHI", opcode, X_XNS_GET_CLIENT_INFO, 2, pixmap))
    r = manager.recv_response(timeout=5.0)
    assert not isinstance(r, X11Error), r
    return struct.unpack_from("<II", r.data, 24)


def _bound_client(xserver, sockdir, ns, ceiling):
    """A client connected through a listener for *ns* with *ceiling*."""
    manager, opcode = _manager(xserver)
    listener = Listener(sockdir, f"{ns}-{ceiling:x}.sock")
    handle = _add_listener(manager, opcode, ns, [listener.sock.fileno(), listener.close_r],
                           ceiling=ceiling)
    assert isinstance(handle, int), handle
    listener.hand_over()
    client = RawX11Connection(xserver.display_num, path=listener.path)
    return client, listener


def _inspect_from(xserver, cookie, spy):
    owner = _connect(xserver, cookie)
    wid = owner.create_window()
    return _errors_of(spy, struct.pack("<BxHI", X_GET_WINDOW_ATTRIBUTES, 2, wid))


def _redirect_root_manual(xserver, conn):
    """CompositeRedirectSubwindows(root, Manual), as picom does."""
    opcode = _opcode(xserver, "Composite")
    return _errors_of(conn, struct.pack("<BBHIBxxx", opcode, 2, 3, conn.root_window, 1))


@pytest.fixture
def sockdir():
    """Short directory for sockets: tmp_path can be longer than a Unix
    socket path may be."""
    d = Path(tempfile.mkdtemp(prefix="xcap"))
    yield d
    shutil.rmtree(d, ignore_errors=True)


class TestGrantsAndCeilings:
    @pytest.fixture
    def capmode(self):
        return "enforce"

    def test_runtime_grant_takes_effect(self, xserver):
        manager, opcode = _manager(xserver)
        spy = _connect(xserver, SANDBOX_COOKIE)
        assert _inspect_from(xserver, SANDBOX_COOKIE, spy) == [BAD_ACCESS]

        r = _set_grants(manager, opcode, "sandbox", GRANT_WINDOW_INSPECT)
        assert not isinstance(r, X11Error), r
        assert _query_grants(manager, opcode, "sandbox") == GRANT_WINDOW_INSPECT
        # the same, already connected client: next request is allowed
        assert _inspect_from(xserver, SANDBOX_COOKIE, spy) == []

        _set_grants(manager, opcode, "sandbox", 0)       # revoke
        assert _inspect_from(xserver, SANDBOX_COOKIE, spy) == [BAD_ACCESS]

    def test_ceiling_limits_namespace_grants(self, xserver, sockdir):
        """"granted" holds window.inspect; a listener with ceiling none
        takes it away from its clients, one with window.inspect doesn't."""
        limited, l1 = _bound_client(xserver, sockdir, "granted", 0)
        allowed, l2 = _bound_client(xserver, sockdir, "granted", GRANT_WINDOW_INSPECT)
        assert _inspect_from(xserver, GRANTED_COOKIE, limited) == [BAD_ACCESS]
        assert _inspect_from(xserver, GRANTED_COOKIE, allowed) == []
        # a client of the same namespace via its cookie has no ceiling
        assert _inspect_from(xserver, GRANTED_COOKIE, _connect(xserver, GRANTED_COOKIE)) == []

    def test_ceiling_survives_later_grants(self, xserver, sockdir):
        manager, opcode = _manager(xserver)
        limited, l1 = _bound_client(xserver, sockdir, "sandbox", 0)
        _set_grants(manager, opcode, "sandbox", GRANT_ALL)
        assert _inspect_from(xserver, SANDBOX_COOKIE, limited) == [BAD_ACCESS]

    def test_ceiling_grants_nothing_by_itself(self, xserver, sockdir):
        client, l1 = _bound_client(xserver, sockdir, "sandbox", GRANT_ALL)
        assert _inspect_from(xserver, SANDBOX_COOKIE, client) == [BAD_ACCESS]

    def test_client_info_reports_grants_and_ceiling(self, xserver, sockdir):
        manager, opcode = _manager(xserver)
        client, l1 = _bound_client(xserver, sockdir, "granted", GRANT_WINDOW_INSPECT)
        grants, ceiling = _grants_of(manager, opcode, client)
        assert ceiling == GRANT_WINDOW_INSPECT
        assert grants == GRANT_WINDOW_INSPECT    # granted: inspect+enumerate, AND ceiling
        grants, ceiling = _grants_of(manager, opcode, _connect(xserver, GRANTED_COOKIE))
        assert ceiling == 0xFFFFFFFF and grants == GRANT_WINDOW_INSPECT | (1 << 5)

    def test_session_compositor_needs_nothing(self, xserver):
        assert _redirect_root_manual(xserver, _connect(xserver, ROOT_COOKIE)) == []

    def test_cannot_grant_builtin_namespaces(self, xserver):
        manager, opcode = _manager(xserver)
        for ns in ("root", "anon"):
            r = _set_grants(manager, opcode, ns, GRANT_WINDOW_INSPECT)
            assert isinstance(r, X11Error) and r.error_code == BAD_ACCESS, (ns, r)

    def test_unknown_grant_bits_rejected(self, xserver):
        manager, opcode = _manager(xserver)
        r = _set_grants(manager, opcode, "sandbox", 1 << 20)
        assert isinstance(r, X11Error) and r.error_code == BAD_VALUE
