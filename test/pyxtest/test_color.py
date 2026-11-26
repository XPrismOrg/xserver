# SPDX-License-Identifier: MIT
#
# XPRISM-COLOR v0.1 (Xext/composite/compcolorext.c, doc/color.md):
# applications say which colour space their windows' pixels are in, and
# the scene compositor shows them converted for the (sRGB) screen.
#
# Expected pixels come from a reference implementation here, written
# from the standards (IEC 61966-2-1, SMPTE ST 2084) and the gamut mapping
# doc/color.md describes, not from the server's code.

import math
import struct

import pytest
from test_compositor import (BLUE, GREEN, RED, SCREEN_H, SCREEN_W, Client,
                             _becomes_direct, _get_image, _is_composited)
from test_scene_protocol import ONE, TRANSFORM, Scene
from xclient import RawX11Connection, X11Error

QUERY_VERSION, SET, GET, CLEAR = 0, 1, 2, 3
CUSTOM, SRGB, P3, BT2020 = 0, 1, 2, 3
TF_SRGB, TF_GAMMA22, TF_LINEAR, TF_PQ, TF_HLG = 0, 1, 2, 3, 4

BAD_REQUEST, BAD_VALUE, BAD_WINDOW, BAD_MATCH, BAD_LENGTH = 1, 2, 3, 8, 16

XY = {
    SRGB: [(0.64, 0.33), (0.30, 0.60), (0.15, 0.06), (0.3127, 0.3290)],
    P3: [(0.680, 0.320), (0.265, 0.690), (0.150, 0.060), (0.3127, 0.3290)],
    BT2020: [(0.708, 0.292), (0.170, 0.797), (0.131, 0.046), (0.3127, 0.3290)],
}


@pytest.fixture
def xserver_args():
    return ["-br", "-compositor", "on"]


# ---- reference -----------------------------------------------------------

def srgb_decode(v):
    return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4


def srgb_encode(v):
    v = min(max(v, 0.0), 1.0)
    return 12.92 * v if v <= 0.0031308 else 1.055 * v ** (1 / 2.4) - 0.055


def pq_to_nits(v):
    m1, m2 = 2610 / 16384, 2523 / 4096 * 128
    c1, c2, c3 = 3424 / 4096, 2413 / 4096 * 32, 2392 / 4096 * 32
    p = v ** (1 / m2)
    return 10000 * (max(p - c1, 0) / (c2 - c3 * p)) ** (1 / m1)


def nits_to_pq(n):
    m1, m2 = 2610 / 16384, 2523 / 4096 * 128
    c1, c2, c3 = 3424 / 4096, 2413 / 4096 * 32, 2392 / 4096 * 32
    p = (min(max(n, 0) / 10000, 1)) ** m1
    return ((c1 + c2 * p) / (1 + c3 * p)) ** m2


def tone_map(nits, src_max, dst_max):
    """ITU-R BT.2390 EETF (black level 0) in the PQ domain."""
    if dst_max >= src_max or nits <= 0:
        return nits
    src_pq = nits_to_pq(src_max)
    e1 = nits_to_pq(min(nits, src_max)) / src_pq
    max_l = nits_to_pq(dst_max) / src_pq
    ks = max(1.5 * max_l - 0.5, 0)
    if e1 > ks:
        t = (e1 - ks) / (1 - ks)
        e1 = ((2 * t**3 - 3 * t**2 + 1) * ks + (t**3 - 2 * t**2 + t) * (1 - ks)
              + (-2 * t**3 + 3 * t**2) * max_l)
    return pq_to_nits(e1 * src_pq)


def hlg_grey(v, peak=1000):
    """Display light of an HLG grey signal, cd/m^2 (BT.2100 OOTF)."""
    a = 0.17883277
    b, c = 1 - 4 * a, 0.5 - a * math.log(4 * a)
    e = v * v / 3 if v <= 0.5 else (math.exp((v - c) / a) + b) / 12
    gamma = 1.2 + 0.42 * math.log10(peak / 1000)
    return peak * e ** gamma


def _inv(m):
    a, b, c = m[0]
    d, e, f = m[1]
    g, h, i = m[2]
    det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
    return [[(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det],
            [(f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det],
            [(d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det]]


def _mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def _apply(m, v):
    return [sum(m[i][k] * v[k] for k in range(3)) for i in range(3)]


def to_xyz(xy):
    cols = [[x / y, 1, (1 - x - y) / y] for x, y in xy[:3]]
    p = [[cols[j][i] for j in range(3)] for i in range(3)]
    wx, wy = xy[3]
    s = _apply(_inv(p), [wx / wy, 1, (1 - wx - wy) / wy])
    return [[p[i][j] * s[j] for j in range(3)] for i in range(3)]


def to_srgb_matrix(xy):
    """Linear RGB in primaries xy to linear sRGB (same D65 white)."""
    return _mul(_inv(to_xyz(XY[SRGB])), to_xyz(xy))


def gamut_map(rgb):
    """Into [0, 1] by moving towards the luminance, which keeps the hue."""
    y = 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2]
    y = min(max(y, 0.0), 1.0)
    s = 1.0
    for c in rgb:
        if c > 1 and c - y > 0:
            s = min(s, (1 - y) / (c - y))
        elif c < 0:
            s = min(s, y / (y - c))
    return [y + s * (c - y) for c in rgb]


def expect(pixel, xy, decode):
    """The sRGB pixel shown for an RGB pixel in a space with primaries xy
    and the transfer function decode (to linear light, 1 = white)."""
    lin = [decode(((pixel >> s) & 0xFF) / 255) for s in (16, 8, 0)]
    out = gamut_map(_apply(to_srgb_matrix(xy), lin))
    r, g, b = (round(srgb_encode(c) * 255) for c in out)
    return (r << 16) | (g << 8) | b


def near(got, want, tol=2):
    return all(abs(((got >> s) & 0xFF) - ((want >> s) & 0xFF)) <= tol for s in (16, 8, 0))


# ---- client ----------------------------------------------------------------

def desc(primaries=SRGB, transfer=TF_SRGB, xy=None, ref_white=0, min_lum=0, max_lum=0):
    coords = [0] * 8
    if xy:
        coords = [round(v * 1e6) for p in xy for v in p]
    return struct.pack("<BBH8III I", primaries, transfer, 0, *coords, ref_white, min_lum, max_lum)


class Color(Client):
    """A client speaking XPRISM-COLOR (optionally byte-swapped)."""

    def __init__(self, xvfb, swapped=False):
        super().__init__(xvfb)
        if swapped:
            self.c = RawX11Connection(xvfb.display_num, swapped=True)
            self.root = self.c.root_window
        self.bo = ">" if swapped else "<"
        ext = self.c.query_extension("XPRISM-COLOR")
        assert ext, "XPRISM-COLOR missing"
        self.op = ext.opcode

    def _req(self, minor, body):
        hdr = struct.pack(self.bo + "BBH", self.op, minor, 1 + len(body) // 4)
        return self.c.send_request(hdr + body)

    def _errors(self):
        sync = self.c.send_request(struct.pack(self.bo + "BxH", 43, 1))   # GetInputFocus
        errs = []
        while True:
            r = self.c.recv_response(timeout=5.0)
            assert r is not None, "no answer"
            if isinstance(r, X11Error):
                errs.append(r.error_code)
            elif r.response_type == 1 and r.sequence == sync:
                return errs

    def _reply(self, seq):
        while True:
            r = self.c.recv_response(timeout=5.0)
            assert r is not None, "no answer"
            if isinstance(r, X11Error):
                return r
            if r.response_type == 1 and r.sequence == seq:
                return r

    def version(self):
        r = self._reply(self._req(QUERY_VERSION, struct.pack(self.bo + "II", 0, 1)))
        assert not isinstance(r, X11Error), r
        return struct.unpack_from(self.bo + "II", r.data, 8)

    def set(self, wid, d):
        if self.bo == ">":     # desc() packs little-endian: repack
            f = struct.unpack("<BBH8III I", d)
            d = struct.pack(">BBH8III I", *f)
        self._req(SET, struct.pack(self.bo + "I", wid) + d)
        return self._errors()

    def get(self, wid):
        """(set, primaries, transfer, xy (8 ints), refWhite, minLum, maxLum)"""
        r = self._reply(self._req(GET, struct.pack(self.bo + "I", wid)))
        if isinstance(r, X11Error):
            return r.error_code
        f = struct.unpack_from(self.bo + "BBH8III I", r.data, 8)
        return (r.data[1], f[0], f[1], list(f[3:11]), f[11], f[12], f[13])

    def clear(self, wid):
        self._req(CLEAR, struct.pack(self.bo + "I", wid))
        return self._errors()

    def child(self, parent, x, y, w, h, pixel):
        wid = self.c.alloc_id()
        self.req(1, 0, struct.pack("<IIhhHHHHIII", wid, parent, x, y, w, h, 0, 1, 0, 2, pixel))
        self.map(wid)
        return wid


def _window(x, pixel, x0=100, y0=100, w=100, h=100):
    wid = x.window(x0, y0, w, h, pixel)
    x.map(wid)
    return wid


# ---- protocol --------------------------------------------------------------

def test_version(xvfb):
    assert Color(xvfb).version() == (0, 1)


def test_unset_window_is_srgb(xvfb):
    x = Color(xvfb)
    w = _window(x, RED)
    got = x.get(w)
    assert got[:3] == (0, SRGB, TF_SRGB)
    assert got[3] == [round(v * 1e6) for p in XY[SRGB] for v in p]
    assert got[4] == 80 and got[6] == 80


def test_set_get_clear(xvfb):
    x = Color(xvfb)
    w = _window(x, RED)
    assert x.set(w, desc(P3, TF_SRGB)) == []
    got = x.get(w)
    assert got[:3] == (1, P3, TF_SRGB)
    assert got[3] == [round(v * 1e6) for p in XY[P3] for v in p]
    assert x.clear(w) == []
    assert x.get(w)[0] == 0


def test_hdr_defaults(xvfb):
    x = Color(xvfb)
    w = _window(x, RED)
    assert x.set(w, desc(BT2020, TF_PQ)) == []
    assert x.get(w)[4:] == (203, 0, 10000)
    assert x.set(w, desc(BT2020, TF_HLG, min_lum=50)) == []
    assert x.get(w)[4:] == (203, 50, 1000)
    assert x.set(w, desc(BT2020, TF_PQ, ref_white=100, max_lum=4000)) == []
    assert x.get(w)[4:] == (100, 0, 4000)


def test_custom_primaries(xvfb):
    x = Color(xvfb)
    w = _window(x, RED)
    xy = [(0.7, 0.3), (0.2, 0.7), (0.14, 0.05), (0.3127, 0.329)]
    assert x.set(w, desc(CUSTOM, TF_GAMMA22, xy=xy)) == []
    got = x.get(w)
    assert got[1] == CUSTOM
    assert got[3] == [round(v * 1e6) for p in xy for v in p]


@pytest.mark.parametrize("bad", [
    desc(4),                                                # primaries
    desc(SRGB, 5),                                          # transfer
    desc(CUSTOM, xy=[(0, 0.3), (0.3, 0.6), (0.15, 0.06), (0.3127, 0.329)]),
    desc(CUSTOM, xy=[(0.7, 0.4), (0.3, 0.6), (0.15, 0.06), (0.3127, 0.329)]),  # x + y > 1
    # three primaries on a line
    desc(CUSTOM, xy=[(0.2, 0.2), (0.3, 0.3), (0.4, 0.4), (0.3127, 0.329)]),
    desc(SRGB, TF_PQ, ref_white=10001),
    desc(SRGB, TF_PQ, max_lum=10001),
])
def test_bad_descriptions(xvfb, bad):
    x = Color(xvfb)
    w = _window(x, RED)
    assert x.set(w, bad) == [BAD_VALUE]
    assert x.get(w)[0] == 0


def test_bad_windows(xvfb):
    x = Color(xvfb)
    assert x.set(0x1fffff, desc(P3)) == [BAD_WINDOW]
    assert x.get(0x1fffff) == BAD_WINDOW
    assert x.clear(0x1fffff) == [BAD_WINDOW]
    io = x.c.alloc_id()
    x.req(1, 0, struct.pack("<IIhhHHHHII", io, x.root, 0, 0, 10, 10, 0, 2, 0, 0))  # InputOnly
    assert x.set(io, desc(P3)) == [BAD_MATCH]


def test_bad_length(xvfb):
    x = Color(xvfb)
    w = _window(x, RED)
    x._req(SET, struct.pack("<I", w) + desc(P3)[:-4])
    assert x._errors() == [BAD_LENGTH]
    x._req(9, b"")
    assert x._errors() == [BAD_REQUEST]


def test_swapped_client(xvfb):
    x = Color(xvfb, swapped=True)
    assert x.version() == (0, 1)
    creator = Color(xvfb)               # core requests: Client packs them little-endian
    w = _window(creator, RED)
    assert creator.sync() == []
    assert x.set(w, desc(BT2020, TF_PQ, max_lum=1000)) == []
    got = x.get(w)
    assert got[:3] == (1, BT2020, TF_PQ)
    assert got[3] == [round(v * 1e6) for p in XY[BT2020] for v in p]
    assert got[4:] == (203, 0, 1000)


# ---- conversion ------------------------------------------------------------

GREY = 0x808080
ORANGE = 0xC86432


def test_untagged_windows_are_untouched(xvfb):
    x = Color(xvfb)
    _window(x, ORANGE)
    assert x.pixel(150, 150) == ORANGE


def test_explicit_srgb_is_untouched(xvfb):
    x = Color(xvfb)
    w = _window(x, ORANGE)
    assert x.set(w, desc(SRGB, TF_SRGB)) == []
    assert x.pixel(150, 150) == ORANGE


def test_linear_light(xvfb):
    x = Color(xvfb)
    w = _window(x, GREY)
    assert x.set(w, desc(SRGB, TF_LINEAR)) == []
    want = expect(GREY, XY[SRGB], lambda v: v)
    assert near(x.pixel(150, 150), want), (hex(x.pixel(150, 150)), hex(want))
    assert want == 0xBCBCBC


def test_gamma22(xvfb):
    x = Color(xvfb)
    w = _window(x, ORANGE)
    assert x.set(w, desc(SRGB, TF_GAMMA22)) == []
    want = expect(ORANGE, XY[SRGB], lambda v: v ** 2.2)
    assert near(x.pixel(150, 150), want), (hex(x.pixel(150, 150)), hex(want))


@pytest.mark.parametrize("primaries", [P3, BT2020])
@pytest.mark.parametrize("pixel", [ORANGE, GREY, 0x3060A0, RED, GREEN, BLUE])
def test_wide_gamut(xvfb, primaries, pixel):
    x = Color(xvfb)
    w = _window(x, pixel)
    assert x.set(w, desc(primaries, TF_SRGB)) == []
    want = expect(pixel, XY[primaries], srgb_decode)
    got = x.pixel(150, 150)
    assert near(got, want), (hex(got), hex(want))


def test_white_stays_white(xvfb):
    x = Color(xvfb)
    w = _window(x, 0xFFFFFF)
    assert x.set(w, desc(BT2020, TF_SRGB)) == []
    assert near(x.pixel(150, 150), 0xFFFFFF, 1)


def test_out_of_gamut_keeps_its_hue(xvfb):
    """P3's red is redder than sRGB can show: it is desaturated, not
    clipped channel by channel (which would give pure red)."""
    x = Color(xvfb)
    w = _window(x, RED)
    assert x.set(w, desc(P3, TF_SRGB)) == []
    got = x.pixel(150, 150)
    r, g, b = got >> 16, (got >> 8) & 0xFF, got & 0xFF
    assert r == 255 and 0 < g < b < 80, hex(got)


def test_clear_shows_the_pixels_again(xvfb):
    x = Color(xvfb)
    w = _window(x, ORANGE)
    assert x.set(w, desc(P3)) == []
    assert x.pixel(150, 150) != ORANGE
    assert x.clear(w) == []
    assert x.pixel(150, 150) == ORANGE


def test_change_of_description(xvfb):
    x = Color(xvfb)
    w = _window(x, GREY)
    assert x.set(w, desc(SRGB, TF_LINEAR)) == []
    assert near(x.pixel(150, 150), 0xBCBCBC)
    assert x.set(w, desc(SRGB, TF_GAMMA22)) == []
    assert near(x.pixel(150, 150), expect(GREY, XY[SRGB], lambda v: v ** 2.2))


def test_drawing_is_converted(xvfb):
    x = Color(xvfb)
    w = _window(x, GREY)
    assert x.set(w, desc(SRGB, TF_LINEAR)) == []
    assert near(x.pixel(150, 150), 0xBCBCBC)
    x.fill(w, 0x404040, 10, 10, 20, 20)
    want = expect(0x404040, XY[SRGB], lambda v: v)
    assert near(x.pixel(120, 120), want), hex(x.pixel(120, 120))
    assert near(x.pixel(150, 150), 0xBCBCBC)      # the rest as before


def test_window_contents_are_not_changed(xvfb):
    """Conversion is for the screen: the window keeps its own pixels."""
    x = Color(xvfb)
    w = _window(x, GREY)
    assert x.set(w, desc(SRGB, TF_LINEAR)) == []
    x.pixel(0, 0)
    data = _get_image(x, w, 50, 50)
    assert struct.unpack_from("<I", data, 32)[0] & 0xFFFFFF == GREY


def test_tagged_child(xvfb):
    x = Color(xvfb)
    top = _window(x, GREY)
    child = x.child(top, 50, 0, 50, 100, GREY)
    assert x.set(child, desc(SRGB, TF_LINEAR)) == []
    assert x.pixel(120, 150) == GREY                  # the parent's part
    assert near(x.pixel(170, 150), 0xBCBCBC)          # the child's


def test_children_inherit(xvfb):
    x = Color(xvfb)
    top = _window(x, GREY)
    child = x.child(top, 50, 0, 50, 100, GREY)
    assert x.set(top, desc(SRGB, TF_LINEAR)) == []
    assert near(x.pixel(120, 150), 0xBCBCBC)
    assert near(x.pixel(170, 150), 0xBCBCBC)
    # a child can say it is sRGB after all
    assert x.set(child, desc(SRGB, TF_SRGB)) == []
    assert near(x.pixel(120, 150), 0xBCBCBC)
    assert x.pixel(170, 150) == GREY


def test_child_destroyed(xvfb):
    x = Color(xvfb)
    top = _window(x, GREY)
    child = x.child(top, 50, 0, 50, 100, GREY)
    assert x.set(child, desc(SRGB, TF_LINEAR)) == []
    assert near(x.pixel(170, 150), 0xBCBCBC)
    x.req(4, 0, struct.pack("<I", child))     # DestroyWindow
    assert x.pixel(170, 150) == GREY


def test_argb_window(xvfb):
    x = Color(xvfb)
    visual, cmap = x.argb_visual()
    # premultiplied: half-transparent 0x808080
    w = x.window(100, 100, 100, 100, 0x80404040, depth=32, visual=visual, colormap=cmap)
    x.map(w)
    assert x.set(w, desc(SRGB, TF_LINEAR)) == []
    got = x.pixel(150, 150)            # over the black root
    v = round(srgb_encode(0x80 / 255) * 0x80)
    assert near(got, v * 0x010101), (hex(got), v)


def test_pq(xvfb):
    x = Color(xvfb)
    # content mastered for reference white as its peak: no tone mapping
    w = _window(x, 0x000000)
    assert x.set(w, desc(SRGB, TF_PQ, max_lum=203)) == []
    for v in (0x40, 0x80, 0x94):
        x.fill(w, v * 0x010101, 0, 0, 100, 100)
        lin = min(pq_to_nits(v / 255) / 203, 1)
        want = round(srgb_encode(lin) * 255)
        got = x.pixel(150, 150) & 0xFF
        assert abs(got - want) <= 2, (hex(v), got, want)


def test_pq_tone_mapping(xvfb):
    x = Color(xvfb)
    w = _window(x, 0xFFFFFF)
    assert x.set(w, desc(SRGB, TF_PQ, max_lum=10000)) == []
    assert near(x.pixel(150, 150), 0xFFFFFF, 1)            # the peak is white
    # brighter than reference white: compressed into what SDR can show
    shown = []
    for v in (0x60, 0x80, 0x94, 0xB0, 0xD0, 0xFF):
        x.fill(w, v * 0x010101, 0, 0, 100, 100)
        shown.append(x.pixel(150, 150) & 0xFF)
    assert shown == sorted(shown) and len(set(shown)) == len(shown), shown
    # below the knee (about 90 cd/m^2 here) nothing changes, above it
    # the curve is BT.2390's
    want = [round(srgb_encode(tone_map(pq_to_nits(v / 255), 10000, 203) / 203) * 255)
            for v in (0x60, 0x80, 0x94, 0xB0, 0xD0, 0xFF)]
    assert all(abs(g - w) <= 2 for g, w in zip(shown, want)), (shown, want)


def test_hlg(xvfb):
    x = Color(xvfb)
    w = _window(x, 0xFFFFFF)
    assert x.set(w, desc(BT2020, TF_HLG)) == []
    assert near(x.pixel(150, 150), 0xFFFFFF, 1)
    x.fill(w, 0, 0, 0, 100, 100)
    assert x.pixel(150, 150) == 0
    shown = []
    for v in (0x40, 0x80, 0xC0):
        x.fill(w, v * 0x010101, 0, 0, 100, 100)
        shown.append(x.pixel(150, 150))
    assert all(p & 0xFF == p >> 16 == (p >> 8) & 0xFF for p in shown)   # grey
    want = [round(srgb_encode(tone_map(hlg_grey(v / 255), 1000, 203) / 203) * 255)
            for v in (0x40, 0x80, 0xC0)]
    assert all(abs((g & 0xFF) - w) <= 2 for g, w in zip(shown, want)), (shown, want)


def test_fullscreen_window_is_converted(xvfb):
    """Not shown directly: the screen could not convert it."""
    x = Color(xvfb)
    w = _window(x, GREY, 0, 0, SCREEN_W, SCREEN_H)
    assert x.set(w, desc(SRGB, TF_LINEAR)) == []
    assert near(x.pixel(600, 500), 0xBCBCBC)
    assert _is_composited(x, w)


def test_fullscreen_srgb_window_is_shown_directly(xvfb):
    """An sRGB description changes nothing, not even how it is shown."""
    x = Color(xvfb)
    w = _window(x, GREY, 0, 0, SCREEN_W, SCREEN_H)
    assert x.set(w, desc(SRGB, TF_SRGB)) == []
    assert x.pixel(600, 500) == GREY
    assert _becomes_direct(x, w)


def test_root_drawing_over_a_window_is_converted(xvfb):
    """Drawing on the root with IncludeInferiors lands in the windows'
    contents (test_compositor.py), so it is shown in their colour space."""
    x = Color(xvfb)
    w = _window(x, GREY)
    assert x.set(w, desc(SRGB, TF_LINEAR)) == []
    assert near(x.pixel(150, 150), 0xBCBCBC)
    gc = x.c.alloc_id()
    x.req(55, 0, struct.pack("<IIIII", gc, x.root, 1 << 2 | 1 << 15, 0x404040, 1))
    x.req(70, 0, struct.pack("<IIhhHH", x.root, gc, 140, 140, 20, 20))
    want = expect(0x404040, XY[SRGB], lambda v: v)
    assert near(x.pixel(150, 150), want), hex(x.pixel(150, 150))
    assert near(x.pixel(130, 130), 0xBCBCBC)


def test_transformed_window(xvfb):
    x = Color(xvfb)
    w = _window(x, GREY)
    assert x.set(w, desc(SRGB, TF_LINEAR)) == []
    wm = Scene(xvfb)
    wm.become_wm(x)
    assert wm.set(w, TRANSFORM, transform=[ONE, 0, 300 * ONE, 0, ONE, 0, 0, 0, ONE]) == []
    assert near(x.pixel(450, 150), 0xBCBCBC)
    assert x.pixel(150, 150) == 0


def test_window_moves(xvfb):
    x = Color(xvfb)
    w = _window(x, GREY)
    assert x.set(w, desc(SRGB, TF_LINEAR)) == []
    assert near(x.pixel(150, 150), 0xBCBCBC)
    x.move(w, 400)
    assert near(x.pixel(450, 150), 0xBCBCBC)
    assert x.pixel(150, 150) == 0


def test_resized_window(xvfb):
    x = Color(xvfb)
    w = _window(x, GREY)
    assert x.set(w, desc(SRGB, TF_LINEAR)) == []
    assert near(x.pixel(150, 150), 0xBCBCBC)
    x.req(12, 0, struct.pack("<IHxxII", w, 0x4 | 0x8, 300, 200))     # width, height
    assert near(x.pixel(350, 250), 0xBCBCBC)


def test_other_clients_windows(xvfb):
    """Any client that may change a window's attributes may describe it,
    as with its properties."""
    owner = Color(xvfb)
    w = _window(owner, GREY)
    assert owner.sync() == []
    other = Color(xvfb)
    assert other.set(w, desc(SRGB, TF_LINEAR)) == []
    assert near(owner.pixel(150, 150), 0xBCBCBC)


def test_destroyed_with_description(xvfb):
    x = Color(xvfb)
    w = _window(x, GREY)
    assert x.set(w, desc(SRGB, TF_LINEAR)) == []
    x.pixel(0, 0)
    x.req(4, 0, struct.pack("<I", w))
    assert x.pixel(150, 150) == 0
    w2 = _window(x, GREY)
    assert x.pixel(150, 150) == GREY


class TestWithoutCompositor:
    @pytest.fixture
    def xserver_args(self):
        return ["-br", "-compositor", "off"]

    def test_absent(self, xvfb):
        x = Client(xvfb)
        assert not x.c.query_extension("XPRISM-COLOR")
