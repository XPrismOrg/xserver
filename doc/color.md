# Colour management, wide gamut and HDR (Phase IV design)

Status: being implemented. Steps 1 to 4 are done, step 5 in part: the colour library
(`Xext/composite/compcolor.[ch]`, tested by `test/compcolor.c`),
[XPRISM-COLOR 0.3](xprism-color-protocol.md) with window descriptions
and an event when an output's space changes,
output colour spaces in Xvfb, including HDR (PQ and HLG) outputs, and, with
glamor, composition in linear light in half float; the conversion runs
on the GPU with glamor. The modesetting driver remains, see Steps.

## Goals

- Applications can say what colour space their window's pixels are in:
  sRGB (the default, as today), wide-gamut spaces such as Display P3 and
  BT.2020, and HDR content (PQ or HLG transfer functions).
- Each output (monitor) is driven in its own colour space: an SDR sRGB
  monitor, a wide-gamut SDR monitor, and an HDR monitor can be used side
  by side, and a window that spans them looks right on each.
- The built-in compositor converts every window from its colour space
  into each output's, so applications do not need to know which monitor
  they are on.
- Nothing changes for applications and window managers that know
  nothing of this: untagged windows are sRGB, and with only sRGB outputs
  composition gives exactly today's pixels.

Non-goals for Phase IV: ICC profiles evaluated in the server (clients
and colour daemons keep doing that, as with `_ICC_PROFILE` today),
calibration, and colour management without the built-in compositor.

## Model

A colour space is described by parameters, as in Wayland's
color-management-v1 protocol, so that clients and the server agree
without exchanging profiles:

- primaries and white point (named: sRGB/BT.709, Display P3, BT.2020,
  or given as CIE xy coordinates);
- transfer function (sRGB, gamma 2.2, linear, PQ, HLG);
- luminance: reference white, and for HDR the mastering range
  (min/max luminance, MaxCLL, MaxFALL).

The compositor works in linear light, scaled so that 1.0 is SDR
reference white. Each window is converted into it (decode the transfer
function, 3x3 primaries matrix), composed (blending in linear light,
which also fixes the darkened edges of today's blending in sRGB), and
encoded for each output (3x3 matrix, gamut and tone mapping where the
output cannot show the content, encode the output's transfer function).

As built (step 4), with one colour space per screen, that linear light
has the output's primaries, and gamut and tone mapping happen when each
window is converted into it, so that blending (which only mixes colours
the output can show) needs none and the encoding of each frame is the
transfer function alone.

Where a screen's monitors show different spaces (one per CRTC,
`CompositeSetCrtcColorSpace`, Xvfb `-crtccolorspace`), the compositor
composes for the widest of them: the primaries with the largest gamut,
and if any is HDR, PQ up to the brightest peak. What each CRTC shows is
then converted for its own space: in linear light, straight from the
blended light when each frame is encoded (gamut and tone mapping for
that CRTC), otherwise from the composed pixels. What no CRTC shows is
in the screen's own space. Full-screen windows are then never shown
directly.

For outputs that are all SDR sRGB and windows that are all sRGB, the
compositor keeps composing exactly as now, so the pixels are identical
and nothing costs more.

## Protocol

### Windows: XPRISM-COLOR

A new extension, kept apart from XPRISM-SCENE because applications use
it, not the window manager:

- `QueryVersion`
- `SetWindowColorSpace(window, description)` and `ClearWindowColorSpace`;
  the description is the parameters above. Applies to the window's
  contents; child windows inherit it unless they set their own.
- `GetWindowColorSpace(window)`
- `GetOutputColorSpace(output)` (step 3): what the compositor targets on
  a RandR output, so that applications can choose what to render (for
  example render HDR only where some output can show it), and an event
  when it changes.

Content in wide-gamut or HDR spaces needs more than 8 bits per channel:
applications use depth-30 visuals (10 bits per channel) as today, and a
later step adds 16-bit floating point formats (see Rendering).

### Outputs: RandR properties

Output colour state is set through RandR output properties, which
settings tools already know how to use:

- `Colorspace` and `HDR_OUTPUT_METADATA`, passed through to the KMS
  connector properties of the same names (modesetting driver);
- `max bpc`, likewise;
- `CTM` and the gamma table, which exist today; when the compositor
  manages an output's colour, it owns them.

The compositor chooses its target for each output from these.

## Rendering

The composition path needs pixel formats beyond Render's 8 and 10 bits
per channel:

1. Done, with glamor: half float (RGBA16F) pixmaps, internal to the
   compositor (clients never see them). For Render they are depth-32
   a8r8g8b8 pictures; glamor stores them as half float, and code that
   reads or writes their pixels as 8-bit ARGB (software fallbacks)
   still works, at 8 bits. Desktop GL 3.0 renders to them; GLES would
   need half float pixel transfers, and keeps composing in the output's
   encoding.
2. Software composition in linear light (fb would need 64-bit-per-pixel
   pixmaps; pixman has a16b16g16r16 and rgba_float) is not done: without
   glamor, the compositor composes in the output's encoding.
3. KMS scanout of 10-bit and fp16 framebuffers, with direct scanout of
   windows whose colour space matches the output.

## Steps

Each step is tested on Xvfb (software and glamor on a render node),
apart from the KMS parts, which need a display device and wait, like
Phase II, for vkms in CI or real hardware.

1. Done: the colour model and conversions as a library with unit tests
   (descriptions, matrices, transfer functions, tone mapping), checked
   against reference values: the sRGB and Bradford-adapted D50 matrices,
   BT.2087's BT.709 to BT.2020, ST 2084's PQ curve and BT.2408's
   reference white for PQ and HLG, and the BT.2390 EETF's knee and
   peak.
2. Done: XPRISM-COLOR window descriptions
   ([xprism-color-protocol.md](xprism-color-protocol.md)); the compositor
   converts described windows for sRGB outputs, with gamut mapping and
   tone mapping of HDR content to SDR white, on the GPU with glamor and
   on the CPU otherwise. Composition
   stays in sRGB; composition in linear light comes with the
   64-bit-per-pixel buffers of step 3.
3. Done: output colour spaces in Xvfb (`-colorspace`, per screen),
   `GetOutputColorSpace`, and composition for wide-gamut SDR and for
   HDR (PQ and HLG) outputs: every window is converted into the
   output's space and composed in its encoding; windows already in it
   are shown as they are, and can be shown directly. HLG outputs encode
   with the inverse of the BT.2100 OOTF for their peak, so SDR white is
   75% signal on a 1000 cd/m² display (BT.2408); HLG content goes to
   them unchanged, as HLG is scene-referred.
4. Done: composition in linear light with glamor, on outputs other
   than sRGB: windows, the root background and shadows are converted to
   the output's linear light in half float, blended there (translucent
   windows, shadows, background blur), and each frame is encoded for
   the output once. Values above SDR white are kept (HDR), and ARGB
   windows on HDR outputs keep more than 8 bits. Windows in the
   output's own space come back exactly from its black level up, so
   they look the same composited or shown directly. sRGB outputs still
   compose in sRGB, unchanged.
5. modesetting, in part: HDR outputs (`Option "HDR"`, and an `HDR`
   output property per output to change it at run time). Where the
   monitor's EDID offers BT.2020 RGB with PQ or HLG
   (`xf86MonitorHDRColorSpace`, unit-tested in `test/ddc-tests.c`) and
   the connector has `Colorspace` and `HDR_OUTPUT_METADATA`, the mode
   set sends BT.2020 RGB with the monitor's metadata and `max bpc` 10,
   and the compositor shows that CRTC in the monitor's HDR space (one
   space per CRTC). Checked on vkms only so far (which has neither
   property, nor an EDID): it needs an HDR monitor. Remaining: wide-gamut
   SDR monitors from their EDID primaries, fp16 scanout. Hardware cursor
   images are converted for their CRTC's space
   (`CompositeCrtcConvertARGB`).
