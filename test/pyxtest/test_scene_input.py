# SPDX-License-Identifier: MIT OR X11
#
# XPRISM-SCENE transforms and input: a transformed window is hit where it
# is shown, and the coordinates it receives are in its own, untransformed
# space, as if the user pointed at the window itself.

import struct

import pytest
from proto import xtest
from test_compositor import RED, Client, _redirect_root
from test_scene_protocol import ONE, TRANSFORM, Scene
from xclient import X11Error, X11Reply

X_CHANGE_WINDOW_ATTRIBUTES = 2
X_QUERY_POINTER = 38
CW_EVENT_MASK = 1 << 11
BUTTON_PRESS_MASK = 1 << 2
BUTTON_PRESS = 4
MOTION_NOTIFY = 6
BUTTON_RELEASE = 5

RIGHT_300 = [ONE, 0, 300 * ONE, 0, ONE, 0, 0, 0, ONE]
TWICE = [2 * ONE, 0, 0, 0, 2 * ONE, 0, 0, 0, ONE]


@pytest.fixture
def xserver_args():
    return ["-br", "-compositor", "on"]


class Input(Client):
    def __init__(self, xserver):
        super().__init__(xserver)
        ext = self.c.query_extension("XTEST")
        if ext is None:
            pytest.skip("XTEST not available")
        self.xtest = ext.opcode

    def reply(self, seq):
        while True:
            r = self.c.recv_response(timeout=5.0)
            assert r is not None
            assert not isinstance(r, X11Error), r
            if isinstance(r, X11Reply) and r.response_type == 1 and r.sequence == seq:
                return r.data

    def move_to(self, px, py):
        self.c.send_request(xtest.FakeInputRequest(opcode=self.xtest, type=MOTION_NOTIFY,
                                                   root=self.root, root_x=px, root_y=py))
        self.sync()

    def click(self):
        for t in (BUTTON_PRESS, BUTTON_RELEASE):
            self.c.send_request(xtest.FakeInputRequest(opcode=self.xtest, type=t, detail=1))
        # no sync(): it would read, and drop, the events

    def query_pointer(self, wid):
        """(child, win_x, win_y) of core QueryPointer."""
        d = self.reply(self.req(X_QUERY_POINTER, 0, struct.pack("<I", wid)))
        child, = struct.unpack_from("<I", d, 12)
        win_x, win_y = struct.unpack_from("<hh", d, 20)
        return child, win_x, win_y

    def select_buttons(self, wid):
        self.req(X_CHANGE_WINDOW_ATTRIBUTES, 0, struct.pack("<III", wid, CW_EVENT_MASK, BUTTON_PRESS_MASK))
        self.sync()

    def button_press(self):
        """(event window, root_x, root_y, event_x, event_y) of the next
        ButtonPress."""
        while True:
            r = self.c.recv_response(timeout=5.0)
            assert r is not None, "no ButtonPress"
            if not isinstance(r, X11Error) and r.response_type == BUTTON_PRESS:
                event, = struct.unpack_from("<I", r.data, 12)
                return (event,) + struct.unpack_from("<hhhh", r.data, 20)


def _setup(xvfb, transform):
    x = Input(xvfb)
    w = x.window(100, 100, 100, 100, RED)
    x.map(w)
    wm = Scene(xvfb)
    wm.become_wm(x)
    assert wm.set(w, TRANSFORM, transform=transform) == []
    x.pixel(0, 0)   # composed
    return x, wm, w


def test_window_is_hit_where_it_is_shown(xvfb):
    x, wm, w = _setup(xvfb, RIGHT_300)
    x.move_to(450, 150)
    assert x.query_pointer(x.root)[0] == w
    assert x.query_pointer(w)[1:] == (50, 50)
    # where the window really is, nothing is shown: the root is hit
    x.move_to(150, 150)
    assert x.query_pointer(x.root)[0] == 0


def test_scaled_window_gets_its_own_coordinates(xvfb):
    x, wm, w = _setup(xvfb, TWICE)
    x.select_buttons(w)
    x.move_to(250, 250)                 # shown 150,150 into the window
    assert x.query_pointer(w)[1:] == (75, 75)
    x.click()
    event, root_x, root_y, event_x, event_y = x.button_press()
    assert event == w
    assert (root_x, root_y) == (250, 250)      # the root is not transformed
    assert (event_x, event_y) == (75, 75)
    # beyond the window's size but within what is shown
    x.move_to(290, 110)
    assert x.query_pointer(x.root)[0] == w
    assert x.query_pointer(w)[1:] == (95, 5)


def test_children_of_a_transformed_window(xvfb):
    x, wm, w = _setup(xvfb, RIGHT_300)
    child = x.c.alloc_id()
    x.req(1, 0, struct.pack("<IIhhHHHHII", child, w, 50, 50, 50, 50, 0, 1, 0, 0))  # CreateWindow
    x.map(child)
    x.select_buttons(child)
    x.move_to(470, 170)
    assert x.query_pointer(x.root)[0] == w
    assert x.query_pointer(w)[0] == child
    assert x.query_pointer(child)[1:] == (20, 20)
    x.click()
    event, _, _, event_x, event_y = x.button_press()
    assert (event, event_x, event_y) == (child, 20, 20)


def test_xi2_query_pointer(xvfb):
    x, wm, w = _setup(xvfb, TWICE)
    ext = x.c.query_extension("XInputExtension")
    if ext is None:
        pytest.skip("XInput not available")
    x.reply(x.req(ext.opcode, 47, struct.pack("<HH", 2, 2)))   # XIQueryVersion 2.2
    x.move_to(250, 250)
    d = x.reply(x.req(ext.opcode, 40, struct.pack("<IH2x", w, 2)))  # XIQueryPointer, VCP
    win_x, win_y = struct.unpack_from("<ii", d, 24)
    # the pointer's top-left corner of pixel 250 is 150 into the shown
    # window, so 75 into the window
    assert (win_x / 65536, win_y / 65536) == (75.0, 75.0)


def test_input_follows_cleared_transform(xvfb):
    x, wm, w = _setup(xvfb, RIGHT_300)
    assert wm.clear(w, TRANSFORM) == []
    x.pixel(0, 0)
    x.move_to(150, 150)
    assert x.query_pointer(x.root)[0] == w
    assert x.query_pointer(w)[1:] == (50, 50)
    x.move_to(450, 150)
    assert x.query_pointer(x.root)[0] == 0


def test_legacy_compositor_gets_untransformed_input(xvfb):
    """An external compositor does not show XPRISM-SCENE transforms, so
    input must follow the windows' real geometry again."""
    x, wm, w = _setup(xvfb, RIGHT_300)
    legacy = Client(xvfb)
    assert _redirect_root(legacy) == []
    x.move_to(150, 150)
    assert x.query_pointer(x.root)[0] == w
    assert x.query_pointer(w)[1:] == (50, 50)
