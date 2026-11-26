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


def _cap_log(xserver, unconfined=False):
    """the capability log lines; with unconfined, those recording
    unrestricted access, else the others"""
    return [line for line in xserver._read_stderr().splitlines()
            if "capability:" in line and
            ("unrestricted X11 access" in line) == unconfined]


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

    def test_display_configure_is_enforced(self, xserver):
        """(Xnamespace refuses writes to the real root as well)"""
        opcode = _opcode(xserver, Extension.RANDR)
        conn = _connect(xserver, SANDBOX_COOKIE)
        assert _errors_of(conn, struct.pack("<BBHIHHII", opcode, RR_SET_SCREEN_SIZE, 5,
                                            conn.root_window, 640, 480, 169, 127)) == [BAD_ACCESS]

    def test_granted_capability_is_allowed(self, xserver):
        assert _inspect_other_window(xserver, GRANTED_COOKIE) == []
        assert _list_other_window(xserver, GRANTED_COOKIE) == []

    def test_root_namespace_is_unconfined(self, xserver):
        assert _inspect_other_window(xserver, ROOT_COOKIE) == []
        assert _list_other_window(xserver, ROOT_COOKIE) == []
        assert _take_clipboard(_connect(xserver, ROOT_COOKIE)) == []


X_MAP_WINDOW = 8
X_GRAB_KEYBOARD = 31
X_GRAB_KEY = 33
X_SET_INPUT_FOCUS = 42
X_QUERY_KEYMAP = 44
ANY_MODIFIER = 0x8000


def _focused_window(xserver, conn):
    """A mapped window of conn that the window manager (unconfined) gave
    the focus; Xnamespace does not let confined clients take it."""
    wid = conn.create_window()
    conn.send_request(struct.pack("<BxHI", X_MAP_WINDOW, 2, wid))
    assert _errors_of(conn, struct.pack("<BxHI", X_MAP_WINDOW, 2, wid)) == []
    wm = _connect(xserver, ROOT_COOKIE)
    assert _errors_of(wm, struct.pack("<BBHII", X_SET_INPUT_FOCUS, 1, 3, wid, 0)) == []
    return wid


def _focus(conn):
    seq = conn.send_request(struct.pack("<BxH", X_GET_INPUT_FOCUS, 1))
    while True:
        r = conn.recv_response(timeout=5.0)
        assert r is not None and not isinstance(r, X11Error), r
        if r.response_type == 1 and r.sequence == seq:
            return struct.unpack_from("<I", r.data, 8)[0]


def _keymap(conn):
    """QueryKeymap's 32 bytes; refused, they are all zero"""
    seq = conn.send_request(struct.pack("<BxH", X_QUERY_KEYMAP, 1))
    while True:
        r = conn.recv_response(timeout=5.0)
        assert r is not None and not isinstance(r, X11Error), r
        if r.response_type == 1 and r.sequence == seq:
            return r.data[8:40]


def _keymap_with_a_key_down(xserver, conn):
    """conn's QueryKeymap while A is held down (pressed through XTEST)"""
    injector = _connect(xserver, ROOT_COOKIE)
    opcode = _opcode(xserver, Extension.XTEST)
    _errors_of(injector, xtest.FakeInputRequest(opcode=opcode, type=xtest.KeyPress,
                                                detail=KEYCODE_A))
    try:
        return _keymap(conn)
    finally:
        _errors_of(injector, xtest.FakeInputRequest(opcode=opcode, type=xtest.KeyRelease,
                                                    detail=KEYCODE_A))


def _grab_keyboard(conn, wid):
    return _errors_of(conn, struct.pack("<BBHIIBBxx", X_GRAB_KEYBOARD, 0, 4, wid, 0, 1, 1))


def _grab_key(conn, wid):
    return _errors_of(conn, struct.pack("<BBHIHBBBxxx", X_GRAB_KEY, 0, 4, wid,
                                        ANY_MODIFIER, KEYCODE_A, 1, 1))


class TestKeyboardScope:
    """Keys belong to the client with the focus: it may grab the keyboard
    (menus) and see which keys are down; others need input.monitor.
    Passive key grabs on its own windows fire only while it has the
    focus; on the root they are global hot keys and see every key."""

    @pytest.fixture
    def capmode(self):
        return "enforce"

    def test_focused_client(self, xserver):
        conn = _connect(xserver, SANDBOX_COOKIE)
        wid = _focused_window(xserver, conn)
        assert _grab_keyboard(conn, wid) == []
        assert _grab_key(conn, wid) == []
        assert _keymap_with_a_key_down(xserver, conn)[KEYCODE_A // 8] & (1 << KEYCODE_A % 8)

    def test_unfocused_client(self, xserver):
        conn = _connect(xserver, SANDBOX_COOKIE)
        wid = conn.create_window()
        assert _errors_of(conn, struct.pack("<BxHI", X_MAP_WINDOW, 2, wid)) == []
        _focused_window(xserver, _connect(xserver, ROOT_COOKIE))
        assert _grab_keyboard(conn, wid) == [BAD_ACCESS]
        assert _keymap_with_a_key_down(xserver, conn) == bytes(32)
        # its own window: fires only once it has the focus
        assert _grab_key(conn, wid) == []
        assert any("denied input.monitor" in line for line in _cap_log(xserver))

    def test_focus_moves_only_from_its_own_window(self, xserver):
        """A confined client moves the focus among its windows while it
        has it (dialogs); it does not take it from another client."""
        conn = _connect(xserver, SANDBOX_COOKIE)
        first = _focused_window(xserver, conn)
        second = conn.create_window()
        assert _errors_of(conn, struct.pack("<BxHI", X_MAP_WINDOW, 2, second)) == []
        assert _errors_of(conn, struct.pack("<BBHII", X_SET_INPUT_FOCUS, 1, 3, second, 0)) == []
        wm = _connect(xserver, ROOT_COOKIE)
        assert _focus(wm) == second
        other = _focused_window(xserver, wm)
        # refused without an error, as Xnamespace refuses it
        assert _errors_of(conn, struct.pack("<BBHII", X_SET_INPUT_FOCUS, 1, 3, first, 0)) == []
        assert _focus(wm) == other

    def test_hot_key_on_the_root(self, xserver):
        conn = _connect(xserver, SANDBOX_COOKIE)
        _focused_window(xserver, conn)
        assert _grab_key(conn, conn.root_window) == [BAD_ACCESS]

    def test_unconfined(self, xserver):
        root = _connect(xserver, ROOT_COOKIE)
        _focused_window(xserver, _connect(xserver, SANDBOX_COOKIE))
        assert _keymap_with_a_key_down(xserver, root) != bytes(32)
        assert _grab_key(root, root.root_window) == []


class TestDefaultMode:
    """Without -capmode, capabilities are enforced."""

    @pytest.fixture
    def xserver_args(self, xserver_args):
        i = xserver_args.index("-capmode")
        return xserver_args[:i] + xserver_args[i + 2:]

    def test_enforced(self, xserver):
        assert _inspect_other_window(xserver, SANDBOX_COOKIE) == [BAD_ACCESS]
        assert _inspect_other_window(xserver, GRANTED_COOKIE) == []


def _unconfined_log(xserver):
    return _cap_log(xserver, unconfined=True)


class TestLegacyAccess:
    """Unrestricted (legacy X11) access is explicit, never silent: each
    unconfined client is logged, and so is a server without a policy."""

    @pytest.fixture
    def capmode(self):
        return "enforce"

    def test_unconfined_clients_are_logged(self, xserver):
        before = len(_unconfined_log(xserver))     # the harness's own client
        root = _connect(xserver, ROOT_COOKIE)
        sandboxed = _connect(xserver, SANDBOX_COOKIE)
        _focus(root)
        _focus(sandboxed)
        log = _unconfined_log(xserver)[before:]
        assert len(log) == 1, log
        assert re.search(r'client \d+ \(namespace "root", pid \d+, .+\) is unconfined', log[0]), \
            log[0]

    def test_unrecognised_cookies_are_confined(self, xserver):
        """the anon namespace: what a client whose cookie no namespace
        lists gets, restricted"""
        conn = RawX11Connection(xserver.display_num, auth=(COOKIE_PROTO, b"\x99" * 16))
        _focus(conn)
        assert not any('"anon"' in line for line in _unconfined_log(xserver))
        assert _inspect_other_window(xserver, b"\x99" * 16) == [BAD_ACCESS]


class TestNoPolicy:
    @pytest.fixture
    def xserver_args(self):
        return []

    def test_legacy_mode_is_logged(self, xserver):
        for _ in range(2):
            _focus(RawX11Connection(xserver.display_num))
        log = _unconfined_log(xserver)
        assert len(log) == 1, log
        assert "no namespace configuration" in log[0]


XKB_USE_EXTENSION, XKB_SELECT_EVENTS, XKB_GET_STATE = 0, 1, 4
XKB_STATE_NOTIFY_MASK = 1 << 2
XKB_USE_CORE_KBD = 0x100
X_QUERY_POINTER = 38
KEYCODE_SHIFT_L, KEYCODE_CAPS_LOCK = 50, 66
SHIFT_MASK, LOCK_MASK = 1, 2


def _xkb(conn):
    """XKB opcode and first event of conn, initialised and selecting
    StateNotify"""
    ext = conn.query_extension("XKEYBOARD")
    conn.send_request(struct.pack("<BBHHH", ext.opcode, XKB_USE_EXTENSION, 2, 1, 0))
    r = conn.recv_response(timeout=5.0)
    assert r is not None and not isinstance(r, X11Error) and r.data[1], r
    assert _errors_of(conn, struct.pack("<BBHHHHHHH", ext.opcode, XKB_SELECT_EVENTS, 4,
                                        XKB_USE_CORE_KBD, XKB_STATE_NOTIFY_MASK, 0,
                                        XKB_STATE_NOTIFY_MASK, 0, 0)) == []
    return ext


def _xkb_state(conn, ext):
    """(mods, baseMods, latchedMods, lockedMods) from XkbGetState"""
    seq = conn.send_request(struct.pack("<BBHHxx", ext.opcode, XKB_GET_STATE, 2,
                                        XKB_USE_CORE_KBD))
    while True:
        r = conn.recv_response(timeout=5.0)
        assert r is not None and not isinstance(r, X11Error), r
        if r.response_type == 1 and r.sequence == seq:
            return tuple(r.data[8:12])


def _state_notifies(conn, ext, timeout=0.5):
    """(baseMods, lockedMods, keycode) of the StateNotify events received"""
    out = []
    while True:
        r = conn.recv_response(timeout=timeout)
        if r is None:
            return out
        if not isinstance(r, X11Error) and (r.response_type & 0x7f) == ext.first_event \
                and r.data[1] == 2:                         # XkbStateNotify
            out.append((r.data[10], r.data[12], r.data[28]))
            timeout = 0.2


def _core_mods(conn):
    """the modifiers in QueryPointer's mask (on a window of conn's own:
    Xnamespace refuses the root to confined clients)"""
    wid = conn.create_window()
    seq = conn.send_request(struct.pack("<BxHI", X_QUERY_POINTER, 2, wid))
    while True:
        r = conn.recv_response(timeout=5.0)
        assert r is not None and not isinstance(r, X11Error), r
        if r.response_type == 1 and r.sequence == seq:
            return struct.unpack_from("<H", r.data, 24)[0] & 0xff


def _press(xserver, keycode, release=False):
    injector = _connect(xserver, ROOT_COOKIE)
    opcode = _opcode(xserver, Extension.XTEST)
    if not release:
        _errors_of(injector, xtest.FakeInputRequest(opcode=opcode, type=xtest.KeyPress,
                                                    detail=keycode))
    else:
        _errors_of(injector, xtest.FakeInputRequest(opcode=opcode, type=xtest.KeyRelease,
                                                    detail=keycode))


class TestKeyState:
    """The modifiers held down are keyboard input too: a client without
    the focus sees only the locked ones (Caps Lock, the layout), through
    XKB, QueryPointer and XIQueryPointer, and no StateNotify when a
    modifier is merely pressed."""

    @pytest.fixture
    def capmode(self):
        return "enforce"

    def test_held_modifiers(self, xserver):
        focused = _connect(xserver, SANDBOX_COOKIE)
        _focused_window(xserver, focused)
        other = _connect(xserver, SANDBOX_COOKIE)
        fx, ox = _xkb(focused), _xkb(other)
        _press(xserver, KEYCODE_SHIFT_L)
        try:
            assert _xkb_state(focused, fx)[:2] == (SHIFT_MASK, SHIFT_MASK)
            assert _xkb_state(other, ox) == (0, 0, 0, 0)
            assert _core_mods(focused) == SHIFT_MASK
            assert _core_mods(other) == 0
        finally:
            _press(xserver, KEYCODE_SHIFT_L, release=True)

    def test_state_notify(self, xserver):
        focused = _connect(xserver, SANDBOX_COOKIE)
        _focused_window(xserver, focused)
        other = _connect(xserver, SANDBOX_COOKIE)
        fx, ox = _xkb(focused), _xkb(other)
        _press(xserver, KEYCODE_SHIFT_L)
        _press(xserver, KEYCODE_SHIFT_L, release=True)
        assert (SHIFT_MASK, 0, KEYCODE_SHIFT_L) in _state_notifies(focused, fx)
        assert _state_notifies(other, ox) == []

    def test_locked_modifiers(self, xserver):
        focused = _connect(xserver, SANDBOX_COOKIE)
        _focused_window(xserver, focused)
        other = _connect(xserver, SANDBOX_COOKIE)
        ox = _xkb(other)
        _press(xserver, KEYCODE_CAPS_LOCK)
        _press(xserver, KEYCODE_CAPS_LOCK, release=True)
        try:
            notes = _state_notifies(other, ox)
            assert notes and all(n == (0, LOCK_MASK, 0) for n in notes), notes
            assert _xkb_state(other, ox) == (LOCK_MASK, 0, 0, LOCK_MASK)
            assert _core_mods(other) == LOCK_MASK
        finally:
            _press(xserver, KEYCODE_CAPS_LOCK)
            _press(xserver, KEYCODE_CAPS_LOCK, release=True)

    def test_xi_query_pointer(self, xserver):
        other = _connect(xserver, SANDBOX_COOKIE)
        _focused_window(xserver, _connect(xserver, SANDBOX_COOKIE))
        op = other.query_extension(Extension.XI).opcode
        assert _errors_of(other, struct.pack("<BBHHH", op, 47, 2, 2, 2)) == []
        own = other.create_window()
        _press(xserver, KEYCODE_SHIFT_L)
        try:
            seq = other.send_request(struct.pack("<BBHIHxx", op, 40, 3, own, 2))
            while True:
                r = other.recv_response(timeout=5.0)
                assert r is not None and not isinstance(r, X11Error), r
                if r.response_type == 1 and r.sequence == seq:
                    break
            base, latched = struct.unpack_from("<II", r.data, 36)
            assert (base, latched) == (0, 0)
        finally:
            _press(xserver, KEYCODE_SHIFT_L, release=True)

    def test_unconfined(self, xserver):
        _focused_window(xserver, _connect(xserver, SANDBOX_COOKIE))
        root = _connect(xserver, ROOT_COOKIE)
        rx = _xkb(root)
        _press(xserver, KEYCODE_SHIFT_L)
        try:
            assert _xkb_state(root, rx)[1] == SHIFT_MASK
            assert _core_mods(root) == SHIFT_MASK
        finally:
            _press(xserver, KEYCODE_SHIFT_L, release=True)


X_CREATE_GC = 55
X_COPY_AREA = 62
GC_SUBWINDOW_MODE = 1 << 15
CLIP_BY_CHILDREN, INCLUDE_INFERIORS = 0, 1


def _embedding(xserver, cookie):
    """a mapped window of a client of *cookie*, with a mapped child of
    another client of the same namespace inside it (like an embedded
    tray icon)"""
    embedder = _connect(xserver, cookie)
    wid = embedder.create_window(width=50, height=50)
    assert _errors_of(embedder, struct.pack("<BxHI", X_MAP_WINDOW, 2, wid)) == []
    other = _connect(xserver, cookie)
    child = other.create_window(width=10, height=10, parent=wid)
    assert _errors_of(other, struct.pack("<BxHI", X_MAP_WINDOW, 2, child)) == []
    return embedder, wid, other, child


def _copy(conn, src, mode):
    pixmap = conn.create_pixmap(50, 50, drawable=src)
    gc = conn.alloc_id()
    errs = _errors_of(conn, struct.pack("<BxHIIII", X_CREATE_GC, 5, gc, pixmap,
                                        GC_SUBWINDOW_MODE, mode))
    return errs + _errors_of(conn, struct.pack("<BxHIIIhhhhHH", X_COPY_AREA, 7, src, pixmap,
                                               gc, 0, 0, 0, 0, 50, 50))


RENDER_QUERY_VERSION, RENDER_QUERY_PICT_FORMATS = 0, 1
RENDER_CREATE_PICTURE, RENDER_COMPOSITE = 4, 8
CP_SUBWINDOW_MODE = 1 << 8
PICT_OP_SRC = 1


def _render_copy(conn, src, mode):
    """Render Composite from a picture of window *src* with *mode*"""
    from proto import render
    op = conn.query_extension("RENDER").opcode
    conn.send_request(render.QueryVersionRequest(opcode=op))
    conn.recv_response(timeout=5.0)
    conn.send_request(render.QueryPictFormatsRequest(opcode=op))
    reply = render.QueryPictFormatsReply.from_reply(conn.recv_response(timeout=5.0).data, "<")
    fmt = next(f.id for f in reply.formats
               if f.type == 1 and f.depth == conn.root_depth and f.alpha_mask == 0)
    pixmap = conn.create_pixmap(50, 50, drawable=src)
    spic, dpic = conn.alloc_id(), conn.alloc_id()
    errs = _errors_of(conn, struct.pack("<BBHIIIII", op, RENDER_CREATE_PICTURE, 6, spic, src,
                                        fmt, CP_SUBWINDOW_MODE, mode))
    errs += _errors_of(conn, struct.pack("<BBHIIII", op, RENDER_CREATE_PICTURE, 5, dpic, pixmap,
                                         fmt, 0))
    return errs + _errors_of(conn, struct.pack("<BBHB3xIIIhhhhhhHH", op, RENDER_COMPOSITE, 9,
                                               PICT_OP_SRC, spic, 0, dpic, 0, 0, 0, 0, 0, 0,
                                               50, 50))


class TestInferiors:
    """Reading one's own window with its inferiors reads the windows
    other clients made inside it: capture.window (gap G1)."""

    @pytest.fixture
    def capmode(self):
        return "enforce"

    def test_get_image_with_another_clients_child(self, xserver):
        embedder, wid, other, child = _embedding(xserver, SANDBOX_COOKIE)
        assert _get_image(embedder, wid) == [BAD_ACCESS]
        # its own children are its own
        own = embedder.create_window(width=10, height=10, parent=wid, x=20)
        assert _errors_of(embedder, struct.pack("<BxHI", X_MAP_WINDOW, 2, own)) == []
        assert _get_image(embedder, own) == []
        # and with the other's child unmapped, nothing else shows
        assert _errors_of(other, struct.pack("<BxHI", 10, 2, child)) == []   # UnmapWindow
        assert _get_image(embedder, wid) == []

    def test_copy_area(self, xserver):
        embedder, wid, other, child = _embedding(xserver, SANDBOX_COOKIE)
        assert _copy(embedder, wid, CLIP_BY_CHILDREN) == []
        assert _copy(embedder, wid, INCLUDE_INFERIORS) == [BAD_ACCESS]

    def test_render_source(self, xserver):
        embedder, wid, other, child = _embedding(xserver, SANDBOX_COOKIE)
        assert _render_copy(embedder, wid, CLIP_BY_CHILDREN) == []
        assert _render_copy(embedder, wid, INCLUDE_INFERIORS) == [BAD_ACCESS]

    def test_unconfined(self, xserver):
        embedder, wid, other, child = _embedding(xserver, ROOT_COOKIE)
        assert _get_image(embedder, wid) == []
        assert _copy(embedder, wid, INCLUDE_INFERIORS) == []
        assert _render_copy(embedder, wid, INCLUDE_INFERIORS) == []


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
