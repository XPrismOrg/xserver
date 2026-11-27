# WX11

Wayland applications as X11 windows on XPrism. WX11 is a Wayland
compositor whose only backend is the X server: each `xdg_toplevel`
becomes a real X window that any X11 window manager manages. It runs as a separate process,
comparable to XWayland in reverse, and is built on [Smithay](https://github.com/Smithay/smithay).

```
cargo build --release
DISPLAY=:0 target/release/wx11            # prints WAYLAND_DISPLAY=wayland-N
WAYLAND_DISPLAY=wayland-N some-wayland-app
```

Options: `--display DISPLAY` (default `$DISPLAY`), `--socket NAME` (in
`$XDG_RUNTIME_DIR`, default the first free `wayland-N`), `--sandbox-display
DISPLAY` (see below).

## Security

Each Wayland client gets an **X connection of its own**, which creates and
draws its windows (with `_NET_WM_PID` set to the client's process). XPrism
therefore sees every Wayland application as a separate client: its
capability checks, permission prompts and audit log apply per application,
not to WX11 as a whole. When a client disconnects, its X connection closes
and X removes everything it had.

Sandboxes (Flatpak and others) register a listener with
`wp_security_context_v1`. Clients that come through one connect to
`--sandbox-display` instead, typically an XPrism namespace socket:

```
xns-listen -n sandboxed --create --app-id org.example.App $XDG_RUNTIME_DIR/xprism-sandbox &
wx11 --sandbox-display $XDG_RUNTIME_DIR/xprism-sandbox
```

so XPrism confines them as it confines sandboxed X11 applications. The
socket is used with the session's credentials (`XAUTHORITY`); it decides
the namespace. Without `--sandbox-display` such clients get an ordinary
connection, and WX11 says so. Sandboxed clients cannot create security
contexts themselves. Their copy and paste go through their own X
connection too, so the namespace's `clipboard.read` and `clipboard.write`
grants decide whether they reach the session's clipboard.

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
  and a click outside it closes it; popups are kept on the screen as far
  as the client allows (flip, slide, resize)
- the clipboard and the primary selection (text) both ways: a Wayland
  copy or selection owns X's CLIPBOARD or PRIMARY (UTF8_STRING, TEXT,
  STRING as Latin-1, TARGETS), and X's are offered to the Wayland client
  with the keyboard (`wl_data_device`, `zwp_primary_selection_v1`);
  large X selections arrive in chunks (INCR). Transfers never block
  WX11: a paste is written as the client reads it, and a transfer that
  makes no progress for 10 seconds (a client that never reads its paste,
  or never finishes writing its copy) is given up, the X client's paste
  then refused
- transparency: an `argb8888` buffer gets a 32-bit ARGB window, which a
  compositing manager (or XPrism's compositor) blends
- windows X lost while covered (no compositor) are repainted on Expose

Buffers are copied into the window with `PutImage` for now.

## Not yet done (later releases)

- zero-copy dma-buf: DRI3 `PixmapFromBuffers` and Present, explicit sync
  (`linux-drm-syncobj-v1`), `wp_presentation` feedback
- touch, and one `wl_seat` per XPrism seat (XInput2)
- subsurfaces
- clipboard types other than text, and drag and drop (XDND)
- one namespace per sandboxed application (today all sandboxed clients
  share `--sandbox-display`)

## Tests

`test/pyxtest/test_wx11.py` runs `wx11` against the test server, with
`wx11-test-client` (in `src/bin/`) as the Wayland client. It is skipped
unless `cargo build` has been run here (or `WX11_DIR` names the target
directory).
