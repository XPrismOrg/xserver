# SPDX-License-Identifier: MIT
#
# clipboard.read / clipboard.write grants give a confined namespace the
# session's PRIMARY, SECONDARY and CLIPBOARD instead of its private copies
# (Xext/namespace/hook-selection.c). Every test does a real transfer: the
# owner answers SelectionRequest by writing a property on the requestor's
# window and sending SelectionNotify, as toolkits do.

import struct
import time

import pytest
from xclient import RawX11Connection, X11Error

COOKIE_PROTO = b"MIT-MAGIC-COOKIE-1"
SESSION_COOKIE = bytes.fromhex("11111111111111111111111111111111")
SANDBOX_COOKIE = bytes.fromhex("22222222222222222222222222222222")
READER_COOKIE = bytes.fromhex("33333333333333333333333333333333")
WRITER_COOKIE = bytes.fromhex("44444444444444444444444444444444")

X_CHANGE_PROPERTY = 18
X_GET_PROPERTY = 20
X_SET_SELECTION_OWNER = 22
X_GET_SELECTION_OWNER = 23
X_CONVERT_SELECTION = 24
X_SEND_EVENT = 25
SELECTION_REQUEST = 30
SELECTION_NOTIFY = 31
ATOM_STRING = 31

X_XNS_SET_NAMESPACE_GRANTS = 13
GRANT_CLIPBOARD_READ = 1 << 8


@pytest.fixture
def xserver_args(tmp_path):
    conf = tmp_path / "ns.conf"
    p = COOKIE_PROTO.decode()
    conf.write_text(
        f"auth {p} {SESSION_COOKIE.hex()}\n"
        f"namespace sandbox root\n"
        f"  auth {p} {SANDBOX_COOKIE.hex()}\n"
        f"namespace reader root\n"
        f"  auth {p} {READER_COOKIE.hex()}\n"
        f"  grant clipboard.read\n"
        f"namespace writer root\n"
        f"  auth {p} {WRITER_COOKIE.hex()}\n"
        f"  grant clipboard.write\n"
    )
    # enforce: the capability checks must agree with the grants too
    return ["-namespace", str(conf), "-capmode", "enforce"]


def _connect(xserver, cookie):
    return RawX11Connection(xserver.display_num, auth=(COOKIE_PROTO, cookie))


def _pad(b):
    return b + b"\0" * (-len(b) % 4)


def _reply(conn, seq):
    while True:
        r = conn.recv_response(timeout=5.0)
        assert r is not None, "no reply"
        assert not isinstance(r, X11Error), r
        if r.response_type == 1 and r.sequence == seq:
            return r


def _event(conn, code, timeout=2.0):
    """next event *code*, or None"""
    while True:
        r = conn.recv_response(timeout=timeout)
        if r is None:
            return None
        assert not isinstance(r, X11Error), r
        if (r.response_type & 0x7F) == code:
            return r


def _set_owner(conn, wid, sel):
    conn.send_request(struct.pack("<BxHIII", X_SET_SELECTION_OWNER, 4, wid, sel, 0))


def _owner(conn, sel):
    seq = conn.send_request(struct.pack("<BxHI", X_GET_SELECTION_OWNER, 2, sel))
    return struct.unpack_from("<I", _reply(conn, seq).data, 8)[0]


def _convert(conn, requestor, sel, prop):
    conn.send_request(struct.pack("<BxHIIIII", X_CONVERT_SELECTION, 6,
                                  requestor, sel, ATOM_STRING, prop, 0))


def _answer(conn, data):
    """Owner side: answer the next SelectionRequest with *data*. False if
    none came."""
    ev = _event(conn, SELECTION_REQUEST)
    if ev is None:
        return False
    _t, _owner_w, requestor, sel, target, prop = struct.unpack_from("<IIIIII", ev.data, 4)
    body = _pad(data)
    conn.send_request(struct.pack("<BBHIIIBxxxI", X_CHANGE_PROPERTY, 0,
                                  6 + len(body) // 4, requestor, prop, ATOM_STRING,
                                  8, len(data)) + body)
    notify = struct.pack("<BxHIIIII", SELECTION_NOTIFY, 0, 0, requestor, sel,
                         target, prop).ljust(32, b"\0")
    conn.send_request(struct.pack("<BBHII", X_SEND_EVENT, 0, 11, requestor, 0) + notify)
    return True


def _received(conn, wid, prop):
    """Requestor side: wait for SelectionNotify, read the property."""
    ev = _event(conn, SELECTION_NOTIFY)
    assert ev is not None, "no SelectionNotify"
    got_prop = struct.unpack_from("<I", ev.data, 20)[0]
    assert got_prop == prop, "conversion refused"
    seq = conn.send_request(struct.pack("<BBHIIIII", X_GET_PROPERTY, 0, 6,
                                        wid, prop, 0, 0, 1024))
    r = _reply(conn, seq)
    n = struct.unpack_from("<I", r.data, 16)[0]
    return r.data[32:32 + n]


def _session_owns_clipboard(xserver):
    session = _connect(xserver, SESSION_COOKIE)
    wid = session.create_window()
    clip = session.intern_atom("CLIPBOARD")
    _set_owner(session, wid, clip)
    assert _owner(session, clip) == wid
    return session, wid, clip


def _paste(xserver, cookie, owner_conn, data=b"secret from the session"):
    """A client of *cookie* pastes CLIPBOARD; *owner_conn* serves it.
    Returns what arrived, or None if the owner never got the request."""
    client = _connect(xserver, cookie)
    wid = client.create_window()
    clip = client.intern_atom("CLIPBOARD")
    prop = client.intern_atom("XPRISM_PASTE")
    _convert(client, wid, clip, prop)
    if not _answer(owner_conn, data):
        return None
    return _received(client, wid, prop)


class TestClipboardGrants:
    def test_no_grant_keeps_session_clipboard_private(self, xserver):
        session, wid, clip = _session_owns_clipboard(xserver)
        sandbox = _connect(xserver, SANDBOX_COOKIE)
        assert _owner(sandbox, clip) == 0
        assert _paste(xserver, SANDBOX_COOKIE, session) is None

    def test_read_grant_pastes_from_session(self, xserver):
        session, wid, clip = _session_owns_clipboard(xserver)
        reader = _connect(xserver, READER_COOKIE)
        assert _owner(reader, clip) == wid
        assert _paste(xserver, READER_COOKIE, session) == b"secret from the session"

    def test_read_grant_cannot_replace_session_clipboard(self, xserver):
        session, wid, clip = _session_owns_clipboard(xserver)
        reader = _connect(xserver, READER_COOKIE)
        _set_owner(reader, reader.create_window(), clip)
        assert _owner(session, clip) == wid

    def test_write_grant_copies_to_session(self, xserver):
        writer = _connect(xserver, WRITER_COOKIE)
        wwid = writer.create_window()
        clip = writer.intern_atom("CLIPBOARD")
        _set_owner(writer, wwid, clip)
        assert _owner(writer, clip) == wwid   # sees its own ownership
        session = _connect(xserver, SESSION_COOKIE)
        assert _owner(session, clip) == wwid
        assert _paste(xserver, SESSION_COOKIE, writer, b"from the app") == b"from the app"

    def test_write_grant_cannot_read_session_clipboard(self, xserver):
        session, wid, clip = _session_owns_clipboard(xserver)
        assert _paste(xserver, WRITER_COOKIE, session) is None

    def test_no_grant_cannot_replace_session_clipboard(self, xserver):
        session, wid, clip = _session_owns_clipboard(xserver)
        sandbox = _connect(xserver, SANDBOX_COOKIE)
        _set_owner(sandbox, sandbox.create_window(), clip)
        assert _owner(session, clip) == wid

    def test_runtime_grant_and_revoke(self, xserver):
        session, wid, clip = _session_owns_clipboard(xserver)
        ext = session.query_extension("X-NAMESPACE")
        sandbox = _connect(xserver, SANDBOX_COOKIE)
        assert _owner(sandbox, clip) == 0

        def set_grants(grants):
            name = _pad(b"sandbox")
            body = struct.pack("<IHH", grants, len(b"sandbox"), 0) + name
            seq = session.send_request(struct.pack("<BBH", ext.opcode,
                                       X_XNS_SET_NAMESPACE_GRANTS, 1 + len(body) // 4) + body)
            _owner(session, clip)  # round trip; SetNamespaceGrants has no reply

        set_grants(GRANT_CLIPBOARD_READ)
        assert _owner(sandbox, clip) == wid      # same connection, next request
        set_grants(0)
        assert _owner(sandbox, clip) == 0


BAD_ACCESS = 10
BAD_WINDOW = 3
# a denied window lookup can surface as BadWindow (e.g. in SendEvent)
DENIED = ([BAD_ACCESS], [BAD_WINDOW])


def _errors(conn, data):
    conn.send_request(data)
    seq = conn.send_request(struct.pack("<BxHI", X_GET_SELECTION_OWNER, 2, 0))
    errors = []
    while True:
        r = conn.recv_response(timeout=5.0)
        assert r is not None
        if isinstance(r, X11Error):
            errors.append(r.error_code)
        elif r.response_type == 1 and r.sequence == seq:
            return errors


def _change_prop(window, prop, data=b"x"):
    body = _pad(data)
    return struct.pack("<BBHIIIBxxxI", X_CHANGE_PROPERTY, 0, 6 + len(body) // 4,
                       window, prop, ATOM_STRING, 8, len(data)) + body


def _notify(requestor, sel, prop):
    ev = struct.pack("<BxHIIIII", SELECTION_NOTIFY, 0, 0, requestor, sel,
                     ATOM_STRING, prop).ljust(32, b"\0")
    return struct.pack("<BBHII", X_SEND_EVENT, 0, 11, requestor, 0) + ev


def test_unowned_selection_notify_names_the_selection(xserver):
    """Pasting a selection nobody owns answers SelectionNotify, property
    None, for the selection the client asked: not its namespace's
    internal name (once named it twice, so the paste never finished)."""
    conn = _connect(xserver, SANDBOX_COOKIE)
    wid = conn.create_window()
    clip = conn.intern_atom("CLIPBOARD")
    _convert(conn, wid, clip, conn.intern_atom("XPRISM_PASTE"))
    ev = _event(conn, SELECTION_NOTIFY)
    assert ev is not None, "no SelectionNotify"
    requestor, selection, _target, prop = struct.unpack_from("<IIII", ev.data, 8)
    assert (requestor, selection, prop) == (wid, clip, 0)


class TestSelectionReplyIsNarrow:
    """What a confined owner may do to the requestor's window: set the one
    requested property and send one SelectionNotify, nothing else."""

    def _request(self, xserver):
        writer = _connect(xserver, WRITER_COOKIE)
        clip = writer.intern_atom("CLIPBOARD")
        _set_owner(writer, writer.create_window(), clip)
        session = _connect(xserver, SESSION_COOKIE)
        swid = session.create_window()
        prop = session.intern_atom("XPRISM_PASTE")
        other = session.intern_atom("XPRISM_OTHER")
        assert _owner(session, clip) != 0
        _convert(session, swid, clip, prop)
        assert _event(writer, SELECTION_REQUEST) is not None
        self.session = session  # keep it open: closing it destroys swid
        return writer, swid, clip, prop, other

    def test_nothing_without_a_request(self, xserver):
        writer = _connect(xserver, WRITER_COOKIE)
        clip = writer.intern_atom("CLIPBOARD")
        prop = writer.intern_atom("XPRISM_PASTE")
        session = _connect(xserver, SESSION_COOKIE)
        swid = session.create_window()
        session.flush_responses(timeout=0.2)
        assert _errors(writer, _change_prop(swid, prop)) in DENIED
        assert _errors(writer, _notify(swid, clip, prop)) in DENIED

    def test_only_the_requested_property(self, xserver):
        writer, swid, clip, prop, other = self._request(xserver)
        assert _errors(writer, _change_prop(swid, other)) in DENIED
        assert _errors(writer, _change_prop(swid, prop)) == []

    def test_one_notify_per_request(self, xserver):
        writer, swid, clip, prop, other = self._request(xserver)
        assert _errors(writer, _notify(swid, clip, prop)) == []
        assert _errors(writer, _notify(swid, clip, prop)) in DENIED
        # and the token is gone for the property too
        assert _errors(writer, _change_prop(swid, prop)) in DENIED

    def test_only_selection_notify(self, xserver):
        """Any other event is dropped (SendEvent denials are silent in X11);
        the token stays for the real reply."""
        writer, swid, clip, prop, other = self._request(xserver)
        fake_key = struct.pack("<BBHI", 2, 38, 0, 0).ljust(32, b"\0")  # KeyPress
        send = struct.pack("<BBHII", X_SEND_EVENT, 0, 11, swid, 0) + fake_key
        assert _errors(writer, send) == []
        assert _errors(writer, _notify(swid, clip, prop)) == []
        seen = []
        while True:
            r = self.session.recv_response(timeout=2.0)
            assert r is not None, "SelectionNotify never arrived"
            if isinstance(r, X11Error):
                continue
            seen.append(r.response_type & 0x7F)
            if seen[-1] == SELECTION_NOTIFY:
                break
        assert 2 not in seen, "fake KeyPress was delivered"


X_CHANGE_WINDOW_ATTRIBUTES = 2
CW_EVENT_MASK = 1 << 11
PROPERTY_CHANGE_MASK = 1 << 22
KEY_PRESS_MASK = 1 << 0
PROPERTY_NOTIFY = 28
PROPERTY_DELETE = 1


def _select(window, mask):
    return struct.pack("<BxHIII", X_CHANGE_WINDOW_ATTRIBUTES, 4, window,
                       CW_EVENT_MASK, mask)


def _put(window, prop, ptype, data, fmt=8):
    body = _pad(data)
    return struct.pack("<BBHIIIBxxxI", X_CHANGE_PROPERTY, 0, 6 + len(body) // 4,
                       window, prop, ptype, fmt, len(data) * 8 // fmt) + body


def _take(conn, wid, prop):
    """GetProperty with delete: (type, data)"""
    seq = conn.send_request(struct.pack("<BBHIIIII", X_GET_PROPERTY, 1, 6,
                                        wid, prop, 0, 0, 1 << 20))
    r = _reply(conn, seq)
    ptype, n = struct.unpack_from("<I", r.data, 8)[0], struct.unpack_from("<I", r.data, 16)[0]
    return ptype, r.data[32:32 + n]


def _property_event(conn, wid, prop, state, timeout=2.0):
    """next PropertyNotify of *prop* on *wid* with *state*, or None"""
    while True:
        ev = _event(conn, PROPERTY_NOTIFY, timeout)
        if ev is None:
            return None
        w, a = struct.unpack_from("<II", ev.data, 4)
        if (w, a, ev.data[16]) == (wid, prop, state):
            return ev


class TestIncrTransfer:
    """Large data from a confined owner to another namespace, in chunks
    (ICCCM INCR): the owner watches the requestor delete each chunk."""

    CHUNKS = [b"A" * 3000, b"B" * 3000, b"C" * 1234]

    def _start(self, xserver):
        writer = _connect(xserver, WRITER_COOKIE)
        clip = writer.intern_atom("CLIPBOARD")
        _set_owner(writer, writer.create_window(), clip)
        session = _connect(xserver, SESSION_COOKIE)
        swid = session.create_window()
        prop = session.intern_atom("XPRISM_PASTE")
        incr = session.intern_atom("INCR")
        session.send_request(_select(swid, PROPERTY_CHANGE_MASK))
        assert _owner(session, clip) != 0  # round trip, and no error before it
        _convert(session, swid, clip, prop)
        assert _event(writer, SELECTION_REQUEST) is not None
        self.session = session
        return writer, swid, clip, prop, incr

    def _begin(self, writer, swid, clip, prop, incr):
        """the owner's INCR reply; the session takes the INCR property"""
        assert _errors(writer, _select(swid, PROPERTY_CHANGE_MASK)) == []
        total = sum(map(len, self.CHUNKS))
        assert _errors(writer, _put(swid, prop, incr, struct.pack("<I", total), 32)) == []
        assert _errors(writer, _notify(swid, clip, prop)) == []
        assert _event(self.session, SELECTION_NOTIFY) is not None
        ptype, _ = _take(self.session, swid, prop)
        assert ptype == incr

    def _chunk(self, writer, swid, prop, data, heard=True):
        """the owner hears the deletion (unless *heard* is False: already
        swallowed by _errors) and writes *data*; the session takes it"""
        if heard:
            assert _property_event(writer, swid, prop, PROPERTY_DELETE) is not None, \
                "owner never heard the deletion"
        assert _errors(writer, _put(swid, prop, ATOM_STRING, data)) == []
        assert _property_event(self.session, swid, prop, 0) is not None
        ptype, got = _take(self.session, swid, prop)
        assert ptype == ATOM_STRING
        return got

    def test_chunks_arrive(self, xserver):
        writer, swid, clip, prop, incr = self._start(xserver)
        self._begin(writer, swid, clip, prop, incr)
        got = b"".join(self._chunk(writer, swid, prop, c) for c in self.CHUNKS)
        assert got == b"".join(self.CHUNKS)
        assert self._chunk(writer, swid, prop, b"") == b""
        # the transfer is over: the token is gone
        assert _errors(writer, _put(swid, prop, ATOM_STRING, b"more")) in DENIED
        assert _errors(writer, _notify(swid, clip, prop)) in DENIED
        assert _errors(writer, _select(swid, PROPERTY_CHANGE_MASK)) in DENIED

    def test_transfer_is_narrow(self, xserver):
        writer, swid, clip, prop, incr = self._start(xserver)
        other = self.session.intern_atom("XPRISM_OTHER")
        self._begin(writer, swid, clip, prop, incr)
        # property changes only, one property, no second notify
        assert _errors(writer, _select(swid, PROPERTY_CHANGE_MASK | KEY_PRESS_MASK)) in DENIED
        assert _errors(writer, _put(swid, other, ATOM_STRING, b"x")) in DENIED
        assert _errors(writer, _notify(swid, clip, prop)) in DENIED
        # and hears nothing of the requestor's other properties
        self.session.send_request(_put(swid, other, ATOM_STRING, b"private"))
        assert _property_event(writer, swid, other, 0, timeout=1.0) is None
        assert self._chunk(writer, swid, prop, b"still works", heard=False) == b"still works"

    def test_no_watching_without_incr(self, xserver):
        writer, swid, clip, prop, incr = self._start(xserver)
        assert _errors(writer, _put(swid, prop, ATOM_STRING, b"small")) == []
        assert _errors(writer, _notify(swid, clip, prop)) == []
        assert _errors(writer, _select(swid, PROPERTY_CHANGE_MASK)) in DENIED

    def test_ends_with_the_requestor(self, xserver):
        """the requestor's client goes: its window ids may come back"""
        writer, swid, clip, prop, incr = self._start(xserver)
        self._begin(writer, swid, clip, prop, incr)
        self.session.close()
        # the next session client gets the same window id back, once the
        # server has seen the requestor go (slow machines take a while)
        deadline = time.monotonic() + 10
        while True:
            session = _connect(xserver, SESSION_COOKIE)
            wid = session.create_window()
            if wid == swid or time.monotonic() > deadline:
                break
            session.close()
            time.sleep(0.1)
        assert wid == swid
        assert _owner(session, clip) != 0
        assert _errors(writer, _put(swid, prop, ATOM_STRING, b"x")) in DENIED
