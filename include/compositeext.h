/*
 * Copyright © 2009 NVIDIA Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#ifndef _COMPOSITEEXT_H_
#define _COMPOSITEEXT_H_

#include "misc.h"
#include "scrnintstr.h"

extern _X_EXPORT Bool CompositeRegisterAlternateVisuals(ScreenPtr pScreen,
                                                        VisualID * vids,
                                                        int nVisuals);

extern _X_EXPORT Bool compIsAlternateVisual(ScreenPtr pScreen, XID visual);

extern _X_EXPORT RESTYPE CompositeClientWindowType;

/*
 * XPrism: colour conversion of window contents for the built-in
 * compositor (XPRISM-COLOR, doc/color.md), done by the acceleration
 * architecture when it can. Pixels of src within region (pixmap
 * coordinates; src and dst have the same size and, unless one of them is
 * a floating point pixmap below, the same depth) are converted into dst,
 * per pixel:
 *
 *   un-premultiply, decode transfer (to linear light, 1.0 = refWhite),
 *   multiply by matrix (to the output's primaries; 1.0 is now the
 *   output's reference white), tone map luminance above toneKnee
 *   (BT.2390 in the PQ domain, on the content's cd/m^2 = value * refWhite:
 *   toneSrcPQ = PQ(content peak), toneMaxL = PQ(output peak) /
 *   toneSrcPQ, toneKS = its knee), move towards luminance (outLuma) until
 *   every channel is within [0, outPeak], encode outTransfer (PQ:
 *   of value * outRefWhite), premultiply.
 *
 * Returns FALSE, having converted nothing, when it cannot; the server
 * then converts on the CPU.
 */
typedef struct {
    int transfer;               /* 0 sRGB, 1 gamma 2.2, 2 linear, 3 PQ, 4 HLG */
    float matrix[3][3];         /* linear source RGB to linear output RGB */
    float refWhite;             /* cd/m^2 */
    float maxLum;               /* cd/m^2; HLG: the display peak of the OOTF */
    float hlgGamma;             /* HLG system gamma */
    float luma[3];              /* luminance weights of the source primaries */
    float outLuma[3];           /* and of the output's */
    Bool tone;
    float toneKnee;             /* relative to refWhite */
    float toneSrcPQ, toneMaxL, toneKS;
    int outTransfer;            /* sRGB, gamma 2.2, linear, PQ or HLG */
    float outRefWhite;          /* cd/m^2 */
    float outPeak;              /* relative to outRefWhite; 1 for SDR */
    float outMaxLum;            /* the output's peak, cd/m^2 (HLG) */
    float outHlgGamma;          /* its HLG system gamma */
    /* composition in linear light: src is a floating point pixmap, its
       channels not limited to 1 (then transfer is linear); dst is one,
       given linear values up to outPeak (then outTransfer is linear) */
    Bool srcFloat, dstFloat;
} CompositeColorConvertRec;

typedef Bool (*CompositeColorConvertProcPtr) (PixmapPtr src, PixmapPtr dst,
                                              RegionPtr region,
                                              const CompositeColorConvertRec *conv);

/* Set (or with NULL, remove) the screen's conversion function. */
extern _X_EXPORT void CompositeSetColorConvert(ScreenPtr pScreen,
                                               CompositeColorConvertProcPtr proc);

/*
 * XPrism: a pixmap for composition in linear light, by the acceleration
 * architecture: depth 32, used with Render's a8r8g8b8 format, but its
 * channels are 16-bit floating point and keep values above 1 (HDR). NULL
 * if it cannot make one. Clients never see these pixmaps.
 */
typedef PixmapPtr (*CompositeCreateFloatPixmapProcPtr) (ScreenPtr pScreen,
                                                        int width, int height);

extern _X_EXPORT void CompositeSetFloatPixmap(ScreenPtr pScreen,
                                              CompositeCreateFloatPixmapProcPtr proc);

/*
 * XPrism: the colour space the screen's output shows (doc/color.md).
 * Without one, sRGB. The built-in compositor composes for it. Output
 * transfer functions: sRGB, gamma 2.2, linear or PQ (HDR); for PQ,
 * maxLum is the display's peak and refWhite where SDR white is shown.
 */
typedef struct {
    float xy[8];                /* CIE xy of red, green, blue and white */
    int transfer;               /* as in CompositeColorConvertRec */
    float refWhite, minLum, maxLum;     /* cd/m^2 */
} CompositeColorSpaceRec;

extern _X_EXPORT void CompositeSetOutputColorSpace(ScreenPtr pScreen,
                                                   const CompositeColorSpaceRec *space);

/*
 * XPrism: for what is drawn over the composed screen, such as the
 * software cursor. CompositeOutputPixel: an sRGB pixel of the root
 * window's format as the output shows it (the pixel itself on sRGB
 * outputs). CompositeOutputPixmap: a copy of a depth-32 a8r8g8b8 sRGB
 * pixmap as the output shows it, or NULL on sRGB outputs.
 */
extern _X_EXPORT CARD32 CompositeOutputPixel(ScreenPtr pScreen, CARD32 pixel);
extern _X_EXPORT PixmapPtr CompositeOutputPixmap(ScreenPtr pScreen, PixmapPtr argb);

#endif                          /* _COMPOSITEEXT_H_ */
