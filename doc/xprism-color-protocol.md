# XPRISM-COLOR extension, version 0.1

The XPRISM-COLOR extension lets applications say which colour space the
pixels of their windows are in: sRGB, wide-gamut spaces such as Display
P3 and BT.2020, or HDR content encoded with the PQ or HLG transfer
functions. The [built-in compositor](compositor.md) then shows each
window converted for the screen, so applications do not need to know
what the screen is. See [color.md](color.md) for the design and what
later versions add (per-output colour spaces, HDR outputs).

The wire format is defined in `Xext/composite/xprismcolorproto.h`.

## Availability

The extension is only present while the built-in compositor is enabled
(`-compositor on`). Clients should check with
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

## How windows are shown

The screen is sRGB in version 0.1. The compositor shows a described
window by decoding its transfer function, converting its primaries to
sRGB's, tone mapping HDR content, bringing colours outside the sRGB
gamut into it, and encoding sRGB:

- **Tone mapping**: content brighter than reference white (PQ and HLG
  with maxLum above refWhite) is compressed with the ITU-R BT.2390 EETF
  so that maxLum lands on SDR white. Below the knee of the curve nothing
  changes.
- **Gamut mapping**: a colour that sRGB cannot show is moved towards
  the grey of its own luminance until it fits. That keeps its hue (P3's
  pure red becomes the most saturated sRGB red of the same hue, not
  sRGB's pure red with its channels clipped).

The CPU and the GPU (glamor) do the same conversion, and the tests
check both against the same reference values.

The window's own contents are not changed: `GetImage` on the window
returns the pixels the application drew. `GetImage` on the root window
returns what the screen shows.

Composition (blending of translucent windows) still happens in sRGB, as
before; [color.md](color.md) describes composition in linear light.

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

Returns the version the server implements, 0.1.

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

A full-screen window with a description that needs converting is
composited instead of being shown directly: the screen cannot convert
it.
