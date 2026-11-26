# SPDX-License-Identifier: MIT
#
# XPRISM-SCENE protocol v0.1 (Xext/composite/compscene.c,
# doc/xprism-scene-protocol.md): the window manager describes how the
# scene compositor shows each top-level window.

import struct
import time

import pytest
from test_compositor import BLUE, RED, ROOT_BG, Client, _near
from xclient import RawX11Connection, X11Error

X_CHANGE_WINDOW_ATTRIBUTES = 2
CW_EVENT_MASK = 1 << 11
SUBSTRUCTURE_REDIRECT = 1 << 20

SET, GET, CLEAR = 1, 2, 3
OPACITY, TRANSFORM = 1, 2
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

    def become_wm(self):
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
    assert Scene(xvfb).version() == (0, 1)


def test_only_the_window_manager_may_describe_the_scene(xvfb):
    x, w = _red_over_blue(xvfb)
    outsider = Scene(xvfb)
    assert outsider.set(w, OPACITY, 0x80000000) == [BAD_ACCESS]
    assert isinstance(outsider.get(w), X11Error)
    wm = Scene(xvfb)
    assert wm.become_wm() == []
    assert wm.set(w, OPACITY, 0x80000000) == []
    assert outsider.clear(w, OPACITY) == [BAD_ACCESS]


def test_opacity(xvfb):
    x, w = _red_over_blue(xvfb)
    wm = Scene(xvfb)
    wm.become_wm()
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
    wm.become_wm()
    assert wm.set(w, OPACITY, 0xFFFFFFFF) == []
    assert x.pixel(150, 150) == RED


def test_transform_scale(xvfb):
    x = Client(xvfb)
    w = x.window(100, 100, 100, 100, RED)
    x.map(w)
    wm = Scene(xvfb)
    wm.become_wm()
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
    wm.become_wm()
    right = [ONE, 0, 300 * ONE, 0, ONE, 0, 0, 0, ONE]
    assert wm.set(w, TRANSFORM, transform=right) == []
    assert x.pixel(150, 150) == ROOT_BG
    assert x.pixel(450, 150) == RED


def test_drawing_into_transformed_window_shows(xvfb):
    x = Client(xvfb)
    w = x.window(100, 100, 100, 100, RED)
    x.map(w)
    wm = Scene(xvfb)
    wm.become_wm()
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
    wm.become_wm()
    assert wm.set(w, 1 << 5) == [BAD_VALUE]
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
    assert wm.version() == (0, 1)
    assert wm.become_wm() == []
    assert wm.set(w, OPACITY, 0x80000000) == []
    assert _near(x.pixel(150, 150), 0x80007F)
    mask, opacity, xf = wm.get(w)
    assert (mask, opacity, xf) == (OPACITY, 0x80000000, IDENTITY)


def test_window_manager_exit_clears_its_scene(xvfb):
    x, w = _red_over_blue(xvfb)
    wm = Scene(xvfb)
    wm.become_wm()
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
