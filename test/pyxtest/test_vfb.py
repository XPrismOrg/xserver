# SPDX-License-Identifier: MIT
#
# Xvfb screen and CRTC configuration (-screen, -crtcs).

import os
import struct
import subprocess

import pytest
from xclient import RawX11Connection, X11Error

X_GET_GEOMETRY = 14


def _root_size(conn):
    conn.send_request(struct.pack("<BxHI", X_GET_GEOMETRY, 2, conn.root_window))
    r = conn.recv_response(timeout=5.0)
    assert r is not None and not isinstance(r, X11Error), r
    return struct.unpack_from("<HH", r.data, 16)


@pytest.fixture
def xserver_args(request):
    return request.param


@pytest.mark.parametrize("xserver_args", [["-screen", "0", "640x480x24"]], indirect=True)
def test_screen_smaller_than_default(xvfb, xserver_args):
    """The default CRTC used to keep the default 1280x1024 size whatever
    -screen said, and the server refused any smaller screen."""
    conn = RawX11Connection(xvfb.display_num)
    assert _root_size(conn) == (640, 480)


@pytest.mark.parametrize(
    "xserver_args",
    [["-screen", "0", "640x480x24", "-screen", "1", "800x600x24"]],
    indirect=True,
)
def test_screens_of_different_sizes(xvfb, xserver_args):
    conn = RawX11Connection(xvfb.display_num)
    assert conn.num_screens == 2
    assert _root_size(conn) == (640, 480)


@pytest.mark.parametrize(
    "xserver_args",
    [["-crtcs", "2@320x240", "-screen", "0", "640x480x24",
      "-screen", "1", "640x480x24", "-crtcs", "3"]],
    indirect=True,
)
def test_crtcs_before_and_after_screen(xvfb, xserver_args):
    """-crtcs before the first -screen applies to every screen; each
    screen must own its CRTCs (they were shared, so -crtcs on one screen
    reallocated the others' and exit freed them twice)."""
    conn = RawX11Connection(xvfb.display_num)
    assert conn.num_screens == 2
    assert _root_size(conn) == (640, 480)


def test_crtc_larger_than_screen_is_refused():
    xvfb = os.environ.get("XVFB_PATH")
    if not xvfb:
        pytest.skip("XVFB_PATH not set")
    r = subprocess.run(
        [xvfb, ":399", "-nolisten", "tcp",
         "-screen", "0", "640x480x24", "-screen", "1", "640x480x24",
         "-crtcs", "2@800x600"],
        capture_output=True, text=True, timeout=30,
    )
    assert r.returncode != 0
    assert "exceeds screen 1's size 640x480" in r.stderr, r.stderr
    assert "double free" not in r.stderr and r.returncode == 1, r.stderr


def _xinerama_heads(conn):
    ext = conn.query_extension("XINERAMA")
    assert ext
    conn.send_request(struct.pack("<BBH", ext.opcode, 5, 1))   # QueryScreens
    r = conn.recv_response(timeout=5.0)
    assert r is not None and not isinstance(r, X11Error), r
    n = struct.unpack_from("<I", r.data, 8)[0]
    return [struct.unpack_from("<hhHH", r.data, 32 + 8 * k) for k in range(n)]


@pytest.mark.parametrize(
    "xserver_args",
    [["+xinerama", "-screen", "0", "640x480x24", "-screen", "1", "800x600x24"]],
    indirect=True,
)
def test_xinerama_screens_side_by_side(xvfb, xserver_args):
    """Without -origin each screen is right of the previous one (they all
    used to be at 0,0, on top of each other), and heads are listed in
    screen order (they came out reversed)."""
    conn = RawX11Connection(xvfb.display_num)
    assert _xinerama_heads(conn) == [(0, 0, 640, 480), (640, 0, 800, 600)]
    assert _root_size(conn) == (1440, 600)


@pytest.mark.parametrize(
    "xserver_args",
    [["+xinerama", "-screen", "0", "640x480x24",
      "-screen", "1", "640x480x24", "-origin", "0,480"]],
    indirect=True,
)
def test_xinerama_origin(xvfb, xserver_args):
    conn = RawX11Connection(xvfb.display_num)
    assert _xinerama_heads(conn) == [(0, 0, 640, 480), (0, 480, 640, 480)]
    assert _root_size(conn) == (640, 960)


def test_bad_origin_is_refused():
    xvfb = os.environ.get("XVFB_PATH")
    if not xvfb:
        pytest.skip("XVFB_PATH not set")
    r = subprocess.run([xvfb, ":399", "-nolisten", "tcp", "-origin", "1,2,3"],
                       capture_output=True, text=True, timeout=30)
    assert r.returncode != 0 and "Invalid -origin" in r.stderr, r.stderr
