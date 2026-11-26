# XPRISM-COLOR extension, version 0.2

The XPRISM-COLOR extension lets applications say which colour space the
pixels of their windows are in: sRGB, wide-gamut spaces such as Display
P3 and BT.2020, or HDR content encoded with the PQ or HLG transfer
functions. The [built-in compositor](compositor.md) then shows each
window converted for the output it is on, so applications do not need
to know what the output is. Version 0.2 adds outputs in colour spaces
other than sRGB, including HDR outputs, and `GetOutputColorSpace`. See
[color.md](color.md) for the design.

The wire format is defined in `Xext/composite/xprismcolorproto.h`.

## Availability

The extension is only present while the built-in compositor is enabled
(the default; `-compositor off` disables it). Clients should check with
`QueryExtension("XPRISM-COLOR")`.

## Access

A client may describe any window that it may change the attributes of
(`DixSetAttrAccess`), and read the description of any window it may read
the attributes of, as with window properties. Isolated clients
([Xnamespace](Xnamespace.md)) are limited to their namespace's windows
in the same way.

## Model

A description applies to the contents of the window, and child windows
inherit it unless they have their own. Each pixel of the screen is
therefore in the colour space of the innermost window that covers it and
has a description, and in sRGB if there is none: windows without a
description, and applications that know nothing of this extension, look
exactly as before.

A description gives:

- **primaries**: the chromaticities of the red, green and blue primaries
  and of the white point, either named or as CIE 1931 xy coordinates;
- **transfer function**: how pixel values encode light;
- **luminance**: the luminance of reference (SDR) white and the range the
  content uses, in cd/m².

Linear light is relative to reference white: decoding an SDR transfer
function gives 1.0 for full-scale white, and decoding PQ gives the
absolute luminance divided by the reference white luminance, so that
PQ's 203 cd/m² (ITU-R BT.2408) is 1.0.

### Primaries

| Value | Name | Red | Green | Blue | White |
|---|---|---|---|---|---|
| 0 | Custom | the coordinates sent | | | |
| 1 | sRGB (BT.709) | 0.640, 0.330 | 0.300, 0.600 | 0.150, 0.060 | D65 0.3127, 0.3290 |
| 2 | Display P3 | 0.680, 0.320 | 0.265, 0.690 | 0.150, 0.060 | D65 |
| 3 | BT.2020 | 0.708, 0.292 | 0.170, 0.797 | 0.131, 0.046 | D65 |

Custom coordinates are sent in millionths. Each x and y must be greater
than 0 with x + y ≤ 1, and the three primaries must not lie on a line.
White points other than D65 are adapted with the Bradford transform.

### Transfer functions

| Value | Name | |
|---|---|---|
| 0 | sRGB | the IEC 61966-2-1 piecewise curve |
| 1 | Gamma 2.2 | a pure power function |
| 2 | Linear | |
| 3 | PQ | SMPTE ST 2084, absolute luminance up to 10000 cd/m² |
| 4 | HLG | ITU-R BT.2100 hybrid log-gamma, with the BT.2100 OOTF for a display of the content's maximum luminance |

### Luminance

| Field | Unit | 0 means |
|---|---|---|
| refWhite | cd/m² | 80 for SDR transfer functions, 203 for PQ and HLG |
| minLum | 1/10000 cd/m² | 0 |
| maxLum | cd/m² | refWhite for SDR transfer functions, 10000 for PQ, 1000 for HLG |

For PQ, maxLum is the peak of the content (its mastering display); for
HLG, the peak of the display that the content's OOTF assumes. Values
above 10000 cd/m² are refused.

## Outputs

Each output shows a colour space too: sRGB unless the display driver
says otherwise. The driver sets it (on Xvfb, the `-colorspace` option,
see Xvfb(1)); the modesetting driver will set it from the monitor's
capabilities and its `Colorspace` and `HDR_OUTPUT_METADATA` properties.
An output's transfer function is sRGB, gamma 2.2, linear or PQ (HDR);
for PQ, the output's reference white is where SDR white is shown (203
cd/m² by default, BT.2408) and its maxLum the display's peak.

## How windows are shown

The compositor shows every window in the colour space of its output.
Where a window's space is the output's, its pixels are shown as they
are; otherwise the compositor converts them: it decodes the window's
transfer function, converts its primaries to the output's, tone maps
content brighter than the output can show, brings colours outside the
output's gamut into it, and encodes the output's transfer function.
Windows without a description are sRGB, so on an output that is not
sRGB they are converted too; on sRGB outputs, as in version 0.1, they
cost nothing.

- **Reference white** is shown at the output's reference white: SDR
  white is full scale on SDR outputs, and 203 cd/m² on a PQ output.
- **Tone mapping**: content brighter than the output can show (PQ and
  HLG with maxLum above what the output's peak is relative to reference
  white) is compressed with the ITU-R BT.2390 EETF so that maxLum lands
  on the output's peak; on SDR outputs that is SDR white. Below the knee
  of the curve nothing changes.
- **Gamut mapping**: a colour that the output cannot show is moved
  towards the grey of its own luminance until it fits (on a PQ output,
  up to its peak). That keeps its hue (P3's pure red on an sRGB output
  becomes the most saturated sRGB red of the same hue, not sRGB's pure
  red with its channels clipped).

The root window's background (also after drawing into its tile), the
shadows of XPRISM-SCENE and the software cursor (core and ARGB cursors)
are sRGB and are converted the same way.

A full-screen window is shown directly (without composition) only when
it is shown as it is: its colour space is the output's, and for PQ, its
reference white is the output's and its maxLum no higher than the
output's peak.

The CPU and the GPU (glamor) do the same conversion, and the tests
check both against the same reference values.

The window's own contents are not changed: `GetImage` on the window
returns the pixels the application drew. `GetImage` on the root window
returns what the screen shows.

Composition (blending of translucent windows, shadows and background
blur): with glamor, on outputs other than sRGB, in the output's linear
light, in half float: every window is converted to linear light, the
compositor blends there, and each frame is encoded for the output.
Half of white over black is then half the light (sRGB 188, not 128),
light above SDR white blends as light, and ARGB windows on HDR outputs
keep more than 8 bits per channel. Windows in the output's own space
come back exactly, from the output's black level up (below it, half
float keeps too little, and the display shows black anyway).
Otherwise (without glamor, or on sRGB outputs) composition happens in
the output's encoding, as composition in sRGB did before. Setting
`XPRISM_NO_LINEAR_COMPOSITION` in the server's environment turns
composition in linear light off. See [color.md](color.md).

## Requests

### QueryVersion

```
QueryVersion
    client-major-version: CARD32
    client-minor-version: CARD32
  ->
    major-version: CARD32
    minor-version: CARD32
```

Returns the version the server implements, 0.2.

### SetWindowColorSpace

```
SetWindowColorSpace
    window: WINDOW
    primaries: CARD8
    transfer: CARD8
    xy: LISTofCARD32 (8: red x, y, green x, y, blue x, y, white x, y)
    refWhite: CARD32
    minLum: CARD32
    maxLum: CARD32

Errors: Window, Match, Value, Access
```

Sets the window's description, replacing any it had. `xy` is used for
Custom primaries and ignored otherwise. The next frame shows the window
converted.

`Match` if the window is InputOnly. `Value` if primaries or transfer is
not one of the values above, if custom coordinates are out of range or
the primaries lie on a line, or if a luminance is above 10000 cd/m².

### GetWindowColorSpace

```
GetWindowColorSpace
    window: WINDOW
  ->
    set: BOOL
    primaries: CARD8
    transfer: CARD8
    xy: LISTofCARD32 (8)
    refWhite: CARD32
    minLum: CARD32
    maxLum: CARD32

Errors: Window, Match, Access
```

Returns the window's own description (not one it inherits), with the
defaults filled in and the coordinates of named primaries given. `set` is
False if the window has none; the rest then describes sRGB.

### ClearWindowColorSpace

```
ClearWindowColorSpace
    window: WINDOW

Errors: Window, Match, Access
```

Removes the window's description: it inherits its parent's again.

Descriptions belong to windows, not to the clients that set them: they
stay when the client disconnects, and go when the window is destroyed.

### GetOutputColorSpace (0.2)

```
GetOutputColorSpace
    output: OUTPUT (RandR)
  ->
    set: BOOL
    primaries: CARD8
    transfer: CARD8
    xy: LISTofCARD32 (8)
    refWhite: CARD32
    minLum: CARD32
    maxLum: CARD32

Errors: RandR's for an unknown output
```

Returns what the compositor shows on the output, in the form of
`GetWindowColorSpace`; `set` is False for sRGB. Applications can use it
to choose what to render, for example HDR only where an output can
show it. An event when it changes will come with the modesetting
driver, where outputs change at run time; on Xvfb they do not.

## Performance

Windows without a description cost nothing, and an sRGB description
none either. Described windows are converted where they are drawn on:

- with glamor, by a shader on the GPU: about 1 ms for a full screen of
  1280×1024, whatever its contents (measured on NVIDIA and AMD render
  nodes, readback of the screen included);
- otherwise by the CPU, with a cache of converted pixels: about 4 ms for
  a full screen of the few colours typical of user interfaces, 50 to
  150 ms (HLG the slowest) when every pixel differs, as in photographs
  and video. Setting `XPRISM_NO_GPU_COLOR` in the server's environment
  forces this path, for comparison.

On an output that is not sRGB, every top-level window whose pixels
are not in the output's space has a converted copy of its contents,
which doubles their memory. Composing in linear light, every window
has one, in half float: 8 bytes per pixel, three times the memory of
the window alone; each frame also uses a half float buffer as large as
what changed. A frame costs within 1 ms of composing in the output's
encoding (a full 1280×1024 screen on NVIDIA and AMD render nodes).

## Limitations of version 0.2

- Composition in linear light needs glamor on desktop GL. Otherwise
  composition is in the output's encoding, and converted copies have
  the window's depth: ARGB (depth 32) windows on HDR outputs are
  converted at 8 bits per channel, which bands in gradients.
- Windows of depths other than 24, 30 and 32 (16-bit visuals) are not
  converted, as before; composing in linear light, their pixels are
  taken as linear.
- One colour space per screen: on the modesetting driver, screens with
  several monitors will need one per CRTC.
- Outputs with the HLG transfer function are not supported (content
  with it is).
- Hardware cursors (modesetting) will need the same conversion as the
  software cursor, which is converted (step 5).
