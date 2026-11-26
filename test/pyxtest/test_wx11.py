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
    assert _pixel(conn, wb, 5, 5) == 0xffff00


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
