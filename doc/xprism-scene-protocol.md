# The XPRISM-SCENE protocol, version 0.1

XPRISM-SCENE is how a window manager tells XPrism's
[scene compositor](compositor.md) how to show each top-level window. The
window manager decides *what* the screen should look like; the server
draws it. The window manager never needs to read other windows' pixels
for this (design decision AD-1).

Version 0.1 covers two per-window attributes: **opacity** and a
**transform**. Shadows, blur regions, corner radius and animation
timing will follow in later versions.

The wire format is defined in `Xext/composite/xprismsceneproto.h`.

## Availability

The extension is present only when the scene compositor runs
(`-compositor on`). Use `QueryExtension("XPRISM-SCENE")` to find out.

## Who may use it

Only **the window manager** may use it: the client that holds
`SubstructureRedirect` on the root window of the target window's screen.
For anyone else, every request except `QueryVersion` fails with
`BadAccess`.

Requests name **top-level windows** (children of a root window):
`BadMatch` for any other window, `BadWindow` for an unknown one. A
reparenting window manager describes its frame windows.

## Attributes

| Bit | Attribute | Meaning |
|---|---|---|
| `1 << 0` | opacity | `CARD32`, 0 (invisible) to `0xffffffff` (opaque), the same scale as `_NET_WM_WINDOW_OPACITY`. When set, it takes precedence over that property. |
| `1 << 1` | transform | 3x3 matrix of 16.16 fixed-point numbers, row-major. Maps a point of the window to where it is shown. Both are relative to the window's outer top-left corner (border included). Must be invertible, and must keep the whole window in front of the viewer (positive `w` at every corner). Otherwise `BadMatch`. |

Examples of transforms (`1.0` = `0x10000`):

- shift 300 px to the right: `[1 0 300; 0 1 0; 0 0 1]`
- half size, anchored at the top-left corner: `[0.5 0 0; 0 0.5 0; 0 0 1]`
- half size around the centre of a `w`x`h` window:
  `[0.5 0 w/4; 0 0.5 h/4; 0 0 1]`

**Transforms are visual only in 0.1.** Input still goes to the window's
real geometry. This is suited to animations (scale-in, slide), not to
permanently moving a window, which is what `ConfigureWindow` is for.

Attributes are kept until they are cleared or the window is destroyed.
When the window manager that set them disconnects, the server clears
them, so a crashed window manager can't leave windows invisible or
shrunk.

## Requests

All requests use the extension's major opcode and the minor opcode
below. Multi-byte values are in the client's byte order.

### QueryVersion (0)

```
majorVersion, minorVersion: CARD32   (the client's)
  =>
majorVersion, minorVersion: CARD32   (the server's: 0, 1)
```

### SetNodeAttributes (1)

```
window:    WINDOW
mask:      CARD32         which attributes to set
opacity:   CARD32         used if mask has opacity
transform: INT32[9]       used if mask has transform
```

Sets the attributes in `mask` and leaves the others unchanged. Errors:
`BadValue` (unknown mask bits), `BadWindow`, `BadMatch` (not top-level,
or an unusable transform), `BadAccess` (not the window manager). The
change shows on the next frame.

### GetNodeAttributes (2)

```
window: WINDOW
  =>
mask:      CARD32         attributes currently set
opacity:   CARD32         0xffffffff if not set
transform: INT32[9]       identity if not set
```

Errors as for `SetNodeAttributes`.

### ClearNodeAttributes (3)

```
window: WINDOW
mask:   CARD32            which attributes to clear
```

Errors as for `SetNodeAttributes`.

## Tests

`test/pyxtest/test_scene_protocol.py` covers:

- only the window manager may use the protocol;
- opacity, and that it overrides the property;
- scale and translation, including drawing into a transformed window;
- every error case;
- byte-swapped clients;
- cleanup when the window manager exits.
