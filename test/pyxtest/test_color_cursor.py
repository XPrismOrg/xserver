# SPDX-License-Identifier: MIT
#
# The software cursor on outputs other than sRGB: cursors are sRGB, and
# are drawn over the composed screen as the output shows them. GetImage
# leaves the cursor out, so these tests read the screen from Xvfb's
# framebuffer file (-fbdir).
#
# Expected pixels come from the reference in test_color.py.

import struct
import time

import pytest
from proto import render
from test_color import P3, SRGB, XY, Color, _window, srgb_decode, srgb_encode
from test_color_output import as8, near8, rgb8, shown
from test_compositor import RED, _get_image
from xclient import Extension, X11Reply

X_CHANGE_WINDOW_ATTRIBUTES = 2
X_WARP_POINTER = 41
X_CREATE_PIXMAP = 53
X_CREATE_GC = 55
X_POLY_FILL_RECTANGLE = 70
X_CREATE_CURSOR = 93
CW_CURSOR = 0x4000
GREEN = 0x00FF00


class Screen:
    """The screen as Xvfb's -fbdir file holds it (XWD, 32 bits per pixel)."""

    def __init__(self, path):
        self.path = path

    def pixel(self, px, py):
        with open(self.path, "rb") as f:
            head = struct.unpack(">25I", f.read(100))
            size, bpl, ncolors = head[0], head[12], head[19]
            f.seek(size + 12 * ncolors + py * bpl + 4 * px)
            return struct.unpack("<I", f.read(4))[0] & 0xFFFFFF


def pixmap(x, depth, w, h, pixel):
    pid = x.c.alloc_id()
    x.req(X_CREATE_PIXMAP, depth, struct.pack("<IIHH", pid, x.root, w, h))
    gc = x.c.alloc_id()
    x.req(X_CREATE_GC, 0, struct.pack("<IIII", gc, pid, 0x4, pixel))  # GCForeground
    x.req(X_POLY_FILL_RECTANGLE, 0, struct.pack("<IIhhHH", pid, gc, 0, 0, w, h))
    return pid


def core_cursor(x, rgb16):
    """A 16x16 cursor all in the foreground colour."""
    bits = pixmap(x, 1, 16, 16, 1)
    cid = x.c.alloc_id()
    x.req(X_CREATE_CURSOR, 0, struct.pack("<IIIHHHHHHHH", cid, bits, bits,
                                          *rgb16, 0, 0, 0, 0, 0))
    return cid


def argb_cursor(x, argb):
    """A 16x16 ARGB cursor, argb but for its last two columns (Render
    CreateCursor)."""
    ext = x.c.query_extension(Extension.RENDER)
    x.c.send_request(render.QueryVersionRequest(opcode=ext.opcode))
    x.c.recv_response(timeout=5.0)
    x.c.send_request(render.QueryPictFormatsRequest(opcode=ext.opcode))
    r = x.c.recv_response(timeout=5.0)
    assert isinstance(r, X11Reply)
    formats = render.QueryPictFormatsReply.from_reply(r.data, "<").formats
    fmt = next(f.id for f in formats if f.type == 1 and f.depth == 32 and
               f.alpha == 24 and f.alpha_mask == 0xFF and f.red == 16)
    pid = pixmap(x, 32, 16, 16, argb)
    # more than two colours: Render makes cursors of one or two colours
    # core cursors
    for col, pixel in ((14, 0xFF0000FF), (15, 0xFFFFFFFF)):
        gc = x.c.alloc_id()
        x.req(X_CREATE_GC, 0, struct.pack("<IIII", gc, pid, 0x4, pixel))
        x.req(X_POLY_FILL_RECTANGLE, 0, struct.pack("<IIhhHH", pid, gc, col, 0, 1, 16))
    pict = x.c.alloc_id()
    x.req(ext.opcode, 4, struct.pack("<IIII", pict, pid, fmt, 0))       # CreatePicture
    cid = x.c.alloc_id()
    x.req(ext.opcode, 27, struct.pack("<IIHH", cid, pict, 0, 0))       # CreateCursor
    return cid


def show_cursor(x, wid, cid):
    x.req(X_CHANGE_WINDOW_ATTRIBUTES, 0, struct.pack("<III", wid, CW_CURSOR, cid))
    x.req(X_WARP_POINTER, 0, struct.pack("<IIhhHHhh", 0, wid, 0, 0, 0, 0, 50, 50))
    x.sync()


def screen_pixel(screen, px, py, want):
    """The pixel once the cursor is drawn (in the block handler)."""
    got = None
    for _ in range(50):
        got = screen.pixel(px, py)
        if near8(got, want, tol=1):
            break
        time.sleep(0.02)
    return got


class TestP3:
    @pytest.fixture
    def xserver_args(self, tmp_path):
        return ["-br", "-compositor", "on", "-colorspace", "p3", "-fbdir", str(tmp_path)]

    @pytest.fixture
    def screen(self, xvfb, tmp_path):
        return Screen(tmp_path / "Xvfb_screen0")

    def test_core_cursor(self, xvfb, screen):
        x = Color(xvfb)
        w = _window(x, 0)
        show_cursor(x, w, core_cursor(x, (0xFFFF, 0, 0)))
        want = as8(shown(rgb8(RED), XY[SRGB], srgb_decode, XY[P3], srgb_encode))
        got = screen_pixel(screen, 158, 158, want)
        assert near8(got, want, tol=1), (hex(got), hex(want))

    def test_argb_cursor(self, xvfb, screen):
        x = Color(xvfb)
        w = _window(x, 0)
        show_cursor(x, w, argb_cursor(x, 0xFF000000 | GREEN))
        want = as8(shown(rgb8(GREEN), XY[SRGB], srgb_decode, XY[P3], srgb_encode))
        got = screen_pixel(screen, 158, 158, want)
        assert near8(got, want, tol=1), (hex(got), hex(want))


class TestSRGB:
    """Without an output colour space, the cursor is drawn as it is."""

    @pytest.fixture
    def xserver_args(self, tmp_path):
        return ["-br", "-compositor", "on", "-fbdir", str(tmp_path)]

    def test_core_cursor(self, xvfb, tmp_path):
        x = Color(xvfb)
        w = _window(x, 0)
        show_cursor(x, w, core_cursor(x, (0xFFFF, 0, 0)))
        screen = Screen(tmp_path / "Xvfb_screen0")
        assert screen_pixel(screen, 158, 158, RED) == RED

    def test_cursor_is_not_drawn_into_windows(self, xvfb, tmp_path):
        """The compositor copies drawing on the root window over windows
        into their contents (IncludeInferiors); the cursor is not such
        drawing: the window's own pixels stay as the client drew them."""
        x = Color(xvfb)
        w = _window(x, 0)
        show_cursor(x, w, core_cursor(x, (0xFFFF, 0, 0)))
        screen = Screen(tmp_path / "Xvfb_screen0")
        assert screen_pixel(screen, 158, 158, RED) == RED
        data = _get_image(x, w, 58, 58)
        assert isinstance(data, bytes), data
        assert struct.unpack_from("<I", data, 32)[0] & 0xFFFFFF == 0
