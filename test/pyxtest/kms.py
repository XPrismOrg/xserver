# SPDX-License-Identifier: MIT
#
# Read the kernel's modesetting state through DRM ioctls, without libdrm
# and without being the DRM master: any client may read the state of the
# CRTCs, planes and connectors. Used to check that what the X server
# reports through RandR is what the kernel actually shows.

import ctypes
import fcntl
import os
import struct

_DRM = ord("d")


def _iowr(nr, size):
    return (3 << 30) | (size << 16) | (_DRM << 8) | nr


SET_CLIENT_CAP = _iowr(0x0D, 16)
MODE_GETRESOURCES = _iowr(0xA0, 64)
MODE_GETCRTC = _iowr(0xA1, 104)
MODE_GETPROPERTY = _iowr(0xAA, 64)
MODE_GETPROPBLOB = _iowr(0xAC, 16)
MODE_GETPLANERESOURCES = _iowr(0xB5, 16)
MODE_GETPLANE = _iowr(0xB6, 32)
MODE_OBJ_GETPROPERTIES = _iowr(0xB9, 32)

CAP_UNIVERSAL_PLANES = 2
CAP_ATOMIC = 3

OBJECT_CRTC = 0xCCCCCCCC
OBJECT_CONNECTOR = 0xC0C0C0C0
OBJECT_PLANE = 0xEEEEEEEE

_MODEINFO = "<IHHHHHHHHHHIII32s"  # 68 bytes


def _buf(fmt_or_size):
    size = fmt_or_size if isinstance(fmt_or_size, int) else struct.calcsize(fmt_or_size)
    return bytearray(size)


def _array(n):
    a = (ctypes.c_uint32 * max(n, 1))()
    return a, ctypes.addressof(a)


def _array64(n):
    a = (ctypes.c_uint64 * max(n, 1))()
    return a, ctypes.addressof(a)


class Mode:
    def __init__(self, raw):
        f = struct.unpack(_MODEINFO, raw)
        self.clock = f[0]
        self.hdisplay, self.vdisplay = f[1], f[6]
        self.vrefresh = f[11]
        self.name = f[14].split(b"\0", 1)[0].decode()

    def __repr__(self):
        return f"<Mode {self.hdisplay}x{self.vdisplay}@{self.vrefresh}>"


class Device:
    """A DRM primary node opened for reading its state."""

    def __init__(self, path):
        self.fd = os.open(path, os.O_RDWR | os.O_CLOEXEC)
        for cap in (CAP_UNIVERSAL_PLANES, CAP_ATOMIC):
            fcntl.ioctl(self.fd, SET_CLIENT_CAP, struct.pack("<QQ", cap, 1))
        self._prop_names = {}

    def close(self):
        os.close(self.fd)

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()

    def resources(self):
        b = _buf(64)
        fcntl.ioctl(self.fd, MODE_GETRESOURCES, b)
        n_fb, n_crtc, n_conn, n_enc = struct.unpack_from("<IIII", b, 32)
        fbs, pf = _array(n_fb)
        crtcs, pc = _array(n_crtc)
        conns, pn = _array(n_conn)
        encs, pe = _array(n_enc)
        b = bytearray(struct.pack("<QQQQIIII", pf, pc, pn, pe, n_fb, n_crtc, n_conn, n_enc) + bytes(16))
        fcntl.ioctl(self.fd, MODE_GETRESOURCES, b)
        return list(crtcs)[:n_crtc], list(conns)[:n_conn]

    def planes(self):
        b = _buf(16)
        fcntl.ioctl(self.fd, MODE_GETPLANERESOURCES, b)
        n = struct.unpack_from("<I", b, 8)[0]
        ids, p = _array(n)
        b = bytearray(struct.pack("<QI", p, n) + bytes(4))
        fcntl.ioctl(self.fd, MODE_GETPLANERESOURCES, b)
        return list(ids)[:n]

    def crtc(self, crtc_id):
        """(fb_id, x, y, mode or None)"""
        b = bytearray(struct.pack("<QII", 0, 0, crtc_id) + bytes(104 - 16))
        fcntl.ioctl(self.fd, MODE_GETCRTC, b)
        fb, x, y, _gamma, valid = struct.unpack_from("<IIIII", b, 16)
        return fb, x, y, Mode(bytes(b[36:104])) if valid else None

    def _prop_name(self, prop_id):
        if prop_id not in self._prop_names:
            b = bytearray(struct.pack("<QQII", 0, 0, prop_id, 0) + bytes(40))
            fcntl.ioctl(self.fd, MODE_GETPROPERTY, b)
            self._prop_names[prop_id] = bytes(b[24:56]).split(b"\0", 1)[0].decode()
        return self._prop_names[prop_id]

    def props(self, obj_id, obj_type):
        """{name: value} of an object's properties."""
        b = bytearray(struct.pack("<QQIII", 0, 0, 0, obj_id, obj_type) + bytes(4))
        fcntl.ioctl(self.fd, MODE_OBJ_GETPROPERTIES, b)
        n = struct.unpack_from("<I", b, 16)[0]
        ids, pi = _array(n)
        vals, pv = _array64(n)
        b = bytearray(struct.pack("<QQIII", pi, pv, n, obj_id, obj_type) + bytes(4))
        fcntl.ioctl(self.fd, MODE_OBJ_GETPROPERTIES, b)
        return {self._prop_name(ids[i]): vals[i] for i in range(n)}

    def blob(self, blob_id):
        b = bytearray(struct.pack("<IIQ", blob_id, 0, 0))
        fcntl.ioctl(self.fd, MODE_GETPROPBLOB, b)
        n = struct.unpack_from("<I", b, 4)[0]
        data = (ctypes.c_uint8 * max(n, 1))()
        b = bytearray(struct.pack("<IIQ", blob_id, n, ctypes.addressof(data)))
        fcntl.ioctl(self.fd, MODE_GETPROPBLOB, b)
        return bytes(data)[:n]

    def state(self):
        """A summary: per CRTC its activity, mode and primary plane, per
        connector the CRTC it is routed to."""
        crtcs, conns = self.resources()
        out = {"crtcs": {}, "connectors": {}}
        planes = {p: self.props(p, OBJECT_PLANE) for p in self.planes()}
        for c in crtcs:
            p = self.props(c, OBJECT_CRTC)
            fb, x, y, mode = self.crtc(c)
            primary = next((pp for pp in planes.values()
                            if pp.get("type") == 1 and pp.get("CRTC_ID") == c), None)
            out["crtcs"][c] = {
                "active": bool(p.get("ACTIVE", 0)),
                "mode": mode,
                "fb": fb,
                "vrr": bool(p.get("VRR_ENABLED", 0)),
                "gamma_lut": p.get("GAMMA_LUT", 0),
                "rotation": primary.get("rotation") if primary else None,
                "plane_fb": primary.get("FB_ID") if primary else None,
            }
        for c in conns:
            p = self.props(c, OBJECT_CONNECTOR)
            out["connectors"][c] = {"crtc": p.get("CRTC_ID", 0), "dpms": p.get("DPMS")}
        return out


def find_vkms():
    """The primary node of a vkms device, or None."""
    base = "/sys/class/drm"
    if not os.path.isdir(base):
        return None
    for name in sorted(os.listdir(base)):
        if not name.startswith("card") or "-" in name:
            continue
        try:
            driver = os.path.basename(os.readlink(f"{base}/{name}/device/driver"))
        except OSError:
            driver = ""
        uevent = ""
        try:
            with open(f"{base}/{name}/device/uevent") as f:
                uevent = f.read()
        except OSError:
            pass
        if "vkms" in os.path.realpath(f"{base}/{name}") or driver == "vkms" or "vkms" in uevent:
            return f"/dev/dri/{name}"
    return None


if __name__ == "__main__":
    import pprint
    import sys
    with Device(sys.argv[1] if len(sys.argv) > 1 else find_vkms()) as d:
        pprint.pprint(d.state())
