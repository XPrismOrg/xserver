/*
 * SPDX-License-Identifier: MIT
 *
 * Colour spaces and conversions for the built-in compositor (doc/color.md).
 * Pure arithmetic, no server state, so that it can be unit tested and
 * later turned into shader or lookup-table form.
 *
 * Linear light is relative: 1.0 is the colour space's reference white
 * (SDR white). Absolute luminance (cd/m^2) appears only where a transfer
 * function defines it (PQ, HLG) and is converted with the space's
 * reference white luminance.
 */
#ifndef _XSERVER_COMPCOLOR_H_
#define _XSERVER_COMPCOLOR_H_

#include <stdbool.h>

typedef struct {
    double x, y;                /* CIE 1931 chromaticity */
} XpColorXY;

typedef struct {
    XpColorXY r, g, b, white;
} XpColorPrimaries;

typedef enum {
    XpColorTransferSRGB,        /* IEC 61966-2-1 piecewise curve */
    XpColorTransferGamma22,     /* pure power 2.2 */
    XpColorTransferLinear,
    XpColorTransferPQ,          /* SMPTE ST 2084, absolute up to 10000 cd/m^2 */
    XpColorTransferHLG,         /* ITU-R BT.2100 hybrid log-gamma */
} XpColorTransfer;

typedef struct {
    XpColorPrimaries primaries;
    XpColorTransfer transfer;
    /* luminance of reference white, cd/m^2: 80 for sRGB displays as
       specified, 203 for HDR content (ITU-R BT.2408) */
    double refWhite;
    /* the range the content uses or the display shows, cd/m^2 */
    double minLum, maxLum;
} XpColorSpace;

typedef double XpColorMatrix[3][3];

/* named primaries */
extern const XpColorPrimaries xpColorPrimariesSRGB;     /* = BT.709 */
extern const XpColorPrimaries xpColorPrimariesDisplayP3;
extern const XpColorPrimaries xpColorPrimariesBT2020;

/* ready-made spaces */
extern const XpColorSpace xpColorSpaceSRGB;
extern const XpColorSpace xpColorSpaceDisplayP3;
extern const XpColorSpace xpColorSpaceBT2100PQ;
extern const XpColorSpace xpColorSpaceBT2100HLG;

/* Linear RGB in primaries p to CIE XYZ (Y = 1 for white). FALSE if the
   primaries are degenerate. */
bool xpColorRGBToXYZ(const XpColorPrimaries *p, XpColorMatrix m);

/* Linear RGB in src to linear RGB in dst, adapting the white point with
   the Bradford transform when they differ. */
bool xpColorConversion(const XpColorPrimaries *src, const XpColorPrimaries *dst,
                       XpColorMatrix m);

void xpColorMatrixMultiply(const XpColorMatrix a, const XpColorMatrix b, XpColorMatrix out);
bool xpColorMatrixInvert(const XpColorMatrix m, XpColorMatrix out);
void xpColorMatrixApply(const XpColorMatrix m, const double in[3], double out[3]);

/*
 * Transfer functions. Decoding takes a signal in [0, 1] to linear light
 * relative to the space's reference white; encoding is the inverse and
 * clamps to [0, 1]. For PQ and HLG the space's refWhite converts between
 * absolute and relative luminance. HLG includes the BT.2100 OOTF for a
 * display of maxLum (system gamma per BT.2100, black level 0).
 */
double xpColorDecode(const XpColorSpace *cs, double v);
double xpColorEncode(const XpColorSpace *cs, double l);

/* Whole pixels: required for HLG, whose OOTF depends on luminance (the
   scalar functions above treat the value as grey); same as the scalar
   functions per channel otherwise. */
void xpColorDecodeRGB(const XpColorSpace *cs, const double in[3], double out[3]);
void xpColorEncodeRGB(const XpColorSpace *cs, const double in[3], double out[3]);

/* PQ alone, in cd/m^2 */
double xpColorPQToNits(double v);
double xpColorNitsToPQ(double nits);

/*
 * Tone mapping (ITU-R BT.2390 EETF): luminance from a source range to a
 * smaller display range, in cd/m^2. Values within the lower part of the
 * display range are left alone; above the knee they roll off smoothly
 * so that srcMax lands on dstMax. Identity when dstMax >= srcMax.
 */
double xpColorToneMap(double nits, double srcMax, double dstMax);

/* The luminance below which xpColorToneMap changes nothing, cd/m^2. */
double xpColorToneMapKnee(double srcMax, double dstMax);

#endif /* _XSERVER_COMPCOLOR_H_ */
