# SPDX-License-Identifier: MIT
#
# Flatpak applications connect with the session's socket and cookie; the
# server recognises them by their cgroup and moves them out of the root
# namespace (Xext/namespace/classify.c, "classify" in the config). A
# systemd user scope named like Flatpak's stands in for a real Flatpak.

import os
import socket
import struct
import subprocess
import sys
import time
import uuid

import pytest
from xclient import RawX11Connection, X11Error
from test_xns_listen import COOKIE_PROTO, ROOT_COOKIE, _manager, _client_info

EDITOR_ID = "org.example.XPrismEditor"
BOXED_COOKIE = bytes.fromhex("55555555555555555555555555555555")

CHILD = """
import sys, struct
from xclient import RawX11Connection
conn = RawX11Connection(int(sys.argv[1]), auth=(bytes.fromhex(sys.argv[2]), bytes.fromhex(sys.argv[3])))
pixmap = conn.create_pixmap(1, 1)
wid = conn.create_window()
conn.flush_responses(timeout=0.2)
print(pixmap, wid, flush=True)
sys.stdin.read()
"""


@pytest.fixture
def xserver_args(tmp_path):
    conf = tmp_path / "ns.conf"
    conf.write_text(
        f"auth {COOKIE_PROTO.decode()} {ROOT_COOKIE.hex()}\n"
        "namespace flatpak root\n"
        "  classify flatpak\n"
        "  grant clipboard.read window.inspect\n"
        "namespace editor root\n"
        f"  classify flatpak {EDITOR_ID}\n"
        "namespace boxed root\n"
        f"  auth {COOKIE_PROTO.decode()} {BOXED_COOKIE.hex()}\n"
    )
    return ["-namespace", str(conf)]


def _scopes_work():
    try:
        r = subprocess.run(["systemd-run", "--user", "--scope", "--quiet",
                            f"--unit=xprism-probe-{uuid.uuid4().hex[:8]}.scope", "true"],
                           capture_output=True, timeout=20)
        return r.returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


needs_scopes = pytest.mark.skipif(not _scopes_work(),
                                  reason="no systemd user manager for transient scopes")


class Child:
    """an X client in the scope *unit*"""

    def __init__(self, xserver, unit, cookie=ROOT_COOKIE):
        env = dict(os.environ, PYTHONPATH=os.path.dirname(os.path.abspath(__file__)))
        cmd = [sys.executable, "-c", CHILD, str(xserver.display_num),
               COOKIE_PROTO.hex(), cookie.hex()]
        if unit:
            cmd = ["systemd-run", "--user", "--scope", "--quiet", f"--unit={unit}"] + cmd
        self.proc = subprocess.Popen(cmd, env=env, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, text=True)
        line = self.proc.stdout.readline().split()
        assert len(line) == 2, "child client did not start"
        self.pixmap, self.window = map(int, line)

    def close(self):
        self.proc.stdin.close()
        self.proc.wait(timeout=10)


def _flatpak(xserver, app_id):
    return Child(xserver, f"app-flatpak-{app_id}-{uuid.uuid4().int % 100000}.scope")


def _info(xserver, child):
    manager, opcode = _manager(xserver)
    manager.send_request(struct.pack("<BBHI", opcode, 12, 2, child.pixmap))
    r = manager.recv_response(timeout=5.0)
    assert r is not None and not isinstance(r, X11Error), r
    lens = struct.unpack_from("<HHHH", r.data, 16)
    off, strs = 32, []
    for n in lens:
        strs.append(r.data[off: off + n].decode())
        off += n + (-n % 4)
    return strs     # [namespace, app id, instance id, engine]


@needs_scopes
class TestClassify:
    def test_each_app_gets_a_namespace(self, xserver):
        a = _flatpak(xserver, "org.example.AppA")
        b = _flatpak(xserver, "org.example.AppB")
        a2 = _flatpak(xserver, "org.example.AppA")
        try:
            assert _info(xserver, a) == ["flatpak.org.example.AppA", "org.example.AppA",
                                         "", "flatpak"]
            assert _info(xserver, b)[0] == "flatpak.org.example.AppB"
            assert _info(xserver, a2)[0] == "flatpak.org.example.AppA"
        finally:
            for c in (a, b, a2):
                c.close()

    def test_exact_rule_wins(self, xserver):
        c = _flatpak(xserver, EDITOR_ID)
        try:
            assert _info(xserver, c)[:2] == ["editor", EDITOR_ID]
        finally:
            c.close()

    def test_a_namespace_cookie_decides(self, xserver):
        """only clients that would be in root are classified"""
        c = Child(xserver, f"app-flatpak-org.example.AppA-{uuid.uuid4().int % 100000}.scope",
                  cookie=BOXED_COOKIE)
        try:
            assert _info(xserver, c)[:2] == ["boxed", ""]
        finally:
            c.close()

    def test_other_scopes_stay_in_root(self, xserver):
        c = Child(xserver, f"app-gnome-org.example.Term-{uuid.uuid4().int % 100000}.scope")
        try:
            assert _info(xserver, c)[0] == "root"
        finally:
            c.close()

    def test_apps_are_confined(self, xserver):
        """an app sees neither the session's windows nor another app's, but
        its own (the template's window.inspect grant came along)"""
        session = RawX11Connection(xserver.display_num, auth=(COOKIE_PROTO, ROOT_COOKIE))
        swid = session.create_window()
        session.flush_responses(timeout=0.2)
        a = _flatpak(xserver, "org.example.AppA")
        b = _flatpak(xserver, "org.example.AppB")
        try:
            # the probe runs inside app A's sandbox scope
            unit = f"app-flatpak-org.example.AppA-{uuid.uuid4().int % 100000}.scope"
            probe = subprocess.run(
                ["systemd-run", "--user", "--scope", "--quiet", f"--unit={unit}",
                 sys.executable, "-c",
                 "import sys, struct\n"
                 "from xclient import RawX11Connection, X11Error\n"
                 "c = RawX11Connection(int(sys.argv[1]), auth=(bytes.fromhex(sys.argv[2]), bytes.fromhex(sys.argv[3])))\n"
                 "for w in sys.argv[4:]:\n"
                 "    c.send_request(struct.pack('<BxHI', 3, 2, int(w)))\n"
                 "    r = c.recv_response(timeout=5)\n"
                 "    print('denied' if isinstance(r, X11Error) else 'seen')\n",
                 str(xserver.display_num), COOKIE_PROTO.hex(), ROOT_COOKIE.hex(),
                 str(swid), str(b.window), str(a.window)],
                env=dict(os.environ, PYTHONPATH=os.path.dirname(os.path.abspath(__file__))),
                capture_output=True, text=True, timeout=30)
            assert probe.stdout.split() == ["denied", "denied", "seen"], probe.stderr
        finally:
            a.close()
            b.close()


def _pidfd_supported():
    s1, s2 = socket.socketpair()
    try:
        s1.getsockopt(socket.SOL_SOCKET, 77, 4)    # SO_PEERPIDFD
        return True
    except OSError:
        return False
    finally:
        s1.close()
        s2.close()


class _Adopted(RawX11Connection):
    """connection setup over a socket someone else connected"""

    def __init__(self, sock, display_num, auth):
        self._adopt = sock
        super().__init__(display_num, auth=auth)

    def _connect(self):
        self.sock = self._adopt
        self._peer_pid = None
        self._handshake()


@pytest.mark.skipif(not _pidfd_supported(), reason="no SO_PEERPIDFD")
def test_gone_connector_is_not_trusted(xserver):
    """A sandbox could connect, hand the socket on and exit, so that its
    pid describes some other process by the time the server looks. The
    server sees through the pidfd that the connecting process is gone and
    trusts nothing: the client gets the anonymous namespace."""
    parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
    pid = os.fork()
    if pid == 0:
        try:
            x = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            x.connect(f"/tmp/.X11-unix/X{xserver.display_num}")
            socket.send_fds(child, [b"x"], [x.fileno()])
        finally:
            os._exit(0)
    child.close()
    _, fds, _, _ = socket.recv_fds(parent, 1, 1)
    os.waitpid(pid, 0)
    time.sleep(0.1)
    conn = _Adopted(socket.socket(fileno=fds[0]), xserver.display_num,
                    (COOKIE_PROTO, ROOT_COOKIE))
    manager, opcode = _manager(xserver)
    assert _client_info(manager, opcode, conn)[1][0] == "anon"
