# SPDX-License-Identifier: MIT OR X11
#
# Render's convolution filter, compared with pixman's arithmetic done in
# Python. In software this checks the reference against pixman; with glamor
# on a render node it checks glamor's GPU path (glamor_convolve.c), which
# the compositor's background blur relies on, and its software fallback
# for kernels too large for it.

import struct

import pytest
from proto import render
from test_explicit_sync import RENDER_NODE
from xclient import RawX11Connection, X11Error, X11Reply

X_CREATE_PIXMAP = 53
X_CREATE_GC = 55
X_POLY_FILL_RECTANGLE = 70
X_PUT_IMAGE = 72
X_GET_IMAGE = 73
Z_PIXMAP = 2

RENDER_CREATE_PICTURE = 4
RENDER_COMPOSITE = 8
RENDER_SET_PICTURE_FILTER = 30
CP_REPEAT = 1
REPEAT_NONE, REPEAT_PAD = 0, 2
OP_SRC = 1

SRC_W, SRC_H = 16, 12
DST_W, DST_H = 20, 16
DST_FILL = 0x80405060


@pytest.fixture(params=["software", "glamor"])
def renderer(request):
    if request.param == "glamor" and RENDER_NODE is None:
        pytest.skip("no render node")
    return request.param


@pytest.fixture
def xserver_args(renderer):
    if renderer == "glamor":
        return ["-glamor", "-dri", RENDER_NODE]
    return []


def _fixed(v):
    return int(round(v * 65536))


def _gauss(n):
    w = [2.718281828 ** (-((k - (n - 1) / 2) ** 2) / (2 * max(n / 4, 0.5) ** 2)) for k in range(n)]
    return [x / sum(w) for x in w]


def _box(n):
    return [1.0 / n] * n


SHARPEN = [0, -0.25, 0, -0.25, 2.0, -0.25, 0, -0.25, 0]


def _source_pixel(x, y):
    """A pattern with every channel varying and a non-trivial alpha."""
    a = (x * 37 + y * 11 + 90) & 0xFF
    r = (x * 23 + y * 7) & 0xFF
    g = (x * 5 + y * 29 + 40) & 0xFF
    b = (x * 13 ^ y * 17) & 0xFF
    return (a << 24) | (r << 16) | (g << 8) | b


def _reference(params, repeat, src_depth, x, y):
    """pixman's bits_image_fetch_pixel_convolution for source pixel
    center (x + 0.5, y + 0.5), as ARGB."""
    cw, ch = params[0] >> 16, params[1] >> 16
    px, py = (x << 16) + 0x8000, (y << 16) + 0x8000
    x1 = (px - 1 - (((cw << 16) - 0x10000) >> 1)) >> 16
    y1 = (py - 1 - (((ch << 16) - 0x10000) >> 1)) >> 16
    tot = [0, 0, 0, 0]
    k = 2
    for j in range(y1, y1 + ch):
        for i in range(x1, x1 + cw):
            f = params[k]
            k += 1
            if not f:
                continue
            if repeat == REPEAT_PAD:
                sx, sy = min(max(i, 0), SRC_W - 1), min(max(j, 0), SRC_H - 1)
            elif 0 <= i < SRC_W and 0 <= j < SRC_H:
                sx, sy = i, j
            else:
                continue
            p = _source_pixel(sx, sy)
            if src_depth == 24:
                p |= 0xFF000000
            for c in range(4):
                tot[c] += ((p >> (24 - 8 * c)) & 0xFF) * f
    out = 0
    for c in range(4):
        v = min(max((tot[c] + 0x8000) >> 16, 0), 0xFF)
        out |= v << (24 - 8 * c)
    return out


class Render:
    def __init__(self, xserver):
        self.c = RawX11Connection(xserver.display_num)
        ext = self.c.query_extension("RENDER")
        self.opcode = ext.opcode
        self.c.send_request(render.QueryVersionRequest(opcode=self.opcode))
        self.c.recv_response(timeout=5.0)
        self.c.send_request(render.QueryPictFormatsRequest(opcode=self.opcode))
        reply = render.QueryPictFormatsReply.from_reply(self.c.recv_response(timeout=5.0).data, "<")
        self.formats = {}
        for f in reply.formats:
            if f.type == 1 and f.red == 16 and f.green == 8 and f.blue == 0 and f.red_mask == 0xFF:
                if f.depth == 32 and f.alpha == 24 and f.alpha_mask == 0xFF:
                    self.formats[32] = f.id
                elif f.depth == 24 and f.alpha_mask == 0:
                    self.formats[24] = f.id

    def req(self, opcode, data1, body):
        body += b"\0" * (-len(body) % 4)
        return self.c.send_request(struct.pack("<BBH", opcode, data1, 1 + len(body) // 4) + body)

    def rreq(self, minor, body):
        return self.req(self.opcode, minor, body)

    def reply(self, seq):
        while True:
            r = self.c.recv_response(timeout=5.0)
            assert r is not None
            assert not isinstance(r, X11Error), r
            if isinstance(r, X11Reply) and r.sequence == seq:
                return r

    def picture(self, depth, w, h, pixels, repeat=None):
        pix, gc, pic = self.c.alloc_id(), self.c.alloc_id(), self.c.alloc_id()
        self.req(X_CREATE_PIXMAP, depth, struct.pack("<IIHH", pix, self.c.root_window, w, h))
        self.req(X_CREATE_GC, 0, struct.pack("<II", gc, pix) + struct.pack("<I", 0))
        data = b"".join(struct.pack("<I", pixels(x, y)) for y in range(h) for x in range(w))
        self.req(X_PUT_IMAGE, Z_PIXMAP, struct.pack("<IIHHhhBB2x", pix, gc, w, h, 0, 0, 0, depth) + data)
        if repeat is None:
            self.rreq(RENDER_CREATE_PICTURE, struct.pack("<IIII", pic, pix, self.formats[depth], 0))
        else:
            self.rreq(RENDER_CREATE_PICTURE,
                      struct.pack("<IIIII", pic, pix, self.formats[depth], CP_REPEAT, repeat))
        return pix, pic

    def set_filter(self, pic, params):
        name = b"convolution"
        body = struct.pack("<IH2x", pic, len(name)) + name + b"\0" * (-len(name) % 4)
        body += struct.pack(f"<{len(params)}i", *params)
        self.rreq(RENDER_SET_PICTURE_FILTER, body)

    def composite(self, src, dst, sx, sy, dx, dy, w, h):
        self.rreq(RENDER_COMPOSITE, struct.pack("<B3xIIIhhhhhhHH", OP_SRC, src, 0, dst,
                                                sx, sy, 0, 0, dx, dy, w, h))

    def image(self, pix, w, h):
        r = self.reply(self.req(X_GET_IMAGE, Z_PIXMAP, struct.pack("<IhhHHI", pix, 0, 0, w, h, 0xFFFFFFFF)))
        return struct.unpack_from(f"<{w * h}I", r.data, 32)


# (name, source depth, dest depth, kernel w, h, weights, repeat,
#  source origin, dest origin, size)
CASES = [
    ("gauss-row-pad", 24, 24, 5, 1, _gauss(5), REPEAT_PAD, (0, 0), (0, 0), (16, 12)),
    ("gauss-column-offset", 24, 24, 1, 5, _gauss(5), REPEAT_PAD, (3, 2), (1, 1), (10, 8)),
    ("alpha-none-edges", 32, 32, 5, 1, _gauss(5), REPEAT_NONE, (-2, -1), (2, 3), (16, 12)),
    ("sharpen-clamps", 32, 24, 3, 3, SHARPEN, REPEAT_PAD, (0, 0), (2, 2), (16, 12)),
    ("even-box-none", 24, 32, 4, 2, _box(8), REPEAT_NONE, (1, 0), (0, 0), (16, 12)),
    ("63-taps", 32, 32, 9, 7, _box(63), REPEAT_PAD, (0, 0), (4, 2), (16, 12)),
    ("72-taps", 24, 24, 9, 8, _box(72), REPEAT_PAD, (2, 2), (0, 0), (12, 10)),
]


@pytest.mark.parametrize("case", CASES, ids=[c[0] for c in CASES])
def test_convolution_matches_pixman(xvfb, case):
    name, sdepth, ddepth, kw, kh, weights, repeat, (sx, sy), (dx, dy), (w, h) = case
    x = Render(xvfb)
    if 24 not in x.formats or 32 not in x.formats:
        pytest.skip("no a8r8g8b8/x8r8g8b8 formats")
    params = [kw << 16, kh << 16] + [_fixed(v) for v in weights]

    smask = 0xFFFFFFFF if sdepth == 32 else 0xFFFFFF
    _, src = x.picture(sdepth, SRC_W, SRC_H, lambda px, py: _source_pixel(px, py) & smask, repeat)
    dpix, dst = x.picture(ddepth, DST_W, DST_H, lambda px, py: DST_FILL)
    x.set_filter(src, params)
    x.composite(src, dst, sx, sy, dx, dy, w, h)
    got = x.image(dpix, DST_W, DST_H)

    dmask = 0xFFFFFFFF if ddepth == 32 else 0xFFFFFF
    bad = []
    for py in range(DST_H):
        for px in range(DST_W):
            if dx <= px < dx + w and dy <= py < dy + h:
                want = _reference(params, repeat, sdepth, px - dx + sx, py - dy + sy)
            else:
                want = DST_FILL
            want &= dmask
            g = got[py * DST_W + px] & dmask
            # GPU arithmetic is float, pixman's is 16.16 fixed point
            if any(abs(((g >> s) & 0xFF) - ((want >> s) & 0xFF)) > 1 for s in (0, 8, 16, 24)):
                bad.append((px, py, hex(g), hex(want)))
    assert not bad, f"{len(bad)} pixels differ, first: {bad[:5]}"
