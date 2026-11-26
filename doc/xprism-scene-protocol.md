# XPRISM-SCENE extension, version 0.4

The XPRISM-SCENE extension lets the window manager control how the
[built-in compositor](compositor.md) presents each top-level window. The
window manager specifies the result and the server draws it, so the
window manager does not need access to the contents of other clients'
windows.

Version 0.1 defines two attributes per window, opacity and a transform.
Version 0.2 adds two decorations, rounded corners and a drop shadow, and
version 0.3 a third, background blur. From version 0.4, input follows
transforms. Later versions are expected to add animation timing.

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

From version 0.4, input follows what is shown: the pointer hits a
transformed window, and its children, where they are displayed, and
the coordinates in events and `QueryPointer` replies are in the
window's own untransformed space, as if the user pointed at the window
itself. Root coordinates are not transformed. This applies while the
built-in compositor draws the screen; when an external compositor takes
over, transforms are not shown and input follows the windows' geometry
again. (In 0.1 to 0.3, transforms affected only the display.)

Transforms are intended for animations and effects such as scaled
previews; windows should still be moved and resized with
`ConfigureWindow`, which is where `TranslateCoordinates`, grabs that
confine the pointer, and `WarpPointer` relative to a window still find
them.

## Decorations (0.2, 0.3)

| Mask bit | Decoration | Description |
|---|---|---|
| `1 << 2` | corner radius | The window's corners are rounded with this radius, in pixels, up to 1024. The radius is limited to half the window's shorter side. Corners are antialiased, and the rounding is applied together with the window's opacity and transform. |
| `1 << 3` | shadow | A shadow with the window's outer size and rounded corners is drawn below the window, moved by `dx`, `dy` and blurred with a radius of `blur` pixels, up to 128. The colour is ARGB without premultiplication, so `0x80000000` is half-transparent black. Shadows are not drawn for transformed windows. |

| `1 << 4` | background blur (0.3) | What shows through the window is blurred with a radius of up to 64 pixels. Only has an effect while the window is translucent, through its opacity or an alpha channel, and is not applied to transformed windows. |

The shadow is drawn over the windows below, and the window is drawn over
its own shadow. Background blur shows the windows below the window and
the root background, never the windows above it; a blurring window seen
through another one is itself blurred, up to three levels deep. A window with a decoration is always composited, even if
it covers the whole screen.

## Lifetime

Attributes and decorations remain in effect until they are cleared or
the window is destroyed. When the window manager that set them
disconnects, the server clears them.

## Requests

Requests use the extension's major opcode and the minor opcodes below.
Multi-byte values are in the client's byte order.

### QueryVersion (0)

```
majorVersion: CARD32
minorVersion: CARD32
  =>
majorVersion: CARD32    (0)
minorVersion: CARD32    (4)
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

`mask` may only contain attribute bits (`1 << 0`, `1 << 1`).

Errors: `BadValue` (other bits in `mask`), `BadWindow`, `BadMatch`
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
mask:   CARD32          attributes and decorations to clear
```

Clears both attributes and, since 0.2, decorations.

Errors: as for `SetNodeAttributes`, except that `mask` may contain any
defined bit.

### SetNodeDecoration (4, since 0.2)

```
window:         WINDOW
mask:           CARD32  decorations to set
cornerRadius:   CARD16  used if mask includes corner radius
shadowBlur:     CARD16  used if mask includes shadow
shadowDx:       INT16
shadowDy:       INT16
shadowColor:    CARD32
backgroundBlur: CARD16  0.3; used if mask includes background blur
unused:         CARD16
```

Sets the decorations selected by `mask`, which may only contain
decoration bits (`1 << 2` to `1 << 4`); other decorations are unchanged.
The request is 28 bytes long. The 24-byte form of version 0.2, which ends
after `shadowColor`, is still accepted, but cannot set background blur.

Errors: `BadLength` (neither 24 nor 28 bytes), `BadValue` (other bits in
`mask`, background blur in a 24-byte request, a radius over 1024, a
shadow blur over 128 or a background blur over 64), `BadWindow`,
`BadMatch` (not a top-level window), `BadAccess` (the client is not the
window manager).

### GetNodeDecoration (5, since 0.2)

```
window: WINDOW
  =>
mask:         CARD32    decorations that are set
cornerRadius: CARD16    0 if not set
shadowBlur:   CARD16    the shadow fields are 0 if no shadow is set
shadowDx:     INT16
shadowDy:     INT16
shadowColor:  CARD32
backgroundBlur: CARD16  0.3; 0 if not set
```

Errors: `BadWindow`, `BadMatch`, `BadAccess`, as for
`SetNodeDecoration`.
