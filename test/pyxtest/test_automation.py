# SPDX-License-Identifier: MIT
#
# Scoped automation (doc/capabilities.md, dix/capability.c): a namespace
# with "inject-into app" may use XTest without input.inject, through a
# seat of its own, and only into the windows of namespace "app".

import struct
import time

import pytest
from proto import xtest
from xclient import Extension, RawX11Connection, X11Error

COOKIE = b"MIT-MAGIC-COOKIE-1"
ROOT, APP, BOT, OTHER = (bytes([n]) * 16 for n in (0x11, 0x22, 0x33, 0x44))

X_CREATE_WINDOW, X_CHANGE_WINDOW_ATTRIBUTES, X_MAP_WINDOW = 1, 2, 8
X_QUERY_POINTER, X_GET_INPUT_FOCUS = 38, 43
CW_EVENT_MASK = 1 << 11
KEY_PRESS_MASK, BUTTON_PRESS_MASK = 1 << 0, 1 << 2
EV_KEY_PRESS, EV_BUTTON_PRESS = 2, 4
XTEST_GRAB_CONTROL = 3
XI_QUERY_DEVICE = 48
XI_ALL_MASTER_DEVICES = 1
BAD_ACCESS = 10
KEYCODE_A = 38


@pytest.fixture
def xserver_args(tmp_path):
    conf = tmp_path / "ns.conf"
    conf.write_text(
        f"auth {COOKIE.decode()} {ROOT.hex()}\n"
        f"namespace app\n"
        f"  auth {COOKIE.decode()} {APP.hex()}\n"
        f"namespace bot\n"
        f"  auth {COOKIE.decode()} {BOT.hex()}\n"
        f"  inject-into app\n"
        f"namespace other\n"
        f"  auth {COOKIE.decode()} {OTHER.hex()}\n"
    )
    return ["-namespace", str(conf)]


def _connect(xserver, cookie):
    return RawX11Connection(xserver.display_num, auth=(COOKIE, cookie))


def _errors_of(conn, request):
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


def _window(conn, x, y, mask):
    """a mapped 100x100 window at x, y selecting mask"""
    wid = conn.create_window(x=x, y=y)
    assert _errors_of(conn, struct.pack("<BxHIII", X_CHANGE_WINDOW_ATTRIBUTES, 4, wid,
                                        CW_EVENT_MASK, mask)) == []
    assert _errors_of(conn, struct.pack("<BxHI", X_MAP_WINDOW, 2, wid)) == []
    return wid


def _events(conn, kind, timeout=0.5):
    out = []
    while True:
        r = conn.recv_response(timeout=timeout)
        if r is None:
            return out
        if not isinstance(r, X11Error) and (r.response_type & 0x7f) == kind:
            out.append(r)
            timeout = 0.2


class Bot:
    def __init__(self, xserver, cookie=BOT):
        self.c = _connect(xserver, cookie)
        ext = self.c.query_extension(Extension.XTEST)
        assert ext, "XTEST hidden"
        self.op = ext.opcode

    def fake(self, type, detail=0, x=0, y=0):
        return _errors_of(self.c, xtest.FakeInputRequest(
            opcode=self.op, type=type, detail=detail, root_x=x, root_y=y))

    def click(self, x, y):
        errs = self.fake(xtest.MotionNotify, x=x, y=y)
        errs += self.fake(xtest.ButtonPress, 1)
        errs += self.fake(xtest.ButtonRelease, 1)
        return errs


def _pointer(conn):
    seq = conn.send_request(struct.pack("<BxHI", X_QUERY_POINTER, 2, conn.root_window))
    while True:
        r = conn.recv_response(timeout=5.0)
        assert r is not None and not isinstance(r, X11Error), r
        if r.response_type == 1 and r.sequence == seq:
            return struct.unpack_from("<hh", r.data, 16)


def _masters(xserver):
    root = _connect(xserver, ROOT)
    op = root.query_extension(Extension.XI).opcode
    root.send_request(struct.pack("<BBHHH", op, 47, 2, 2, 2))      # XIQueryVersion 2.2
    root.recv_response(timeout=5.0)
    seq = root.send_request(struct.pack("<BBHHxx", op, XI_QUERY_DEVICE, 2,
                                        XI_ALL_MASTER_DEVICES))
    while True:
        r = root.recv_response(timeout=5.0)
        assert r is not None and not isinstance(r, X11Error), r
        if r.response_type == 1 and r.sequence == seq:
            return struct.unpack_from("<H", r.data, 8)[0]


def test_clicks_into_its_scope_with_its_own_pointer(xserver):
    app = _connect(xserver, APP)
    _window(app, 10, 10, BUTTON_PRESS_MASK)
    user = _pointer(_connect(xserver, ROOT))
    bot = Bot(xserver)
    assert bot.click(50, 50) == []
    assert len(_events(app, EV_BUTTON_PRESS)) == 1
    # the user's pointer stayed where it was
    assert _pointer(_connect(xserver, ROOT)) == user


def test_types_into_its_scope(xserver):
    app = _connect(xserver, APP)
    _window(app, 10, 10, KEY_PRESS_MASK)
    bot = Bot(xserver)
    assert bot.fake(xtest.MotionNotify, x=50, y=50) == []
    assert bot.fake(xtest.KeyPress, KEYCODE_A) == []
    assert bot.fake(xtest.KeyRelease, KEYCODE_A) == []
    (ev,) = _events(app, EV_KEY_PRESS)
    assert ev.data[1] == KEYCODE_A


def test_not_outside_its_scope(xserver):
    other = _connect(xserver, OTHER)
    _window(other, 300, 300, BUTTON_PRESS_MASK | KEY_PRESS_MASK)
    bot = Bot(xserver)
    assert bot.fake(xtest.MotionNotify, x=350, y=350) == []      # moving is free
    assert bot.fake(xtest.ButtonPress, 1) == [BAD_ACCESS]
    assert bot.fake(xtest.KeyPress, KEYCODE_A) == [BAD_ACCESS]
    assert _events(other, EV_BUTTON_PRESS) == []
    # nor the root, outside every window
    assert bot.fake(xtest.MotionNotify, x=600, y=10) == []
    assert bot.fake(xtest.ButtonPress, 1) == [BAD_ACCESS]


def test_no_grab_control(xserver):
    """XTest's GrabControl would let it ignore server grabs."""
    bot = Bot(xserver)
    assert _errors_of(bot.c, struct.pack("<BBHBxxx", bot.op, XTEST_GRAB_CONTROL, 2, 1)) \
        == [BAD_ACCESS]


def test_seat_lives_with_the_client(xserver):
    before = _masters(xserver)
    bot = Bot(xserver)
    assert bot.fake(xtest.MotionNotify, x=5, y=5) == []
    assert _masters(xserver) == before + 2              # a pointer and a keyboard
    bot.c.close()
    for _ in range(50):
        if _masters(xserver) == before:
            break
        time.sleep(0.05)
    assert _masters(xserver) == before


def test_the_user_goes_first(xserver):
    """While the user types, its clicks wait (here the user's input comes
    through the core XTest devices, as from an on-screen keyboard)."""
    app = _connect(xserver, APP)
    _window(app, 10, 10, BUTTON_PRESS_MASK)
    bot = Bot(xserver)
    assert bot.fake(xtest.MotionNotify, x=50, y=50) == []
    user = _connect(xserver, ROOT)
    uop = user.query_extension(Extension.XTEST).opcode
    assert _errors_of(user, xtest.FakeInputRequest(opcode=uop, type=xtest.KeyPress,
                                                   detail=KEYCODE_A)) == []
    assert _errors_of(user, xtest.FakeInputRequest(opcode=uop, type=xtest.KeyRelease,
                                                   detail=KEYCODE_A)) == []
    start = time.monotonic()
    assert bot.fake(xtest.ButtonPress, 1) == []
    waited = time.monotonic() - start
    assert bot.fake(xtest.ButtonRelease, 1) == []
    assert 0.3 < waited < 2.0, waited
    assert len(_events(app, EV_BUTTON_PRESS)) == 1


def test_without_a_scope_xtest_is_hidden(xserver):
    assert not _connect(xserver, OTHER).query_extension(Extension.XTEST)
    assert not _connect(xserver, APP).query_extension(Extension.XTEST)


def test_unconfined_clients_use_the_core_devices(xserver):
    root = _connect(xserver, ROOT)
    op = root.query_extension(Extension.XTEST).opcode
    before = _masters(xserver)
    assert _errors_of(root, xtest.FakeInputRequest(opcode=op, type=xtest.MotionNotify,
                                                   root_x=77, root_y=66)) == []
    assert _pointer(root) == (77, 66)
    assert _masters(xserver) == before


class TestAudit:
    """In audit mode nothing is refused: XTest works as unconfined."""

    @pytest.fixture
    def xserver_args(self, xserver_args):
        return xserver_args + ["-capmode", "audit"]

    def test_core_devices(self, xserver):
        bot = Bot(xserver)
        before = _masters(xserver)
        assert bot.fake(xtest.MotionNotify, x=33, y=44) == []
        assert _pointer(_connect(xserver, ROOT)) == (33, 44)
        assert _masters(xserver) == before


X_XNS_SET_NAMESPACE_INJECT_SCOPE = 16


def _set_scope(xserver, ns, target):
    mgr = _connect(xserver, ROOT)
    op = mgr.query_extension("X-NAMESPACE").opcode
    n, t = ns.encode(), target.encode()
    body = struct.pack("<HH", len(n), len(t)) + n + b"\0" * (-len(n) % 4) + t + \
        b"\0" * (-len(t) % 4)
    return _errors_of(mgr, struct.pack("<BBH", op, X_XNS_SET_NAMESPACE_INJECT_SCOPE,
                                       1 + len(body) // 4) + body)


def test_scope_set_at_run_time(xserver):
    app = _connect(xserver, APP)
    _window(app, 10, 10, BUTTON_PRESS_MASK)
    assert _set_scope(xserver, "other", "app") == []
    assert Bot(xserver, OTHER).click(50, 50) == []
    assert len(_events(app, EV_BUTTON_PRESS)) == 1
    # and taken back
    assert _set_scope(xserver, "other", "") == []
    assert not _connect(xserver, OTHER).query_extension(Extension.XTEST)
    assert _set_scope(xserver, "root", "app") == [BAD_ACCESS]
    assert _set_scope(xserver, "nosuch", "app") == [15]           # BadName


def test_xns_listen_inject_into(xserver, tmp_path):
    import os
    import subprocess
    from test_xns_listen import _tool, _xauth_file
    env = dict(os.environ, DISPLAY=f":{xserver.display_num}",
               XAUTHORITY=_xauth_file(tmp_path / "xauth", xserver.display_num))
    sock = tmp_path / "bot.sock"
    proc = subprocess.Popen([_tool(), "-n", "bot2", "--create", "--inject-into", "app",
                             str(sock)], env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True)
    try:
        assert proc.stdout.readline().strip() == "ready", proc.stderr.read()
        app = _connect(xserver, APP)
        _window(app, 10, 10, BUTTON_PRESS_MASK)
        bot = Bot.__new__(Bot)
        bot.c = RawX11Connection(xserver.display_num, path=str(sock))
        bot.op = bot.c.query_extension(Extension.XTEST).opcode
        assert bot.click(50, 50) == []
        assert len(_events(app, EV_BUTTON_PRESS)) == 1
    finally:
        proc.terminate()
        proc.wait(timeout=10)
