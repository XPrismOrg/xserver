# SPDX-License-Identifier: MIT
#
# XPrism scene compositor (-compositor on, Xext/composite/compscene.c).
#
# Scenes are built with plain core requests and checked by reading the
# root window back with GetImage, which returns what the server put on
# the screen. Tests marked "classic" run with the compositor both off
# and on: the compositor must not change what opaque windows look like.
# The others need the compositor: ARGB blending, the opacity hint, and
# stepping aside for a legacy compositor.

import struct
import time

import pytest
from xclient import RawX11Connection, X11Error

X_CREATE_WINDOW = 1
X_MAP_WINDOW = 8
X_UNMAP_WINDOW = 10
X_CONFIGURE_WINDOW = 12
X_CHANGE_PROPERTY = 18
X_CREATE_GC = 55
X_POLY_FILL_RECTANGLE = 70
X_GET_IMAGE = 73
X_GET_INPUT_FOCUS = 43
X_CREATE_COLORMAP = 78

CW_BACK_PIXEL = 0x0002
CW_BORDER_PIXEL = 0x0008
CW_COLORMAP = 0x2000
CONFIG_X = 0x1
CONFIG_STACK_MODE = 0x40
ABOVE = 0
XA_CARDINAL = 6
Z_PIXMAP = 2

COMPOSITE_REDIRECT_SUBWINDOWS = 2
COMPOSITE_REDIRECT_MANUAL = 1

ROOT_BG = 0x000000  # -br: black root background
RED = 0xFF0000
GREEN = 0x00FF00
BLUE = 0x0000FF


def _pad(b):
    return b + b"\0" * (-len(b) % 4)


class Client:
    def __init__(self, xserver):
        self.c = RawX11Connection(xserver.display_num)
        self.root = self.c.root_window

    def req(self, opcode, data1, body):
        body = _pad(body)
        return self.c.send_request(struct.pack("<BBH", opcode, data1, 1 + len(body) // 4) + body)

    def sync(self):
        """Round trip; returns the error codes received meanwhile."""
        seq = self.req(X_GET_INPUT_FOCUS, 0, b"")
        errs = []
        while True:
            r = self.c.recv_response(timeout=5.0)
            assert r is not None, "no reply"
            if isinstance(r, X11Error):
                errs.append(r.error_code)
            elif r.response_type == 1 and r.sequence == seq:
                return errs

    def window(self, x, y, w, h, pixel, depth=0, visual=0, colormap=0):
        wid = self.c.alloc_id()
        mask = CW_BACK_PIXEL | CW_BORDER_PIXEL
        values = [pixel, 0]
        if colormap:
            mask |= CW_COLORMAP
            values.append(colormap)
        body = struct.pack("<IIhhHHHHII", wid, self.root, x, y, w, h, 0, 1, visual, mask)
        body += struct.pack(f"<{len(values)}I", *values)
        self.req(X_CREATE_WINDOW, depth, body)
        return wid

    def map(self, wid):
        self.req(X_MAP_WINDOW, 0, struct.pack("<I", wid))

    def unmap(self, wid):
        self.req(X_UNMAP_WINDOW, 0, struct.pack("<I", wid))

    def move(self, wid, x):
        self.req(X_CONFIGURE_WINDOW, 0, struct.pack("<IHxxi", wid, CONFIG_X, x))

    def raise_(self, wid):
        self.req(X_CONFIGURE_WINDOW, 0, struct.pack("<IHxxI", wid, CONFIG_STACK_MODE, ABOVE))

    def opacity(self, wid, value):
        atom = self.c.intern_atom("_NET_WM_WINDOW_OPACITY")
        body = struct.pack("<IIIBxxxI", wid, atom, XA_CARDINAL, 32, 1) + struct.pack("<I", value)
        self.req(X_CHANGE_PROPERTY, 0, body)

    def fill(self, wid, pixel, x, y, w, h):
        gc = self.c.alloc_id()
        self.req(X_CREATE_GC, 0, struct.pack("<IIII", gc, wid, 0x4, pixel))  # GCForeground
        self.req(X_POLY_FILL_RECTANGLE, 0, struct.pack("<IIhhHH", wid, gc, x, y, w, h))

    def pixel(self, x, y):
        """RGB of one pixel of the root window, as the screen shows it."""
        self.sync()
        seq = self.req(X_GET_IMAGE, Z_PIXMAP, struct.pack("<IhhHHI", self.root, x, y, 1, 1, 0xFFFFFFFF))
        while True:
            r = self.c.recv_response(timeout=5.0)
            assert r is not None
            assert not isinstance(r, X11Error), r
            if r.response_type == 1 and r.sequence == seq:
                return struct.unpack_from("<I", r.data, 32)[0] & 0xFFFFFF

    def argb_visual(self):
        """(visual, colormap) of a depth-32 TrueColor visual."""
        setup_visual = self._find_depth32_visual()
        if not setup_visual:
            pytest.skip("no depth-32 visual")
        cmap = self.c.alloc_id()
        self.req(X_CREATE_COLORMAP, 0, struct.pack("<III", cmap, self.root, setup_visual))
        return setup_visual, cmap

    def _find_depth32_visual(self):
        # QueryTree is not enough; ask the server for the visuals through a
        # fresh connection setup parse (xclient keeps only the root visual)
        return _depth32_visual(self.c)


def _depth32_visual(conn):
    import socket

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(f"/tmp/.X11-unix/X{conn.display_num}")
    s.sendall(struct.pack("<BxHHHHxx", 0x6C, 11, 0, 0, 0))
    hdr = b""
    while len(hdr) < 8:
        hdr += s.recv(8 - len(hdr))
    n = struct.unpack_from("<H", hdr, 6)[0] * 4
    data = b""
    while len(data) < n:
        data += s.recv(n - len(data))
    s.close()
    vendor_len, _, nscreens, nformats = struct.unpack_from("<HHBB", data, 16)
    off = 32 + ((vendor_len + 3) & ~3) + 8 * nformats
    ndepths = data[off + 39]
    off += 40
    for _ in range(ndepths):
        depth, nvis = data[off], struct.unpack_from("<H", data, off + 2)[0]
        off += 8
        for v in range(nvis):
            vid, cls = struct.unpack_from("<IB", data, off + 24 * v)
            if depth == 32 and cls == 4:  # TrueColor
                return vid
        off += 24 * nvis
    return 0


@pytest.fixture(params=["off", "on"])
def compositor(request):
    return request.param


@pytest.fixture
def xserver_args(compositor):
    return ["-br", "-compositor", compositor]


def _scene(xvfb):
    """Bottom window red at (100,100), top window green at (150,150)."""
    x = Client(xvfb)
    bottom = x.window(100, 100, 100, 100, RED)
    top = x.window(150, 150, 100, 100, GREEN)
    x.map(bottom)
    x.map(top)
    return x, bottom, top


GC_FOREGROUND = 1 << 2
GC_SUBWINDOW_MODE = 1 << 15
INCLUDE_INFERIORS = 1


def _get_image(x, drawable, gx, gy, w=1, h=1):
    """GetImage reply data, or the error code."""
    seq = x.req(X_GET_IMAGE, Z_PIXMAP, struct.pack("<IhhHHI", drawable, gx, gy, w, h, 0xFFFFFFFF))
    while True:
        r = x.c.recv_response(timeout=5.0)
        assert r is not None
        if isinstance(r, X11Error):
            return r.error_code
        if r.response_type == 1 and r.sequence == seq:
            return r.data


class TestProtocolSemantics:
    """X11 rules that the compositor's redirection must not change. These
    are what the X Test Suite caught with the compositor on."""

    def test_root_drawing_leaves_windows_alone(self, xvfb):
        x = Client(xvfb)
        w = x.window(100, 100, 100, 100, RED)
        x.map(w)
        x.pixel(0, 0)
        x.fill(x.root, GREEN, 50, 50, 300, 300)       # ClipByChildren
        assert x.pixel(150, 150) == RED
        assert x.pixel(60, 60) == GREEN

    def test_root_drawing_with_include_inferiors_reaches_windows(self, xvfb):
        x = Client(xvfb)
        w = x.window(100, 100, 100, 100, RED)
        x.map(w)
        x.pixel(0, 0)
        gc = x.c.alloc_id()
        x.req(X_CREATE_GC, 0, struct.pack("<IIIII", gc, x.root,
                                          GC_FOREGROUND | GC_SUBWINDOW_MODE,
                                          GREEN, INCLUDE_INFERIORS))
        x.req(X_POLY_FILL_RECTANGLE, 0, struct.pack("<IIhhHH", x.root, gc, 140, 140, 20, 20))
        assert x.pixel(150, 150) == GREEN
        assert x.pixel(130, 130) == RED
        data = _get_image(x, w, 50, 50)                 # the window's own contents
        assert isinstance(data, bytes), data
        assert struct.unpack_from("<I", data, 32)[0] & 0xFFFFFF == GREEN

    def test_include_inferiors_line_right_after_map(self, xvfb):
        """Only the line is drawn; the rest of its bounding box must keep
        the window's contents, even though the window was just mapped."""
        x = Client(xvfb)
        w = x.window(100, 100, 100, 100, RED)
        x.map(w)                                        # no frame yet
        gc = x.c.alloc_id()
        x.req(X_CREATE_GC, 0, struct.pack("<IIIII", gc, x.root,
                                          GC_FOREGROUND | GC_SUBWINDOW_MODE,
                                          GREEN, INCLUDE_INFERIORS))
        x.req(65, 0, struct.pack("<IIhhhh", x.root, gc, 110, 110, 190, 190))  # PolyLine
        assert x.pixel(150, 150) == GREEN
        assert x.pixel(180, 120) == RED
        data = _get_image(x, w, 80, 20)
        assert struct.unpack_from("<I", data, 32)[0] & 0xFFFFFF == RED

    def test_include_inferiors_drawing_right_after_move(self, xvfb):
        """Move a window and draw over it on the root in one batch: the
        compositor composes the move just before the drawing, and the
        drawing must still reach the window (the X Test Suite's
        XDrawImageString-9 failed this way under load)."""
        x = Client(xvfb)
        w = x.window(100, 100, 100, 100, RED)
        x.map(w)
        gc = x.c.alloc_id()
        x.req(X_CREATE_GC, 0, struct.pack("<IIIII", gc, x.root,
                                          GC_FOREGROUND | GC_SUBWINDOW_MODE,
                                          GREEN, INCLUDE_INFERIORS))
        assert x.sync() == []
        move = struct.pack("<IHxxi", w, CONFIG_X, 300)
        fill = struct.pack("<IIhhHH", x.root, gc, 310, 110, 20, 20)
        batch = (struct.pack("<BBH", X_CONFIGURE_WINDOW, 0, 1 + len(move) // 4) + move +
                 struct.pack("<BBH", X_POLY_FILL_RECTANGLE, 0, 1 + len(fill) // 4) + fill)
        x.c.seq += 1                    # two requests in one write
        x.c.send_request(batch)
        data = _get_image(x, w, 20, 20)
        assert isinstance(data, bytes), data
        assert struct.unpack_from("<I", data, 32)[0] & 0xFFFFFF == GREEN
        assert x.pixel(320, 120) == GREEN
        assert x.pixel(340, 140) == RED

    def test_get_image_of_window_partly_off_screen(self, xvfb):
        x = Client(xvfb)
        w = x.window(SCREEN_W - 50, 100, 100, 100, RED)
        x.map(w)
        assert x.sync() == []
        assert _get_image(x, w, 0, 0, 100, 10) == BAD_MATCH      # beyond the screen
        assert isinstance(_get_image(x, w, 0, 0, 50, 10), bytes)  # on screen


class TestClassicLook:
    """Opaque scenes look the same with the compositor off and on."""

    def test_stacking(self, xvfb):
        x, bottom, top = _scene(xvfb)
        assert x.pixel(10, 10) == ROOT_BG
        assert x.pixel(110, 110) == RED
        assert x.pixel(175, 175) == GREEN  # overlap: top wins
        assert x.pixel(240, 240) == GREEN

    def test_raise(self, xvfb):
        x, bottom, top = _scene(xvfb)
        x.raise_(bottom)
        assert x.pixel(175, 175) == RED

    def test_move_uncovers(self, xvfb):
        x, bottom, top = _scene(xvfb)
        x.move(top, 600)
        assert x.pixel(175, 175) == RED
        assert x.pixel(240, 240) == ROOT_BG
        assert x.pixel(610, 160) == GREEN

    def test_unmap_uncovers(self, xvfb):
        x, bottom, top = _scene(xvfb)
        x.unmap(top)
        assert x.pixel(175, 175) == RED
        assert x.pixel(240, 240) == ROOT_BG

    def test_drawing_shows(self, xvfb):
        x, bottom, top = _scene(xvfb)
        x.pixel(0, 0)  # let a frame happen first
        x.fill(top, BLUE, 40, 40, 20, 20)
        assert x.pixel(200, 200) == BLUE
        assert x.pixel(160, 160) == GREEN

    def test_disconnect_removes_windows(self, xvfb):
        x, bottom, top = _scene(xvfb)
        x.pixel(0, 0)
        x.c.close()
        y = Client(xvfb)
        assert y.pixel(175, 175) == ROOT_BG


def _near(got, want, tol=2):
    return all(abs(((got >> s) & 0xFF) - ((want >> s) & 0xFF)) <= tol for s in (16, 8, 0))


def _three(xvfb):
    """a red (bottom), b green, c blue (top), overlapping diagonally."""
    x = Client(xvfb)
    a = x.window(100, 100, 100, 100, RED)
    b = x.window(150, 150, 100, 100, GREEN)
    c = x.window(200, 200, 100, 100, BLUE)
    for w in (a, b, c):
        x.map(w)
    return x, a, b, c


class TestRestack:
    def test_raise_bottom_to_top(self, xvfb):
        x, a, b, c = _three(xvfb)
        assert x.pixel(175, 175) == GREEN
        x.raise_(a)
        assert x.pixel(175, 175) == RED      # a over b
        assert x.pixel(225, 225) == BLUE     # b/c order unchanged
        assert x.pixel(195, 195) == RED      # a over b, under nothing

    def test_lower_top_to_bottom(self, xvfb):
        x, a, b, c = _three(xvfb)
        x.req(X_CONFIGURE_WINDOW, 0, struct.pack("<IHxxI", c, CONFIG_STACK_MODE, 1))  # Below
        assert x.pixel(225, 225) == GREEN    # b over c now
        assert x.pixel(175, 175) == GREEN    # b over a, unchanged
        assert x.pixel(280, 280) == BLUE

    def test_map_below_keeps_order(self, xvfb):
        x, a, b, c = _three(xvfb)
        d = x.window(0, 0, 50, 50, GREEN)
        x.map(d)
        x.raise_(a)
        assert x.pixel(175, 175) == RED
        assert x.pixel(25, 25) == GREEN


class TestDamageIsNarrow:
    """Every frame paints onto the root window, so the DAMAGE extension on
    the root reports what the compositor repainted."""

    @pytest.fixture
    def compositor(self):
        return "on"

    def _painted(self, x, action):
        ext = x.c.query_extension("DAMAGE")
        assert ext
        x.req(ext.opcode, 0, struct.pack("<II", 1, 1))       # QueryVersion
        damage = x.c.alloc_id()
        x.req(ext.opcode, 1, struct.pack("<IIBxxx", damage, x.root, 0))  # RawRectangles
        x.pixel(0, 0)                                         # frame done
        action()
        # reading the root composes the frame; keep the events meanwhile
        seq = x.req(X_GET_IMAGE, Z_PIXMAP,
                    struct.pack("<IhhHHI", x.root, 0, 0, 1, 1, 0xFFFFFFFF))
        boxes, done = [], False
        while True:
            r = x.c.recv_response(timeout=5.0 if not done else 0.3)
            if r is None:
                break
            if isinstance(r, X11Error):
                continue
            if r.response_type == 1 and r.sequence == seq:
                done = True
            elif r.response_type & 0x7F == ext.first_event:
                bx, by, bw, bh = struct.unpack_from("<hhHH", r.data, 16)
                boxes.append((bx, by, bx + bw, by + bh))
        x.req(ext.opcode, 2, struct.pack("<I", damage))       # Destroy
        return boxes

    def test_restack_repaints_only_the_overlap(self, xvfb):
        x, a, b, c = _three(xvfb)
        # raising a over b and c: only a∩b changes (a∩c is empty)
        boxes = self._painted(x, lambda: x.raise_(a))
        assert x.pixel(175, 175) == RED
        assert boxes, "nothing repainted"
        for x1, y1, x2, y2 in boxes:
            assert 150 <= x1 and x2 <= 200 and 150 <= y1 and y2 <= 200, boxes


class TestComposition:
    """What only a compositor can do."""

    @pytest.fixture
    def compositor(self):
        return "on"

    def test_argb_window_blends(self, xvfb):
        x = Client(xvfb)
        under = x.window(100, 100, 100, 100, BLUE)
        visual, cmap = x.argb_visual()
        # premultiplied 50% red
        over = x.window(100, 100, 100, 100, 0x80800000, depth=32, visual=visual, colormap=cmap)
        x.map(under)
        x.map(over)
        assert x.sync() == []
        got = x.pixel(150, 150)
        assert _near(got, 0x80007F), hex(got)

    def test_opacity_hint(self, xvfb):
        x = Client(xvfb)
        under = x.window(100, 100, 100, 100, BLUE)
        over = x.window(100, 100, 100, 100, RED)
        x.map(under)
        x.map(over)
        assert x.pixel(150, 150) == RED
        x.opacity(over, 0x80000000)
        got = x.pixel(150, 150)
        assert _near(got, 0x80007F), hex(got)
        x.opacity(over, 0xFFFFFFFF)
        assert x.pixel(150, 150) == RED


def _redirect_root(x):
    ext = x.c.query_extension("Composite")
    assert ext
    body = struct.pack("<IBxxx", x.root, COMPOSITE_REDIRECT_MANUAL)
    x.c.send_request(struct.pack("<BBH", ext.opcode, COMPOSITE_REDIRECT_SUBWINDOWS, 1 + len(body) // 4) + body)
    return x.sync()


def test_legacy_compositor_can_take_over(xvfb):
    """picom & co. can redirect the root, compositor off or on."""
    legacy = Client(xvfb)
    assert _redirect_root(legacy) == []
    x = Client(xvfb)
    w = x.window(100, 100, 100, 100, RED)
    x.map(w)
    assert x.sync() == []


class TestLegacyCompositor:
    @pytest.fixture
    def compositor(self):
        return "on"

    def test_compositor_resumes_after_legacy_exits(self, xvfb):
        legacy = Client(xvfb)
        assert _redirect_root(legacy) == []
        legacy.c.close()

        x = Client(xvfb)
        under = x.window(100, 100, 100, 100, BLUE)
        over = x.window(100, 100, 100, 100, RED)
        x.map(under)
        x.map(over)
        x.opacity(over, 0x80000000)
        got = x.pixel(150, 150)
        assert _near(got, 0x80007F), hex(got)

    def test_resumed_compositor_keeps_root_drawing_off_windows(self, xvfb):
        """Windows that already exist when the compositor takes the screen
        back must clip the root as before."""
        legacy = Client(xvfb)
        assert _redirect_root(legacy) == []
        x = Client(xvfb)
        w = x.window(100, 100, 100, 100, RED)
        x.map(w)
        assert x.sync() == []
        legacy.c.close()
        # The server notices the exit, and the compositor takes the screen
        # back on its next frame. Until then the window draws straight to
        # the screen and already shows red; root drawing done in between is
        # repainted over (doc/compositor.md), so wait for the takeover.
        for _ in range(100):
            if x.pixel(150, 150) == RED and _is_composited(x, w):
                break
            time.sleep(0.05)
        assert _is_composited(x, w)
        assert x.pixel(150, 150) == RED
        x.fill(x.root, GREEN, 50, 50, 300, 300)
        assert x.pixel(150, 150) == RED
        assert x.pixel(60, 60) == GREEN


COMPOSITE_NAME_WINDOW_PIXMAP = 6
BAD_MATCH = 8
SCREEN_W, SCREEN_H = 1280, 1024


def _is_composited(x, wid):
    """Composite names a pixmap only for redirected windows: BadMatch means
    the window renders straight to the screen."""
    ext = x.c.query_extension("Composite")
    assert ext
    body = struct.pack("<II", wid, x.c.alloc_id())
    x.c.send_request(struct.pack("<BBH", ext.opcode, COMPOSITE_NAME_WINDOW_PIXMAP, 3) + body)
    errs = x.sync()
    assert errs in ([], [BAD_MATCH]), errs
    return errs == []


def _becomes_direct(x, wid, timeout=3.0):
    """Direct scanout starts in the block handler, which runs when the
    server is idle: give it the chance."""
    end = time.monotonic() + timeout
    while _is_composited(x, wid):
        if time.monotonic() > end:
            return False
        time.sleep(0.01)
    return True


class TestDirectScanout:
    @pytest.fixture
    def compositor(self):
        return "on"

    def _fullscreen(self, xvfb):
        x = Client(xvfb)
        fs = x.window(0, 0, SCREEN_W, SCREEN_H, GREEN)
        x.map(fs)
        x.pixel(0, 0)
        return x, fs

    def test_fullscreen_window_is_shown_directly(self, xvfb):
        x, fs = self._fullscreen(xvfb)
        assert _becomes_direct(x, fs)
        assert x.pixel(600, 500) == GREEN
        x.fill(fs, BLUE, 590, 490, 20, 20)
        assert x.pixel(600, 500) == BLUE

    def test_window_above_brings_composition_back(self, xvfb):
        x, fs = self._fullscreen(xvfb)
        popup = x.window(100, 100, 100, 100, RED)
        x.map(popup)
        assert x.pixel(150, 150) == RED
        assert x.pixel(600, 500) == GREEN     # contents kept across the switch
        assert _is_composited(x, fs)
        x.unmap(popup)
        assert x.pixel(150, 150) == GREEN
        assert _becomes_direct(x, fs)

    def test_translucent_fullscreen_is_composited(self, xvfb):
        x, fs = self._fullscreen(xvfb)
        x.opacity(fs, 0x80000000)
        x.pixel(0, 0)
        assert _is_composited(x, fs)
        x.opacity(fs, 0xFFFFFFFF)
        x.pixel(0, 0)
        assert _becomes_direct(x, fs)

    def test_smaller_window_is_composited(self, xvfb):
        x = Client(xvfb)
        w = x.window(0, 0, SCREEN_W - 1, SCREEN_H, GREEN)
        x.map(w)
        x.pixel(0, 0)
        assert _is_composited(x, w)

    def test_resize_out_of_fullscreen(self, xvfb):
        x, fs = self._fullscreen(xvfb)
        x.move(fs, 200)
        assert x.pixel(100, 100) == ROOT_BG
        assert x.pixel(300, 100) == GREEN
        assert _is_composited(x, fs)

    def test_window_below_never_shows_through(self, xvfb):
        x = Client(xvfb)
        below = x.window(100, 100, 100, 100, RED)
        x.map(below)
        fs = x.window(0, 0, SCREEN_W, SCREEN_H, GREEN)
        x.map(fs)
        x.pixel(0, 0)
        assert _becomes_direct(x, fs)
        x.fill(below, BLUE, 0, 0, 100, 100)   # damage under the direct window
        assert x.pixel(150, 150) == GREEN


class TestXinerama:
    """Two screens side by side; the compositor runs on each, and Xinerama
    shows each window on both. Windows straddle the boundary at x=1280."""

    @pytest.fixture
    def xserver_args(self, compositor):
        self.compositor = compositor
        return ["-br", "+xinerama", "-screen", "1", "1280x1024x24",
                "-compositor", compositor]

    def test_window_across_screens(self, xvfb):
        x = Client(xvfb)
        under = x.window(1180, 100, 200, 200, BLUE)
        over = x.window(1230, 150, 100, 100, RED)
        x.map(under)
        x.map(over)
        for px in (1240, 1279, 1280, 1320):
            assert x.pixel(px, 200) == RED, px
        assert x.pixel(1200, 120) == BLUE
        assert x.pixel(1360, 120) == BLUE
        x.opacity(over, 0x80000000)
        for px in (1279, 1280):
            got = x.pixel(px, 200)
            if self.compositor == "on":
                assert _near(got, 0x80007F), (px, hex(got))
            else:
                assert got == RED, (px, hex(got))     # no compositor, no opacity

    def test_reading_the_root_sees_the_current_scene(self, xvfb):
        """A change and a read of the root in one batch: the read must
        compose first on every screen, as it does without Xinerama."""
        if self.compositor != "on":
            pytest.skip("only the compositor composes")
        x = Client(xvfb)
        w = x.window(1230, 100, 100, 100, RED)
        x.map(w)
        assert x.pixel(1250, 150) == RED
        atom = x.c.intern_atom("_NET_WM_WINDOW_OPACITY")
        prop = struct.pack("<IIIBxxxI", w, atom, XA_CARDINAL, 32, 1) + struct.pack("<I", 0x80000000)
        for px in (1250, 1310):
            get = struct.pack("<IhhHHI", x.root, px, 150, 1, 1, 0xFFFFFFFF)
            batch = (struct.pack("<BBH", X_CHANGE_PROPERTY, 0, 1 + len(prop) // 4) + prop +
                     struct.pack("<BBH", X_GET_IMAGE, Z_PIXMAP, 1 + len(get) // 4) + get)
            x.c.seq += 1
            seq = x.c.send_request(batch)
            while True:
                r = x.c.recv_response(timeout=5.0)
                assert r is not None and not isinstance(r, X11Error), r
                if r.response_type == 1 and r.sequence == seq:
                    break
            got = struct.unpack_from("<I", r.data, 32)[0] & 0xFFFFFF
            assert _near(got, 0x7F0000), (px, hex(got))

    def test_move_between_screens(self, xvfb):
        x = Client(xvfb)
        w = x.window(100, 100, 100, 100, RED)
        x.map(w)
        assert x.pixel(150, 150) == RED
        x.move(w, 2000)
        assert x.pixel(150, 150) != RED
        assert x.pixel(2050, 150) == RED
        x.fill(w, GREEN, 10, 10, 20, 20)
        assert x.pixel(2020, 120) == GREEN

    def test_root_drawing_across_screens(self, xvfb):
        x = Client(xvfb)
        w = x.window(1230, 100, 100, 100, RED)
        x.map(w)
        assert x.sync() == []
        x.fill(x.root, GREEN, 1200, 50, 160, 300)        # ClipByChildren
        assert x.pixel(1210, 60) == GREEN
        assert x.pixel(1350, 60) == GREEN
        assert x.pixel(1279, 150) == RED
        assert x.pixel(1280, 150) == RED
