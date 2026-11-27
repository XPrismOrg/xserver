# SPDX-License-Identifier: MIT
#
# Composition in linear light (doc/color.md, "Composition in linear light"): on outputs other
# than sRGB, with glamor, the compositor blends windows in a half float
# buffer in the output's linear light and encodes the result once. These
# tests need a GPU (--xvfb-args='-glamor -dri /dev/dri/renderD128') and
# are skipped without one; the CPU keeps composing in the output's
# encoding, which test_color_output.py covers.
#
# Expected pixels come from the reference in test_color.py.

import struct

import pytest
from test_color import (BT2020, P3, SRGB, TF_PQ, XY, Color, _apply, _window,
                        desc, nits_to_pq, pq_to_nits, srgb_decode, srgb_encode)
from test_color_output import (as8, channels10, matrix, near8, pq_encode_rel,
                               raw_pixel, rgb8)
from test_compositor import _get_image
from test_scene_protocol import BLUR, SHADOW, Scene

X_CREATE_GC = 55
X_PUT_IMAGE = 72
Z_PIXMAP = 2
HALF = 0x80000000           # the opacity hint for 50%
HALF_A = HALF / 0xFFFFFFFF


@pytest.fixture(autouse=True)
def gpu(request):
    if "-glamor" not in request.config.getoption("--xvfb-args"):
        pytest.skip("composition in linear light needs glamor on a GPU")


def put_row(x, wid, depth, pixels, px=0, py=0):
    """Put 32-bit pixels into one row of wid."""
    gc = x.c.alloc_id()
    x.req(X_CREATE_GC, 0, struct.pack("<II I", gc, wid, 0))
    data = struct.pack(f"<{len(pixels)}I", *pixels)
    x.req(X_PUT_IMAGE, Z_PIXMAP,
          struct.pack("<IIHHhhBBxx", wid, gc, len(pixels), 1, px, py, 0, depth) + data)


def root_row(x, px, py, n):
    x.sync()
    data = _get_image(x, x.root, px, py, n, 1)
    assert isinstance(data, bytes), data
    return struct.unpack_from(f"<{n}I", data, 32)


def p3_linear(rgb):
    """sRGB channels to P3 linear light"""
    return _apply(matrix(XY[SRGB], XY[P3]), [srgb_decode(c) for c in rgb])


class TestP3:
    """Display P3 (sRGB transfer), 8 bits per channel."""

    @pytest.fixture
    def xserver_args(self):
        return ["-br", "-compositor", "on", "-colorspace", "p3"]

    def test_translucent_window(self, xvfb):
        """Half of white over black is half the light: 188, not 128."""
        x = Color(xvfb)
        w = _window(x, 0xFFFFFF)
        x.opacity(w, HALF)
        want = as8([srgb_encode(HALF_A)] * 3)
        assert near8(x.pixel(150, 150), want, tol=1), hex(x.pixel(150, 150))

    def test_argb_window(self, xvfb):
        x = Color(xvfb)
        visual, cmap = x.argb_visual()
        w = x.window(100, 100, 100, 100, 0x80800000, depth=32, visual=visual, colormap=cmap)
        x.map(w)
        a = 0x80 / 255
        want = as8([srgb_encode(a * c) for c in p3_linear([1, 0, 0])])
        assert near8(x.pixel(150, 150), want, tol=1), hex(x.pixel(150, 150))

    def test_translucent_shadow(self, xvfb):
        x = Color(xvfb)
        w = _window(x, 0x808080)
        wm = Scene(xvfb)
        wm.become_wm(x)
        assert wm.decorate(w, SHADOW, dx=20, dy=20, color=0x80FF0000) == []
        a = 0x80 / 255
        want = as8([srgb_encode(a * c) for c in p3_linear(rgb8(0xFF0000))])
        assert near8(x.pixel(210, 210), want, tol=1), hex(x.pixel(210, 210))

    def test_background_blur(self, xvfb):
        """What the blur reads and writes stays in half float: a dark grey
        backdrop comes back as it was (8 bits of linear light would turn
        16 into 13 or 22)."""
        x = Color(xvfb)
        _window(x, 0x101010, x0=100, y0=100, w=200, h=200)
        visual, cmap = x.argb_visual()
        glass = x.window(150, 150, 50, 50, 0, depth=32, visual=visual, colormap=cmap)
        x.map(glass)
        wm = Scene(xvfb)
        wm.become_wm(x)
        assert wm.decorate(glass, BLUR, bg_blur=16) == []
        assert near8(x.pixel(175, 175), 0x101010, tol=1), hex(x.pixel(175, 175))


class TestPQ:
    """BT.2020 with PQ, 1000 cd/m^2 peak, on a depth-30 screen."""

    @pytest.fixture
    def xserver_args(self):
        return ["-br", "-compositor", "on", "-screen", "0", "1280x1024x30",
                "-colorspace", "bt2020,pq,1000"]

    def test_translucent_hdr_window(self, xvfb):
        """Half of 800 cd/m^2 is 400: blended as light, above SDR white
        (203), so the buffer keeps values above 1."""
        x = Color(xvfb)
        c = round(nits_to_pq(800) * 1023)
        w = _window(x, (c << 20) | (c << 10) | c)
        assert x.set(w, desc(BT2020, TF_PQ, max_lum=1000)) == []
        x.opacity(w, HALF)
        want = round(nits_to_pq(pq_to_nits(c / 1023) * HALF_A) * 1023)
        got = channels10(raw_pixel(x, 150, 150))
        assert all(abs(g - want) <= 1 for g in got), (got, want)

    def test_argb_window_has_10_bits(self, xvfb):
        """An ARGB window's pixels reach the 10-bit screen without being
        rounded to 8 bits of PQ on the way."""
        x = Color(xvfb)
        visual, cmap = x.argb_visual()
        w = x.window(100, 100, 256, 10, 0, depth=32, visual=visual, colormap=cmap)
        x.map(w)
        put_row(x, w, 32, [0xFF000000 | v * 0x010101 for v in range(256)])
        bt2020 = matrix(XY[SRGB], XY[BT2020])
        got = root_row(x, 100, 100, 256)
        bad = []
        for v, p in enumerate(got):
            lin = _apply(bt2020, [srgb_decode(v / 255)] * 3)
            want = [round(pq_encode_rel(c) * 1023) for c in lin]
            if any(abs(g - e) > 1 for g, e in zip(channels10(p), want)):
                bad.append((v, channels10(p), want))
        assert not bad, bad[:8]

    def test_output_space_is_kept_exactly(self, xvfb):
        """Every 10-bit code of a window in the output's own space comes
        back unchanged through linear light, as when shown directly, down
        to the output's black level. Below it (codes 1 to 15: under 0.005
        cd/m^2) half float keeps too little, and the output shows none of
        it anyway."""
        x = Color(xvfb)
        w = _window(x, 0, x0=100, y0=100, w=1024, h=10)
        assert x.set(w, desc(BT2020, TF_PQ, max_lum=1000)) == []
        codes = [(i, 1023 - i, (i * 37) % 1024) for i in range(1024)]
        put_row(x, w, 30, [(r << 20) | (g << 10) | b for r, g, b in codes])
        black = round(nits_to_pq(0.005) * 1023)
        assert black == 15
        got = [tuple(channels10(p)) for p in root_row(x, 100, 100, 1024)]
        bad = [(i, g, e) for i, (g, e) in enumerate(zip(got, codes))
               if any(gc != ec if ec > black else gc > black for gc, ec in zip(g, e))]
        assert not bad, (len(bad), bad[:8])
