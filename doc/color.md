# Colour management, wide gamut and HDR (Phase IV design)

Status: being implemented. Steps 1 to 3 are done: the colour library
(`Xext/composite/compcolor.[ch]`, tested by `test/compcolor.c`),
[XPRISM-COLOR 0.2](xprism-color-protocol.md) with window descriptions,
and output colour spaces in Xvfb, including HDR (PQ) outputs; the
conversion runs on the GPU with glamor. Composition in linear light and
the modesetting driver remain, see Steps.

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

The compositor works in linear light with BT.2020 primaries, scaled so
that 1.0 is SDR reference white. Each window is converted into it
(decode the transfer function, 3x3 primaries matrix), composed (blending
in linear light, which also fixes the darkened edges of today's
blending in sRGB), and encoded for each output (3x3 matrix, gamut and
tone mapping where the output cannot show the content, encode the
output's transfer function).

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

1. Software and glamor composition in linear BT.2020 at 16 bits per
   channel, first with integer formats, which is enough for SDR wide
   gamut. pixman has a16b16g16r16, but X pixmaps and Render pictures stop
   at 32 bits per pixel, so these are buffers internal to the
   compositor: fb and glamor both need 64-bit-per-pixel pixmaps that
   clients never see.
2. Half-float formats for HDR headroom, in glamor first (GL renders to
   RGBA16F everywhere glamor runs); software composition of HDR stays
   possible but slow.
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
   HDR (PQ) outputs: every window is converted into the output's space
   and composed in its encoding; windows already in it are shown as
   they are, and can be shown directly.
4. HDR: half-float composition with glamor, in linear light (the
   blending of translucent windows); PQ encoding and tone mapping are
   done (steps 2 and 3).
5. modesetting: `Colorspace`, `HDR_OUTPUT_METADATA`, `max bpc`, 10-bit
   and fp16 scanout (needs a display device).
