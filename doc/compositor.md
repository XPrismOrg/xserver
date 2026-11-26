# Built-in compositor

XPrism can composite windows inside the server instead of relying on an
external compositing manager. The server produces the final image for
each screen, and the window manager describes effects through the
[XPRISM-SCENE](xprism-scene-protocol.md) extension without needing access
to other windows' contents.

The compositor is experimental and disabled by default. Enable it with:

```sh
Xvfb :1 -compositor on
```

## Operation

When enabled, the server holds Composite's manual redirection of every
top-level window. Each window renders into its own pixmap, as it would
under an external compositor, and Composite itself draws nothing to the
screen. Once per main-loop iteration, and before any client reads from a
root window, the compositor updates each screen in three steps:

1. It records the viewable `InputOutput` top-level windows from bottom to
   top, with their bounds (including the border, respecting shapes),
   opacity and transform.
2. It compares this list with the previous one to find the area that
   changed. A window that appeared, disappeared, moved, was resized, or
   changed opacity or transform contributes its old and new bounds. A
   change in stacking order contributes only the overlap of each pair of
   windows whose relative order changed. Drawing inside a window
   contributes the drawn area, and a change of the root background
   contributes the whole screen.
3. It repaints only that area: first the root background, then each
   window in stacking order using Render's `Over` operator. Windows with
   an alpha channel (depth-32 visuals) are blended with what is below
   them. Window opacity is applied as a constant alpha mask; it is taken
   from XPRISM-SCENE if the window manager set it there, otherwise from
   the `_NET_WM_WINDOW_OPACITY` property. Transforms set through
   XPRISM-SCENE are applied with bilinear filtering. Rounded corners are
   applied as an antialiased alpha mask, and a window's shadow is drawn
   just before the window. Both masks are computed once per window and
   size, and reused until either changes.

For a window with background blur, the compositor renders the layers
below it (root background, windows, their shadows) into a scratch
picture covering the repainted part of the window plus the blur radius,
halves it until the remaining radius is at most 4 pixels, blurs it with
two Gaussian passes and scales it back up behind the window. Reading the
layers from the windows' own pixmaps rather than from the screen keeps
windows above out of the blur. A change below a blurring window, within
the blur radius, repaints the affected part of its blur.

Windows whose applications described their colour space through
[XPRISM-COLOR](xprism-color-protocol.md) are shown converted to sRGB: the
compositor keeps a converted copy of each such window's contents,
updated where the window is drawn on, and composes that instead.

All drawing goes through Render, so it is GPU-accelerated wherever the
driver accelerates Render (glamor) and done in software otherwise. Opaque
windows look exactly the same with the compositor enabled as without it.

## Core protocol drawing and reading

Clients see the same results as without the compositor, which the X Test
Suite checks in both modes:

- Drawing on the root window with the default `ClipByChildren` stays
  outside the windows: windows the compositor redirects still clip the
  root, unlike windows an external compositor redirects.
- Drawing on the root window with `IncludeInferiors` also changes the
  windows below it. The compositor copies the drawn pixels into each
  window's pixmap, so reading the window back returns them.
- `GetImage` on a window fails with `BadMatch` when the area is not fully
  on the screen, as for any window that is not redirected.

With a translucent window, `IncludeInferiors` drawing copies the composed
pixels, including what shows through the window, into the window's
pixmap: an approximation, since a translucent window has no single pixel
value at that point.

## Fullscreen windows

A window is drawn directly to the screen, without composition, when all
of the following hold:

- it is the topmost viewable window;
- it covers the whole screen and has no shape;
- it is opaque: no alpha channel and full opacity;
- it has no transform and no decorations;
- its pixels need no colour conversion (XPRISM-COLOR).

The compositor then removes the redirection from that window. Nothing is
copied per frame, and on hardware Present can page-flip the window's
buffers. As soon as one of the conditions stops holding, for example
because a popup appears above the window or the window is resized, the
window is redirected again. Composite copies the window contents in both
directions, so the switch is not visible. While a window is drawn
directly, the compositor never paints over it.

## External compositors

External compositing managers such as picom, KWin and Mutter continue to
work. When a client requests manual redirection of a root window's
children, which is the step that only one compositor can take, the
built-in compositor releases that screen instead of the request failing
with `BadAccess`. When the client's redirection ends, because it exits or
undoes the redirection, the built-in compositor takes the screen back.
Both events are recorded in the server log.

Taking over requires the `compositor.legacy` capability, see
[capabilities.md](capabilities.md).

## Source files

| File | Contents |
|---|---|
| `Xext/composite/compscene.c` | The compositor and the XPRISM-SCENE extension |
| `Xext/composite/xprismsceneproto.h` | XPRISM-SCENE wire format |
| `Xext/composite/compcolor.c` | Colour spaces and conversions (no server state) |
| `Xext/composite/compcolorext.c` | The XPRISM-COLOR extension and conversion of windows' contents |
| `Xext/composite/xprismcolorproto.h` | XPRISM-COLOR wire format |
| `Xext/composite/compalloc.c` | Hooks for external compositors taking over and releasing a screen |
| `Xext/composite/compinit.c` | Initialization; composing pending changes before a root window is read |
| `mi/mivaltree.c` | Clipping: windows the compositor redirects still clip the root |
| `test/pyxtest/test_compositor.py` | Compositor tests |
| `test/pyxtest/test_scene_protocol.py` | XPRISM-SCENE tests |
| `test/pyxtest/test_color.py` | XPRISM-COLOR tests |
| `test/compcolor.c` | Unit tests of the colour library |

## Limitations

- The compositor is disabled by default. It will be enabled by default
  after it has been tested on real hardware with glamor.
- All screens are updated from the main loop rather than at each
  output's vertical blank. Per-output scheduling depends on the atomic
  modesetting work planned for Phase II.
- Direct drawing of fullscreen windows applies only to windows that cover
  the whole screen. A window covering one of several monitors is still
  composited, and hardware overlay planes are not used.
- Wallpapers set as the root window background (xsetroot, feh,
  nitrogen) work. Programs that draw directly onto the root window are
  overwritten wherever the compositor repaints, which is also the case
  with external compositors.
- The window manager can currently set opacity, transforms, rounded
  corners, shadows and background blur. Input follows transforms
  (pointer hits and event coordinates), but `TranslateCoordinates`,
  pointer confinement and `WarpPointer` relative to a window use its
  actual geometry. The shadow of a transformed window is recomputed
  whenever it is repainted, unlike that of other windows, which is
  cached.
- Background blur costs about 1 to 16 ms to repaint a whole 600x400
  window in software, depending on the radius (small radii cost most),
  and well under 1 ms for small updates. With glamor the blur runs on
  the GPU: about 1 to 1.5 ms on an AMD GPU and 2 to 4 ms on an NVIDIA
  one for any radius, measured on Xvfb, whose screen is in system
  memory, so the final copy to the screen is included.
- Shadow and corner masks are computed on the CPU whenever a decorated
  window changes size. Only the corners and a band as wide as the blur
  are computed; the edges and the middle are repeated from them. For a
  1200x950 window this takes about 0.1 ms for the corners and under 1 ms
  for a shadow with a 32-pixel blur, rising to about 9 ms with the
  largest blur (128 pixels).
- With Xinerama, each screen composes its own part of the scene. A
  window's opacity and XPRISM-SCENE settings are those set on it as
  clients see it (its screen-0 copy) and apply on every screen it
  covers. Each screen's composition is tested on Xvfb, not on real
  hardware.
