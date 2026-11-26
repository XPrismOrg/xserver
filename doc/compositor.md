# The scene compositor

XPrism moves final composition into the server (design decision AD-1,
"the server composites, the window manager decides"): the server draws every
output itself, and the window manager will describe effects through the
XPrism-Scene protocol instead of reading other windows' pixels. This
document describes the first piece, the scene compositor, which is
**experimental and off by default**.

```sh
Xvfb :1 -compositor on
```

## What it does

With `-compositor on`, the server takes Composite's *manual*
redirection of every top-level window on each screen. Every window then
renders into its own pixmap, as with an external compositor, and Composite
paints nothing to the screen. Once per main-loop iteration (in a block
handler) the compositor runs one frame per screen:

1. **Snapshot.** It walks the root's children bottom to top and records
   each viewable `InputOutput` window as a node: id, bounds (border
   included, shape-aware), and opacity.
2. **Damage.** It compares the snapshot with the previous frame. New,
   removed, moved, resized or re-opacified nodes add their old and new
   bounds. A restack adds only the overlap of each pair of windows whose
   relative order flipped, which is the only place it can show. Every window's content damage (a Damage object
   per top-level) is added on top, and a change of the root background
   damages the whole screen.
3. **Paint.** Only inside the damaged region, it paints the root
   background, then every node in stacking order with Render's `Over`
   operator. Windows with an alpha channel (depth-32 ARGB visuals)
   blend with what is under them. A window's opacity becomes a constant
   alpha mask. The opacity comes from the window manager through
   [XPRISM-SCENE](xprism-scene-protocol.md), or else from
   `_NET_WM_WINDOW_OPACITY`. A transform set the same way is applied
   with bilinear filtering.

Rendering goes through Render, so it runs on the GPU wherever the DDX
accelerates Render (glamor), and in software (fb) elsewhere.

What changes for users with the compositor on:

- Translucent (ARGB) windows and window opacity work without picom or
  any other external compositor.
- Opaque windows look exactly as they do with it off. The tests check
  this.

## Legacy compositor mode

External compositors (picom, KWin-X11, Mutter-X11, xcompmgr) work as
before. When a client asks for manual redirection of a root window's
subwindows, the only redirection a compositor can't share, the scene
compositor steps aside for that screen instead of the request failing
with `BadAccess`. When that client's redirection goes away (it exits or
unredirects), the scene compositor takes the screen back on the next
frame. The server log records both events.

Taking over requires the `compositor.legacy` capability (see
[capabilities](capabilities.md)), checked in `compRedirectSubwindows`.
Today only unconfined clients can redirect a root window at all: the
plain session and privileged namespaces hold every capability, and
Xnamespace keeps confined clients off the real root before this check
is reached. The check starts denying things once session clients are
no longer all-powerful by default (Phase V).

## Code

| Where | What |
|---|---|
| `Xext/composite/compscene.c` | The compositor: snapshot, damage, paint, activation, legacy hand-over |
| `Xext/composite/compalloc.c` | Two hooks: `compSceneYield` before a manual subwindow redirection, `compSceneResume` when one is freed |
| `Xext/composite/compinit.c` | `compSceneScreenInit` at the end of Composite's screen init |
| `os/utils.c` | The `-compositor on\|off` option |
| `test/pyxtest/test_compositor.py` | Scene tests: identical results off and on for opaque scenes, ARGB blending, opacity, legacy hand-over |

## Limits (current state)

- **Off by default.** It becomes the default once it has run on real
  hardware with glamor, together with the Phase II display work.
- **One clock.** It paints in the main loop, not per output at each
  output's vblank. Per-output scheduling comes with Phase II's atomic KMS
  work; the frame function is already per screen and will become per
  output.
- **No direct scanout yet.** A fullscreen window is still copied every
  frame. Direct scanout needs atomic KMS (Phase II).
- **Drawing on the root window.** Wallpapers set as the root background
  (xsetroot, feh, nitrogen) work. Clients that draw *directly* onto the
  root window are painted over wherever windows change. External
  compositors have the same limitation.
- **Few effects yet.** The window manager can set opacity and a
  transform per window through [XPRISM-SCENE](xprism-scene-protocol.md)
  0.1. Shadows, blur and corner radius come in later versions, and
  transforms don't affect input yet.
- **Xinerama** is untested with the compositor on.
