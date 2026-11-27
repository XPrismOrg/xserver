# SPDX-License-Identifier: MIT
#
# WX11 (wx11/, DECISIONS.md AD-3): Wayland toplevels as X windows. Runs
# wx11 against the test server and wx11-test-client against wx11.
# Skipped unless wx11 is built (cargo build in wx11/, or WX11_DIR set to
# its target directory).

import os
import shutil
import struct
import subprocess
import tempfile
import time
from pathlib import Path

import pytest
from proto import xtest
from xclient import RawX11Connection, X11Error

TARGET = Path(os.environ.get("WX11_DIR", Path(__file__).parents[2] / "wx11" / "target" / "debug"))
WX11, CLIENT = TARGET / "wx11", TARGET / "wx11-test-client"

BUILT = WX11.exists() and CLIENT.exists()
if not BUILT and os.environ.get("WX11_REQUIRED") == "1":
    raise RuntimeError(f"WX11_REQUIRED is set but wx11 is not built in {TARGET}")
pytestmark = pytest.mark.skipif(not BUILT, reason=f"wx11 not built ({TARGET})")

X_GET_WINDOW_ATTRIBUTES, X_GET_GEOMETRY, X_QUERY_TREE = 3, 14, 15
X_GET_PROPERTY, X_SEND_EVENT, X_CONFIGURE_WINDOW = 20, 25, 12
X_GET_IMAGE, X_GET_INPUT_FOCUS = 73, 43
X_SET_INPUT_FOCUS = 42
CLIENT_MESSAGE = 33
KEY_PRESS, KEY_RELEASE, BUTTON_PRESS, BUTTON_RELEASE, MOTION_NOTIFY = 2, 3, 4, 5, 6
# X keycodes (evdev + 8) and the evdev codes Wayland clients get
X_KEY_A, EV_KEY_A = 38, 30
X_SHIFT_L = 50
Z_PIXMAP = 2
CW_WIDTH, CW_HEIGHT = 1 << 2, 1 << 3
CW_STACK_MODE, STACK_ABOVE = 1 << 6, 0
CW_X, CW_Y = 1 << 0, 1 << 1


def _reply(conn, seq):
    while True:
        r = conn.recv_response(timeout=5.0)
        assert r is not None, "no answer from the server"
        assert not isinstance(r, X11Error), r
        if r.response_type == 1 and r.sequence == seq:
            return r


def _children(conn):
    r = _reply(conn, conn.send_request(struct.pack("<BxHI", X_QUERY_TREE, 2, conn.root_window)))
    n = struct.unpack_from("<H", r.data, 16)[0]
    return list(struct.unpack_from(f"<{n}I", r.data, 32))


def _property(conn, win, atom, type_=0):
    r = _reply(conn, conn.send_request(struct.pack("<BBHIIIII", X_GET_PROPERTY, 0, 6, win, atom,
                                                   type_, 0, 1024)))
    n = struct.unpack_from("<I", r.data, 16)[0]
    return r.data[32:32 + n]


def _geometry(conn, win):
    r = _reply(conn, conn.send_request(struct.pack("<BxHI", X_GET_GEOMETRY, 2, win)))
    return struct.unpack_from("<HH", r.data, 16)


def _pixel(conn, win, x, y):
    r = _reply(conn, conn.send_request(struct.pack("<BBHIhhHHI", X_GET_IMAGE, Z_PIXMAP, 5, win,
                                                   x, y, 1, 1, 0xFFFFFFFF)))
    return struct.unpack_from("<I", r.data, 32)[0] & 0xFFFFFF


def _window_named(conn, title, timeout=5.0):
    """the window whose WM_NAME is title (str, or bytes as stored)"""
    want = title if isinstance(title, bytes) else title.encode()
    wm_name = conn.intern_atom("WM_NAME")
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        for w in _children(conn):
            if _property(conn, w, wm_name) == want:
                return w
        time.sleep(0.05)
    return None


class Proc:
    """a process whose stdout lines we wait for"""

    def __init__(self, args, env):
        self.p = subprocess.Popen(args, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.buf = b""
        self.pending = []           # lines read while waiting for another

    def line(self, timeout=10.0):
        """the next stdout line, or None after timeout or at the end.
        Reads the pipe directly: a buffered readline() could hold lines
        that select() then no longer sees."""
        import select
        end = time.monotonic() + timeout
        while b"\n" not in self.buf:
            left = end - time.monotonic()
            if left <= 0 or not select.select([self.p.stdout], [], [], left)[0]:
                return None
            chunk = os.read(self.p.stdout.fileno(), 4096)
            if not chunk:
                return None
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return line.decode().strip()

    def wait_for(self, want, timeout=10.0):
        """wait for the line want; lines read before it stay pending, so
        that a later wait_for still finds them"""
        if want in self.pending:
            self.pending.remove(want)
            return
        end = time.monotonic() + timeout
        seen = []
        while time.monotonic() < end:
            line = self.line(end - time.monotonic())
            if line is None:
                break
            if line == want:
                self.pending.extend(seen)
                return
            seen.append(line)
        self.pending.extend(seen)
        raise AssertionError(f"no {want!r} (saw {self.pending}; stderr: {self.stderr()})")

    def heard(self, timeout=0.5):
        """every line pending or arriving within timeout"""
        out, self.pending = self.pending, []
        while (line := self.line(timeout)) is not None:
            out.append(line)
        return out

    def stderr(self):
        if self.p.poll() is None:
            return "(running)"
        return self.p.stderr.read().decode(errors="replace")

    def stop(self):
        if self.p.poll() is None:
            self.p.terminate()
            try:
                self.p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.p.kill()


@pytest.fixture
def wx11(xserver):
    """wx11 on the test server; yields the environment for clients"""
    runtime = tempfile.mkdtemp(prefix="wx11-")      # short: AF_UNIX paths
    env = dict(os.environ, DISPLAY=f":{xserver.display_num}", XDG_RUNTIME_DIR=runtime)
    env.pop("WAYLAND_DISPLAY", None)
    proc = Proc([str(WX11)], env)
    line = proc.line()
    assert line and line.startswith("WAYLAND_DISPLAY="), (line, proc.stderr())
    env["WAYLAND_DISPLAY"] = line.split("=", 1)[1]
    yield env
    proc.stop()
    shutil.rmtree(runtime, ignore_errors=True)


@pytest.fixture
def client(wx11):
    procs = []

    def start(*args):
        p = Proc([str(CLIENT), *args], wx11)
        procs.append(p)
        return p
    yield start
    for p in procs:
        p.stop()


def test_toplevel_is_an_x_window(xserver, client):
    c = client("--title", "Hello WX11", "--app-id", "org.example.Hello",
               "--color", "00ff00", "--size", "64x48")
    c.wait_for("drawn 64 48")
    conn = RawX11Connection(xserver.display_num)
    win = _window_named(conn, "Hello WX11")
    assert win, "no X window for the toplevel"
    assert _geometry(conn, win) == (64, 48)
    assert _property(conn, win, conn.intern_atom("_NET_WM_NAME")) == b"Hello WX11"
    assert _property(conn, win, conn.intern_atom("WM_CLASS")) == \
        b"org.example.Hello\0org.example.Hello\0"
    assert _pixel(conn, win, 10, 10) == 0x00ff00


def test_unicode_title(xserver, client):
    c = client("--title", "Zażółć ✓")
    c.wait_for("drawn 64 48")
    conn = RawX11Connection(xserver.display_num)
    # WM_NAME is Latin-1 (ó is, ż ł ć ✓ become "?"); _NET_WM_NAME UTF-8
    win = _window_named(conn, b"Za?\xf3?? ?")
    assert win
    assert _property(conn, win, conn.intern_atom("_NET_WM_NAME")) == "Zażółć ✓".encode()


def test_window_manager_resizes(xserver, client):
    c = client("--title", "resize me")
    c.wait_for("drawn 64 48")
    conn = RawX11Connection(xserver.display_num)
    win = _window_named(conn, "resize me")
    conn.send_request(struct.pack("<BxHIHxxII", X_CONFIGURE_WINDOW, 5, win,
                                  CW_WIDTH | CW_HEIGHT, 120, 90))
    c.wait_for("configure 120 90")
    c.wait_for("drawn 120 90")
    assert _geometry(conn, win) == (120, 90)
    assert _pixel(conn, win, 110, 80) == 0xff0000


def test_window_manager_closes(xserver, client):
    c = client("--title", "close me")
    c.wait_for("drawn 64 48")
    conn = RawX11Connection(xserver.display_num)
    win = _window_named(conn, "close me")
    wm_protocols = conn.intern_atom("WM_PROTOCOLS")
    wm_delete = conn.intern_atom("WM_DELETE_WINDOW")
    event = struct.pack("<BBHII5I", CLIENT_MESSAGE, 32, 0, win, wm_protocols, wm_delete, 0, 0, 0, 0)
    conn.send_request(struct.pack("<BBHII", X_SEND_EVENT, 0, 11, win, 0) + event)
    c.wait_for("closed")
    c.p.wait(timeout=5)
    end = time.monotonic() + 5
    while win in _children(conn) and time.monotonic() < end:
        time.sleep(0.05)
    assert win not in _children(conn)


def test_two_clients_two_windows(xserver, client):
    a = client("--title", "first", "--color", "0000ff")
    b = client("--title", "second", "--color", "ffff00")
    a.wait_for("drawn 64 48")
    b.wait_for("drawn 64 48")
    conn = RawX11Connection(xserver.display_num)
    wa, wb = _window_named(conn, "first"), _window_named(conn, "second")
    assert wa and wb and wa != wb
    # both open at 0, 0: raising one covers the other. Without a compositor
    # X then loses the covered contents, and WX11 must repaint on Expose
    for win, color in [(wa, 0x0000ff), (wb, 0xffff00), (wa, 0x0000ff)]:
        conn.send_request(struct.pack("<BxHIHxxI", X_CONFIGURE_WINDOW, 4, win,
                                      CW_STACK_MODE, STACK_ABOVE))
        end = time.monotonic() + 5
        while _pixel(conn, win, 5, 5) != color and time.monotonic() < end:
            time.sleep(0.05)
        assert _pixel(conn, win, 5, 5) == color


class Input:
    """XTest input from a separate (unconfined) client"""

    def __init__(self, xserver):
        self.c = RawX11Connection(xserver.display_num)
        self.op = self.c.query_extension("XTEST").opcode

    def fake(self, type_, detail=0, x=0, y=0):
        self.c.send_request(xtest.FakeInputRequest(opcode=self.op, type=type_, detail=detail,
                                                   root=self.c.root_window, root_x=x, root_y=y))

    def key(self, code):
        self.fake(KEY_PRESS, code)
        self.fake(KEY_RELEASE, code)

    def focus(self, win):
        self.c.send_request(struct.pack("<BBHII", X_SET_INPUT_FOCUS, 1, 3, win, 0))
        _reply(self.c, self.c.send_request(struct.pack("<BxH", X_GET_INPUT_FOCUS, 1)))


def _shown(xserver, client, title, *args):
    c = client("--title", title, *args)
    c.wait_for("drawn 64 48")
    conn = RawX11Connection(xserver.display_num)
    win = _window_named(conn, title)
    assert win
    return c, win


def test_keyboard_follows_x_focus(xserver, client):
    c, win = _shown(xserver, client, "typing")
    inp = Input(xserver)
    inp.focus(win)
    c.wait_for("keyboard enter")
    inp.key(X_KEY_A)
    c.wait_for(f"key {EV_KEY_A} pressed")
    c.wait_for(f"key {EV_KEY_A} released")


def test_keymap_and_modifiers(xserver, client):
    c, win = _shown(xserver, client, "shift")
    c.wait_for("keymap")
    inp = Input(xserver)
    inp.focus(win)
    inp.fake(KEY_PRESS, X_SHIFT_L)
    c.wait_for("modifiers 1")              # Shift is the first modifier
    inp.fake(KEY_RELEASE, X_SHIFT_L)
    c.wait_for("modifiers 0")


def test_keys_go_to_the_focused_toplevel_only(xserver, client):
    a, wa = _shown(xserver, client, "left")
    b, wb = _shown(xserver, client, "right")
    inp = Input(xserver)
    inp.focus(wa)
    a.wait_for("keyboard enter")
    inp.focus(wb)
    a.wait_for("keyboard leave")
    b.wait_for("keyboard enter")
    inp.key(X_KEY_A)
    b.wait_for(f"key {EV_KEY_A} pressed")
    # a heard no key after leaving
    assert not [line for line in a.heard() if line.startswith("key ")]


def test_pointer_motion_buttons_and_wheel(xserver, client):
    c, win = _shown(xserver, client, "pointing")
    inp = Input(xserver)
    inp.fake(MOTION_NOTIFY, x=10, y=20)     # the window is at 0,0
    c.wait_for("pointer enter 10 20")
    inp.fake(MOTION_NOTIFY, x=30, y=40)
    c.wait_for("motion 30 40")
    inp.fake(BUTTON_PRESS, 1)
    c.wait_for("button 0x110 pressed")      # BTN_LEFT
    inp.fake(BUTTON_RELEASE, 1)
    c.wait_for("button 0x110 released")
    inp.fake(BUTTON_PRESS, 3)
    inp.fake(BUTTON_RELEASE, 3)
    c.wait_for("button 0x111 pressed")      # BTN_RIGHT
    inp.fake(BUTTON_PRESS, 5)
    inp.fake(BUTTON_RELEASE, 5)
    c.wait_for("axis vertical 15")          # a wheel step down
    inp.fake(MOTION_NOTIFY, x=500, y=500)   # off the window
    c.wait_for("pointer leave")


def test_focus_to_an_x_window_leaves_wayland(xserver, client):
    c, win = _shown(xserver, client, "losing focus")
    inp = Input(xserver)
    inp.focus(win)
    c.wait_for("keyboard enter")
    inp.focus(inp.c.root_window)        # focus leaves for a non-Wayland window
    c.wait_for("keyboard leave")
    inp.key(X_KEY_A)
    assert not [line for line in c.heard() if line.startswith("key ")]


def _place(conn, win):
    """x, y, width, height of win"""
    r = _reply(conn, conn.send_request(struct.pack("<BxHI", X_GET_GEOMETRY, 2, win)))
    return struct.unpack_from("<hhHH", r.data, 12)


def _override_redirect(conn, win):
    r = _reply(conn, conn.send_request(struct.pack("<BxHI", X_GET_WINDOW_ATTRIBUTES, 2, win)))
    return r.data[27] == 1


def _popup_window(conn, host, timeout=5.0):
    """the window shown next to host (the popup)"""
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        others = [w for w in _children(conn) if w != host and _override_redirect(conn, w)]
        if others:
            return others[-1]
        time.sleep(0.05)
    return None


def test_popup_is_an_override_redirect_window(xserver, client):
    c, host = _shown(xserver, client, "menu host", "--popup", "30x20+10+5",
                     "--popup-color", "00ffff")
    c.wait_for("popup configure 10 5 30 20")
    c.wait_for("popup drawn")
    conn = RawX11Connection(xserver.display_num)
    popup = _popup_window(conn, host)
    assert popup, "no X window for the popup"
    assert not _override_redirect(conn, host)
    assert _place(conn, popup) == (10, 5, 30, 20)     # the host is at 0, 0
    assert _pixel(conn, popup, 3, 3) == 0x00ffff


def test_grabbing_popup_gets_input_and_closes_on_click_outside(xserver, client):
    c, host = _shown(xserver, client, "menu", "--popup", "30x20+10+5", "--grab")
    inp = Input(xserver)
    inp.fake(MOTION_NOTIFY, x=5, y=5)
    c.wait_for("pointer enter 5 5")
    inp.fake(BUTTON_PRESS, 1)               # opens the menu, grabbing
    inp.fake(BUTTON_RELEASE, 1)
    c.wait_for("popup drawn")
    c.wait_for("keyboard enter popup")
    conn = RawX11Connection(xserver.display_num)
    popup = _popup_window(conn, host)
    assert popup
    inp.fake(MOTION_NOTIFY, x=20, y=10)     # into the popup
    c.wait_for("pointer enter popup 10 5")
    inp.fake(BUTTON_PRESS, 1)               # a click inside keeps it open
    inp.fake(BUTTON_RELEASE, 1)
    c.wait_for("button 0x110 released")
    assert "popup done" not in c.heard()
    inp.fake(MOTION_NOTIFY, x=300, y=300)   # off every window
    inp.fake(BUTTON_PRESS, 1)
    inp.fake(BUTTON_RELEASE, 1)
    c.wait_for("popup done")
    end = time.monotonic() + 5
    while popup in _children(conn) and time.monotonic() < end:
        time.sleep(0.05)
    assert popup not in _children(conn)
    c.wait_for("keyboard enter")            # back to the window


def _clipboard_text(conn, target_name):
    """paste CLIPBOARD as target_name from an X client"""
    from test_clipboard_grants import _received
    win = conn.create_window()
    clip, target = conn.intern_atom("CLIPBOARD"), conn.intern_atom(target_name)
    prop = conn.intern_atom("WX11_TEST")
    conn.send_request(struct.pack("<BxHIIIII", 24, 6, win, clip, target, prop, 0))
    return _received(conn, win, prop)


def _wait_owner(conn, timeout=5.0):
    from test_clipboard_grants import _owner
    clip = conn.intern_atom("CLIPBOARD")
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if _owner(conn, clip):
            return True
        time.sleep(0.05)
    return False


def test_wayland_copy_pastes_in_x(xserver, client):
    c, win = _shown(xserver, client, "copier", "--copy", "hello from wayland ✓")
    Input(xserver).focus(win)
    c.wait_for("copied")
    conn = RawX11Connection(xserver.display_num)
    assert _wait_owner(conn), "WX11 did not take CLIPBOARD"
    assert _clipboard_text(conn, "UTF8_STRING") == "hello from wayland ✓".encode()
    c.wait_for("sent")
    # STRING is Latin-1: ✓ is not in it
    assert _clipboard_text(conn, "STRING") == b"hello from wayland ?"


def test_x_copy_pastes_in_wayland(xserver, client):
    from test_clipboard_grants import _answer, _set_owner
    owner = RawX11Connection(xserver.display_num)
    owner_win = owner.create_window()
    _set_owner(owner, owner_win, owner.intern_atom("CLIPBOARD"))
    c, win = _shown(xserver, client, "paster", "--paste")
    Input(xserver).focus(win)           # the focused client sees the clipboard
    assert _answer(owner, "hello from X ✓".encode()), "WX11 did not ask the X owner"
    c.wait_for("pasted hello from X ✓")


def test_x_copy_after_wayland_copy_takes_over(xserver, client):
    from test_clipboard_grants import _answer, _set_owner
    a, wa = _shown(xserver, client, "first copier", "--copy", "old")
    inp = Input(xserver)
    inp.focus(wa)
    a.wait_for("copied")
    conn = RawX11Connection(xserver.display_num)
    assert _wait_owner(conn)
    owner_win = conn.create_window()
    _set_owner(conn, owner_win, conn.intern_atom("CLIPBOARD"))   # an X client copies
    b, wb = _shown(xserver, client, "second paster", "--paste")
    inp.focus(wb)
    assert _answer(conn, b"new"), "WX11 kept offering the old Wayland text"
    b.wait_for("pasted new")


def _depth(conn, win):
    r = _reply(conn, conn.send_request(struct.pack("<BxHI", X_GET_GEOMETRY, 2, win)))
    return r.data[1]


def _argb(conn, win, x, y):
    r = _reply(conn, conn.send_request(struct.pack("<BBHIhhHHI", X_GET_IMAGE, Z_PIXMAP, 5, win,
                                                   x, y, 1, 1, 0xFFFFFFFF)))
    return struct.unpack_from("<I", r.data, 32)[0]


def test_alpha_buffer_gets_an_argb_window(xserver, client):
    # half-transparent red, premultiplied
    c, win = _shown(xserver, client, "glass", "--alpha", "--color", "80800000")
    conn = RawX11Connection(xserver.display_num)
    assert _depth(conn, win) == 32
    assert _argb(conn, win, 5, 5) == 0x80800000


def test_opaque_buffer_keeps_the_screen_depth(xserver, client):
    c, win = _shown(xserver, client, "solid", "--color", "00ff00")
    conn = RawX11Connection(xserver.display_num)
    assert _depth(conn, win) == 24
    assert _pixel(conn, win, 5, 5) == 0x00ff00


def test_each_wayland_client_is_its_own_x_client(xserver, client):
    """XPrism must see every Wayland application as a client of its own
    (permissions, prompts, audit), not all of them as WX11"""
    a, wa = _shown(xserver, client, "app one")
    b, wb = _shown(xserver, client, "app two")
    conn = RawX11Connection(xserver.display_num)
    mask = conn._resource_id_mask
    assert wa & ~mask != wb & ~mask, "both windows belong to one X client"
    net_wm_pid = conn.intern_atom("_NET_WM_PID")
    assert _cardinal(conn, wa, net_wm_pid) == a.p.pid
    assert _cardinal(conn, wb, net_wm_pid) == b.p.pid


def _cardinal(conn, win, atom):
    """a 32-bit property's first value"""
    r = _reply(conn, conn.send_request(struct.pack("<BBHIIIII", X_GET_PROPERTY, 0, 6, win, atom,
                                                   0, 0, 1)))
    assert r.data[1] == 32, "not a 32-bit property"
    return struct.unpack_from("<I", r.data, 32)[0]


def test_killed_client_takes_its_x_connection_along(xserver, client):
    conn = RawX11Connection(xserver.display_num)
    before = set(_children(conn))
    c, win = _shown(xserver, client, "doomed")
    # its X connection made the window and a selection window of its own
    assert len(set(_children(conn)) - before) == 2
    c.p.kill()                      # no xdg_toplevel.destroy: just gone
    end = time.monotonic() + 5
    while set(_children(conn)) != before and time.monotonic() < end:
        time.sleep(0.05)
    # both are gone: the connection was closed, not only the window
    assert set(_children(conn)) == before


class TestSandbox:
    """A client that comes through a wp_security_context_v1 listener gets
    its X connection on --sandbox-display: here an xns-listen socket, so it
    lands in that XPrism namespace."""

    @pytest.fixture
    def xserver_args(self, tmp_path):
        from test_xns_listen import COOKIE_PROTO, ROOT_COOKIE
        conf = tmp_path / "ns.conf"
        conf.write_text(f"auth {COOKIE_PROTO.decode()} {ROOT_COOKIE.hex()}\n")
        return ["-namespace", str(conf)]

    def _namespace_of(self, manager, opcode, xid):
        from test_xns_listen import X_XNS_GET_CLIENT_INFO
        manager.send_request(struct.pack("<BBHI", opcode, X_XNS_GET_CLIENT_INFO, 2, xid))
        r = manager.recv_response(timeout=5.0)
        assert r is not None and not isinstance(r, X11Error), r
        ns_len, app_len = struct.unpack_from("<HH", r.data, 16)
        off = 32 + ns_len + (-ns_len % 4)
        return r.data[32:32 + ns_len].decode(), r.data[off:off + app_len].decode()

    def test_sandboxed_client_lands_in_the_namespace(self, xserver, tmp_path):
        from test_xns_listen import _manager, _start, _xauth_file
        os.environ.setdefault("XSERVER_BUILDDIR", str(Path(__file__).parents[2] / "build"))
        env = dict(os.environ, DISPLAY=f":{xserver.display_num}",
                   XAUTHORITY=_xauth_file(tmp_path / "xauth", xserver.display_num))
        sock = tmp_path / "sandbox.sock"
        xns = _start(["-n", "sandbox1", "--create", "--app-id", "org.example.Boxed",
                      str(sock)], env)
        runtime = tempfile.mkdtemp(prefix="wx11-")
        env["XDG_RUNTIME_DIR"] = runtime
        env.pop("WAYLAND_DISPLAY", None)
        procs = [Proc([str(WX11), "--sandbox-display", str(sock)], env)]
        try:
            line = procs[0].line()
            assert line and line.startswith("WAYLAND_DISPLAY="), (line, procs[0].stderr())
            env["WAYLAND_DISPLAY"] = line.split("=", 1)[1]
            boxed = Proc([str(CLIENT), "--title", "boxed",
                          "--security-context", "org.example.Boxed"], env)
            free = Proc([str(CLIENT), "--title", "free"], env)
            procs += [boxed, free]
            boxed.wait_for("drawn 64 48")
            free.wait_for("drawn 64 48")

            manager, opcode = _manager(xserver)
            boxed_win, free_win = _window_named(manager, "boxed"), _window_named(manager, "free")
            if not (boxed_win and free_win):
                procs[0].stop()
                pytest.fail(f"windows {boxed_win} {free_win}; wx11: {procs[0].stderr()}")
            assert self._namespace_of(manager, opcode, boxed_win) == \
                ("sandbox1", "org.example.Boxed")
            assert self._namespace_of(manager, opcode, free_win)[0] == "root"
        finally:
            for p in reversed(procs):
                p.stop()
            xns.terminate()
            xns.wait(timeout=5)
            shutil.rmtree(runtime, ignore_errors=True)


def _selection_text(conn, selection, target_name="UTF8_STRING"):
    """paste selection (an atom name) as target_name from an X client"""
    from test_clipboard_grants import _received
    win = conn.create_window()
    sel, target = conn.intern_atom(selection), conn.intern_atom(target_name)
    prop = conn.intern_atom("WX11_TEST")
    conn.send_request(struct.pack("<BxHIIIII", 24, 6, win, sel, target, prop, 0))
    return _received(conn, win, prop)


def test_wayland_primary_selection_pastes_in_x(xserver, client):
    c, win = _shown(xserver, client, "selecting", "--primary", "--copy", "selected text")
    Input(xserver).focus(win)
    c.wait_for("copied")
    conn = RawX11Connection(xserver.display_num)
    from test_clipboard_grants import _owner
    end = time.monotonic() + 5
    while not _owner(conn, 1) and time.monotonic() < end:       # PRIMARY is atom 1
        time.sleep(0.05)
    assert _selection_text(conn, "PRIMARY") == b"selected text"
    # the clipboard is another matter
    assert not _owner(conn, conn.intern_atom("CLIPBOARD"))


def test_x_primary_selection_pastes_in_wayland(xserver, client):
    from test_clipboard_grants import _answer, _set_owner
    owner = RawX11Connection(xserver.display_num)
    _set_owner(owner, owner.create_window(), 1)                 # PRIMARY
    c, win = _shown(xserver, client, "middle click", "--primary", "--paste")
    Input(xserver).focus(win)
    assert _answer(owner, b"from X primary"), "WX11 did not ask the PRIMARY owner"
    c.wait_for("pasted from X primary")


@pytest.mark.parametrize("slide", [True, False])
def test_menu_at_the_screen_edge(xserver, client, slide):
    """a 100-wide menu opened 60 px into a window 70 px from the right edge
    would end 90 px off screen: slid back if the client allows it"""
    args = ["--popup", "100x20+60+5", "--grab"] + (["--popup-slide"] if slide else [])
    c, host = _shown(xserver, client, "edge", *args)
    conn = RawX11Connection(xserver.display_num)
    screen_w, _ = _geometry(conn, conn.root_window)
    conn.send_request(struct.pack("<BxHIHxxii", X_CONFIGURE_WINDOW, 5, host,
                                  CW_X | CW_Y, screen_w - 70, 0))
    end = time.monotonic() + 5
    while _place(conn, host)[0] != screen_w - 70 and time.monotonic() < end:
        time.sleep(0.05)
    inp = Input(xserver)
    inp.fake(MOTION_NOTIFY, x=screen_w - 65, y=5)
    inp.fake(BUTTON_PRESS, 1)
    inp.fake(BUTTON_RELEASE, 1)
    c.wait_for("popup drawn")
    popup = _popup_window(conn, host)
    x, y, w, h = _place(conn, popup)
    assert (y, w, h) == (5, 100, 20)
    assert x == (screen_w - 100 if slide else screen_w - 10)
