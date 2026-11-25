# SPDX-License-Identifier: MIT
#
# RandR requests that change the display configuration must look up the
# CRTC, output or provider they change with write access, so XACE
# policies (Xnamespace, SELinux, XPrism capabilities) see a write. They
# used to use DixReadAccess, which let a client confined by Xnamespace
# change gamma, transforms, panning and output properties of the whole
# screen.

import struct

import pytest
from xclient import RawX11Connection, X11Error

COOKIE_PROTO = b"MIT-MAGIC-COOKIE-1"
SESSION_COOKIE = bytes.fromhex("11111111111111111111111111111111")
SANDBOX_COOKIE = bytes.fromhex("22222222222222222222222222222222")

RR_QUERY_VERSION = 0
RR_GET_SCREEN_RESOURCES = 8
RR_GET_OUTPUT_INFO = 9
RR_LIST_OUTPUT_PROPERTIES = 10
RR_CHANGE_OUTPUT_PROPERTY = 13
RR_DELETE_OUTPUT_PROPERTY = 14
RR_GET_CRTC_GAMMA_SIZE = 22
RR_SET_CRTC_GAMMA = 24
BAD_ACCESS = 10
ATOM_INTEGER = 19
X_GET_INPUT_FOCUS = 43


@pytest.fixture
def xserver_args(tmp_path):
    conf = tmp_path / "ns.conf"
    p = COOKIE_PROTO.decode()
    conf.write_text(
        f"auth {p} {SESSION_COOKIE.hex()}\n"
        f"namespace sandbox\n"
        f"  auth {p} {SANDBOX_COOKIE.hex()}\n"
    )
    # capabilities off: this is about the lookups XACE sees, not about
    # the capability layer (which checks RandR at dispatch anyway)
    return ["-namespace", str(conf), "-capmode", "off"]


def _pad(b):
    return b + b"\0" * (-len(b) % 4)


class RandR:
    def __init__(self, xserver, cookie):
        self.c = RawX11Connection(xserver.display_num, auth=(COOKIE_PROTO, cookie))
        ext = self.c.query_extension("RANDR")
        assert ext, "no RANDR"
        self.op = ext.opcode
        self.reply(RR_QUERY_VERSION, struct.pack("<II", 1, 6))

    def send(self, minor, body=b""):
        body = _pad(body)
        return self.c.send_request(struct.pack("<BBH", self.op, minor, 1 + len(body) // 4) + body)

    def reply(self, minor, body=b""):
        seq = self.send(minor, body)
        while True:
            r = self.c.recv_response(timeout=5.0)
            assert r is not None
            if isinstance(r, X11Error):
                return r
            if r.response_type == 1 and r.sequence == seq:
                return r

    def errors(self, minor, body=b""):
        self.send(minor, body)
        seq = self.c.send_request(struct.pack("<BxH", X_GET_INPUT_FOCUS, 1))
        errs = []
        while True:
            r = self.c.recv_response(timeout=5.0)
            assert r is not None
            if isinstance(r, X11Error):
                errs.append(r.error_code)
            elif r.response_type == 1 and r.sequence == seq:
                return errs


def _resources(rr, root):
    r = rr.reply(RR_GET_SCREEN_RESOURCES, struct.pack("<I", root))
    assert not isinstance(r, X11Error), r
    ncrtc, noutput = struct.unpack_from("<HH", r.data, 16)
    crtcs = list(struct.unpack_from(f"<{ncrtc}I", r.data, 32))
    outputs = list(struct.unpack_from(f"<{noutput}I", r.data, 32 + 4 * ncrtc))
    return crtcs, outputs


def _props(rr, output):
    r = rr.reply(RR_LIST_OUTPUT_PROPERTIES, struct.pack("<I", output))
    assert not isinstance(r, X11Error), r
    n = struct.unpack_from("<H", r.data, 8)[0]
    return set(struct.unpack_from(f"<{n}I", r.data, 32))


def _change_prop(output, atom, value=1):
    return struct.pack("<IIIBBxxI", output, atom, ATOM_INTEGER, 32, 0, 1) + struct.pack("<I", value)


@pytest.fixture
def setup(xserver):
    session = RandR(xserver, SESSION_COOKIE)
    crtcs, outputs = _resources(session, session.c.root_window)
    if not crtcs or not outputs:
        pytest.skip("server has no RandR CRTC/output")
    sandbox = RandR(xserver, SANDBOX_COOKIE)
    atom = session.c.intern_atom("XPRISM_TEST_PROP")
    return session, sandbox, crtcs[0], outputs[0], atom


class TestRandRWritesNeedWriteAccess:
    def test_confined_client_cannot_change_output_property(self, setup):
        session, sandbox, crtc, output, atom = setup
        assert sandbox.errors(RR_CHANGE_OUTPUT_PROPERTY, _change_prop(output, atom)) == [BAD_ACCESS]
        assert atom not in _props(session, output)

    def test_confined_client_cannot_delete_output_property(self, setup):
        session, sandbox, crtc, output, atom = setup
        assert session.errors(RR_CHANGE_OUTPUT_PROPERTY, _change_prop(output, atom)) == []
        assert sandbox.errors(RR_DELETE_OUTPUT_PROPERTY, struct.pack("<II", output, atom)) == [BAD_ACCESS]
        assert atom in _props(session, output)

    def test_confined_client_cannot_set_gamma(self, setup):
        session, sandbox, crtc, output, atom = setup
        r = session.reply(RR_GET_CRTC_GAMMA_SIZE, struct.pack("<I", crtc))
        size = struct.unpack_from("<H", r.data, 8)[0]
        ramp = struct.pack(f"<{3 * size}H", *([0] * (3 * size)))  # all black
        assert sandbox.errors(RR_SET_CRTC_GAMMA, struct.pack("<IHxx", crtc, size) + ramp) == [BAD_ACCESS]

    def test_confined_client_can_still_read(self, setup):
        session, sandbox, crtc, output, atom = setup
        r = sandbox.reply(RR_GET_OUTPUT_INFO, struct.pack("<II", output, 0))
        assert not isinstance(r, X11Error), r

    def test_session_client_can_still_configure(self, setup):
        session, sandbox, crtc, output, atom = setup
        assert session.errors(RR_CHANGE_OUTPUT_PROPERTY, _change_prop(output, atom)) == []
        assert atom in _props(session, output)
        assert session.errors(RR_DELETE_OUTPUT_PROPERTY, struct.pack("<II", output, atom)) == []
