# SPDX-License-Identifier: MIT
#
# XPRISM-COLOR 0.2: outputs in colour spaces other than sRGB (Xvfb
# -colorspace, doc/color.md step 3). The compositor shows every window,
# described or not, converted for the output; windows in the output's
# own space are shown as they are, and can be shown directly.
#
# Expected pixels come from the reference in test_color.py.

import os
import resource
import struct
import subprocess

import pytest
from test_color import (BT2020, GREY, ORANGE, P3, SRGB, TF_GAMMA22, TF_PQ,
                        TF_SRGB, XY, Color, _apply, _inv, _mul, _window, desc,
                        nits_to_pq, pq_to_nits, srgb_decode, srgb_encode,
                        to_xyz, tone_map)
from test_compositor import (RED, SCREEN_H, SCREEN_W, _becomes_direct,
                             _get_image, _is_composited)
from test_scene_protocol import SHADOW, Scene
from xclient import X11Error

GET_OUTPUT = 4


def matrix(src_xy, out_xy):
    return _mul(_inv(to_xyz(out_xy)), to_xyz(src_xy))


def gamut(rgb, luma, peak):
    y = min(max(sum(w * c for w, c in zip(luma, rgb)), 0.0), peak)
    s = 1.0
    for c in rgb:
        if c > peak and c - y > 0:
            s = min(s, (peak - y) / (c - y))
        elif c < 0:
            s = min(s, y / (y - c))
    return [y + s * (c - y) for c in rgb]


def shown(rgb, src_xy, decode, out_xy, encode, peak=1.0):
    """Channels in [0, 1] as the output shows a pixel (channels in [0, 1])
    of a space with primaries src_xy and transfer decode."""
    lin = _apply(matrix(src_xy, out_xy), [decode(c) for c in rgb])
    luma = to_xyz(out_xy)[1]
    return [encode(c) for c in gamut(lin, luma, peak)]


def rgb8(p):
    return [((p >> s) & 0xFF) / 255 for s in (16, 8, 0)]


def as8(ch):
    r, g, b = (round(c * 255) for c in ch)
    return (r << 16) | (g << 8) | b


def near8(got, want, tol=2):
    return all(abs(((got >> s) & 0xFF) - ((want >> s) & 0xFF)) <= tol for s in (16, 8, 0))


def raw_pixel(x, px, py):
    x.sync()
    data = _get_image(x, x.root, px, py)
    assert isinstance(data, bytes), data
    return struct.unpack_from("<I", data, 32)[0]


def px30(p):
    """An 8-bit RGB pixel as x2r10g10b10."""
    r, g, b = (round(((p >> s) & 0xFF) * 1023 / 255) for s in (16, 8, 0))
    return (r << 20) | (g << 10) | b


def rgb30(p):
    return [c / 1023 for c in channels10(p)]


def channels10(p):
    """x2r10g10b10"""
    return [(p >> s) & 0x3FF for s in (20, 10, 0)]


def output_id(x):
    """The first RandR output of the screen."""
    ext = x.c.query_extension("RANDR")
    assert ext
    x.c.send_request(struct.pack("<BBHII", ext.opcode, 0, 3, 1, 6))       # QueryVersion 1.6
    seq = x.c.send_request(struct.pack("<BBHI", ext.opcode, 8, 2, x.root))  # GetScreenResources
    while True:
        r = x.c.recv_response(timeout=5.0)
        assert r is not None and not isinstance(r, X11Error), r
        if r.response_type == 1 and r.sequence == seq:
            ncrtcs, noutputs = struct.unpack_from("<HH", r.data, 16)
            assert noutputs
            return struct.unpack_from("<I", r.data, 32 + 4 * ncrtcs)[0]


def get_output(x, output):
    r = x._reply(x._req(GET_OUTPUT, struct.pack("<I", output)))
    if isinstance(r, X11Error):
        return r.error_code
    f = struct.unpack_from("<BBH8III I", r.data, 8)
    return (r.data[1], f[0], f[1], list(f[3:11]), f[11], f[12], f[13])


# ---- the default: sRGB ------------------------------------------------------

class TestSRGBOutput:
    @pytest.fixture
    def xserver_args(self):
        return ["-br", "-compositor", "on"]

    def test_reported(self, xvfb):
        x = Color(xvfb)
        got = get_output(x, output_id(x))
        assert got[:3] == (0, SRGB, TF_SRGB)
        assert got[4] == 80

    def test_bad_output(self, xvfb):
        x = Color(xvfb)
        assert isinstance(get_output(x, 0x1fffff), int)


# ---- wide gamut SDR -----------------------------------------------------------

class TestP3Output:
    @pytest.fixture
    def xserver_args(self):
        return ["-br", "-compositor", "on", "-colorspace", "p3"]

    def test_reported(self, xvfb):
        x = Color(xvfb)
        got = get_output(x, output_id(x))
        assert got[:3] == (1, P3, TF_SRGB)
        assert got[3] == [round(v * 1e6) for p in XY[P3] for v in p]
        assert got[4:] == (80, 2000, 80)

    @pytest.mark.parametrize("pixel", [ORANGE, GREY, RED, 0x3060A0])
    def test_srgb_windows_are_converted(self, xvfb, pixel):
        x = Color(xvfb)
        _window(x, pixel)
        want = as8(shown(rgb8(pixel), XY[SRGB], srgb_decode, XY[P3], srgb_encode))
        got = x.pixel(150, 150)
        assert near8(got, want), (hex(got), hex(want))
        if pixel != GREY:
            assert got != pixel

    def test_p3_windows_are_shown_as_they_are(self, xvfb):
        x = Color(xvfb)
        w = _window(x, ORANGE)
        assert x.set(w, desc(P3, TF_SRGB)) == []
        assert x.pixel(150, 150) == ORANGE

    def test_bt2020_window(self, xvfb):
        x = Color(xvfb)
        w = _window(x, ORANGE)
        assert x.set(w, desc(BT2020, TF_SRGB)) == []
        want = as8(shown(rgb8(ORANGE), XY[BT2020], srgb_decode, XY[P3], srgb_encode))
        assert near8(x.pixel(150, 150), want)

    def test_drawing_is_converted(self, xvfb):
        x = Color(xvfb)
        w = _window(x, GREY)
        x.pixel(0, 0)
        x.fill(w, RED, 0, 0, 20, 20)
        want = as8(shown(rgb8(RED), XY[SRGB], srgb_decode, XY[P3], srgb_encode))
        assert near8(x.pixel(110, 110), want)

    def test_root_background(self, xvfb):
        x = Color(xvfb)
        x.req(2, 0, struct.pack("<III", x.root, 0x2, ORANGE))      # CWBackPixel
        x.req(61, 0, struct.pack("<IhhHH", x.root, 0, 0, 0, 0))     # ClearArea
        want = as8(shown(rgb8(ORANGE), XY[SRGB], srgb_decode, XY[P3], srgb_encode))
        assert near8(x.pixel(10, 10), want), hex(x.pixel(10, 10))

    def test_root_background_tile(self, xvfb):
        """Also a regression test, under valgrind or ASan: the compositor
        ends its redirection of the root window when the server exits,
        after the root's tile was freed, and painted with it. On sRGB
        outputs a scratch GC's reference to the tile hid that."""
        x = Color(xvfb)
        pix = x.c.alloc_id()
        x.req(53, 24, struct.pack("<IIHH", pix, x.root, 16, 16))   # CreatePixmap
        x.fill(pix, ORANGE, 0, 0, 16, 16)
        x.req(2, 0, struct.pack("<III", x.root, 0x1, pix))         # CWBackPixmap
        x.req(61, 0, struct.pack("<IhhHH", x.root, 0, 0, 0, 0))     # ClearArea
        want = as8(shown(rgb8(ORANGE), XY[SRGB], srgb_decode, XY[P3], srgb_encode))
        assert near8(x.pixel(10, 10), want), hex(x.pixel(10, 10))
        # a new tile is converted too, even where the old one was freed
        # and the new one has its address
        x.req(54, 0, struct.pack("<I", pix))                        # FreePixmap
        x.req(2, 0, struct.pack("<III", x.root, 0x2, 0))           # CWBackPixel: tile gone
        x.req(61, 0, struct.pack("<IhhHH", x.root, 0, 0, 0, 0))
        x.pixel(10, 10)
        pix2 = x.c.alloc_id()
        x.req(53, 24, struct.pack("<IIHH", pix2, x.root, 16, 16))
        x.fill(pix2, GREY, 0, 0, 16, 16)
        x.req(2, 0, struct.pack("<III", x.root, 0x1, pix2))
        x.req(61, 0, struct.pack("<IhhHH", x.root, 0, 0, 0, 0))
        want = as8(shown(rgb8(GREY), XY[SRGB], srgb_decode, XY[P3], srgb_encode))
        assert near8(x.pixel(10, 10), want), hex(x.pixel(10, 10))

    def test_drawing_into_root_tile(self, xvfb):
        """Drawing into the tile shows when the root is next painted, as
        on sRGB outputs."""
        x = Color(xvfb)
        pix = x.c.alloc_id()
        x.req(53, 24, struct.pack("<IIHH", pix, x.root, 16, 16))   # CreatePixmap
        x.fill(pix, ORANGE, 0, 0, 16, 16)
        x.req(2, 0, struct.pack("<III", x.root, 0x1, pix))         # CWBackPixmap
        x.req(61, 0, struct.pack("<IhhHH", x.root, 0, 0, 0, 0))     # ClearArea
        x.pixel(10, 10)
        x.fill(pix, GREY, 0, 0, 16, 16)
        x.req(61, 0, struct.pack("<IhhHH", x.root, 0, 0, 0, 0))
        want = as8(shown(rgb8(GREY), XY[SRGB], srgb_decode, XY[P3], srgb_encode))
        assert near8(x.pixel(10, 10), want), hex(x.pixel(10, 10))

    def test_shadow_colour(self, xvfb):
        x = Color(xvfb)
        w = _window(x, GREY)
        wm = Scene(xvfb)
        wm.become_wm(x)
        assert wm.decorate(w, SHADOW, dx=20, dy=20, color=0xFFFF0000) == []
        want = as8(shown(rgb8(RED), XY[SRGB], srgb_decode, XY[P3], srgb_encode))
        assert near8(x.pixel(210, 210), want), hex(x.pixel(210, 210))

    def test_fullscreen_p3_window_is_shown_directly(self, xvfb):
        x = Color(xvfb)
        w = _window(x, ORANGE, 0, 0, SCREEN_W, SCREEN_H)
        assert x.set(w, desc(P3, TF_SRGB)) == []
        assert x.pixel(600, 500) == ORANGE
        assert _becomes_direct(x, w)

    def test_fullscreen_srgb_window_is_composited(self, xvfb):
        x = Color(xvfb)
        w = _window(x, ORANGE, 0, 0, SCREEN_W, SCREEN_H)
        want = as8(shown(rgb8(ORANGE), XY[SRGB], srgb_decode, XY[P3], srgb_encode))
        assert near8(x.pixel(600, 500), want)
        assert _is_composited(x, w)


class TestGamma22Output:
    @pytest.fixture
    def xserver_args(self):
        return ["-br", "-compositor", "on", "-colorspace", "srgb,gamma22"]

    def test_reported(self, xvfb):
        x = Color(xvfb)
        assert get_output(x, output_id(x))[:3] == (1, SRGB, TF_GAMMA22)

    @pytest.mark.parametrize("pixel", [GREY, ORANGE, 0x202020])
    def test_srgb_windows(self, xvfb, pixel):
        x = Color(xvfb)
        _window(x, pixel)
        want = as8(shown(rgb8(pixel), XY[SRGB], srgb_decode, XY[SRGB],
                         lambda v: min(max(v, 0), 1) ** (1 / 2.2)))
        got = x.pixel(150, 150)
        assert near8(got, want), (hex(got), hex(want))


# ---- HDR ------------------------------------------------------------------------

def pq_encode_rel(v):
    """Relative linear light (1 = 203 cd/m^2) to PQ."""
    return nits_to_pq(max(v, 0) * 203)


class TestPQOutput:
    """BT.2020 with PQ, 1000 cd/m^2 peak, on a depth-30 screen."""

    @pytest.fixture
    def xserver_args(self):
        return ["-br", "-compositor", "on", "-screen", "0", "1280x1024x30",
                "-colorspace", "bt2020,pq,1000"]

    def test_reported(self, xvfb):
        x = Color(xvfb)
        got = get_output(x, output_id(x))
        assert got[:3] == (1, BT2020, TF_PQ)
        assert got[4:] == (203, 50, 1000)

    def test_sdr_white_at_reference_white(self, xvfb):
        """BT.2408: SDR white is shown at 203 cd/m^2."""
        x = Color(xvfb)
        _window(x, px30(0xFFFFFF))
        want = round(nits_to_pq(203) * 1023)
        got = channels10(raw_pixel(x, 150, 150))
        assert all(abs(c - want) <= 2 for c in got), (got, want)

    @pytest.mark.parametrize("pixel", [ORANGE, RED, GREY])
    def test_sdr_colours(self, xvfb, pixel):
        x = Color(xvfb)
        _window(x, px30(pixel))
        want = [round(c * 1023) for c in shown(rgb30(px30(pixel)), XY[SRGB], srgb_decode,
                                               XY[BT2020], pq_encode_rel, 1000 / 203)]
        got = channels10(raw_pixel(x, 150, 150))
        assert all(abs(g - w) <= 3 for g, w in zip(got, want)), (got, want)

    def test_hdr_content_within_the_peak_as_it_is(self, xvfb):
        x = Color(xvfb)
        w = _window(x, px30(0xC08040))
        assert x.set(w, desc(BT2020, TF_PQ, max_lum=1000)) == []
        want = channels10(px30(0xC08040))
        got = channels10(raw_pixel(x, 150, 150))
        assert all(abs(g - w) <= 2 for g, w in zip(got, want)), (got, want)

    def test_brighter_content_is_tone_mapped_to_the_peak(self, xvfb):
        x = Color(xvfb)
        w = _window(x, 0x000000)
        assert x.set(w, desc(BT2020, TF_PQ, max_lum=4000)) == []
        for nits in (100, 500, 1000, 2000, 4000):
            v = round(nits_to_pq(nits) * 1023)
            x.fill(w, (v << 20) | (v << 10) | v, 0, 0, 100, 100)
            got = channels10(raw_pixel(x, 150, 150))[0]
            want = round(nits_to_pq(tone_map(pq_to_nits(v / 1023), 4000, 1000)) * 1023)
            assert abs(got - want) <= 3, (nits, got, want)
        assert abs(got - round(nits_to_pq(1000) * 1023)) <= 3      # the peak at the peak

    def test_bright_saturated_colour(self, xvfb):
        """Tone mapped on its luminance, then within the output's gamut and
        peak (not SDR white's)."""
        x = Color(xvfb)
        sig = [0.80, 0.55, 0.35]                  # PQ: about 1500, 180, 25 cd/m^2
        p = sum(round(c * 1023) << s for c, s in zip(sig, (20, 10, 0)))
        w = _window(x, p)
        assert x.set(w, desc(BT2020, TF_PQ, max_lum=4000)) == []
        luma = to_xyz(XY[BT2020])[1]
        lin = [pq_to_nits(round(c * 1023) / 1023) / 203 for c in sig]
        y = sum(a * b for a, b in zip(luma, lin))
        k = tone_map(y * 203, 4000, 1000) / 203 / y
        out = gamut([c * k for c in lin], luma, 1000 / 203)
        want = [round(pq_encode_rel(c) * 1023) for c in out]
        got = channels10(raw_pixel(x, 150, 150))
        assert all(abs(g - w) <= 3 for g, w in zip(got, want)), (got, want)

    def test_fullscreen_hdr_window_is_shown_directly(self, xvfb):
        x = Color(xvfb)
        w = _window(x, px30(0xC08040), 0, 0, SCREEN_W, SCREEN_H)
        assert x.set(w, desc(BT2020, TF_PQ, max_lum=1000)) == []
        x.sync()
        assert _becomes_direct(x, w)


@pytest.mark.parametrize("arg", ["foo", "p3,foo", "srgb,srgb,1000", "bt2020,pq,100",
                                 "bt2020,pq,20000", "bt2020,pq,x"])
def test_bad_option(arg):
    xvfb = os.environ.get("XVFB_PATH")
    if not xvfb:
        pytest.skip("XVFB_PATH not set")
    # FatalError aborts: no core dumps (they hold the environment)
    r = subprocess.run([xvfb, ":97", "-colorspace", arg], capture_output=True, timeout=30,
                       preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
    assert r.returncode != 0
    assert b"colorspace" in r.stderr
