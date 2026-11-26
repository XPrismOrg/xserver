# SPDX-License-Identifier: MIT OR X11
#
# Explicit sync (DRI3 1.4 / Present 1.4) end to end: Xvfb with glamor on a
# DRM render node presents a pixmap only once the client signals the
# acquire point, and signals the release point after the copy.
#
# Needs a render node whose driver has timeline sync objects; skipped
# otherwise (as in CI, which has no GPU).

import fcntl
import glob
import os
import struct
import ctypes
import time

import pytest
from test_compositor import RED, Client, _near
from xclient import X11Error

DRM_CAP_SYNCOBJ_TIMELINE = 0x14
PRESENT_CAPABILITY_SYNCOBJ = 16


def _iowr(nr, size):
    return (3 << 30) | (size << 16) | (ord("d") << 8) | nr


DRM_IOCTL_GET_CAP = _iowr(0x0C, 16)
DRM_IOCTL_SYNCOBJ_CREATE = _iowr(0xBF, 8)
DRM_IOCTL_SYNCOBJ_DESTROY = _iowr(0xC0, 8)
DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD = _iowr(0xC1, 16)
DRM_IOCTL_SYNCOBJ_QUERY = _iowr(0xCB, 24)
DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL = _iowr(0xCD, 24)


def _ioctl(fd, req, buf):
    b = bytearray(buf)
    fcntl.ioctl(fd, req, b)
    return bytes(b)


def _has_timeline_syncobj(fd):
    try:
        out = _ioctl(fd, DRM_IOCTL_GET_CAP, struct.pack("<QQ", DRM_CAP_SYNCOBJ_TIMELINE, 0))
    except OSError:
        return False
    return struct.unpack("<QQ", out)[1] != 0


def _render_node():
    for node in sorted(glob.glob("/dev/dri/renderD*")):
        try:
            fd = os.open(node, os.O_RDWR | os.O_CLOEXEC)
        except OSError:
            continue
        try:
            if _has_timeline_syncobj(fd):
                return node
        finally:
            os.close(fd)
    return None


class Syncobj:
    """A DRM timeline sync object, as a Vulkan or EGL driver makes one."""

    def __init__(self, fd):
        self.fd = fd
        self.handle = struct.unpack("<II", _ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, struct.pack("<II", 0, 0)))[0]

    def export(self):
        out = _ioctl(self.fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, struct.pack("<IIiI", self.handle, 0, -1, 0))
        return struct.unpack("<IIiI", out)[2]

    def signal(self, point):
        handles = (ctypes.c_uint32 * 1)(self.handle)
        points = (ctypes.c_uint64 * 1)(point)
        arg = struct.pack("<QQII", ctypes.addressof(handles), ctypes.addressof(points), 1, 0)
        _ioctl(self.fd, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, arg)

    def point(self):
        """The last signalled point."""
        handles = (ctypes.c_uint32 * 1)(self.handle)
        points = (ctypes.c_uint64 * 1)(0)
        arg = struct.pack("<QQII", ctypes.addressof(handles), ctypes.addressof(points), 1, 0)
        _ioctl(self.fd, DRM_IOCTL_SYNCOBJ_QUERY, arg)
        return points[0]

    def close(self):
        _ioctl(self.fd, DRM_IOCTL_SYNCOBJ_DESTROY, struct.pack("<II", self.handle, 0))


# XPRISM_RENDER_NODE picks one, e.g. to test each GPU of a machine.
RENDER_NODE = os.environ.get("XPRISM_RENDER_NODE") or _render_node()

pytestmark = pytest.mark.skipif(RENDER_NODE is None, reason="no render node with timeline sync objects")


@pytest.fixture(params=["off", "on"])
def compositor(request):
    return request.param


@pytest.fixture
def xserver_args(compositor):
    return ["-br", "-compositor", compositor, "-glamor", "-dri", RENDER_NODE]


class SyncClient(Client):
    def __init__(self, xserver):
        super().__init__(xserver)
        self.present = self._ext("Present", 1, 4)
        self.dri3 = self._ext("DRI3", 1, 4)

    def _ext(self, name, major, minor):
        ext = self.c.query_extension(name)
        if ext is None:
            pytest.skip(f"{name} not available")
        self.reply(self.req(ext.opcode, 0, struct.pack("<II", major, minor)))
        return ext.opcode

    def reply(self, seq):
        while True:
            r = self.c.recv_response(timeout=5.0)
            assert r is not None
            assert not isinstance(r, X11Error), r
            if r.response_type == 1 and r.sequence == seq:
                return r.data

    def capabilities(self, target):
        data = self.reply(self.req(self.present, 4, struct.pack("<I", target)))
        return struct.unpack_from("<I", data, 8)[0]

    def import_syncobj(self, syncobj, drawable):
        xid = self.c.alloc_id()
        req = struct.pack("<BBHII", self.dri3, 10, 3, xid, drawable)
        fd = syncobj.export()
        try:
            self.c.send_request_with_fds(req, [fd])
        finally:
            os.close(fd)
        return xid

    def pixmap(self, w, h, pixel):
        pix = self.c.alloc_id()
        self.req(53, 24, struct.pack("<IIHH", pix, self.root, w, h))  # CreatePixmap
        self.fill(pix, pixel, 0, 0, w, h)
        return pix

    def present_synced(self, window, pixmap, serial, acquire, acquire_point, release, release_point):
        body = struct.pack("<IIIIIhhIIIQQIIQQQ", window, pixmap, serial, 0, 0, 0, 0, 0,
                           acquire, release, acquire_point, release_point, 0, 0, 0, 0, 0)
        self.req(self.present, 5, body)

    def wait_pixel(self, x, y, want, timeout=3.0):
        end = time.monotonic() + timeout
        while True:
            got = self.pixel(x, y)
            if _near(got, want) or time.monotonic() > end:
                return got
            time.sleep(0.02)


@pytest.fixture
def drm():
    fd = os.open(RENDER_NODE, os.O_RDWR | os.O_CLOEXEC)
    yield fd
    os.close(fd)


def test_window_reports_syncobj(xvfb):
    x = SyncClient(xvfb)
    w = x.window(10, 10, 64, 64, 0)
    assert x.capabilities(w) & PRESENT_CAPABILITY_SYNCOBJ


def test_copy_waits_for_acquire_and_signals_release(xvfb, drm):
    x = SyncClient(xvfb)
    w = x.window(100, 100, 64, 64, 0)
    x.map(w)
    pix = x.pixmap(64, 64, RED)
    acquire, release = Syncobj(drm), Syncobj(drm)
    try:
        a = x.import_syncobj(acquire, w)
        r = x.import_syncobj(release, w)
        x.present_synced(w, pix, 1, a, 1, r, 1)
        x.sync()

        # The client's rendering is not done: nothing may show yet.
        time.sleep(0.3)
        assert x.pixel(132, 132) == 0
        assert release.point() == 0

        acquire.signal(1)
        assert _near(x.wait_pixel(132, 132, RED), RED)
        end = time.monotonic() + 3.0
        while release.point() < 1 and time.monotonic() < end:
            time.sleep(0.02)
        assert release.point() == 1
    finally:
        acquire.close()
        release.close()


def test_already_signalled_acquire_presents_at_once(xvfb, drm):
    x = SyncClient(xvfb)
    w = x.window(100, 100, 64, 64, 0)
    x.map(w)
    pix = x.pixmap(64, 64, RED)
    acquire, release = Syncobj(drm), Syncobj(drm)
    try:
        acquire.signal(5)
        a = x.import_syncobj(acquire, w)
        r = x.import_syncobj(release, w)
        x.present_synced(w, pix, 1, a, 5, r, 7)
        assert _near(x.wait_pixel(132, 132, RED), RED)
        end = time.monotonic() + 3.0
        while release.point() < 7 and time.monotonic() < end:
            time.sleep(0.02)
        assert release.point() == 7
    finally:
        acquire.close()
        release.close()
