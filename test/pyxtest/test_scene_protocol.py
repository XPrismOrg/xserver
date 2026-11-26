# SPDX-License-Identifier: MIT
#
# XPRISM-SCENE protocol v0.3 (Xext/composite/compscene.c,
# doc/xprism-scene-protocol.md): the window manager describes how the
# scene compositor shows each top-level window.

import struct
import time

import pytest
from test_compositor import (BLUE, GREEN, RED, ROOT_BG, SCREEN_H, SCREEN_W,
                             Client, _becomes_direct, _is_composited, _near)
from xclient import RawX11Connection, X11Error

X_CHANGE_WINDOW_ATTRIBUTES = 2
CW_EVENT_MASK = 1 << 11
SUBSTRUCTURE_REDIRECT = 1 << 20

SET, GET, CLEAR, SET_DECO, GET_DECO = 1, 2, 3, 4, 5
OPACITY, TRANSFORM, RADIUS, SHADOW, BLUR = 1, 2, 4, 8, 16
WHITE = 0xFFFFFF
ONE = 1 << 16
IDENTITY = [ONE, 0, 0, 0, ONE, 0, 0, 0, ONE]

BAD_REQUEST, BAD_VALUE, BAD_WINDOW, BAD_MATCH, BAD_ACCESS = 1, 2, 3, 8, 10


@pytest.fixture
def xserver_args():
    return ["-br", "-compositor", "on"]


class Scene:
    """A client speaking XPRISM-SCENE (optionally byte-swapped)."""

    def __init__(self, xvfb, swapped=False):
        self.c = RawX11Connection(xvfb.display_num, swapped=swapped)
        self.bo = ">" if swapped else "<"
        ext = self.c.query_extension("XPRISM-SCENE")
        assert ext, "XPRISM-SCENE missing"
        self.op = ext.opcode

    def _req(self, minor, fmt, *args):
        body = struct.pack(self.bo + fmt, *args)
        hdr = struct.pack(self.bo + "BBH", self.op, minor, 1 + len(body) // 4)
        return self.c.send_request(hdr + body)

    def _result(self, seq):
        while True:
            r = self.c.recv_response(timeout=5.0)
            assert r is not None, "no answer"
            if isinstance(r, X11Error) or (r.response_type == 1 and r.sequence == seq):
                return r

    def _errors(self, seq):
        sync = self.c.send_request(struct.pack(self.bo + "BxH", 43, 1))  # GetInputFocus
        errs = []
        while True:
            r = self.c.recv_response(timeout=5.0)
            assert r is not None, "no answer"
            if isinstance(r, X11Error):
                errs.append(r.error_code)
            elif r.response_type == 1 and r.sequence == sync:
                return errs

    def become_wm(self, *clients):
        """Take SubstructureRedirect on the root. Requests from different
        connections are not ordered, so first wait until the given clients'
        earlier requests (MapWindow, ConfigureWindow) are done: otherwise
        they could be redirected to this window manager instead."""
        for other in clients:
            assert other.sync() == []
        body = struct.pack(self.bo + "III", self.c.root_window, CW_EVENT_MASK, SUBSTRUCTURE_REDIRECT)
        hdr = struct.pack(self.bo + "BxH", X_CHANGE_WINDOW_ATTRIBUTES, 1 + len(body) // 4)
        return self._errors(self.c.send_request(hdr + body))

    def version(self):
        r = self._result(self._req(0, "II", 0, 1))
        assert not isinstance(r, X11Error), r
        return struct.unpack_from(self.bo + "II", r.data, 8)

    def set(self, wid, mask, opacity=0xFFFFFFFF, transform=IDENTITY):
        return self._errors(self._req(SET, "III9i", wid, mask, opacity, *transform))

    def get(self, wid):
        r = self._result(self._req(GET, "I", wid))
        if isinstance(r, X11Error):
            return r
        mask, opacity = struct.unpack_from(self.bo + "II", r.data, 8)
        return mask, opacity, list(struct.unpack_from(self.bo + "9i", r.data, 16))

    def clear(self, wid, mask):
        return self._errors(self._req(CLEAR, "II", wid, mask))

    def move(self, wid, x):
        """ConfigureWindow x; from the WM, so not redirected back to it."""
        body = struct.pack(self.bo + "IHxxi", wid, 1, x)
        hdr = struct.pack(self.bo + "BxH", 12, 1 + len(body) // 4)
        return self._errors(self.c.send_request(hdr + body))

    def map(self, wid):
        """MapWindow from the WM, so not redirected back to it."""
        return self._errors(self.c.send_request(
            struct.pack(self.bo + "BxHI", 8, 2, wid)))

    def decorate(self, wid, mask, radius=0, blur=0, dx=0, dy=0, color=0,
                 bg_blur=0, v02=False):
        if v02:   # the 0.2 request, without backgroundBlur
            return self._errors(self._req(SET_DECO, "IIHHhhI", wid, mask, radius,
                                          blur, dx, dy, color))
        return self._errors(self._req(SET_DECO, "IIHHhhIHxx", wid, mask, radius,
                                      blur, dx, dy, color, bg_blur))

    def decoration(self, wid):
        r = self._result(self._req(GET_DECO, "I", wid))
        if isinstance(r, X11Error):
            return r
        return struct.unpack_from(self.bo + "IHHhhIH", r.data, 8)


def _red_over_blue(xvfb):
    x = Client(xvfb)
    under = x.window(100, 100, 100, 100, BLUE)
    over = x.window(100, 100, 100, 100, RED)
    x.map(under)
    x.map(over)
    return x, over


class TestAbsent:
    @pytest.fixture
    def xserver_args(self):
        return ["-compositor", "off"]

    def test_extension_absent_when_compositor_off(self, xvfb):
        c = RawX11Connection(xvfb.display_num)
        assert c.query_extension("XPRISM-SCENE") is None


def test_query_version(xvfb):
    assert Scene(xvfb).version() == (0, 3)


def test_only_the_window_manager_may_describe_the_scene(xvfb):
    x, w = _red_over_blue(xvfb)
    outsider = Scene(xvfb)
    assert outsider.set(w, OPACITY, 0x80000000) == [BAD_ACCESS]
    assert isinstance(outsider.get(w), X11Error)
    wm = Scene(xvfb)
    assert wm.become_wm(x) == []
    assert wm.set(w, OPACITY, 0x80000000) == []
    assert outsider.clear(w, OPACITY) == [BAD_ACCESS]


def test_opacity(xvfb):
    x, w = _red_over_blue(xvfb)
    wm = Scene(xvfb)
    wm.become_wm(x)
    assert x.pixel(150, 150) == RED
    assert wm.set(w, OPACITY, 0x80000000) == []
    assert _near(x.pixel(150, 150), 0x80007F)
    assert wm.get(w)[:2] == (OPACITY, 0x80000000)
    assert wm.clear(w, OPACITY) == []
    assert x.pixel(150, 150) == RED
    assert wm.get(w)[0] == 0


def test_scene_opacity_overrides_hint(xvfb):
    x, w = _red_over_blue(xvfb)
    x.opacity(w, 0x80000000)
    wm = Scene(xvfb)
    wm.become_wm(x)
    assert wm.set(w, OPACITY, 0xFFFFFFFF) == []
    assert x.pixel(150, 150) == RED


def test_transform_scale(xvfb):
    x = Client(xvfb)
    w = x.window(100, 100, 100, 100, RED)
    x.map(w)
    wm = Scene(xvfb)
    wm.become_wm(x)
    half = [ONE // 2, 0, 0, 0, ONE // 2, 0, 0, 0, ONE]
    assert wm.set(w, TRANSFORM, transform=half) == []
    assert x.pixel(120, 120) == RED          # inside the scaled window
    assert x.pixel(180, 180) == ROOT_BG      # where it was, now uncovered
    assert wm.get(w)[2] == half
    assert wm.clear(w, TRANSFORM) == []
    assert x.pixel(180, 180) == RED


def test_transform_translate(xvfb):
    x = Client(xvfb)
    w = x.window(100, 100, 100, 100, RED)
    x.map(w)
    wm = Scene(xvfb)
    wm.become_wm(x)
    right = [ONE, 0, 300 * ONE, 0, ONE, 0, 0, 0, ONE]
    assert wm.set(w, TRANSFORM, transform=right) == []
    assert x.pixel(150, 150) == ROOT_BG
    assert x.pixel(450, 150) == RED


def test_drawing_into_transformed_window_shows(xvfb):
    x = Client(xvfb)
    w = x.window(100, 100, 100, 100, RED)
    x.map(w)
    wm = Scene(xvfb)
    wm.become_wm(x)
    right = [ONE, 0, 300 * ONE, 0, ONE, 0, 0, 0, ONE]
    wm.set(w, TRANSFORM, transform=right)
    x.pixel(0, 0)
    x.fill(w, BLUE, 0, 0, 100, 100)
    assert x.pixel(450, 150) == BLUE


def test_bad_requests(xvfb):
    x = Client(xvfb)
    w = x.window(100, 100, 100, 100, RED)
    child = x.c.create_window(parent=w)
    wm = Scene(xvfb)
    wm.become_wm(x)
    assert wm.set(w, 1 << 5) == [BAD_VALUE]
    assert wm.set(w, RADIUS) == [BAD_VALUE]        # a decoration, not an attribute
    assert wm.decorate(w, OPACITY) == [BAD_VALUE]
    assert wm.decorate(w, RADIUS, radius=1025) == [BAD_VALUE]
    assert wm.decorate(w, SHADOW, blur=129) == [BAD_VALUE]
    assert wm.decorate(child, RADIUS, radius=4) == [BAD_MATCH]
    assert wm.set(0x7FFFFFF0, OPACITY) == [BAD_WINDOW]
    assert wm.set(child, OPACITY) == [BAD_MATCH]                   # not top-level
    assert wm.set(w, TRANSFORM, transform=[0] * 9) == [BAD_MATCH]  # singular
    behind = [ONE, 0, 0, 0, ONE, 0, 0, 0, -ONE]
    assert wm.set(w, TRANSFORM, transform=behind) == [BAD_MATCH]
    assert wm.clear(w, 1 << 5) == [BAD_VALUE]
    assert wm._errors(wm._req(9, "")) == [BAD_REQUEST]


def test_swapped_client(xvfb):
    x, w = _red_over_blue(xvfb)
    wm = Scene(xvfb, swapped=True)
    assert wm.version() == (0, 3)
    assert wm.become_wm(x) == []
    assert wm.set(w, OPACITY, 0x80000000) == []
    assert _near(x.pixel(150, 150), 0x80007F)
    mask, opacity, xf = wm.get(w)
    assert (mask, opacity, xf) == (OPACITY, 0x80000000, IDENTITY)


def test_window_manager_exit_clears_its_scene(xvfb):
    x, w = _red_over_blue(xvfb)
    wm = Scene(xvfb)
    wm.become_wm(x)
    wm.set(w, OPACITY | TRANSFORM, 0x80000000,
           [ONE, 0, 300 * ONE, 0, ONE, 0, 0, 0, ONE])
    assert x.pixel(150, 150) == BLUE
    wm.c.close()
    # the server notices the disconnect on its own schedule
    for _ in range(50):
        if x.pixel(150, 150) == RED:
            break
        time.sleep(0.05)
    assert x.pixel(150, 150) == RED


# ---- 0.2: decoration ------------------------------------------------------

def _wm_and_window(xvfb, pixel=RED):
    x = Client(xvfb)
    w = x.window(100, 100, 100, 100, pixel)
    x.map(w)
    wm = Scene(xvfb)
    assert wm.become_wm(x) == []
    return x, w, wm


def test_outsider_may_not_decorate(xvfb):
    x, w, _ = _wm_and_window(xvfb)
    outsider = Scene(xvfb)
    assert outsider.decorate(w, RADIUS, radius=10) == [BAD_ACCESS]
    assert isinstance(outsider.decoration(w), X11Error)


def test_rounded_corners(xvfb):
    x, w, wm = _wm_and_window(xvfb)
    assert x.pixel(100, 100) == RED
    assert wm.decorate(w, RADIUS, radius=20) == []
    assert x.pixel(100, 100) == ROOT_BG      # corner cut away
    assert x.pixel(199, 199) == ROOT_BG
    assert x.pixel(150, 100) == RED          # middle of an edge
    assert x.pixel(100, 150) == RED
    assert x.pixel(150, 150) == RED
    assert wm.decoration(w) == (RADIUS, 20, 0, 0, 0, 0, 0)
    assert wm.clear(w, RADIUS) == []
    assert x.pixel(100, 100) == RED
    assert wm.decoration(w)[0] == 0


def test_rounded_corners_follow_move_and_opacity(xvfb):
    x, w, wm = _wm_and_window(xvfb)
    wm.decorate(w, RADIUS, radius=20)
    assert wm.set(w, OPACITY, 0x80000000) == []
    assert x.pixel(100, 100) == ROOT_BG
    assert _near(x.pixel(150, 150), 0x7F0000)  # half red over black
    assert wm.move(w, 400) == []
    assert x.pixel(400, 100) == ROOT_BG
    assert _near(x.pixel(450, 150), 0x7F0000)
    assert x.pixel(150, 150) == ROOT_BG


def test_sharp_shadow(xvfb):
    x, w, wm = _wm_and_window(xvfb)
    assert wm.decorate(w, SHADOW, blur=0, dx=20, dy=20, color=0xFFFFFFFF) == []
    assert x.pixel(210, 210) == WHITE        # shadow beyond the window
    assert x.pixel(150, 150) == RED          # window drawn above its shadow
    assert x.pixel(110, 210) == ROOT_BG      # shadow is offset, not grown
    assert x.pixel(225, 225) == ROOT_BG
    assert wm.decoration(w) == (SHADOW, 0, 0, 20, 20, 0xFFFFFFFF, 0)


def test_shadow_colour_is_not_premultiplied(xvfb):
    x, w, wm = _wm_and_window(xvfb)
    wm.decorate(w, SHADOW, dx=20, dy=20, color=0x80FFFFFF)  # half-opaque white
    assert _near(x.pixel(210, 210), 0x808080)


def test_blurred_shadow_fades(xvfb):
    x, w, wm = _wm_and_window(xvfb)
    wm.decorate(w, SHADOW, blur=16, color=0xFF00FF00)
    inner = (x.pixel(203, 150) >> 8) & 0xFF
    outer = (x.pixel(212, 150) >> 8) & 0xFF
    assert 0 < outer < inner < 0xFF, (inner, outer)
    assert x.pixel(230, 150) == ROOT_BG       # beyond blur radius
    assert x.pixel(150, 150) == RED


def test_shadow_hidden_behind_translucent_window(xvfb):
    x, w, wm = _wm_and_window(xvfb)
    wm.set(w, OPACITY, 0x80000000)
    wm.decorate(w, RADIUS | SHADOW, radius=20, dx=10, dy=10, color=0xFFFFFFFF)
    assert _near(x.pixel(150, 150), 0x7F0000)   # no white through the window
    assert x.pixel(205, 150) == WHITE           # shadow beside it
    assert x.pixel(198, 198) == WHITE           # and in the rounded-off corner


def test_shadow_falls_on_windows_below(xvfb):
    x = Client(xvfb)
    below = x.window(50, 50, 300, 300, BLUE)
    x.map(below)
    w = x.window(100, 100, 100, 100, RED)
    x.map(w)
    wm = Scene(xvfb)
    wm.become_wm(x)
    wm.decorate(w, SHADOW, dx=20, dy=20, color=0x80000000)
    assert _near(x.pixel(210, 210), 0x00007F)
    assert x.pixel(300, 300) == BLUE


def test_shadow_moves_with_window(xvfb):
    x, w, wm = _wm_and_window(xvfb)
    wm.decorate(w, SHADOW, dx=20, dy=20, color=0xFFFFFFFF)
    assert x.pixel(210, 210) == WHITE
    assert wm.move(w, 500) == []
    assert x.pixel(210, 210) == ROOT_BG       # old shadow repainted
    assert x.pixel(610, 210) == WHITE


def test_shadow_change_repaints(xvfb):
    x, w, wm = _wm_and_window(xvfb)
    wm.decorate(w, SHADOW, dx=20, dy=20, color=0xFFFFFFFF)
    assert x.pixel(210, 210) == WHITE
    wm.decorate(w, SHADOW, dx=-20, dy=-20, color=0xFFFFFFFF)
    assert x.pixel(210, 210) == ROOT_BG
    assert x.pixel(90, 90) == WHITE
    wm.clear(w, SHADOW)
    assert x.pixel(90, 90) == ROOT_BG


def test_decorated_fullscreen_window_is_composited(xvfb):
    x = Client(xvfb)
    fs = x.window(0, 0, SCREEN_W, SCREEN_H, GREEN)
    x.map(fs)
    x.pixel(0, 0)
    assert _becomes_direct(x, fs)
    wm = Scene(xvfb)
    wm.become_wm(x)
    wm.decorate(fs, RADIUS, radius=30)
    assert x.pixel(0, 0) == ROOT_BG
    assert _is_composited(x, fs)
    wm.clear(fs, RADIUS)
    assert x.pixel(0, 0) == GREEN
    assert _becomes_direct(x, fs)


def test_swapped_decoration(xvfb):
    x, w = _red_over_blue(xvfb)
    wm = Scene(xvfb, swapped=True)
    wm.become_wm(x)
    assert wm.decorate(w, RADIUS | SHADOW | BLUR, radius=12, blur=6, dx=-3, dy=5,
                       color=0x80102030, bg_blur=9) == []
    assert wm.decoration(w) == (RADIUS | SHADOW | BLUR, 12, 6, -3, 5, 0x80102030, 9)


def test_window_manager_exit_clears_decoration(xvfb):
    x, w, wm = _wm_and_window(xvfb)
    wm.decorate(w, RADIUS | SHADOW, radius=20, dx=20, dy=20, color=0xFFFFFFFF)
    assert x.pixel(100, 100) == ROOT_BG
    wm.c.close()
    for _ in range(50):
        if x.pixel(100, 100) == RED:
            break
        time.sleep(0.05)
    assert x.pixel(100, 100) == RED
    assert x.pixel(210, 210) == ROOT_BG


def test_destroying_decorated_window(xvfb):
    x, w, wm = _wm_and_window(xvfb)
    wm.decorate(w, RADIUS | SHADOW, radius=20, blur=8, color=0xFFFFFFFF)
    assert x.pixel(150, 150) == RED
    x.req(4, 0, struct.pack("<I", w))   # DestroyWindow
    assert x.pixel(150, 150) == ROOT_BG
    assert x.pixel(205, 150) == ROOT_BG


# ---- 0.3: background blur --------------------------------------------------

def _blur_scene(xvfb):
    """Red left of x=400, blue right of it; a nearly transparent black
    window across the edge."""
    x = Client(xvfb)
    red = x.window(0, 0, 400, 400, RED)
    blue = x.window(400, 0, 400, 400, BLUE)
    glass = x.window(300, 100, 200, 200, 0x000000)
    for w in (red, blue, glass):
        x.map(w)
    wm = Scene(xvfb)
    wm.become_wm(x)
    wm.set(glass, OPACITY, 0x10000000)
    return x, wm, glass, blue


def _rgb(p):
    return (p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF


def test_background_blur(xvfb):
    x, wm, glass, _ = _blur_scene(xvfb)
    assert _rgb(x.pixel(398, 200))[2] == 0          # sharp edge without blur
    assert wm.decorate(glass, BLUR, bg_blur=16) == []
    r, g, b = _rgb(x.pixel(398, 200))
    assert r > 60 and b > 60, (r, g, b)               # red and blue mixed
    r, g, b = _rgb(x.pixel(310, 200))
    assert r > 200 and b < 4, (r, g, b)               # far from the edge
    assert x.pixel(398, 350) == RED                   # nothing outside the window
    assert x.pixel(402, 50) == BLUE
    assert wm.decoration(glass)[6] == 16
    assert wm.clear(glass, BLUR) == []
    assert _rgb(x.pixel(398, 200))[2] == 0


def test_background_blur_follows_changes_nearby(xvfb):
    x, wm, glass, blue = _blur_scene(xvfb)
    wm.decorate(glass, BLUR, bg_blur=16)
    assert _rgb(x.pixel(450, 297))[1] == 0
    # just below the glass, outside it, but within the blur radius
    x.fill(blue, GREEN, 0, 300, 400, 10)
    assert _rgb(x.pixel(450, 297))[1] > 20


def test_background_blur_with_rounded_corners(xvfb):
    x = Client(xvfb)
    for w in (x.window(0, 0, 400, 400, RED), x.window(400, 0, 400, 400, BLUE),
              x.window(0, 0, 800, 100, GREEN)):          # edge along the glass top
        x.map(w)
    glass = x.window(300, 100, 200, 200, 0x000000)
    x.map(glass)
    wm = Scene(xvfb)
    wm.become_wm(x)
    wm.set(glass, OPACITY, 0x10000000)
    wm.decorate(glass, BLUR | RADIUS, bg_blur=16, radius=30)
    assert x.pixel(301, 101) == RED                   # cut-away corner: unblurred
    assert x.pixel(499, 101) == BLUE
    r, g, b = _rgb(x.pixel(398, 200))
    assert r > 60 and b > 60, (r, g, b)
    r, g, b = _rgb(x.pixel(350, 102))
    assert g > 60, (r, g, b)                          # green blurred in at the top


def test_background_blur_ignores_windows_above(xvfb):
    """The blur reads the layers below the glass, not the finished screen,
    which also has the white window above the glass."""
    x, wm, glass, _ = _blur_scene(xvfb)
    above = x.window(500, 100, 40, 200, WHITE)     # right beside the glass
    assert x.sync() == []
    assert wm.map(above) == []
    assert x.pixel(520, 200) == WHITE
    wm.decorate(glass, BLUR, bg_blur=16)
    r, g, b = _rgb(x.pixel(495, 200))                # blue below, white near
    assert g < 4 and b > 200, (r, g, b)


def test_background_blur_requests(xvfb):
    x, wm, glass, _ = _blur_scene(xvfb)
    assert wm.decorate(glass, BLUR, bg_blur=65) == [BAD_VALUE]
    assert wm.decorate(glass, BLUR, v02=True) == [BAD_VALUE]   # no field in 0.2
    assert wm.decorate(glass, RADIUS, radius=8, v02=True) == []
    assert wm.decoration(glass)[:2] == (RADIUS, 8)
    bad = wm._req(SET_DECO, "IIHHhhIHxxI", glass, RADIUS, 8, 0, 0, 0, 0, 0, 0)
    assert wm._errors(bad) == [16]                    # BadLength


def _set_root_background(x, mask, value):
    """ChangeWindowAttributes on the root (CWBackPixel 2, CWBackPixmap 1),
    then ClearArea to show it."""
    x.req(2, 0, struct.pack("<III", x.root, mask, value))
    x.req(61, 0, struct.pack("<IhhHH", x.root, 0, 0, 0, 0))
    assert x.sync() == []


def _glass_only(xvfb, x):
    glass = x.window(300, 100, 200, 200, 0x000000)
    x.map(glass)
    wm = Scene(xvfb)
    wm.become_wm(x)
    wm.set(glass, OPACITY, 0x10000000)
    return wm, glass


def test_background_blur_reads_root_colour(xvfb):
    x = Client(xvfb)
    x.map(x.window(0, 0, 400, 800, RED))
    _set_root_background(x, 2, GREEN)
    wm, glass = _glass_only(xvfb, x)
    wm.decorate(glass, BLUR, bg_blur=16)
    r, g, b = _rgb(x.pixel(398, 200))
    assert r > 60 and g > 60, (r, g, b)               # red window and green root
    r, g, b = _rgb(x.pixel(480, 200))
    assert g > 200 and r < 4, (r, g, b)


def test_background_blur_reads_root_tile(xvfb):
    """Stripes 32 pixels wide: the tile must stay aligned to the root."""
    x = Client(xvfb)
    tile = x.c.alloc_id()
    x.req(53, 24, struct.pack("<IIHH", tile, x.root, 64, 1))  # CreatePixmap
    x.fill(tile, RED, 0, 0, 32, 1)
    x.fill(tile, BLUE, 32, 0, 32, 1)
    _set_root_background(x, 1, tile)
    assert x.pixel(336, 200) == RED and x.pixel(368, 200) == BLUE
    wm, glass = _glass_only(xvfb, x)
    wm.decorate(glass, BLUR, bg_blur=16)
    r, g, b = _rgb(x.pixel(336, 200))                 # middle of a red stripe
    assert r > b + 60, (r, g, b)
    r, g, b = _rgb(x.pixel(368, 200))                 # middle of a blue stripe
    assert b > r + 60, (r, g, b)
    r, g, b = _rgb(x.pixel(352, 200))                 # on an edge: mixed
    assert abs(r - b) < 40, (r, g, b)


class TestXinerama:
    """Each window exists once per screen; the window manager describes the
    one it knows (screen 0's), and every screen must show it that way."""

    @pytest.fixture
    def xserver_args(self):
        return ["-br", "+xinerama", "-screen", "1", "1280x1024x24",
                "-compositor", "on"]

    def test_decoration_and_opacity_across_screens(self, xvfb):
        x = Client(xvfb)
        w = x.window(1230, 100, 100, 100, RED)     # boundary at x=1280
        x.map(w)
        wm = Scene(xvfb)
        wm.become_wm(x)
        assert wm.decorate(w, RADIUS, radius=20) == []
        assert x.pixel(1230, 100) == ROOT_BG       # corner on screen 0
        assert x.pixel(1329, 100) == ROOT_BG       # corner on screen 1
        assert x.pixel(1279, 100) == RED
        assert x.pixel(1280, 100) == RED
        assert wm.set(w, OPACITY, 0x80000000) == []
        for px in (1250, 1279, 1280, 1310):
            assert _near(x.pixel(px, 150), 0x7F0000), (px, hex(x.pixel(px, 150)))
        assert wm.clear(w, OPACITY | RADIUS) == []
        assert x.pixel(1329, 100) == RED
