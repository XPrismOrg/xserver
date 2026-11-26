# XPRISM-SCENE extension, version 0.1

The XPRISM-SCENE extension lets the window manager control how the
[built-in compositor](compositor.md) presents each top-level window. The
window manager specifies the result and the server draws it, so the
window manager does not need access to the contents of other clients'
windows.

Version 0.1 defines two attributes per window: opacity and a transform.
Later versions are expected to add shadows, blur regions, rounded corners
and animation timing.

The wire format is defined in `Xext/composite/xprismsceneproto.h`.

## Availability

The extension is only present while the built-in compositor is enabled
(`-compositor on`). Clients should check with
`QueryExtension("XPRISM-SCENE")`.

## Access

Only the window manager may use the extension. The window manager is the
client that has selected `SubstructureRedirect` on the root window of the
target window's screen. Every request except `QueryVersion` fails with
`BadAccess` for any other client.

Requests apply to top-level windows, that is, children of a root window.
Any other window results in `BadMatch`, and an unknown window in
`BadWindow`. Reparenting window managers apply attributes to their frame
windows.

## Attributes

| Mask bit | Attribute | Description |
|---|---|---|
| `1 << 0` | opacity | A `CARD32` from 0 (transparent) to `0xffffffff` (opaque), on the same scale as `_NET_WM_WINDOW_OPACITY`. If set, it is used instead of that property. |
| `1 << 1` | transform | A 3x3 matrix of 16.16 fixed-point values in row-major order. It maps a point of the window to the position where it is displayed. Both are relative to the window's outer top-left corner, including the border. The matrix must be invertible, and the transformed `w` coordinate must be positive at all four corners of the window; otherwise the request fails with `BadMatch`. |

Examples, where `1.0` is `0x10000`:

| Effect | Matrix |
|---|---|
| Move 300 pixels to the right | `[1 0 300; 0 1 0; 0 0 1]` |
| Scale to half size around the top-left corner | `[0.5 0 0; 0 0.5 0; 0 0 1]` |
| Scale to half size around the centre of a `w` by `h` window | `[0.5 0 w/4; 0 0.5 h/4; 0 0 1]` |

In version 0.1, transforms affect only how a window is displayed. Input
is still delivered according to the window's actual geometry. Transforms
are intended for animations; windows should be moved and resized with
`ConfigureWindow`.

Attributes remain in effect until they are cleared or the window is
destroyed. When the window manager that set them disconnects, the server
clears them.

## Requests

Requests use the extension's major opcode and the minor opcodes below.
Multi-byte values are in the client's byte order.

### QueryVersion (0)

```
majorVersion: CARD32
minorVersion: CARD32
  =>
majorVersion: CARD32    (0)
minorVersion: CARD32    (1)
```

The client sends the highest version it supports; the server replies
with the version it implements.

### SetNodeAttributes (1)

```
window:    WINDOW
mask:      CARD32       attributes to set
opacity:   CARD32       used if mask includes opacity
transform: INT32[9]     used if mask includes transform
```

Sets the attributes selected by `mask`; other attributes are unchanged.
The change takes effect with the next frame.

Errors: `BadValue` (undefined bits in `mask`), `BadWindow`, `BadMatch`
(not a top-level window, or an unusable transform), `BadAccess` (the
client is not the window manager).

### GetNodeAttributes (2)

```
window: WINDOW
  =>
mask:      CARD32       attributes that are set
opacity:   CARD32       0xffffffff if not set
transform: INT32[9]     the identity matrix if not set
```

Errors: as for `SetNodeAttributes`.

### ClearNodeAttributes (3)

```
window: WINDOW
mask:   CARD32          attributes to clear
```

Errors: as for `SetNodeAttributes`.
