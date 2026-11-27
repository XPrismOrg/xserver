# SPDX-License-Identifier: MIT
#
# Extended dynamic range (EDR, doc/color.md): an SDR output whose display
# can raise its backlight has headroom above SDR white. While a window
# with brighter content is on it, the compositor uses that headroom: it
# dims everything else in the encoding and asks the DDX to raise the
# backlight by the same factor, gradually.
#
# Xvfb has no backlight. -edr H (and its EDR output property) gives the
# most headroom; EDR_BACKLIGHT shows, in thousandths, the factor the
# compositor asked the backlight to be scaled by.

import struct
import time

import pytest
from test_color import (SRGB, TF_PQ, TF_SRGB, Color, _window, desc, nits_to_pq,
                        srgb_decode, srgb_encode, tone_map)
from test_color_crtcs import RandR
from test_color_events import Watcher
from test_compositor import SCREEN_H, SCREEN_W, Client, _becomes_direct, _is_composited
from xclient import X11Error

RR_CHANGE_OUTPUT_PROPERTY, RR_GET_OUTPUT_PROPERTY = 13, 15
GET_OUTPUT = 4
XA_INTEGER, XA_STRING = 19, 31
BAD_VALUE = 2


@pytest.fixture
def xserver_args():
    return ["-br", "-compositor", "on", "-edr", "4"]


def get_output(x, output):
    r = x._reply(x._req(GET_OUTPUT, struct.pack("<I", output)))
    assert not isinstance(r, X11Error), r
    f = struct.unpack_from("<BBH8III I", r.data, 8)
    return (r.data[1], f[0], f[1], f[11], f[13])     # set, primaries, transfer, refWhite, maxLum


def backlight(x, output=0):
    """EDR_BACKLIGHT of an output, in thousandths."""
    rr = RandR(x)
    atom = x.c.intern_atom("EDR_BACKLIGHT")
    d = rr.call(RR_GET_OUTPUT_PROPERTY, "IIIIIBBxx", rr.outputs[output], atom, 0, 0, 1, 0, 0)
    assert struct.unpack_from("<I", d, 8)[0] == XA_INTEGER
    return struct.unpack_from("<i", d, 32)[0]


def wait_backlight(x, want, output=0, timeout=3.0):
    """Wait for EDR_BACKLIGHT to reach want; the values seen on the way."""
    seen = []
    end = time.monotonic() + timeout
    while True:
        v = backlight(x, output)
        if not seen or seen[-1] != v:
            seen.append(v)
        if v == want or time.monotonic() > end:
            return seen
        time.sleep(0.01)


def set_edr(x, spec, output=0):
    """ChangeOutputProperty EDR; the X error code, or None."""
    rr = RandR(x)
    atom = x.c.intern_atom("EDR")
    data = spec.encode()
    pad = b"\0" * (-len(data) % 4)
    x.c.send_request(struct.pack("<BBHIIIBBxxI", rr.op, RR_CHANGE_OUTPUT_PROPERTY,
                                 6 + (len(data) + len(pad)) // 4, rr.outputs[output],
                                 atom, XA_STRING, 8, 0, len(data)) + data + pad)
    errs = x._errors()
    return errs[0] if errs else None


def pq8(nits):
    v = round(nits_to_pq(nits) * 255)
    return v * 0x010101


def white_at(h):
    """SDR white on an output using headroom h"""
    return round(srgb_encode(1 / h) * 255)


def red(p):
    return (p >> 16) & 0xFF


def hdr_window(x, nits=1000, max_lum=1000, x0=300):
    w = _window(x, pq8(nits), x0=x0)
    assert x.set(w, desc(SRGB, TF_PQ, max_lum=max_lum)) == []
    return w


# ---- the output -------------------------------------------------------------

def test_reported(xvfb):
    """Applications are told the most the output can show: sRGB with
    four times SDR white."""
    x = Color(xvfb)
    assert get_output(x, RandR(x).outputs[0]) == (1, SRGB, TF_SRGB, 80, 320)


def test_backlight_is_read_only(xvfb):
    x = Color(xvfb)
    rr = RandR(x)
    atom = x.c.intern_atom("EDR_BACKLIGHT")
    x.c.send_request(struct.pack("<BBHIIIBBxxIi", rr.op, RR_CHANGE_OUTPUT_PROPERTY, 7,
                                 rr.outputs[0], atom, XA_INTEGER, 32, 0, 1, 4000))
    assert x._errors() == [BAD_VALUE]


# ---- SDR content: nothing changes -----------------------------------------------

def test_sdr_only(xvfb):
    x = Color(xvfb)
    _window(x, 0xFFFFFF)
    assert x.pixel(150, 150) & 0xFFFFFF == 0xFFFFFF
    time.sleep(0.3)
    assert backlight(x) == 1000


def test_fullscreen_sdr_window_is_still_shown_directly(xvfb):
    """Without bright content the output shows plain sRGB, so a game in
    full screen keeps direct scanout."""
    x = Color(xvfb)
    w = x.window(0, 0, SCREEN_W, SCREEN_H, 0x336699)
    x.map(w)
    assert _becomes_direct(x, w)
    assert backlight(x) == 1000


# ---- HDR content: the headroom is used ---------------------------------------------

def test_hdr_content_uses_the_headroom(xvfb):
    x = Color(xvfb)
    _window(x, 0xFFFFFF)
    hdr_window(x, nits=1000, max_lum=1000)
    assert wait_backlight(x, 4000)[-1] == 4000
    # SDR white is dimmed by the factor the backlight is raised by
    assert abs(red(x.pixel(150, 150)) - white_at(4)) <= 2
    # HDR content: 1000 cd/m^2 (4.9 times SDR white) is tone mapped into
    # the four times SDR white the output has
    want = round(srgb_encode(tone_map(1000, 1000, 4 * 203) / 203 / 4) * 255)
    assert abs(red(x.pixel(350, 150)) - want) <= 2, (red(x.pixel(350, 150)), want)


def test_reference_white_matches_sdr_white(xvfb):
    """HDR content at reference white (203 cd/m^2) looks like SDR white."""
    x = Color(xvfb)
    _window(x, 0xFFFFFF)
    w = hdr_window(x, nits=1000)
    assert wait_backlight(x, 4000)[-1] == 4000
    x.fill(w, pq8(203), 0, 0, 100, 100)
    assert abs(red(x.pixel(350, 150)) - red(x.pixel(150, 150))) <= 3


def test_gradual(xvfb):
    """The headroom moves in steps, up and down, so that the backlight and
    the dimming change together."""
    x = Color(xvfb)
    w = hdr_window(x)
    up = wait_backlight(x, 4000)
    assert up[-1] == 4000 and len(up) >= 4, up
    assert up == sorted(up), up
    x.unmap(w)
    down = wait_backlight(x, 1000)
    assert down[-1] == 1000 and len(down) >= 4, down
    assert down == sorted(down, reverse=True), down


def test_back_to_sdr(xvfb):
    x = Color(xvfb)
    _window(x, 0xFFFFFF)
    w = hdr_window(x)
    assert wait_backlight(x, 4000)[-1] == 4000
    x.unmap(w)
    assert wait_backlight(x, 1000)[-1] == 1000
    assert x.pixel(150, 150) & 0xFFFFFF == 0xFFFFFF


def test_only_as_much_as_the_content_needs(xvfb):
    """Content up to twice SDR white uses twice, not four times."""
    x = Color(xvfb)
    _window(x, 0xFFFFFF)
    hdr_window(x, nits=406, max_lum=406)
    assert wait_backlight(x, 2000)[-1] == 2000
    time.sleep(0.2)
    assert backlight(x) == 2000
    assert abs(red(x.pixel(150, 150)) - white_at(2)) <= 2
    # its peak is the output's: shown at full scale
    assert red(x.pixel(350, 150)) >= 253


def test_description_cleared(xvfb):
    x = Color(xvfb)
    w = hdr_window(x)
    assert wait_backlight(x, 4000)[-1] == 4000
    assert x.clear(w) == []
    assert wait_backlight(x, 1000)[-1] == 1000


def test_fullscreen_hdr_window_is_composited(xvfb):
    """Its content needs converting for the headroom: not shown directly."""
    x = Color(xvfb)
    w = x.window(0, 0, SCREEN_W, SCREEN_H, pq8(1000))
    assert x.set(w, desc(SRGB, TF_PQ, max_lum=1000)) == []
    x.map(w)
    assert wait_backlight(x, 4000)[-1] == 4000
    time.sleep(0.2)
    assert _is_composited(x, w)


# ---- the limit changes (the user's brightness) -----------------------------------

def test_lower_limit(xvfb):
    x = Color(xvfb)
    hdr_window(x)
    assert wait_backlight(x, 4000)[-1] == 4000
    assert set_edr(x, "2") is None
    assert wait_backlight(x, 2000)[-1] == 2000
    assert get_output(x, RandR(x).outputs[0])[4] == 160
    _window(x, 0xFFFFFF)
    assert abs(red(x.pixel(150, 150)) - white_at(2)) <= 2


def test_limit_off(xvfb):
    x = Color(xvfb)
    hdr_window(x)
    assert wait_backlight(x, 4000)[-1] == 4000
    assert set_edr(x, "") is None
    assert wait_backlight(x, 1000)[-1] == 1000
    assert get_output(x, RandR(x).outputs[0])[0] == 0     # plain sRGB
    _window(x, 0xFFFFFF)
    assert x.pixel(150, 150) & 0xFFFFFF == 0xFFFFFF


def test_limit_change_is_announced(xvfb):
    w = Watcher(xvfb)
    out = RandR(w).outputs[0]
    assert w.select(w.root) == []
    assert set_edr(Color(xvfb), "2") is None
    assert [e[1] for e in w.events()] == [out]


def test_steps_are_not_announced(xvfb):
    """Applications are told the limit, not every step towards it."""
    w = Watcher(xvfb)
    assert w.select(w.root) == []
    other = Color(xvfb)        # its replies and errors are read past events
    hdr_window(other)
    assert wait_backlight(other, 4000)[-1] == 4000
    assert w.events() == []
    assert set_edr(other, "2") is None        # but the limit is
    assert len(w.events()) == 1


@pytest.mark.parametrize("spec", ["0.5", "x", "65"])
def test_bad_limit(xvfb, spec):
    assert set_edr(Color(xvfb), spec) == BAD_VALUE


# ---- content described with headroom ------------------------------------------

def test_extended_srgb_content(xvfb):
    """sRGB whose full scale is twice SDR white: on this output, shown with
    twice the headroom, full scale at full scale."""
    x = Color(xvfb)
    _window(x, 0xFFFFFF)
    w = _window(x, 0xFFFFFF, x0=300)
    assert x.set(w, desc(SRGB, TF_SRGB, max_lum=160)) == []
    assert wait_backlight(x, 2000)[-1] == 2000
    assert red(x.pixel(350, 150)) >= 253
    assert abs(red(x.pixel(150, 150)) - white_at(2)) <= 2


class TestWithoutEDR:
    """An output without EDR clips what is above SDR white: SDR white stays
    SDR white."""

    @pytest.fixture
    def xserver_args(self):
        return ["-br", "-compositor", "on"]

    def test_reported(self, xvfb):
        x = Color(xvfb)
        assert get_output(x, RandR(x).outputs[0])[0] == 0

    def test_extended_srgb_content(self, xvfb):
        x = Color(xvfb)
        w = _window(x, 0x000000)
        assert x.set(w, desc(SRGB, TF_SRGB, max_lum=160)) == []
        for v in (0x40, 0x80, 0xC0, 0xFF):
            x.fill(w, v * 0x010101, 0, 0, 100, 100)
            want = round(srgb_encode(min(srgb_decode(v / 255) * 2, 1)) * 255)
            got = red(x.pixel(150, 150))
            assert abs(got - want) <= 2, (hex(v), got, want)
        assert backlight(x) == 1000


# ---- several CRTCs: each its own ------------------------------------------------

class TestTwoCrtcs:
    @pytest.fixture
    def xserver_args(self):
        return ["-br", "-compositor", "on", "-crtcs", "2@640x480", "-edr", "4"]

    def test_only_the_crtc_with_hdr_content(self, xvfb):
        x = Color(xvfb)
        rr = RandR(x)
        rr.light(1, 640)
        _window(x, 0xFFFFFF)                       # on CRTC 0
        hdr_window(x, x0=700)                      # on CRTC 1
        assert wait_backlight(x, 4000, output=1)[-1] == 4000
        time.sleep(0.2)
        assert backlight(x, 0) == 1000
        assert x.pixel(150, 150) & 0xFFFFFF == 0xFFFFFF


# ---- without the compositor ---------------------------------------------------------

class TestCompositorOff:
    @pytest.fixture
    def xserver_args(self):
        return ["-br", "-compositor", "off", "-edr", "4"]

    def test_nothing_happens(self, xvfb):
        x = Client(xvfb)
        _window(x, 0xFFFFFF)
        time.sleep(0.3)
        assert backlight(x) == 1000
        assert x.pixel(150, 150) & 0xFFFFFF == 0xFFFFFF
