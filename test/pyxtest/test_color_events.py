# SPDX-License-Identifier: MIT
#
# XPRISM-COLOR 0.3: OutputColorSpaceNotify. Clients that select it on a
# window hear when what an output of its screen shows changes (a monitor
# turned to HDR, another one plugged in), and ask GetOutputColorSpace.
#
# Xvfb changes an output's space at run time through its COLORSPACE
# output property, as -colorspace takes it.

import struct

import pytest
from test_color import BT2020, TF_PQ, TF_SRGB, Color
from test_color_crtcs import RandR
from test_color_output import get_output
from xclient import X11Error

SELECT_OUTPUT_INPUT = 5
RR_CHANGE_OUTPUT_PROPERTY = 13
XA_STRING = 31
BAD_VALUE, BAD_WINDOW = 2, 3


@pytest.fixture
def xserver_args():
    return ["-br", "-compositor", "on", "-crtcs", "2@640x480"]


class Watcher(Color):
    def __init__(self, xvfb, swapped=False):
        super().__init__(xvfb, swapped)
        self.event = self.c.query_extension("XPRISM-COLOR").first_event

    def select(self, window, enable=True):
        self._req(SELECT_OUTPUT_INPUT, struct.pack(self.bo + "IBxH", window, int(enable), 0))
        return self._errors()

    def events(self, timeout=0.3):
        """(window, output, timestamp) of the OutputColorSpaceNotify events
        that came, after a round trip."""
        out = []
        sync = self.c.send_request(struct.pack(self.bo + "BxH", 43, 1))
        while True:
            r = self.c.recv_response(timeout=timeout)
            if r is None:
                return out
            if isinstance(r, X11Error):
                raise AssertionError(r)
            if r.response_type == self.event:
                out.append(struct.unpack_from(self.bo + "III", r.data, 4))
            elif r.response_type == 1 and r.sequence == sync:
                # events before the reply came first; look a little more
                timeout = 0.05


def set_colorspace(xvfb, output, spec):
    """ChangeOutputProperty COLORSPACE; the X error code, or None."""
    x = Color(xvfb)
    rr = RandR(x)
    atom = x.c.intern_atom("COLORSPACE")
    data = spec.encode()
    pad = b"\0" * (-len(data) % 4)
    x.c.send_request(struct.pack("<BBHIIIBBxxI", rr.op, RR_CHANGE_OUTPUT_PROPERTY,
                                 6 + (len(data) + len(pad)) // 4, rr.outputs[output],
                                 atom, XA_STRING, 8, 0, len(data)) + data + pad)
    errs = x._errors()
    return errs[0] if errs else None


def outputs(xvfb):
    return RandR(Color(xvfb)).outputs


def test_change_is_announced(xvfb):
    w = Watcher(xvfb)
    assert w.select(w.root) == []
    out = outputs(xvfb)
    assert set_colorspace(xvfb, 1, "bt2020,pq,1000") is None
    ev = w.events()
    assert [(e[0], e[1]) for e in ev] == [(w.root, out[1])], ev
    got = get_output(w, out[1])
    assert got[:3] == (1, BT2020, TF_PQ)
    # the other output did not change
    assert get_output(w, out[0])[0] == 0


def test_back_to_the_screens_space(xvfb):
    w = Watcher(xvfb)
    out = outputs(xvfb)
    assert set_colorspace(xvfb, 0, "p3") is None
    assert w.select(w.root) == []
    assert set_colorspace(xvfb, 0, "") is None
    assert [e[1] for e in w.events()] == [out[0]]
    assert get_output(w, out[0])[0] == 0


def test_same_space_again_is_not_a_change(xvfb):
    w = Watcher(xvfb)
    assert w.select(w.root) == []
    assert set_colorspace(xvfb, 0, "p3") is None
    assert len(w.events()) == 1
    assert set_colorspace(xvfb, 0, "p3") is None
    assert w.events() == []


def test_only_for_clients_that_select(xvfb):
    selected, other = Watcher(xvfb), Watcher(xvfb)
    assert selected.select(selected.root) == []
    assert set_colorspace(xvfb, 0, "p3") is None
    assert len(selected.events()) == 1
    assert other.events() == []


def test_deselect(xvfb):
    w = Watcher(xvfb)
    assert w.select(w.root) == []
    assert w.select(w.root, False) == []
    assert set_colorspace(xvfb, 0, "p3") is None
    assert w.events() == []


def test_selected_on_a_window_that_goes(xvfb):
    """Selected on any window of the screen; gone with it."""
    w = Watcher(xvfb)
    wid = w.window(0, 0, 10, 10, 0)
    assert w.sync() == []
    assert w.select(wid) == []
    assert set_colorspace(xvfb, 0, "p3") is None
    assert [e[0] for e in w.events()] == [wid]
    w.req(4, 0, struct.pack("<I", wid))                 # DestroyWindow
    assert w.sync() == []
    assert set_colorspace(xvfb, 0, "bt2020") is None
    assert w.events() == []


def test_client_that_goes(xvfb):
    w = Watcher(xvfb)
    assert w.select(w.root) == []
    w.c.close()
    stays = Watcher(xvfb)
    assert stays.select(stays.root) == []
    assert set_colorspace(xvfb, 0, "p3") is None
    assert len(stays.events()) == 1


def test_swapped_client(xvfb):
    w = Watcher(xvfb, swapped=True)
    out = outputs(xvfb)
    assert w.select(w.root) == []
    assert set_colorspace(xvfb, 1, "p3") is None
    ev = w.events()
    assert [(e[0], e[1]) for e in ev] == [(w.root, out[1])], ev


def test_bad_requests(xvfb):
    w = Watcher(xvfb)
    assert w.select(0x1234567) == [BAD_WINDOW]
    w._req(SELECT_OUTPUT_INPUT, struct.pack("<IBxH", w.root, 2, 0))
    assert w._errors() == [BAD_VALUE]
    assert set_colorspace(xvfb, 0, "foo") == BAD_VALUE
    # unchanged by it
    assert get_output(w, outputs(xvfb)[0])[0] == 0


def test_srgb_property_is_srgb(xvfb):
    w = Watcher(xvfb)
    assert set_colorspace(xvfb, 0, "srgb") is None
    got = get_output(w, outputs(xvfb)[0])
    assert got[:3] == (0, 1, TF_SRGB)
