# SPDX-License-Identifier: MIT
#
# Isolation tests for the X-Namespace extension: a client confined to a
# namespace must not be able to observe input or other clients' data
# outside of it.

import struct

import pytest
from proto import xi, xtest
from xclient import Extension, RawX11Connection, X11Error

COOKIE_PROTO = b"MIT-MAGIC-COOKIE-1"
ROOT_COOKIE = bytes.fromhex("11111111111111111111111111111111")
SANDBOX_COOKIE = bytes.fromhex("22222222222222222222222222222222")

GENERIC_EVENT = 35
KEYCODE_A = 38


@pytest.fixture
def xserver_args(tmp_path):
    """Start the server with a root namespace and a confined "sandbox"
    namespace that is allowed to use XInput."""
    conf = tmp_path / "ns.conf"
    conf.write_text(
        f"auth {COOKIE_PROTO.decode()} {ROOT_COOKIE.hex()}\n"
        f"namespace sandbox root\n"
        f"  auth {COOKIE_PROTO.decode()} {SANDBOX_COOKIE.hex()}\n"
        f"  allow xinput\n"
    )
    return ["-namespace", str(conf)]


def _connect(xserver, cookie):
    return RawX11Connection(xserver.display_num, auth=(COOKIE_PROTO, cookie))


def _sync(conn):
    """Round-trip (GetInputFocus) so that all earlier requests are processed.

    Returns everything received before the GetInputFocus reply.
    """
    seq = conn.send_request(struct.pack("<BxH", 43, 1))
    received = []
    while True:
        resp = conn.recv_response(timeout=5.0)
        assert resp is not None, "server did not answer GetInputFocus"
        if not isinstance(resp, X11Error) and resp.response_type == 1 and resp.sequence == seq:
            return received
        received.append(resp)


def _select_raw_key_events(conn):
    """Select raw key events for all devices on the root window.

    Returns the responses (e.g. errors) produced by the request.
    """
    ext = conn.query_extension(Extension.XI)
    assert ext, "XInputExtension not available"
    conn.send_request(xi.XIQueryVersionRequest(opcode=ext.opcode))
    conn.recv_response(timeout=5.0)
    conn.send_request(
        xi.XISelectEventsRequest(
            opcode=ext.opcode,
            window=conn.root_window,
            deviceid=xi.XIAllDevices,
            events=[xi.XI_RawKeyPress, xi.XI_RawKeyRelease],
        )
    )
    return _sync(conn), ext.opcode


def _type_key(conn, keycode):
    ext = conn.query_extension(Extension.XTEST)
    assert ext, "XTEST not available"
    conn.send_request(xtest.GetVersionRequest(opcode=ext.opcode))
    conn.recv_response(timeout=5.0)
    for evtype in (xtest.KeyPress, xtest.KeyRelease):
        conn.send_request(
            xtest.FakeInputRequest(opcode=ext.opcode, type=evtype, detail=keycode)
        )
    _sync(conn)


def _raw_key_events(responses, xi_opcode):
    events = []
    for r in responses:
        if isinstance(r, X11Error) or r.response_type & 0x7F != GENERIC_EVENT:
            continue
        extension = r.data[1]
        evtype = struct.unpack_from("<H", r.data, 8)[0]
        if extension == xi_opcode and evtype in (xi.XI_RawKeyPress, xi.XI_RawKeyRelease):
            events.append(evtype)
    return events


class TestNamespaceRawInput:
    """XI2 raw events report every key press on the system, regardless of
    focus. A confined client must not be able to use them as a keylogger."""

    def test_root_namespace_receives_raw_keys(self, xserver):
        """Control: an unconfined (root namespace) client does receive raw
        key events, so the isolation test below is meaningful."""
        observer = _connect(xserver, ROOT_COOKIE)
        injector = _connect(xserver, ROOT_COOKIE)

        _, xi_opcode = _select_raw_key_events(observer)
        _type_key(injector, KEYCODE_A)

        events = _raw_key_events(observer.flush_responses(timeout=1.0), xi_opcode)
        assert xi.XI_RawKeyPress in events and xi.XI_RawKeyRelease in events

    def test_sandbox_cannot_log_keys_via_raw_events(self, xserver):
        """A client in a confined namespace must not receive raw key events
        for input that goes to other namespaces."""
        spy = _connect(xserver, SANDBOX_COOKIE)
        injector = _connect(xserver, ROOT_COOKIE)

        _, xi_opcode = _select_raw_key_events(spy)
        _type_key(injector, KEYCODE_A)

        events = _raw_key_events(spy.flush_responses(timeout=1.0), xi_opcode)
        assert events == [], "confined client received raw key events"


X_COPY_AREA = 62
X_GET_IMAGE = 73
Z_PIXMAP = 2
GC_SUBWINDOW_MODE = 1 << 15
INCLUDE_INFERIORS = 1
BAD_ACCESS = 10


def _errors(responses):
    return [r for r in responses if isinstance(r, X11Error)]


def _copy_root_into_own_pixmap(conn):
    """Copy the root window, including all child windows, into a pixmap
    owned by ``conn``: the classic way to screenshot without GetImage."""
    from proto.x11 import CreateGCRequest

    pixmap = conn.create_pixmap(16, 16)
    gc = conn.alloc_id()
    conn.send_request(
        CreateGCRequest(
            cid=gc, drawable=pixmap, values={GC_SUBWINDOW_MODE: INCLUDE_INFERIORS}
        )
    )
    conn.send_request(
        struct.pack(
            "<BxHIIIhhhhHH", X_COPY_AREA, 7, conn.root_window, pixmap, gc,
            0, 0, 0, 0, 16, 16,
        )
    )
    return _sync(conn)


def _get_root_image(conn):
    conn.send_request(
        struct.pack(
            "<BBHIhhHHI", X_GET_IMAGE, Z_PIXMAP, 5, conn.root_window,
            0, 0, 16, 16, 0xFFFFFFFF,
        )
    )
    return _sync(conn)


class TestNamespaceScreenCapture:
    """A confined client must not be able to read the contents of the
    screen (other namespaces' windows) through the root window."""

    def test_root_namespace_can_copy_root(self, xserver):
        conn = _connect(xserver, ROOT_COOKIE)
        assert _errors(_copy_root_into_own_pixmap(conn)) == []

    def test_sandbox_cannot_copy_root_with_include_inferiors(self, xserver):
        conn = _connect(xserver, SANDBOX_COOKIE)
        errors = _errors(_copy_root_into_own_pixmap(conn))
        assert any(e.error_code == BAD_ACCESS and e.major_code == X_COPY_AREA
                   for e in errors), errors

    def test_sandbox_cannot_get_root_image(self, xserver):
        conn = _connect(xserver, SANDBOX_COOKIE)
        resp = _get_root_image(conn)
        assert any(isinstance(r, X11Error) and r.major_code == X_GET_IMAGE
                   for r in resp), resp


class TestNamespaceRecord:
    """RECORD can capture every client's requests and all device input.
    A confined client must not be able to use it, even when it knows the
    extension's (fixed) opcode."""

    XRECORD_ALL_CLIENTS = 3

    def test_record_hidden_from_sandbox(self, xserver):
        conn = _connect(xserver, SANDBOX_COOKIE)
        assert conn.query_extension(Extension.RECORD) is None

    def test_sandbox_cannot_create_record_context(self, xserver):
        from proto import record

        root = _connect(xserver, ROOT_COOKIE)
        ext = root.query_extension(Extension.RECORD)
        if ext is None:
            pytest.skip("RECORD extension not available")

        spy = _connect(xserver, SANDBOX_COOKIE)
        spy.send_request(
            record.CreateContextRequest(
                opcode=ext.opcode,
                context_id=spy.alloc_id(),
                client_ids=[self.XRECORD_ALL_CLIENTS],
            )
        )
        errors = _errors(_sync(spy))
        assert any(e.error_code == BAD_ACCESS and e.major_code == ext.opcode
                   for e in errors), errors
