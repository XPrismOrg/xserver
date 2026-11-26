# WX11

Wayland applications as X11 windows on XPrism (DECISIONS.md AD-3,
Phase VI). WX11 is a Wayland compositor whose only backend is the X
server: each `xdg_toplevel` becomes a real X window that any X11 window
manager manages. It runs as a separate process, comparable to XWayland
in reverse, and is built on [Smithay](https://github.com/Smithay/smithay).

```
cargo build --release
DISPLAY=:0 target/release/wx11            # prints WAYLAND_DISPLAY=wayland-N
WAYLAND_DISPLAY=wayland-N some-wayland-app
```

Options: `--display DISPLAY` (default `$DISPLAY`), `--socket NAME` (in
`$XDG_RUNTIME_DIR`, default the first free `wayland-N`).

## What works

- `wl_compositor`, `wl_shm` and `xdg_shell` toplevels: one X window
  each, sized to the buffer, with the title as `_NET_WM_NAME` (UTF-8) and
  `WM_NAME` (Latin-1), and the app id as `WM_CLASS`
- the window manager resizing (a configure with the new size) and closing
  (`WM_DELETE_WINDOW` becomes `xdg_toplevel.close`)
- frame callbacks
- input: the keyboard (with the X server's layout, from
  `_XKB_RULES_NAMES`) follows the X focus; pointer motion, enter and
  leave, buttons and the wheel go to the toplevel under the pointer. The X
  server has already decided who gets the input (including XPrism's
  capability checks); WX11 only passes it on
- popups (menus, tooltips) as override-redirect windows placed by their
  positioner; a grabbing popup gets the keyboard and an X pointer grab,
  and a click outside it closes it
- the clipboard (text) both ways: a Wayland copy owns X's CLIPBOARD
  (UTF8_STRING, TEXT, STRING as Latin-1, TARGETS), and the X clipboard is
  offered to the Wayland client with the keyboard

Buffers are copied into the window with `PutImage` for now.

## Still to do (Phase VI)

- zero-copy dma-buf: DRI3 `PixmapFromBuffers` and Present, explicit sync
  (`linux-drm-syncobj-v1`), `wp_presentation` feedback
- touch, and one `wl_seat` per XPrism seat (XInput2)
- subsurfaces, alpha (ARGB visual), keeping popups on screen (positioner
  constraints)
- the PRIMARY selection, clipboard types other than text, large X
  transfers (INCR), and drag and drop (XDND)
- a security context per Wayland client (AD-2)

## Tests

`test/pyxtest/test_wx11.py` runs `wx11` against the test server, with
`wx11-test-client` (in `src/bin/`) as the Wayland client. It is skipped
unless `cargo build` has been run here (or `WX11_DIR` names the target
directory).
