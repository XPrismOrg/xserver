# SPDX-License-Identifier: MIT
#
# Tests for X-NAMESPACE 1.1 listeners: a launcher hands the server a
# listening socket for a namespace, and every client that connects through
# it is placed in that namespace, whatever authorization data it sends.

import os
import socket
import struct
import time

import pytest
from xclient import RawX11Connection, X11ConnectionError, X11Error

COOKIE_PROTO = b"MIT-MAGIC-COOKIE-1"
ROOT_COOKIE = bytes.fromhex("11111111111111111111111111111111")
SANDBOX_COOKIE = bytes.fromhex("22222222222222222222222222222222")

XNS = "X-NAMESPACE"
X_XNS_DELETE_NAMESPACE = 3
X_XNS_GET_CLIENT_NAMESPACE = 9
X_XNS_ADD_LISTENER = 10
X_XNS_REMOVE_LISTENER = 11
X_XNS_GET_CLIENT_INFO = 12

INFO_SOCKET_BOUND = 1 << 0
INFO_PID = 1 << 1
INFO_UID = 1 << 2

APP_ID = b"org.example.Editor"
INSTANCE_ID = b"4242"
ENGINE = b"bubblewrap"

BAD_VALUE = 2
BAD_ACCESS = 10


@pytest.fixture
def xserver_args(tmp_path):
    conf = tmp_path / "ns.conf"
    conf.write_text(
        f"auth {COOKIE_PROTO.decode()} {ROOT_COOKIE.hex()}\n"
        f"namespace sandbox root\n"
        f"  auth {COOKIE_PROTO.decode()} {SANDBOX_COOKIE.hex()}\n"
    )
    return ["-namespace", str(conf)]


def _pad(b):
    return b + b"\x00" * (-len(b) % 4)


def _reply(conn):
    r = conn.recv_response(timeout=5.0)
    assert r is not None, "no reply from server"
    return r


class Listener:
    """A listening socket the test (playing the launcher) owns, plus the
    pipe whose write end keeps it alive."""

    def __init__(self, tmp_path, name="app.sock"):
        self.path = str(tmp_path / name)
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.bind(self.path)
        self.sock.listen(8)
        self.close_r, self.close_w = os.pipe()

    def hand_over(self):
        """After the server has its own copies: drop ours, except the pipe's
        write end, which is what keeps the listener alive."""
        self.sock.close()
        os.close(self.close_r)

    def stop(self):
        os.close(self.close_w)
        self.close_w = -1


def _manager(xserver):
    conn = RawX11Connection(xserver.display_num, auth=(COOKIE_PROTO, ROOT_COOKIE))
    ext = conn.query_extension(XNS)
    assert ext, "X-NAMESPACE not available to the root namespace"
    return conn, ext.opcode


def _add_listener(conn, opcode, ns_name, fds, app_id=b"", instance_id=b"", engine=b""):
    name = ns_name.encode()
    body = struct.pack("<HHHH", len(name), len(app_id), len(instance_id), len(engine))
    for s in (name, app_id, instance_id, engine):
        body += _pad(s)
    req = struct.pack("<BBH", opcode, X_XNS_ADD_LISTENER, 1 + len(body) // 4) + body
    conn.send_request_with_fds(req, fds)
    r = _reply(conn)
    if isinstance(r, X11Error):
        return r
    return struct.unpack_from("<I", r.data, 8)[0]


def _remove_listener(conn, opcode, handle):
    conn.send_request(struct.pack("<BBHI", opcode, X_XNS_REMOVE_LISTENER, 2, handle))
    return _reply(conn)


def _namespace_of(manager, opcode, xid):
    """Ask (as the manager) which namespace owns resource *xid*."""
    manager.send_request(
        struct.pack("<BBHI", opcode, X_XNS_GET_CLIENT_NAMESPACE, 2, xid)
    )
    r = _reply(manager)
    assert not isinstance(r, X11Error), f"GetClientNamespace failed: {r}"
    name_len = struct.unpack_from("<H", r.data, 8)[0]
    return r.data[32 : 32 + name_len].decode()


def _client_namespace(manager, opcode, client):
    """Namespace of *client*, identified through a pixmap it creates."""
    pixmap = client.create_pixmap(1, 1)
    client.flush_responses(timeout=0.2)
    return _namespace_of(manager, opcode, pixmap)


def _client_info(manager, opcode, client):
    """GetClientInfo for *client*, identified through a pixmap it creates."""
    pixmap = client.create_pixmap(1, 1)
    client.flush_responses(timeout=0.2)
    manager.send_request(struct.pack("<BBHI", opcode, X_XNS_GET_CLIENT_INFO, 2, pixmap))
    r = _reply(manager)
    assert not isinstance(r, X11Error), f"GetClientInfo failed: {r}"
    flags = r.data[1]
    pid, uid, *lens = struct.unpack_from("<IIHHHH", r.data, 8)
    off, strs = 32, []
    for n in lens:
        strs.append(r.data[off : off + n])
        off += n + (-n % 4)
    ns, app_id, instance_id, engine = strs
    return dict(flags=flags, pid=pid, uid=uid, ns=ns.decode(),
                app_id=app_id, instance_id=instance_id, engine=engine)


def _server_fds(xserver):
    return len(os.listdir(f"/proc/{xserver._process.pid}/fd"))


def _connect_via(listener, xserver, auth=None):
    return RawX11Connection(xserver.display_num, auth=auth, path=listener.path)


def _wait_refused(listener, xserver, timeout=3.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            _connect_via(listener, xserver).close()
        except X11ConnectionError:
            return True
        time.sleep(0.05)
    return False


@pytest.fixture
def bound(xserver, tmp_path):
    """A listener for "sandbox", already handed to the server."""
    manager, opcode = _manager(xserver)
    listener = Listener(tmp_path)
    handle = _add_listener(
        manager, opcode, "sandbox", [listener.sock.fileno(), listener.close_r],
        app_id=APP_ID, instance_id=INSTANCE_ID, engine=ENGINE,
    )
    assert isinstance(handle, int) and handle != 0, f"AddListener failed: {handle}"
    listener.hand_over()
    yield manager, opcode, listener, handle
    if listener.close_w >= 0:
        listener.stop()


class TestNamespaceListener:
    def test_regular_socket_client_is_not_in_sandbox(self, xserver):
        """Control: the namespace query tells namespaces apart, so the
        checks below are meaningful."""
        manager, opcode = _manager(xserver)
        plain = RawX11Connection(xserver.display_num, auth=(COOKIE_PROTO, ROOT_COOKIE))
        assert _client_namespace(manager, opcode, plain) == "root"

    def test_client_without_auth_lands_in_namespace(self, xserver, bound):
        manager, opcode, listener, _ = bound
        app = _connect_via(listener, xserver)
        assert _client_namespace(manager, opcode, app) == "sandbox"

    def test_auth_data_cannot_escape_the_namespace(self, xserver, bound):
        """A sandboxed app that got hold of the root cookie still lands in
        its own namespace when it connects through its socket.

        Two things ensure this: the server doesn't read auth data on these
        sockets at all, and the namespace layer ignores it for clients that
        came through a listener. Verified by hand that each one alone keeps
        this test passing, and that dropping both makes it fail."""
        manager, opcode, listener, _ = bound
        app = _connect_via(listener, xserver, auth=(COOKIE_PROTO, ROOT_COOKIE))
        assert _client_namespace(manager, opcode, app) == "sandbox"

    def test_namespace_extension_hidden_from_bound_client(self, xserver, bound):
        _, _, listener, _ = bound
        app = _connect_via(listener, xserver)
        assert app.query_extension(XNS) is None

    def test_closing_the_pipe_stops_listening(self, xserver, bound):
        manager, opcode, listener, _ = bound
        app = _connect_via(listener, xserver)

        listener.stop()
        assert _wait_refused(listener, xserver), "socket still accepts clients"

        # clients that were already connected keep working
        assert app.is_connected()
        assert _client_namespace(manager, opcode, app) == "sandbox"

    def test_remove_listener(self, xserver, bound):
        manager, opcode, listener, handle = bound
        r = _remove_listener(manager, opcode, handle)
        assert not isinstance(r, X11Error), f"RemoveListener failed: {r}"
        assert _wait_refused(listener, xserver)

        r = _remove_listener(manager, opcode, handle)
        assert isinstance(r, X11Error) and r.error_code == BAD_VALUE

    def test_deleting_namespace_closes_its_socket(self, xserver, bound):
        manager, opcode, listener, _ = bound
        name = b"sandbox"
        body = struct.pack("<BxH", 1, len(name)) + _pad(name)   # 1: kill clients
        manager.send_request(
            struct.pack("<BBH", opcode, X_XNS_DELETE_NAMESPACE, 1 + len(body) // 4) + body
        )
        r = _reply(manager)
        assert not isinstance(r, X11Error), f"DeleteNamespace failed: {r}"
        assert _wait_refused(listener, xserver)

    def test_client_carries_app_info(self, xserver, bound):
        manager, opcode, listener, _ = bound
        app = _connect_via(listener, xserver)
        info = _client_info(manager, opcode, app)
        assert info["ns"] == "sandbox"
        assert info["flags"] & INFO_SOCKET_BOUND
        assert (info["app_id"], info["instance_id"], info["engine"]) == (
            APP_ID, INSTANCE_ID, ENGINE)
        # the peer identity recorded at connect time: that's us
        assert info["flags"] & INFO_PID and info["pid"] == os.getpid()
        assert info["flags"] & INFO_UID and info["uid"] == os.getuid()

    def test_regular_client_has_no_app_info(self, xserver):
        manager, opcode = _manager(xserver)
        plain = RawX11Connection(xserver.display_num, auth=(COOKIE_PROTO, ROOT_COOKIE))
        info = _client_info(manager, opcode, plain)
        assert info["ns"] == "root"
        assert not info["flags"] & INFO_SOCKET_BOUND
        assert (info["app_id"], info["instance_id"], info["engine"]) == (b"", b"", b"")
        assert info["pid"] == os.getpid()

    def test_app_info_outlives_listener(self, xserver, bound):
        manager, opcode, listener, _ = bound
        app = _connect_via(listener, xserver)
        listener.stop()
        assert _wait_refused(listener, xserver)
        assert _client_info(manager, opcode, app)["app_id"] == APP_ID

    def test_rejects_control_chars_in_app_info(self, xserver, tmp_path):
        manager, opcode = _manager(xserver)
        listener = Listener(tmp_path)
        r = _add_listener(manager, opcode, "sandbox",
                          [listener.sock.fileno(), listener.close_r],
                          app_id=b"evil\x1b[2Jid")
        assert isinstance(r, X11Error) and r.error_code == BAD_VALUE

    def test_truncated_request_is_rejected(self, xserver, tmp_path):
        """Lengths pointing past the end of the request: BadLength, and the
        server stays usable."""
        manager, opcode = _manager(xserver)
        listener = Listener(tmp_path)
        body = struct.pack("<HHHH", 7, 200, 0, 0) + _pad(b"sandbox")
        req = struct.pack("<BBH", opcode, X_XNS_ADD_LISTENER, 1 + len(body) // 4) + body
        manager.send_request_with_fds(req, [listener.sock.fileno(), listener.close_r])
        r = _reply(manager)
        assert isinstance(r, X11Error) and r.error_code == 16   # BadLength
        assert _client_namespace(manager, opcode, manager) == "root"

    def test_rejected_requests_do_not_leak_fds(self, xserver, tmp_path):
        manager, opcode = _manager(xserver)
        _client_namespace(manager, opcode, manager)     # warm up
        before = _server_fds(xserver)
        for i in range(20):
            listener = Listener(tmp_path, f"leak{i}.sock")
            fds = [listener.sock.fileno(), listener.close_r]
            _add_listener(manager, opcode, "root", fds)             # BadAccess
            _add_listener(manager, opcode, "nosuchns", fds)         # BadName
            _add_listener(manager, opcode, "sandbox", fds, app_id=b"\x01")  # BadValue
            body = struct.pack("<HHHH", 7, 200, 0, 0) + _pad(b"sandbox")
            manager.send_request_with_fds(
                struct.pack("<BBH", opcode, X_XNS_ADD_LISTENER, 1 + len(body) // 4) + body, fds)
            _reply(manager)                                          # BadLength
            listener.sock.close()
            os.close(listener.close_r)
            os.close(listener.close_w)
        assert _server_fds(xserver) - before < 5

    def test_cannot_bind_socket_to_root(self, xserver, tmp_path):
        manager, opcode = _manager(xserver)
        listener = Listener(tmp_path)
        r = _add_listener(manager, opcode, "root", [listener.sock.fileno(), listener.close_r])
        assert isinstance(r, X11Error) and r.error_code == BAD_ACCESS

    def test_rejects_non_listening_socket(self, xserver, tmp_path):
        manager, opcode = _manager(xserver)
        a, b = socket.socketpair()
        r, w = os.pipe()
        err = _add_listener(manager, opcode, "sandbox", [a.fileno(), r])
        assert isinstance(err, X11Error) and err.error_code == BAD_VALUE
        # the server must still be usable afterwards
        assert _client_namespace(manager, opcode, manager) == "root"
