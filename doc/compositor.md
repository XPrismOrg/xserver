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
   XPRISM-SCENE are applied with bilinear filtering.

All drawing goes through Render, so it is GPU-accelerated wherever the
driver accelerates Render (glamor) and done in software otherwise. Opaque
windows look exactly the same with the compositor enabled as without it.

## Fullscreen windows

A window is drawn directly to the screen, without composition, when all
of the following hold:

- it is the topmost viewable window;
- it covers the whole screen and has no shape;
- it is opaque: no alpha channel and full opacity;
- it has no transform.

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
| `Xext/composite/compalloc.c` | Hooks for external compositors taking over and releasing a screen |
| `Xext/composite/compinit.c` | Initialization; composing pending changes before a root window is read |
| `test/pyxtest/test_compositor.py` | Compositor tests |
| `test/pyxtest/test_scene_protocol.py` | XPRISM-SCENE tests |

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
- The window manager can currently set only opacity and transforms.
  Transforms affect only the display; input is still delivered according
  to the window's actual geometry.
- Xinerama has not been tested with the compositor enabled.
