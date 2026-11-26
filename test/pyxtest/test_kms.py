# SPDX-License-Identifier: MIT
#
# Xorg's modesetting driver on a virtual KMS device (vkms), with atomic
# modesetting and with the legacy interface: after each RandR or DPMS
# change, the kernel's state (read through DRM ioctls, kms.py) must be
# what the server reports.
#
# Needs a vkms device that this user can drive, on a seat of its own so
# that no desktop session uses it, for instance:
#
#   mkdir -p /run/udev/rules.d
#   echo 'SUBSYSTEM=="drm", DEVPATH=="*/vkms/*", ENV{ID_SEAT}="seat-vkms"' \
#       > /run/udev/rules.d/99-vkms.rules
#   udevadm control --reload
#   modprobe vkms
#
# (as root, before the module is loaded: a desktop that already took the
# device keeps it).
#
# Skipped otherwise.

import fcntl
import os
import struct
import tempfile

import kms
import pytest
from xclient import RawX11Connection, X11Error
from xserver import XServerProcess

DRM_SET_MASTER = 0x641E
DRM_DROP_MASTER = 0x641F

RR_QUERY_VERSION = 0
RR_SET_SCREEN_SIZE = 7
RR_GET_OUTPUT_INFO = 9
RR_GET_CRTC_INFO = 20
RR_SET_CRTC_CONFIG = 21
RR_GET_CRTC_GAMMA_SIZE = 22
RR_SET_CRTC_GAMMA = 24
RR_GET_SCREEN_RESOURCES_CURRENT = 25

ROTATE_0, ROTATE_90 = 1, 2
DPMS_ENABLE, DPMS_FORCE_LEVEL = 4, 6
DPMS_ON, DPMS_OFF = 0, 3


def _seat_of(node):
    """The udev seat of a DRM node (seat0 when none is set)."""
    st = os.stat(node)
    try:
        with open(f"/run/udev/data/c{os.major(st.st_rdev)}:{os.minor(st.st_rdev)}") as f:
            for line in f:
                if line.startswith("E:ID_SEAT="):
                    return line.strip().split("=", 1)[1]
    except OSError:
        pass
    return "seat0"


@pytest.fixture(scope="module")
def vkms():
    node = kms.find_vkms()
    if not node:
        pytest.skip("no vkms device")
    seat = _seat_of(node)
    if seat == "seat0":
        pytest.skip(f"{node} is on seat0, where a desktop session may use it")
    try:
        fd = os.open(node, os.O_RDWR)
    except OSError as e:
        pytest.skip(f"cannot open {node}: {e}")
    try:
        fcntl.ioctl(fd, DRM_SET_MASTER)
        fcntl.ioctl(fd, DRM_DROP_MASTER)
    except OSError as e:
        pytest.skip(f"cannot become DRM master of {node}: {e}")
    finally:
        os.close(fd)
    return node, seat


@pytest.fixture
def device_lock():
    """One server at a time on the device, also across pytest-xdist workers."""
    fd = os.open(os.path.join(tempfile.gettempdir(), "pyxtest-vkms.lock"),
                 os.O_RDWR | os.O_CREAT, 0o600)
    fcntl.flock(fd, fcntl.LOCK_EX)
    try:
        yield
    finally:
        fcntl.flock(fd, fcntl.LOCK_UN)
        os.close(fd)


# None: no option, which must select atomic modesetting (the default)
@pytest.fixture(params=[True, False, None], ids=["atomic", "legacy", "default"])
def server(request, vkms, device_lock, tmp_path, monkeypatch):
    node, seat = vkms
    conf = tmp_path / "xorg.conf"
    conf.write_text(f"""
Section "ServerFlags"
    Option "AutoAddDevices" "false"
    Option "AutoAddGPU" "false"
    Option "AutoBindGPU" "false"
EndSection
Section "Device"
    Identifier "vkms"
    Driver "modesetting"
    Option "kmsdev" "{node}"
    Option "AccelMethod" "none"
{"" if request.param is None else f'    Option "Atomic" "{"true" if request.param else "false"}"'}
EndSection
""")
    (tmp_path / "conf.d").mkdir()
    # open the device directly: no logind session or VT for this seat
    monkeypatch.setenv("LIBSEAT_BACKEND", "noop")
    log = tmp_path / "Xorg.log"
    srv = XServerProcess("xorg", extra_args=[
        "-config", str(conf), "-configdir", str(tmp_path / "conf.d"),
        "-seat", seat, "-novtswitch", "-nolisten", "tcp", "-logfile", str(log)])
    srv.start()
    try:
        text = log.read_text(errors="replace")
        assert ("Atomic modesetting enabled" in text) == (request.param is not False), text[-2000:]
        yield srv, node, log
    finally:
        srv.stop()


class RandR:
    def __init__(self, display):
        self.c = RawX11Connection(display)
        self.op = self.c.query_extension("RANDR").opcode
        self.call(RR_QUERY_VERSION, "II", 1, 6)
        self.resources()

    def send(self, opcode, minor, fmt, *args):
        body = struct.pack("<" + fmt, *args)
        return self.c.send_request(struct.pack("<BBH", opcode, minor, 1 + len(body) // 4) + body)

    def wait(self, seq):
        while True:
            r = self.c.recv_response(timeout=10)
            assert r is not None, "no reply"
            if isinstance(r, X11Error):
                raise AssertionError(f"X error {r}")
            if r.response_type == 1 and r.sequence == seq:
                return r.data

    def call(self, minor, fmt, *args):
        return self.wait(self.send(self.op, minor, fmt, *args))

    def sync(self):
        self.wait(self.c.send_request(struct.pack("<BxH", 43, 1)))

    def resources(self):
        d = self.call(RR_GET_SCREEN_RESOURCES_CURRENT, "I", self.c.root_window)
        self.config_ts = struct.unpack_from("<I", d, 12)[0]
        nc, no, nm = struct.unpack_from("<HHH", d, 16)
        self.crtcs = list(struct.unpack_from(f"<{nc}I", d, 32))
        self.outputs = list(struct.unpack_from(f"<{no}I", d, 32 + 4 * nc))
        self.modes = {}
        off = 32 + 4 * (nc + no)
        for i in range(nm):
            mid, w, h = struct.unpack_from("<IHH", d, off + 32 * i)
            self.modes.setdefault((w, h), mid)

    def crtc_info(self, crtc):
        d = self.call(RR_GET_CRTC_INFO, "II", crtc, self.config_ts)
        x, y, w, h, mode, rot = struct.unpack_from("<hhHHIH", d, 12)
        return {"mode": mode, "w": w, "h": h, "rotation": rot}

    def set_screen_size(self, w, h):
        self.send(self.op, RR_SET_SCREEN_SIZE, "IHHII", self.c.root_window, w, h, w // 4, h // 4)
        self.sync()

    def set_crtc(self, crtc, mode, outputs, rotation=ROTATE_0):
        self.resources()
        d = self.call(RR_SET_CRTC_CONFIG, f"IIIhhIHxx{len(outputs)}I",
                      crtc, 0, self.config_ts, 0, 0, mode, rotation, *outputs)
        assert d[1] == 0, f"SetCrtcConfig status {d[1]}"
        self.resources()

    def set_gamma(self, crtc, red, green, blue):
        n = len(red)
        self.send(self.op, RR_SET_CRTC_GAMMA, f"IHxx{n}H{n}H{n}H" + ("xx" if (3 * n) % 2 else ""),
                  crtc, n, *red, *green, *blue)
        self.sync()

    def gamma_size(self, crtc):
        return struct.unpack_from("<H", self.call(RR_GET_CRTC_GAMMA_SIZE, "I", crtc), 8)[0]

    def screen_saver(self, activate):
        """ForceScreenSaver: the screen saver blanks through the driver's
        SaveScreen, not through the DPMS extension."""
        self.c.send_request(struct.pack("<BBH", 115, 1 if activate else 0, 1))
        self.sync()

    def dpms(self, level):
        op = self.c.query_extension("DPMS").opcode
        self.send(op, DPMS_ENABLE, "")
        self.send(op, DPMS_FORCE_LEVEL, "Hxx", level)
        self.sync()


def _kernel(node):
    with kms.Device(node) as d:
        s = d.state()
    (crtc,) = s["crtcs"].values()      # vkms has one CRTC and one output
    (conn,) = s["connectors"].values()
    return crtc, conn, list(s["crtcs"])[0]


def _assert_showing(node, w, h):
    crtc, conn, crtc_id = _kernel(node)
    assert crtc["active"], crtc
    assert crtc["mode"] and (crtc["mode"].hdisplay, crtc["mode"].vdisplay) == (w, h), crtc
    assert conn["crtc"] == crtc_id, conn
    assert crtc["plane_fb"], f"no framebuffer on the primary plane: {crtc}"


def _assert_off(node):
    crtc, conn, _ = _kernel(node)
    assert not crtc["active"], crtc
    assert conn["dpms"] == DPMS_OFF, conn


def test_initial_mode(server):
    srv, node, _ = server
    rr = RandR(srv.display_num)
    info = rr.crtc_info(rr.crtcs[0])
    _assert_showing(node, info["w"], info["h"])


def test_mode_change(server):
    srv, node, _ = server
    rr = RandR(srv.display_num)
    rr.set_screen_size(1920, 1080)
    rr.set_crtc(rr.crtcs[0], rr.modes[(1920, 1080)], rr.outputs)
    _assert_showing(node, 1920, 1080)
    rr.set_crtc(rr.crtcs[0], rr.modes[(1024, 768)], rr.outputs)
    _assert_showing(node, 1024, 768)


def test_output_off_and_on(server):
    srv, node, _ = server
    rr = RandR(srv.display_num)
    mode = rr.crtc_info(rr.crtcs[0])["mode"]
    rr.set_crtc(rr.crtcs[0], 0, [])
    _assert_off(node)
    crtc, conn, _ = _kernel(node)
    assert conn["crtc"] == 0 and not crtc["plane_fb"], (crtc, conn)
    rr.set_crtc(rr.crtcs[0], mode, rr.outputs)
    _assert_showing(node, 1024, 768)


def test_dpms_off_and_on(server):
    srv, node, _ = server
    rr = RandR(srv.display_num)
    rr.dpms(DPMS_OFF)
    _assert_off(node)
    rr.dpms(DPMS_ON)
    _assert_showing(node, 1024, 768)


def test_output_off_after_dpms_cycle(server):
    """With atomic modesetting, DPMS off left the primary plane on the
    CRTC, and every later commit that turned the CRTC off failed: an
    output switched off after a DPMS cycle stayed on."""
    srv, node, log = server
    rr = RandR(srv.display_num)
    mode = rr.crtc_info(rr.crtcs[0])["mode"]
    rr.dpms(DPMS_OFF)
    rr.dpms(DPMS_ON)
    rr.set_crtc(rr.crtcs[0], 0, [])
    _assert_off(node)
    rr.set_crtc(rr.crtcs[0], mode, rr.outputs)
    _assert_showing(node, 1024, 768)
    assert "commit" not in log.read_text(errors="replace"), "an atomic commit failed"


def test_rotation(server):
    srv, node, _ = server
    rr = RandR(srv.display_num)
    mode = rr.crtc_info(rr.crtcs[0])["mode"]
    rr.set_screen_size(1024, 1024)
    rr.set_crtc(rr.crtcs[0], mode, rr.outputs, ROTATE_90)
    info = rr.crtc_info(rr.crtcs[0])
    assert (info["w"], info["h"], info["rotation"]) == (768, 1024, ROTATE_90), info
    # the driver rotates in software: the kernel shows the unrotated mode
    _assert_showing(node, 1024, 768)
    rr.set_crtc(rr.crtcs[0], mode, rr.outputs, ROTATE_0)
    _assert_showing(node, 1024, 768)


def test_gamma(server):
    srv, node, _ = server
    rr = RandR(srv.display_num)
    crtc = rr.crtcs[0]
    n = rr.gamma_size(crtc)
    assert n > 1
    ramp = [i * 0xFFFF // (n - 1) for i in range(n)]
    rr.set_gamma(crtc, [0] * n, ramp, ramp)   # no red at all
    with kms.Device(node) as d:
        (props,) = [d.props(c, kms.OBJECT_CRTC) for c in d.resources()[0]]
        assert props.get("GAMMA_LUT"), props
        lut = d.blob(props["GAMMA_LUT"])
    entries = [struct.unpack_from("<HHHH", lut, i) for i in range(0, len(lut), 8)]
    assert all(r == 0 for r, g, b, _ in entries), entries[:4]
    assert entries[-1][1] == 0xFFFF and entries[-1][2] == 0xFFFF, entries[-1]


def test_screen_saver_blanks_and_unblanks(server):
    """The screen saver turns outputs and CRTCs off one by one; with atomic
    modesetting, unblanking must set the mode again."""
    srv, node, log = server
    rr = RandR(srv.display_num)
    rr.screen_saver(True)
    _assert_off(node)
    rr.screen_saver(False)
    _assert_showing(node, 1024, 768)
    # and the server's view of the routing is still right afterwards
    mode = rr.crtc_info(rr.crtcs[0])["mode"]
    rr.set_crtc(rr.crtcs[0], 0, [])
    _assert_off(node)
    rr.set_crtc(rr.crtcs[0], mode, rr.outputs)
    _assert_showing(node, 1024, 768)
    assert "commit" not in log.read_text(errors="replace"), "an atomic commit failed"


def test_dpms_after_screen_saver(server):
    srv, node, log = server
    rr = RandR(srv.display_num)
    rr.screen_saver(True)
    rr.screen_saver(False)
    rr.dpms(DPMS_OFF)
    _assert_off(node)
    rr.dpms(DPMS_ON)
    _assert_showing(node, 1024, 768)
    assert "commit" not in log.read_text(errors="replace"), "an atomic commit failed"
