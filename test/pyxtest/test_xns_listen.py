# SPDX-License-Identifier: MIT
#
# End-to-end tests for utils/xns-listen, the launcher-side helper for
# X-NAMESPACE listeners.

import os
import signal
import struct
import subprocess
import sys
import textwrap
from pathlib import Path

import pytest
from xclient import RawX11Connection, X11Error

COOKIE_PROTO = b"MIT-MAGIC-COOKIE-1"
ROOT_COOKIE = bytes.fromhex("11111111111111111111111111111111")
FAMILY_WILD = 0xFFFF

XNS = "X-NAMESPACE"
X_XNS_GET_CLIENT_INFO = 12
INFO_SOCKET_BOUND = 1


def _tool():
    builddir = os.environ.get("XSERVER_BUILDDIR")
    path = Path(builddir or "", "utils/xns-listen/xns-listen")
    if not builddir or not path.exists():
        pytest.skip("xns-listen not built")
    return str(path)


@pytest.fixture
def xserver_args(tmp_path):
    conf = tmp_path / "ns.conf"
    conf.write_text(f"auth {COOKIE_PROTO.decode()} {ROOT_COOKIE.hex()}\n")
    return ["-namespace", str(conf)]


def _xauth_file(path, display_num):
    """Xauthority file holding the root cookie for any host."""
    def field(b):
        return struct.pack(">H", len(b)) + b
    entry = (struct.pack(">H", FAMILY_WILD) + field(b"") +
             field(str(display_num).encode()) + field(COOKIE_PROTO) + field(ROOT_COOKIE))
    path.write_bytes(entry)
    return str(path)


@pytest.fixture
def env(xserver, tmp_path):
    e = dict(os.environ)
    e["DISPLAY"] = f":{xserver.display_num}"
    e["XAUTHORITY"] = _xauth_file(tmp_path / "xauth", xserver.display_num)
    e["PYTHONPATH"] = os.path.dirname(os.path.abspath(__file__))
    return e


def _manager(xserver):
    conn = RawX11Connection(xserver.display_num, auth=(COOKIE_PROTO, ROOT_COOKIE))
    ext = conn.query_extension(XNS)
    assert ext
    return conn, ext.opcode


def _client_info(manager, opcode, client):
    pixmap = client.create_pixmap(1, 1)
    client.flush_responses(timeout=0.2)
    manager.send_request(struct.pack("<BBHI", opcode, X_XNS_GET_CLIENT_INFO, 2, pixmap))
    r = manager.recv_response(timeout=5.0)
    assert r is not None and not isinstance(r, X11Error), r
    lens = struct.unpack_from("<HHHH", r.data, 16)
    off, strs = 32, []
    for n in lens:
        strs.append(r.data[off : off + n].decode())
        off += n + (-n % 4)
    return r.data[1], strs     # flags, [ns, app_id, instance_id, engine]


def _start(args, env):
    proc = subprocess.Popen([_tool()] + args, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True)
    line = proc.stdout.readline()
    if line.strip() != "ready":
        proc.kill()
        _, err = proc.communicate(timeout=5)
        pytest.fail(f"xns-listen did not get ready: {line!r} {err}")
    return proc


class TestXnsListen:
    def test_daemon_mode_binds_and_cleans_up(self, xserver, env, tmp_path):
        sock = tmp_path / "app.sock"
        proc = _start(["-n", "app1", "--create", "--app-id", "org.example.Editor",
                       "--instance-id", "17", "--engine", "bubblewrap", str(sock)], env)
        assert sock.exists()
        assert (sock.stat().st_mode & 0o077) == 0, "socket must be private to the user"

        manager, opcode = _manager(xserver)
        app = RawX11Connection(xserver.display_num, path=str(sock))
        flags, strs = _client_info(manager, opcode, app)
        assert flags & INFO_SOCKET_BOUND
        assert strs == ["app1", "org.example.Editor", "17", "bubblewrap"]

        proc.send_signal(signal.SIGTERM)
        assert proc.wait(timeout=5) == 0
        assert not sock.exists()
        # --create: the namespace was deleted, disconnecting its clients
        assert app.wait_for_disconnect(timeout=5.0)

    def test_command_mode_runs_client_in_namespace(self, xserver, env, tmp_path):
        """The command connects through $XNS_SOCKET and lands in the
        namespace: checked by the manager through a pixmap the command
        creates (X-NAMESPACE being hidden alone would not tell us, since
        it is hidden in the anonymous namespace too)."""
        sock = tmp_path / "cmd.sock"
        script = textwrap.dedent(f"""
            import os, sys
            from xclient import RawX11Connection
            c = RawX11Connection({xserver.display_num}, path=os.environ["XNS_SOCKET"])
            pixmap = c.create_pixmap(1, 1)
            c.flush_responses(timeout=0.2)
            print(pixmap, flush=True)
            sys.stdin.read()          # stay connected until the test is done
        """)
        proc = subprocess.Popen(
            [_tool(), "-n", "app2", "--create", "--app-id", "org.example.Cmd",
             str(sock), "--", sys.executable, "-c", script],
            env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True)
        try:
            assert proc.stdout.readline().strip() == "ready"
            pixmap = int(proc.stdout.readline())

            manager, opcode = _manager(xserver)
            manager.send_request(
                struct.pack("<BBHI", opcode, X_XNS_GET_CLIENT_INFO, 2, pixmap))
            r = manager.recv_response(timeout=5.0)
            assert not isinstance(r, X11Error), r
            ns_len, app_len = struct.unpack_from("<HH", r.data, 16)
            assert r.data[32:32 + ns_len] == b"app2"
            off = 32 + ns_len + (-ns_len % 4)
            assert r.data[off:off + app_len] == b"org.example.Cmd"
        finally:
            proc.stdin.close()
        assert proc.wait(timeout=10) == 0, proc.stderr.read()
        assert not sock.exists()

    def test_command_exit_status_is_passed_on(self, xserver, env, tmp_path):
        r = subprocess.run([_tool(), "-n", "app3", "--create", str(tmp_path / "s"),
                            "--", "sh", "-c", "exit 7"],
                           env=env, capture_output=True, text=True, timeout=30)
        assert r.returncode == 7, r.stderr

    def test_unknown_namespace_without_create_fails(self, xserver, env, tmp_path):
        sock = tmp_path / "s"
        r = subprocess.run([_tool(), "-n", "nosuch", str(sock)],
                           env=env, capture_output=True, text=True, timeout=30)
        assert r.returncode == 1
        assert "no such namespace" in r.stderr
        assert not sock.exists()

    def test_refuses_root_namespace(self, xserver, env, tmp_path):
        r = subprocess.run([_tool(), "-n", "root", str(tmp_path / "s")],
                           env=env, capture_output=True, text=True, timeout=30)
        assert r.returncode == 1
        assert "BadAccess" in r.stderr

    def test_no_admin_capability(self, xserver, env, tmp_path):
        r = subprocess.run([_tool(), "-n", "x", "--create", "--allow", "admin",
                            str(tmp_path / "s")],
                           env=env, capture_output=True, text=True, timeout=30)
        assert r.returncode == 1
        assert "unknown capability" in r.stderr

    def test_without_root_authorization_fails_cleanly(self, xserver, env, tmp_path):
        """Without the root cookie the tool lands in the anonymous namespace,
        where X-NAMESPACE is hidden: it must say so, not crash."""
        env = dict(env, XAUTHORITY=str(tmp_path / "missing"))
        r = subprocess.run([_tool(), "-n", "x", "--create", str(tmp_path / "s")],
                           env=env, capture_output=True, text=True, timeout=30)
        assert r.returncode == 1
        assert "does not offer X-NAMESPACE" in r.stderr
