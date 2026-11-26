# SPDX-License-Identifier: MIT
#
# One colour space per CRTC (CompositeSetCrtcColorSpace, Xvfb
# -crtccolorspace): monitors of one screen showing different spaces, an
# sRGB one next to an HDR one. The compositor composes for the widest
# and converts what each CRTC shows for its own; what no CRTC shows is in
# the screen's space.
#
# Xvfb lights only its first CRTC; the tests light the second through
# RandR, right of the first.

import struct

import pytest
from test_color import (BT2020, GREY, ORANGE, SRGB, TF_PQ, XY, Color, _window, desc,
                        nits_to_pq, pq_to_nits, srgb_decode, srgb_encode, tone_map)
from test_color_output import (channels10, get_output, pq_encode_rel, px30, raw_pixel,
                               rgb30, shown)
from test_compositor import RED
from xclient import X11Error

RR_QUERY_VERSION, RR_CREATE_MODE, RR_ADD_OUTPUT_MODE = 0, 16, 18
RR_GET_CRTC_INFO, RR_SET_CRTC_CONFIG, RR_GET_SCREEN_RESOURCES_CURRENT = 20, 21, 25

W, H = 640, 480


@pytest.fixture
def xserver_args():
    return ["-br", "-compositor", "on", "-screen", "0", f"{2 * W}x{H}x30",
            "-crtcs", f"2@{W}x{H}", "-crtccolorspace", "1:bt2020,pq,1000"]


class RandR:
    def __init__(self, x):
        self.c = x.c
        ext = self.c.query_extension("RANDR")
        assert ext
        self.op = ext.opcode
        self.call(RR_QUERY_VERSION, "II", 1, 6)
        self.resources()

    def call(self, minor, fmt, *args):
        body = struct.pack("<" + fmt, *args)
        seq = self.c.send_request(struct.pack("<BBH", self.op, minor, 1 + len(body) // 4) + body)
        while True:
            r = self.c.recv_response(timeout=10)
            assert r is not None, "no reply"
            assert not isinstance(r, X11Error), r
            if r.response_type == 1 and r.sequence == seq:
                return r.data

    def resources(self):
        d = self.call(RR_GET_SCREEN_RESOURCES_CURRENT, "I", self.c.root_window)
        self.config_ts = struct.unpack_from("<I", d, 12)[0]
        nc, no = struct.unpack_from("<HH", d, 16)
        self.crtcs = list(struct.unpack_from(f"<{nc}I", d, 32))
        self.outputs = list(struct.unpack_from(f"<{no}I", d, 32 + 4 * nc))

    def light(self, i, x, y=0):
        """Show CRTC i, on output i, at x, y."""
        name = f"{W}x{H}-{i}".encode()
        pad = b"\0" * (-len(name) % 4)
        d = self.call(RR_CREATE_MODE, f"IIHHIHHHHHHHHI{len(name) + len(pad)}s",
                      self.c.root_window, 0, W, H, 25175000, W + 16, W + 112, W + 160, 0,
                      H + 10, H + 12, H + 45, len(name), 0, name + pad)
        mode = struct.unpack_from("<I", d, 8)[0]
        self.c.send_request(struct.pack("<BBHII", self.op, RR_ADD_OUTPUT_MODE, 3,
                                        self.outputs[i], mode))
        self.move(i, x, y, mode)

    def move(self, i, x, y=0, mode=None):
        self.resources()
        if mode is None:
            d = self.call(RR_GET_CRTC_INFO, "II", self.crtcs[i], self.config_ts)
            mode = struct.unpack_from("<I", d, 20)[0]
        d = self.call(RR_SET_CRTC_CONFIG, "IIIhhIHxxI", self.crtcs[i], 0, self.config_ts,
                      x, y, mode, 1, self.outputs[i])
        assert d[1] == 0, f"SetCrtcConfig status {d[1]}"


def sdr_on_pq(pixel):
    """An sRGB pixel as the PQ CRTC shows it, 10-bit channels."""
    return [round(c * 1023) for c in shown(rgb30(px30(pixel)), XY[SRGB], srgb_decode,
                                           XY[BT2020], pq_encode_rel, 1000 / 203)]


def near(got, want, tol):
    return all(abs(g - w) <= tol for g, w in zip(got, want))


def test_reported(xvfb):
    x = Color(xvfb)
    rr = RandR(x)
    srgb, pq = (get_output(x, o) for o in rr.outputs)
    assert srgb[0] == 0                         # not set: sRGB
    assert pq[:3] == (1, BT2020, TF_PQ)         # its CRTC's, lit or not
    assert pq[4:] == (203, 50, 1000)


@pytest.mark.parametrize("pixel", [ORANGE, RED, GREY])
def test_each_crtc_shows_its_space(xvfb, pixel):
    x = Color(xvfb)
    RandR(x).light(1, W)
    _window(x, px30(pixel), W - 100, 100, 200, 200)       # across both
    left = channels10(raw_pixel(x, W - 50, 150))
    right = channels10(raw_pixel(x, W + 50, 150))
    # composed in 10-bit PQ without glamor: back in sRGB, near-black
    # channels of saturated colours are a little off (sRGB is steep there)
    assert near(left, channels10(px30(pixel)), 12), left
    assert near(right, sdr_on_pq(pixel), 3), (right, sdr_on_pq(pixel))


def test_hdr_window_across_both(xvfb):
    """On the PQ CRTC as it is; on the sRGB one tone mapped to SDR white."""
    x = Color(xvfb)
    RandR(x).light(1, W)
    w = _window(x, 0, W - 100, 100, 200, 200)
    assert x.set(w, desc(BT2020, TF_PQ, max_lum=1000)) == []
    for nits in (100, 203, 600, 1000):
        v = round(nits_to_pq(nits) * 1023)
        x.fill(w, (v << 20) | (v << 10) | v, 0, 0, 200, 200)
        right = channels10(raw_pixel(x, W + 50, 150))
        assert near(right, [v] * 3, 2), (nits, right, v)
        left = channels10(raw_pixel(x, W - 50, 150))
        shows = tone_map(pq_to_nits(v / 1023), 1000, 203) / 203
        want = round(srgb_encode(shows) * 1023)
        assert near(left, [want] * 3, 4), (nits, left, want)
    assert left[0] >= 1020                      # the peak at SDR white


def test_unlit_parts_are_in_the_screens_space(xvfb):
    """With the PQ CRTC off, the right half is not shown: sRGB, as the
    screen's. Lit, it is converted."""
    x = Color(xvfb)
    _window(x, px30(ORANGE), W - 100, 100, 200, 200)
    assert near(channels10(raw_pixel(x, W + 50, 150)), channels10(px30(ORANGE)), 3)
    RandR(x).light(1, W)
    assert near(channels10(raw_pixel(x, W + 50, 150)), sdr_on_pq(ORANGE), 3)


def test_moving_a_crtc_repaints(xvfb):
    """Moved over the first CRTC, the PQ CRTC no longer shows the right
    half, which is the screen's space again; where they overlap, the
    first CRTC wins."""
    x = Color(xvfb)
    rr = RandR(x)
    rr.light(1, W)
    _window(x, px30(ORANGE), W - 100, 100, 200, 200)
    assert near(channels10(raw_pixel(x, W + 50, 150)), sdr_on_pq(ORANGE), 3)
    rr.move(1, 0)
    assert near(channels10(raw_pixel(x, W + 50, 150)), channels10(px30(ORANGE)), 3)
    assert near(channels10(raw_pixel(x, W - 50, 150)), channels10(px30(ORANGE)), 3)
    rr.move(1, W)
    assert near(channels10(raw_pixel(x, W + 50, 150)), sdr_on_pq(ORANGE), 3)


def test_bad_option():
    import os
    import resource
    import subprocess
    xvfb = os.environ.get("XVFB_PATH")
    if not xvfb:
        pytest.skip("XVFB_PATH not set")
    for args in (["-crtccolorspace", "0:bt2020,pq"],            # before -crtcs
                 ["-crtcs", "2", "-crtccolorspace", "2:p3"],
                 ["-crtcs", "2", "-crtccolorspace", "1:foo"],
                 ["-crtcs", "2", "-crtccolorspace", "p3"]):
        r = subprocess.run([xvfb, ":97", *args], capture_output=True, timeout=30,
                           preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
        assert r.returncode != 0, args
        assert b"crtccolorspace" in r.stderr, args
