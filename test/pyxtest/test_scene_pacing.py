# SPDX-License-Identifier: MIT
#
# Frame pacing of the scene compositor (Xext/composite/compscene.c,
# scenePace): each output is composed at most once per refresh period,
# however often clients draw, and what is held back is composed when the
# output is due, without any further activity. Xvfb's outputs carry no
# timing, so they are paced at 60 Hz.
#
# Frames are counted through the Damage extension on the root window:
# composing draws on it, and every frame's damage events carry that
# frame's timestamp (milliseconds).

import struct
import time

import pytest
from test_color_cursor import Screen
from test_compositor import GREEN, RED, Client

DAMAGE_QUERY_VERSION, DAMAGE_CREATE = 0, 1
DAMAGE_REPORT_RAW = 0


@pytest.fixture
def xserver_args(tmp_path):
    return ["-br", "-compositor", "on", "-fbdir", str(tmp_path)]


def _frames_while_drawing(xvfb, seconds=1.0):
    """Draw into a window as fast as round trips allow, and count the
    distinct timestamps of the root window's damage meanwhile."""
    drawer = Client(xvfb)
    w = drawer.window(100, 100, 200, 200, RED)
    drawer.map(w)
    assert drawer.sync() == []

    watcher = Client(xvfb)
    ext = watcher.c.query_extension("DAMAGE")
    assert ext
    watcher.c.send_request(struct.pack("<BBHII", ext.opcode, DAMAGE_QUERY_VERSION, 3, 1, 1))
    damage = watcher.c.alloc_id()
    watcher.c.send_request(struct.pack("<BBHIIBxxx", ext.opcode, DAMAGE_CREATE, 4,
                                       damage, watcher.root, DAMAGE_REPORT_RAW))
    assert watcher.sync() == []
    time.sleep(0.1)
    watcher.c.flush_responses(timeout=0.05)

    stamps = set()
    end = time.monotonic() + seconds
    colors = (RED, GREEN)
    i = 0
    while time.monotonic() < end:
        drawer.fill(w, colors[i % 2], 0, 0, 200, 200)
        drawer.sync()
        i += 1
        for r in watcher.c.flush_responses(timeout=0):
            if getattr(r, "response_type", None) == ext.first_event:
                stamps.add(struct.unpack_from("<I", r.data, 12)[0])
    time.sleep(0.1)
    for r in watcher.c.flush_responses(timeout=0.05):
        if getattr(r, "response_type", None) == ext.first_event:
            stamps.add(struct.unpack_from("<I", r.data, 12)[0])
    return len(stamps), i, drawer, w


def test_composes_at_most_once_per_refresh(xvfb):
    frames, draws, _, _ = _frames_while_drawing(xvfb)
    assert draws > 200, f"only {draws} draws: the measurement needs more"
    # 60 Hz for one second, with some slack for timer and clock rounding
    # (a frame whose drawing spans a millisecond boundary counts twice)
    assert 20 <= frames <= 80, f"{frames} frames for {draws} draws"


class TestWithoutPacing:
    """The measurement itself: unpaced, every round trip is a frame."""

    @pytest.fixture(autouse=True)
    def no_pacing(self, monkeypatch):
        # autouse: set before the server starts
        monkeypatch.setenv("XPRISM_NO_FRAME_PACING", "1")

    def test_composes_every_change(self, xvfb):
        frames, draws, _, _ = _frames_while_drawing(xvfb)
        # well above the paced maximum (80). Not more: frames are counted
        # by millisecond timestamp, and where the server's clock moves in
        # kernel ticks (FreeBSD's coarse clock, 10 ms in a VM) there are
        # at most ~100 distinct ones a second
        assert frames > 90, f"{frames} frames for {draws} draws"


def test_held_back_damage_is_composed_when_due(xvfb, tmp_path):
    """A change right after a frame is held back until the output is due;
    the screen must show it then, with no further activity."""
    drawer = Client(xvfb)
    w = drawer.window(100, 100, 200, 200, RED)
    drawer.map(w)
    assert drawer.sync() == []
    screen = Screen(tmp_path / "Xvfb_screen0")
    held = []
    for first, second in ((RED, GREEN), (GREEN, RED), (RED, GREEN)):
        time.sleep(0.05)        # the output is due: the first is composed now
        drawer.fill(w, first, 0, 0, 200, 200)
        drawer.sync()
        drawer.fill(w, second, 0, 0, 200, 200)
        drawer.sync()           # within the same refresh period: held back
        held.append(screen.pixel(150, 150) == first)
        # due within a refresh period (17 ms); allow a slow VM with a
        # coarse kernel tick up to a second, still without other activity
        end = time.monotonic() + 1.0
        while screen.pixel(150, 150) != second and time.monotonic() < end:
            time.sleep(0.02)
        assert screen.pixel(150, 150) == second, "held-back change never shown"
    # the round trips take far less than a refresh period, so the second
    # change was held back at least once: the test did test something
    assert any(held), "no change was held back"
