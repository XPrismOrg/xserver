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
 * coordinates; src and dst have the same size and depth) are converted
 * into dst, per pixel:
 *
 *   un-premultiply, decode transfer (to linear light, 1.0 = refWhite),
 *   multiply by matrix, tone map luminance above toneKnee (BT.2390, in
 *   the PQ domain: toneSrcPQ = PQ(maxLum), toneMaxL = PQ(refWhite) /
 *   toneSrcPQ, toneKS = its knee), move towards luminance (outLuma) until
 *   every channel is within [0, 1], encode sRGB, premultiply.
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
} CompositeColorConvertRec;

typedef Bool (*CompositeColorConvertProcPtr) (PixmapPtr src, PixmapPtr dst,
                                              RegionPtr region,
                                              const CompositeColorConvertRec *conv);

/* Set (or with NULL, remove) the screen's conversion function. */
extern _X_EXPORT void CompositeSetColorConvert(ScreenPtr pScreen,
                                               CompositeColorConvertProcPtr proc);

#endif                          /* _COMPOSITEEXT_H_ */
