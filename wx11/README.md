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

Buffers are copied into the window with `PutImage` for now.

## Still to do (Phase VI)

- zero-copy dma-buf: DRI3 `PixmapFromBuffers` and Present, explicit sync
  (`linux-drm-syncobj-v1`), `wp_presentation` feedback
- input: XInput2 to `wl_seat` pointer, keyboard and touch, one seat per
  XPrism seat
- popups as override-redirect windows, subsurfaces, alpha (ARGB visual)
- clipboard and drag and drop, bridged to X selections and XDND
- a security context per Wayland client (AD-2)

## Tests

`test/pyxtest/test_wx11.py` runs `wx11` against the test server, with
`wx11-test-client` (in `src/bin/`) as the Wayland client. It is skipped
unless `cargo build` has been run here (or `WX11_DIR` names the target
directory).
