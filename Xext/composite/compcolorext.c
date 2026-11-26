/* SPDX-License-Identifier: MIT */
/*
 * XPRISM-COLOR (Phase IV, doc/color.md): applications describe the colour
 * space of their windows' pixels, and the scene compositor shows them
 * converted for the output.
 *
 * Each screen's output shows a colour space, sRGB unless the DDX sets
 * another (CompositeSetOutputColorSpace). A top-level window whose
 * contents are, in whole or in part, in a space other than the output's
 * gets a second pixmap holding its contents converted for the output,
 * kept up to date where the window is drawn on; the compositor shows that
 * one instead. Each pixel is in the colour space of the innermost window
 * that covers it and has a description (child windows inherit their
 * parent's), sRGB without one.
 *
 * Conversion: decode the transfer function to linear light, convert the
 * primaries (3x3 matrix), tone map content brighter than the output can
 * show, bring colours outside the output's gamut into it by desaturating
 * towards their luminance (which keeps the hue), encode the output's
 * transfer function. The acceleration architecture does it when it can
 * (glamor), the CPU otherwise, through GetImage and PutImage.
 *
 * Composition in linear light (compColorLinear): on outputs other than
 * sRGB, where the acceleration architecture also makes floating point
 * pixmaps, every window is converted to linear light in the output's
 * primaries instead (1.0 = its reference white, up to its peak), the
 * compositor blends in a floating point buffer, and that is encoded for
 * the output once per frame (compColorEncodeLinear).
 *
 * On sRGB outputs, windows without a description cost nothing, and as
 * long as no window has one the compositor does exactly what it did
 * before.
 */

#include <dix-config.h>

#include <math.h>

#include "dix/dix_priv.h"
#include "dix/request_priv.h"
#include "miext/extinit_priv.h"

#include "randrstr.h"

#include "compint.h"
#include "compcolor.h"
#include "xprismcolorproto.h"
#ifdef XINERAMA
#include "Xext/panoramiX/panoramiXsrv.h"
#include "Xext/panoramiX/panoramiX_priv.h"
#endif

typedef struct {
    /* the description, if set */
    bool set;
    CARD8 primaries;            /* XpColorPrimaries*, as sent */
    XpColorSpace cs;
    /* top-level windows: their contents converted for the output */
    PixmapPtr content;          /* converted from (referenced) */
    PixmapPtr converted;
    RegionRec dirty;            /* still to convert, pixmap coordinates */
    unsigned serial;            /* colorSerial when converted */
} ColorWinRec, *ColorWinPtr;

static DevPrivateKeyRec colorWinKeyRec;
static CompositeColorConvertProcPtr colorAccel[MAXSCREENS];
static CompositeCreateFloatPixmapProcPtr colorFloat[MAXSCREENS];
static unsigned colorTagged;    /* windows with a non-sRGB description */
static unsigned colorSerial = 1;        /* changes with any description */

static ColorWinPtr
colorWin(WindowPtr pWin)
{
    return dixLookupPrivate(&pWin->devPrivates, &colorWinKeyRec);
}

/* With Xinerama the copy of a window on screen 0 holds its description,
   as it holds its properties (compscene.c: sceneSettingsWindow). */
static ColorWinPtr
colorSettings(WindowPtr pWin)
{
#ifdef XINERAMA
    int scr = pWin->drawable.pScreen->myNum;
    WindowPtr pMaster;

    if (scr != 0 && PanoramiXIsEnabled()) {
        PanoramiXRes *res = PanoramiXFindIDByScrnum(XRT_WINDOW,
                                                    pWin->drawable.id, scr);
        if (res && dixLookupWindow(&pMaster, res->info[0].id, serverClient,
                                   DixReadAccess) == Success)
            return colorWin(pMaster);
    }
#endif
    return colorWin(pWin);
}

static bool
colorIsSRGB(const XpColorSpace *cs)
{
    return cs->transfer == XpColorTransferSRGB &&
        !memcmp(&cs->primaries, &xpColorPrimariesSRGB, sizeof(XpColorPrimaries));
}

/* ---- outputs ------------------------------------------------------------ */

/* An output whose signal goes beyond SDR white (PQ, HLG) */
static bool
colorIsHDR(const XpColorSpace *cs)
{
    return cs->transfer == XpColorTransferPQ || cs->transfer == XpColorTransferHLG;
}

static XpColorSpace outputSpace[MAXSCREENS];
static bool outputSet[MAXSCREENS];         /* else sRGB */

static const XpColorSpace *
colorOutput(ScreenPtr pScreen)
{
    return outputSet[pScreen->myNum] ? &outputSpace[pScreen->myNum] : &xpColorSpaceSRGB;
}

void
CompositeSetOutputColorSpace(ScreenPtr pScreen, const CompositeColorSpaceRec *space)
{
    int s = pScreen->myNum;
    XpColorSpace cs = { 0 };
    XpColorMatrix unused;

    if (space) {
        XpColorXY *xy[4] = { &cs.primaries.r, &cs.primaries.g, &cs.primaries.b,
                             &cs.primaries.white };
        /* in millionths, as the protocol has them: named primaries then
           compare equal */
        for (int i = 0; i < 4; i++)
            *xy[i] = (XpColorXY) { lrint(space->xy[2 * i] * 1e6) / 1e6,
                                   lrint(space->xy[2 * i + 1] * 1e6) / 1e6 };
        cs.transfer = (XpColorTransfer) space->transfer;
        cs.refWhite = space->refWhite;
        cs.minLum = space->minLum;
        cs.maxLum = space->maxLum;
    }
    if (!space || colorIsSRGB(&cs)) {
        outputSet[s] = false;
    } else if (space->transfer < XpColorTransferSRGB ||
               space->transfer > XpColorTransferHLG ||
               !(cs.refWhite > 0) || cs.maxLum < cs.refWhite ||
               !xpColorRGBToXYZ(&cs.primaries, unused)) {
        LogMessageVerb(X_WARNING, 1, "compositor: screen %d: unsupported output "
                       "colour space, using sRGB\n", s);
        outputSet[s] = false;
    } else {
        outputSpace[s] = cs;
        outputSet[s] = true;
    }
    colorSerial++;
    if (!colorSerial)
        colorSerial = 1;
}

bool
compColorOutputIsSRGB(ScreenPtr pScreen)
{
    return !outputSet[pScreen->myNum];
}

/* cs as content in it is decoded for out: HLG is scene-referred, so on
   an HLG output its OOTF is the display's, whatever peak the content
   was graded on, and decoding and encoding are each other's inverse. */
static XpColorSpace
colorSource(const XpColorSpace *cs, const XpColorSpace *out)
{
    XpColorSpace src = *cs;

    if (cs->transfer == XpColorTransferHLG && out->transfer == XpColorTransferHLG) {
        src.refWhite = out->refWhite;
        src.maxLum = out->maxLum;
    }
    return src;
}

/* Whether content in space cs is shown as it is on output out. */
static bool
colorShownAsIs(const XpColorSpace *cs, const XpColorSpace *out)
{
    if (cs->transfer != out->transfer ||
        memcmp(&cs->primaries, &out->primaries, sizeof(XpColorPrimaries)))
        return false;
    /* PQ is absolute: the same reference white, no tone mapping */
    if (cs->transfer == XpColorTransferPQ)
        return cs->refWhite == out->refWhite && cs->maxLum <= out->maxLum;
    return true;
}

/* The space of pWin's own pixels, if it has a description, else NULL */
static const XpColorSpace *
colorOwn(WindowPtr pWin)
{
    ColorWinPtr cw = colorSettings(pWin);
    return cw->set ? &cw->cs : NULL;
}

/* Next window of the subtree of top in preorder, or NULL. */
static WindowPtr
colorNext(WindowPtr top, WindowPtr pWin)
{
    if (pWin->firstChild)
        return pWin->firstChild;
    while (pWin != top) {
        if (pWin->nextSib)
            return pWin->nextSib;
        pWin = pWin->parent;
    }
    return NULL;
}

/* Composition in linear light: on outputs other than sRGB, when the
   acceleration architecture converts and has floating point pixmaps. */
bool
compColorLinear(ScreenPtr pScreen)
{
    int s = pScreen->myNum;
    return outputSet[s] && colorAccel[s] && colorFloat[s];
}

static bool
colorNeedsConversion(WindowPtr top)
{
    const XpColorSpace *out = colorOutput(top->drawable.pScreen);

    if (!colorTagged && out == &xpColorSpaceSRGB)
        return false;
    const XpColorSpace *own = colorOwn(top);
    if (!colorShownAsIs(own ? own : &xpColorSpaceSRGB, out))
        return true;
    for (WindowPtr pWin = colorNext(top, top); pWin; pWin = colorNext(top, pWin)) {
        own = colorOwn(pWin);
        if (own && !colorShownAsIs(own, out))
            return true;
    }
    return false;
}

static void
colorRelease(ColorWinPtr cw)
{
    if (cw->converted)
        dixDestroyPixmap(cw->converted, 0);
    if (cw->content)
        dixDestroyPixmap(cw->content, 0);
    cw->converted = cw->content = NULL;
    RegionUninit(&cw->dirty);
    RegionNull(&cw->dirty);
}

/* ---- pixel conversion -------------------------------------------------- */

#define DECODE_LUT 1024
#define ENCODE_LUT 4096
#define TONE_LUT 1024
#define COLOR_CACHE 4096         /* a power of two */

/*
 * From a space cs to an output out. Linear light is relative to the
 * reference white of each; reference white is shown as the output's
 * reference white. Content brighter than the output can show is tone
 * mapped (BT.2390) to its peak; colours outside its gamut are moved
 * towards their luminance until they fit, within [0, peak].
 */
typedef struct {
    const XpColorSpace *cs, *out;
    XpColorMatrix m;            /* linear, cs's primaries to out's */
    double outLuma[3];
    double outPeak;             /* the output's peak, relative */
    bool hlg;
    float decode[DECODE_LUT + 1];
    bool tone;
    double knee, peak;          /* relative to the output's reference white */
    float toneLut[TONE_LUT + 1];
    bool encodeSRGB;            /* else xpColorEncode */
    bool encodeHLG;             /* whole pixels: the OOTF needs luminance */
    /* converted pixels: UI contents repeat the same few colours */
    CARD32 cacheKey[COLOR_CACHE], cacheVal[COLOR_CACHE];
    bool cacheSet[COLOR_CACHE];
    XpColorSpace src;           /* cs as it is decoded for out */
} ColorConv;

static float encodeLut[ENCODE_LUT + 1];

static double
lutGet(const float *lut, int size, double v)
{
    if (v <= 0)
        return lut[0];
    if (v >= 1)
        return lut[size];
    double f = v * size;
    int i = (int) f;
    return lut[i] + (lut[i + 1] - lut[i]) * (f - i);
}

/* Tone mapping from cs to out: content peak and output peak, in the
   content's cd/m^2 (the output's reference white is the content's).
   FALSE if none is needed. */
static bool
colorTonePeaks(const XpColorSpace *cs, const XpColorSpace *out,
               double *srcMax, double *dstMax)
{
    if (cs->transfer != XpColorTransferPQ && cs->transfer != XpColorTransferHLG)
        return false;
    *srcMax = cs->maxLum;
    *dstMax = out->maxLum / out->refWhite * cs->refWhite;
    return *srcMax > *dstMax;
}

static bool
colorConvInit(ColorConv *cv, const XpColorSpace *cs, const XpColorSpace *out)
{
    XpColorMatrix toXYZ;
    double srcMax, dstMax;

    memset(cv, 0, sizeof(*cv));
    cv->src = colorSource(cs, out);
    cs = cv->cs = &cv->src;
    cv->out = out;
    if (!xpColorConversion(&cs->primaries, &out->primaries, cv->m) ||
        !xpColorRGBToXYZ(&out->primaries, toXYZ))
        return false;
    for (int i = 0; i < 3; i++)
        cv->outLuma[i] = toXYZ[1][i];
    cv->outPeak = colorIsHDR(out) ? out->maxLum / out->refWhite : 1;
    cv->encodeHLG = out->transfer == XpColorTransferHLG;
    cv->hlg = cs->transfer == XpColorTransferHLG;
    for (int i = 0; i <= DECODE_LUT; i++)
        cv->decode[i] = (float) xpColorDecode(cs, (double) i / DECODE_LUT);

    if (colorTonePeaks(cs, out, &srcMax, &dstMax)) {
        cv->tone = true;
        cv->knee = xpColorToneMapKnee(srcMax, dstMax) / cs->refWhite;
        cv->peak = srcMax / cs->refWhite;
        for (int i = 0; i <= TONE_LUT; i++) {
            double y = cv->knee + (cv->peak - cv->knee) * i / TONE_LUT;
            cv->toneLut[i] = (float) (xpColorToneMap(y * cs->refWhite, srcMax, dstMax) /
                                      cs->refWhite);
        }
    }
    cv->encodeSRGB = out->transfer == XpColorTransferSRGB;
    if (encodeLut[ENCODE_LUT] == 0)
        for (int i = 0; i <= ENCODE_LUT; i++)
            encodeLut[i] = (float) xpColorEncode(&xpColorSpaceSRGB,
                                                 (double) i / ENCODE_LUT);
    return true;
}

static void
colorConvertPixel(const ColorConv *cv, const double in[3], double out[3])
{
    double lin[3];

    if (cv->hlg) {
        xpColorDecodeRGB(cv->cs, in, lin);
    } else {
        for (int c = 0; c < 3; c++)
            lin[c] = lutGet(cv->decode, DECODE_LUT, in[c]);
    }
    xpColorMatrixApply(cv->m, lin, out);

    double y = cv->outLuma[0] * out[0] + cv->outLuma[1] * out[1] + cv->outLuma[2] * out[2];
    if (cv->tone && y > cv->knee) {
        double t = (y - cv->knee) / (cv->peak - cv->knee);
        double mapped = lutGet(cv->toneLut, TONE_LUT, t);
        for (int c = 0; c < 3; c++)
            out[c] *= mapped / y;
        y = mapped;
    }

    /* gamut: move towards the luminance until every channel fits */
    double p = cv->outPeak;
    y = y < 0 ? 0 : y > p ? p : y;
    double s = 1;
    for (int c = 0; c < 3; c++) {
        if (out[c] > p && out[c] - y > 0)
            s = fmin(s, (p - y) / (out[c] - y));
        else if (out[c] < 0)
            s = fmin(s, y / (y - out[c]));
    }
    if (cv->encodeHLG) {
        double v[3];
        for (int c = 0; c < 3; c++)
            v[c] = y + s * (out[c] - y);
        xpColorEncodeRGB(cv->out, v, out);
        return;
    }
    for (int c = 0; c < 3; c++) {
        double v = y + s * (out[c] - y);
        out[c] = cv->encodeSRGB ? lutGet(encodeLut, ENCODE_LUT, v) :
            xpColorEncode(cv->out, v);
    }
}

typedef struct {
    int shift[4];               /* r, g, b, a */
    CARD32 mask[4];
} ColorLayout;

static bool
colorLayout(PictFormatPtr format, ColorLayout *l)
{
    if (!format || format->type != PictTypeDirect)
        return false;
    const DirectFormatRec *d = &format->direct;
    *l = (ColorLayout) {
        .shift = { d->red, d->green, d->blue, d->alpha },
        .mask = { d->redMask, d->greenMask, d->blueMask, d->alphaMask },
    };
    return l->mask[0] && l->mask[1] && l->mask[2];
}

/* Pixels in layout in, converted, to layout out (alpha: in's) */
static void
colorConvertSpan(ColorConv *cv, const ColorLayout *in_l, const ColorLayout *out_l,
                 CARD32 *px, int n)
{
    for (int i = 0; i < n; i++) {
        CARD32 p = px[i];
        double a = 1, in[3], out[3];
        unsigned h = (p * 2654435761u) >> 20 & (COLOR_CACHE - 1);

        if (cv->cacheSet[h] && cv->cacheKey[h] == p) {
            px[i] = cv->cacheVal[h];
            continue;
        }

        if (in_l->mask[3]) {
            CARD32 av = (p >> in_l->shift[3]) & in_l->mask[3];
            if (!av) {
                px[i] = 0;      /* premultiplied: all zero */
                continue;
            }
            a = (double) av / in_l->mask[3];
        }
        for (int c = 0; c < 3; c++) {
            double v = (double) ((p >> in_l->shift[c]) & in_l->mask[c]) / in_l->mask[c];
            in[c] = fmin(v / a, 1);
        }
        colorConvertPixel(cv, in, out);
        CARD32 q = 0;
        if (out_l->mask[3])
            q = (CARD32) lrint(a * out_l->mask[3]) << out_l->shift[3];
        for (int c = 0; c < 3; c++)
            q |= (CARD32) lrint(out[c] * a * out_l->mask[c]) << out_l->shift[c];
        px[i] = q;
        cv->cacheSet[h] = true;
        cv->cacheKey[h] = p;
        cv->cacheVal[h] = q;
    }
}

void
CompositeSetColorConvert(ScreenPtr pScreen, CompositeColorConvertProcPtr proc)
{
    colorAccel[pScreen->myNum] = proc;
}

/* What the acceleration architecture needs to convert from cs to out. */
static bool
colorAccelParams(const XpColorSpace *cs, const XpColorSpace *out,
                 CompositeColorConvertRec *p)
{
    XpColorMatrix m, toXYZ, outXYZ;
    double srcMax, dstMax;
    XpColorSpace src = colorSource(cs, out);

    cs = &src;
    memset(p, 0, sizeof(*p));
    if (!xpColorConversion(&cs->primaries, &out->primaries, m) ||
        !xpColorRGBToXYZ(&cs->primaries, toXYZ) ||
        !xpColorRGBToXYZ(&out->primaries, outXYZ))
        return false;
    p->transfer = cs->transfer;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++)
            p->matrix[i][j] = (float) m[i][j];
        p->luma[i] = (float) toXYZ[1][i];
        p->outLuma[i] = (float) outXYZ[1][i];
    }
    p->refWhite = (float) cs->refWhite;
    p->maxLum = (float) cs->maxLum;
    p->hlgGamma = (float) (1.2 + 0.42 * log10(cs->maxLum / 1000));
    p->outTransfer = out->transfer;
    p->outRefWhite = (float) out->refWhite;
    p->outPeak = (float) (colorIsHDR(out) ? out->maxLum / out->refWhite : 1);
    p->outMaxLum = (float) out->maxLum;
    p->outHlgGamma = (float) (1.2 + 0.42 * log10(out->maxLum / 1000));
    if (colorTonePeaks(cs, out, &srcMax, &dstMax)) {
        double srcPQ = xpColorNitsToPQ(srcMax);
        double maxL = xpColorNitsToPQ(dstMax) / srcPQ;
        p->tone = TRUE;
        p->toneKnee = (float) (xpColorToneMapKnee(srcMax, dstMax) / cs->refWhite);
        p->toneSrcPQ = (float) srcPQ;
        p->toneMaxL = (float) maxL;
        p->toneKS = (float) fmax(1.5 * maxL - 0.5, 0);
    }
    return true;
}

/* The output's linear light: its primaries, 1.0 = its reference white */
static XpColorSpace
colorLinearOf(const XpColorSpace *out)
{
    XpColorSpace lin = *out;
    lin.transfer = XpColorTransferLinear;
    return lin;
}

enum {
    COLOR_ENCODED,              /* from cs to out's encoding */
    COLOR_TO_LINEAR,            /* from cs to out's linear light, dst float */
    COLOR_FROM_LINEAR,          /* src float, out's linear light, to out */
};

/* Convert pRegion (pixmap coordinates) of src, in layout srcFormat, into
   dst, in dstFormat. cs NULL: shown as it is, copied. Without the
   acceleration architecture, floating point pixmaps are read and written
   as 8 bits per channel, within [0, 1]. */
static void
colorConvertRegion(PixmapPtr src, PictFormatPtr srcFormat, PixmapPtr dst,
                   PictFormatPtr dstFormat, const XpColorSpace *cs,
                   const XpColorSpace *out, RegionPtr pRegion, int mode)
{
    XpColorSpace lin = colorLinearOf(out);

    ScreenPtr pScreen = src->drawable.pScreen;
    int n = RegionNumRects(pRegion);
    BoxPtr b = RegionRects(pRegion);
    ColorConv *cv = NULL;
    ColorLayout inLayout, outLayout;

    if (!n)
        return;
    if (mode == COLOR_FROM_LINEAR)
        cs = &lin;
    if (!cs && mode == COLOR_ENCODED && src->drawable.depth == dst->drawable.depth) {
        GCPtr pGC = GetScratchGC(dst->drawable.depth, pScreen);
        if (!pGC)
            return;
        ValidateGC(&dst->drawable, pGC);
        for (int k = 0; k < n; k++)
            (*pGC->ops->CopyArea) (&src->drawable, &dst->drawable, pGC,
                                   b[k].x1, b[k].y1, b[k].x2 - b[k].x1,
                                   b[k].y2 - b[k].y1, b[k].x1, b[k].y1);
        FreeScratchGC(pGC);
        return;
    }
    if (!cs)
        cs = out;               /* to a deeper pixmap, unchanged */

    CompositeColorConvertProcPtr accel = colorAccel[pScreen->myNum];
    CompositeColorConvertRec params;
    if (accel && colorAccelParams(cs, out, &params)) {
        if (mode == COLOR_TO_LINEAR) {
            params.outTransfer = XpColorTransferLinear;
            params.dstFloat = TRUE;
        }
        params.srcFloat = mode == COLOR_FROM_LINEAR;
        /* Content in the output's own space is shown as it is, as when
           the window is shown directly: not brought within the peak. So
           the encoding of the blended light does not clamp to it either
           (the rest is within it). */
        if (mode == COLOR_FROM_LINEAR ||
            (mode == COLOR_TO_LINEAR && colorShownAsIs(cs, out)))
            /* HLG's signal ends at the display's peak */
            params.outPeak = (float) (out->transfer == XpColorTransferPQ ?
                                      XpColorMaxLuminance / out->refWhite :
                                      out->transfer == XpColorTransferHLG ?
                                      out->maxLum / out->refWhite : 1);
        if ((*accel) (src, dst, pRegion, &params))
            return;
    }

    /* on the CPU, linear light is within [0, 1] */
    cv = calloc(1, sizeof(*cv));
    if (!cv || !colorLayout(srcFormat, &inLayout) ||
        !colorLayout(dstFormat, &outLayout) ||
        !colorConvInit(cv, cs, mode == COLOR_TO_LINEAR ? &lin : out)) {
        free(cv);
        return;
    }
    GCPtr pGC = GetScratchGC(dst->drawable.depth, pScreen);
    if (!pGC) {
        free(cv);
        return;
    }
    ValidateGC(&dst->drawable, pGC);
    for (int k = 0; k < n; k++) {
        int w = b[k].x2 - b[k].x1, h = b[k].y2 - b[k].y1;
        /* in strips, to bound the buffer */
        int rows = max(1, min(h, (1 << 20) / max(w, 1)));
        CARD32 *buf = calloc((size_t) w * rows, sizeof(CARD32));
        if (!buf)
            continue;
        for (int y = b[k].y1; y < b[k].y2; y += rows) {
            int sh = min(rows, b[k].y2 - y);
            (*pScreen->GetImage) (&src->drawable, b[k].x1, y, w, sh, ZPixmap,
                                  ~0UL, (char *) buf);
            colorConvertSpan(cv, &inLayout, &outLayout, buf, w * sh);
            (*pGC->ops->PutImage) (&dst->drawable, pGC, dst->drawable.depth,
                                   b[k].x1, y, w, sh, 0, ZPixmap, (char *) buf);
        }
        free(buf);
    }
    free(cv);
    FreeScratchGC(pGC);
}

/* ---- the compositor's side --------------------------------------------- */

unsigned
compColorState(WindowPtr top)
{
    /* composing in linear light, every window is converted */
    return compColorLinear(top->drawable.pScreen) || colorNeedsConversion(top) ?
        colorSerial : 0;
}

/* Whether top-level window top's pixels are what the output shows. */
bool
compColorShownAsIs(WindowPtr top)
{
    return !colorNeedsConversion(top);
}

void
compColorDamage(WindowPtr top, RegionPtr pRegion)
{
    ColorWinPtr cw = colorWin(top);

    if (!cw->converted)
        return;
    RegionTranslate(pRegion, -cw->content->screen_x, -cw->content->screen_y);
    RegionUnion(&cw->dirty, &cw->dirty, pRegion);
    RegionTranslate(pRegion, cw->content->screen_x, cw->content->screen_y);
}

PixmapPtr
compColorSource(WindowPtr top, PixmapPtr content)
{
    ColorWinPtr cw = colorWin(top);
    ScreenPtr pScreen = top->drawable.pScreen;
    bool linear = compColorLinear(pScreen);

    if (!(linear || colorNeedsConversion(top)) ||
        content->drawable.bitsPerPixel != 32) {
        colorRelease(cw);
        return content;
    }

    int depth = linear ? 32 : content->drawable.depth;
    if (cw->content != content ||
        cw->converted->drawable.width != content->drawable.width ||
        cw->converted->drawable.height != content->drawable.height ||
        cw->converted->drawable.depth != depth) {
        colorRelease(cw);
        cw->converted = linear ?
            compColorFloatPixmap(pScreen, content->drawable.width,
                                 content->drawable.height) :
            (*pScreen->CreatePixmap) (pScreen, content->drawable.width,
                                      content->drawable.height, depth, 0);
        if (!cw->converted)
            return content;
        cw->content = content;
        content->refcnt++;
        cw->serial = 0;
    }
    if (cw->serial != colorSerial) {
        BoxRec all = { 0, 0, content->drawable.width, content->drawable.height };
        RegionReset(&cw->dirty, &all);
        cw->serial = colorSerial;
    }
    if (!RegionNotEmpty(&cw->dirty))
        return cw->converted;

    /* the top-level window's space, then each described window inside it
       over its own part: inner windows come later in preorder */
    const XpColorSpace *out = colorOutput(pScreen);
    PictFormatPtr srcFormat = PictureWindowFormat(top);
    PictFormatPtr dstFormat = linear ? compColorFloatFormat(pScreen) : srcFormat;
    int mode = linear ? COLOR_TO_LINEAR : COLOR_ENCODED;
    const XpColorSpace *own = colorOwn(top);
    if (!own)
        own = &xpColorSpaceSRGB;
    colorConvertRegion(content, srcFormat, cw->converted, dstFormat,
                       !linear && colorShownAsIs(own, out) ? NULL : own, out,
                       &cw->dirty, mode);
    RegionRec part;
    RegionNull(&part);
    for (WindowPtr pWin = colorNext(top, top); pWin; pWin = colorNext(top, pWin)) {
        own = colorOwn(pWin);
        if (!own)
            continue;
        RegionCopy(&part, &pWin->borderClip);
        RegionTranslate(&part, -content->screen_x, -content->screen_y);
        RegionIntersect(&part, &part, &cw->dirty);
        colorConvertRegion(content, srcFormat, cw->converted, dstFormat,
                           !linear && colorShownAsIs(own, out) ? NULL : own, out,
                           &part, mode);
    }
    RegionUninit(&part);
    RegionEmpty(&cw->dirty);
    return cw->converted;
}

unsigned
compColorSerial(void)
{
    return colorSerial;
}

/* A copy of src (sRGB, in format) as the output shows it, or, linear, a
   floating point pixmap in its linear light; NULL if it cannot. */
static PixmapPtr
colorCopy(ScreenPtr pScreen, PixmapPtr src, PictFormatPtr format, bool linear)
{
    const XpColorSpace *out = colorOutput(pScreen);
    PixmapPtr dst;

    if (src->drawable.bitsPerPixel != 32)
        return NULL;
    dst = linear ?
        compColorFloatPixmap(pScreen, src->drawable.width, src->drawable.height) :
        (*pScreen->CreatePixmap) (pScreen, src->drawable.width,
                                  src->drawable.height, src->drawable.depth, 0);
    if (!dst)
        return NULL;
    BoxRec all = { 0, 0, src->drawable.width, src->drawable.height };
    RegionRec r;
    RegionInit(&r, &all, 1);
    colorConvertRegion(src, format, dst, linear ? compColorFloatFormat(pScreen) : format,
                       !linear && colorShownAsIs(&xpColorSpaceSRGB, out) ?
                       NULL : &xpColorSpaceSRGB,
                       out, &r, linear ? COLOR_TO_LINEAR : COLOR_ENCODED);
    RegionUninit(&r);
    return dst;
}

/* A copy of src (sRGB, in format) as the output shows it, or NULL;
   composing in linear light, a floating point pixmap in linear light. */
PixmapPtr
compColorConvertedCopy(ScreenPtr pScreen, PixmapPtr src, PictFormatPtr format)
{
    return colorCopy(pScreen, src, format, compColorLinear(pScreen));
}

CARD32
CompositeOutputPixel(ScreenPtr pScreen, CARD32 pixel)
{
    if (!outputSet[pScreen->myNum] || !pScreen->root)
        return pixel;
    return compColorConvertPixel(pScreen, PictureWindowFormat(pScreen->root), pixel);
}

PixmapPtr
CompositeOutputPixmap(ScreenPtr pScreen, PixmapPtr argb)
{
    PictFormatPtr format = PictureMatchFormat(pScreen, 32, PICT_a8r8g8b8);

    if (!outputSet[pScreen->myNum] || !format || argb->drawable.depth != 32)
        return NULL;
    return colorCopy(pScreen, argb, format, false);
}

/* An sRGB colour (8-bit, for the root background and shadows) as the
   output shows it, in format's layout; for a1r8g8b8-style ARGB, a is
   passed through. */
CARD32
compColorConvertPixel(ScreenPtr pScreen, PictFormatPtr format, CARD32 pixel)
{
    const XpColorSpace *out = colorOutput(pScreen);
    ColorLayout l;
    ColorConv *cv;

    if (out == &xpColorSpaceSRGB || !colorLayout(format, &l))
        return pixel;
    cv = calloc(1, sizeof(*cv));
    if (!cv || !colorConvInit(cv, &xpColorSpaceSRGB, out)) {
        free(cv);
        return pixel;
    }
    ColorLayout opaque = l;
    opaque.mask[3] = 0;
    colorConvertSpan(cv, &opaque, &opaque, &pixel, 1);
    free(cv);
    return pixel | (l.mask[3] << l.shift[3]);
}

/* ---- composition in linear light ---------------------------------------- */

void
CompositeSetFloatPixmap(ScreenPtr pScreen, CompositeCreateFloatPixmapProcPtr proc)
{
    colorFloat[pScreen->myNum] = proc;
}

PixmapPtr
compColorFloatPixmap(ScreenPtr pScreen, int w, int h)
{
    CompositeCreateFloatPixmapProcPtr proc = colorFloat[pScreen->myNum];
    return proc ? (*proc) (pScreen, max(w, 1), max(h, 1)) : NULL;
}

/* Render's format for floating point pixmaps */
PictFormatPtr
compColorFloatFormat(ScreenPtr pScreen)
{
    return PictureMatchFormat(pScreen, 32, PICT_a8r8g8b8);
}

/* A solid picture of an sRGB colour (pixel, in format's layout) with
   alpha, in the output's linear light, premultiplied. */
PicturePtr
compColorLinearSolid(ScreenPtr pScreen, PictFormatPtr format, CARD32 pixel, double alpha)
{
    XpColorSpace lin = colorLinearOf(colorOutput(pScreen));
    ColorLayout l;
    double in[3], out[3];
    int error;

    if (!colorLayout(format, &l))
        return NULL;
    ColorConv *cv = calloc(1, sizeof(*cv));
    if (!cv || !colorConvInit(cv, &xpColorSpaceSRGB, &lin)) {
        free(cv);
        return NULL;
    }
    for (int c = 0; c < 3; c++)
        in[c] = (double) ((pixel >> l.shift[c]) & l.mask[c]) / l.mask[c];
    colorConvertPixel(cv, in, out);
    free(cv);
    xRenderColor color = {
        .red = (CARD16) lrint(fmin(fmax(out[0], 0), 1) * alpha * 0xffff),
        .green = (CARD16) lrint(fmin(fmax(out[1], 0), 1) * alpha * 0xffff),
        .blue = (CARD16) lrint(fmin(fmax(out[2], 0), 1) * alpha * 0xffff),
        .alpha = (CARD16) lrint(alpha * 0xffff),
    };
    return CreateSolidPicture(0, &color, &error);
}

/* Encode pRegion (screen coordinates) of pLin, a floating point pixmap
   in linear light whose origin is at ox, oy on the screen, for the
   output, into pDst, whose origin is at dx, dy. */
void
compColorEncodeLinear(ScreenPtr pScreen, PixmapPtr pLin, int ox, int oy,
                      PicturePtr pDst, int dx, int dy, RegionPtr pRegion)
{
    int w = pLin->drawable.width, h = pLin->drawable.height, error;
    PictFormatPtr format = pDst->pFormat;
    /* through a pixmap of the screen's depth: drawing into pDst through
       Render keeps damage and the DDX informed */
    PixmapPtr pEnc = (*pScreen->CreatePixmap) (pScreen, w, h, format->depth, 0);
    if (!pEnc)
        return;
    RegionTranslate(pRegion, -ox, -oy);
    colorConvertRegion(pLin, compColorFloatFormat(pScreen), pEnc, format, NULL,
                       colorOutput(pScreen), pRegion, COLOR_FROM_LINEAR);
    RegionTranslate(pRegion, ox, oy);
    PicturePtr pSrc = CreatePicture(0, &pEnc->drawable, format, 0, NULL,
                                    serverClient, &error);
    if (pSrc) {
        RegionTranslate(pRegion, -dx, -dy);
        SetPictureClipRegion(pDst, 0, 0, pRegion);
        RegionTranslate(pRegion, dx, dy);
        CompositePicture(PictOpSrc, pSrc, NULL, pDst, 0, 0, 0, 0,
                         ox - dx, oy - dy, w, h);
        FreePicture(pSrc, 0);
    }
    dixDestroyPixmap(pEnc, 0);
}

void
compColorWindowDestroy(WindowPtr pWin)
{
    if (!dixPrivateKeyRegistered(&colorWinKeyRec))
        return;
    ColorWinPtr cw = colorWin(pWin);
    if (cw->set) {
        if (!colorIsSRGB(&cw->cs))
            colorTagged--;
        colorSerial++;
    }
    colorRelease(cw);
    RegionUninit(&cw->dirty);
    memset(cw, 0, sizeof(*cw));
}

void
compColorCloseScreen(ScreenPtr pScreen)
{
    outputSet[pScreen->myNum] = false;
}

bool
compColorScreenInit(ScreenPtr pScreen)
{
    return dixRegisterPrivateKey(&colorWinKeyRec, PRIVATE_WINDOW,
                                 sizeof(ColorWinRec));
}

/* ---- protocol ---------------------------------------------------------- */

static const XpColorPrimaries *const namedPrimaries[] = {
    [XpColorPrimariesSRGB] = &xpColorPrimariesSRGB,
    [XpColorPrimariesDisplayP3] = &xpColorPrimariesDisplayP3,
    [XpColorPrimariesBT2020] = &xpColorPrimariesBT2020,
};

static void
colorSwapDesc(xXpColorSpaceDesc *d)
{
    for (int i = 0; i < 8; i++)
        swapl(&d->xy[i]);
    swapl(&d->refWhite);
    swapl(&d->minLum);
    swapl(&d->maxLum);
}

/* The description sent, with defaults filled in; BadValue if it is not
   one (errorValue set). */
static int
colorFromDesc(ClientPtr client, const xXpColorSpaceDesc *d, XpColorSpace *cs)
{
    memset(cs, 0, sizeof(*cs));
    if (d->primaries > XpColorPrimariesLast) {
        client->errorValue = d->primaries;
        return BadValue;
    }
    if (d->transfer > XpColorTransferFnLast) {
        client->errorValue = d->transfer;
        return BadValue;
    }
    if (d->refWhite > XpColorMaxLuminance || d->maxLum > XpColorMaxLuminance ||
        d->minLum > (CARD32) XpColorMaxLuminance * 10000) {
        client->errorValue = d->refWhite > XpColorMaxLuminance ? d->refWhite :
            d->maxLum > XpColorMaxLuminance ? d->maxLum : d->minLum;
        return BadValue;
    }

    if (d->primaries == XpColorPrimariesCustom) {
        XpColorXY *xy[4] = { &cs->primaries.r, &cs->primaries.g,
                             &cs->primaries.b, &cs->primaries.white };
        for (int i = 0; i < 4; i++) {
            CARD32 x = d->xy[2 * i], y = d->xy[2 * i + 1];
            if (!x || !y || (CARD64) x + y > 1000000) {
                client->errorValue = !x || x > 1000000 ? x : y;
                return BadValue;
            }
            *xy[i] = (XpColorXY) { x / 1e6, y / 1e6 };
        }
        XpColorMatrix unused;
        if (!xpColorRGBToXYZ(&cs->primaries, unused)) {
            client->errorValue = 0;
            return BadValue;    /* the three primaries lie on a line */
        }
    } else {
        cs->primaries = *namedPrimaries[d->primaries];
    }

    /* the wire values are those of compcolor.h's XpColorTransfer */
    cs->transfer = (XpColorTransfer) d->transfer;
    bool hdr = d->transfer == XpColorTransferFnPQ || d->transfer == XpColorTransferFnHLG;
    cs->refWhite = d->refWhite ? d->refWhite : hdr ? 203 : 80;
    cs->minLum = d->minLum / 10000.0;
    cs->maxLum = d->maxLum ? d->maxLum :
        d->transfer == XpColorTransferFnPQ ? 10000 :
        d->transfer == XpColorTransferFnHLG ? 1000 : cs->refWhite;
    return Success;
}

static int
colorLookupWindow(ClientPtr client, XID id, Mask access, WindowPtr *ppWin)
{
    int rc = dixLookupWindow(ppWin, id, client, access);

    if (rc != Success) {
        client->errorValue = id;
        return rc;
    }
    if ((*ppWin)->drawable.class == InputOnly) {
        client->errorValue = id;
        return BadMatch;        /* nothing to show */
    }
    return Success;
}

static void
colorSet(ColorWinPtr cw, bool set, CARD8 primaries, const XpColorSpace *cs)
{
    if (cw->set && !colorIsSRGB(&cw->cs))
        colorTagged--;
    cw->set = set;
    if (set) {
        cw->primaries = primaries;
        cw->cs = *cs;
        if (!colorIsSRGB(cs))
            colorTagged++;
    }
    colorSerial++;
    if (!colorSerial)
        colorSerial = 1;        /* 0 means unconverted */
}

static int
ProcXpColorQueryVersion(ClientPtr client)
{
    X_REQUEST_HEAD_STRUCT(xXpColorQueryVersionReq);
    X_REQUEST_FIELD_CARD32(majorVersion);
    X_REQUEST_FIELD_CARD32(minorVersion);

    xXpColorQueryVersionReply reply = {
        .majorVersion = XPRISM_COLOR_MAJOR,
        .minorVersion = XPRISM_COLOR_MINOR,
    };
    if (client->swapped) {
        swapl(&reply.majorVersion);
        swapl(&reply.minorVersion);
    }
    return X_SEND_REPLY_SIMPLE(client, reply);
}

static int
ProcXpColorSetWindowColorSpace(ClientPtr client)
{
    X_REQUEST_HEAD_STRUCT(xXpColorSetWindowColorSpaceReq);
    X_REQUEST_FIELD_CARD32(window);
    if (client->swapped)
        colorSwapDesc(&stuff->desc);

    XpColorSpace cs;
    int rc = colorFromDesc(client, &stuff->desc, &cs);
    if (rc != Success)
        return rc;
    WindowPtr pWin;
    rc = colorLookupWindow(client, stuff->window, DixSetAttrAccess, &pWin);
    if (rc != Success)
        return rc;

    colorSet(colorWin(pWin), true, stuff->desc.primaries, &cs);
    return Success;             /* the next frame converts */
}

static int
ProcXpColorGetWindowColorSpace(ClientPtr client)
{
    X_REQUEST_HEAD_STRUCT(xXpColorGetWindowColorSpaceReq);
    X_REQUEST_FIELD_CARD32(window);

    WindowPtr pWin;
    int rc = colorLookupWindow(client, stuff->window, DixGetAttrAccess, &pWin);
    if (rc != Success)
        return rc;

    ColorWinPtr cw = colorWin(pWin);
    const XpColorSpace *cs = cw->set ? &cw->cs : &xpColorSpaceSRGB;
    xXpColorGetWindowColorSpaceReply reply = {
        .set = cw->set,
        .desc = {
            .primaries = cw->set ? cw->primaries : XpColorPrimariesSRGB,
            .transfer = (CARD8) cs->transfer,
            .refWhite = (CARD32) lrint(cs->refWhite),
            .minLum = (CARD32) lrint(cs->minLum * 10000),
            .maxLum = (CARD32) lrint(cs->maxLum),
        },
    };
    const XpColorXY *xy[4] = { &cs->primaries.r, &cs->primaries.g,
                               &cs->primaries.b, &cs->primaries.white };
    for (int i = 0; i < 4; i++) {
        reply.desc.xy[2 * i] = (CARD32) lrint(xy[i]->x * 1e6);
        reply.desc.xy[2 * i + 1] = (CARD32) lrint(xy[i]->y * 1e6);
    }
    if (client->swapped)
        colorSwapDesc(&reply.desc);
    return X_SEND_REPLY_SIMPLE(client, reply);
}

/* Fill d from cs; named primaries are recognised. */
static void
colorToDesc(const XpColorSpace *cs, CARD8 primaries, xXpColorSpaceDesc *d)
{
    *d = (xXpColorSpaceDesc) {
        .primaries = primaries,
        .transfer = (CARD8) cs->transfer,
        .refWhite = (CARD32) lrint(cs->refWhite),
        .minLum = (CARD32) lrint(cs->minLum * 10000),
        .maxLum = (CARD32) lrint(cs->maxLum),
    };
    const XpColorXY *xy[4] = { &cs->primaries.r, &cs->primaries.g,
                               &cs->primaries.b, &cs->primaries.white };
    for (int i = 0; i < 4; i++) {
        d->xy[2 * i] = (CARD32) lrint(xy[i]->x * 1e6);
        d->xy[2 * i + 1] = (CARD32) lrint(xy[i]->y * 1e6);
    }
}

static int
ProcXpColorGetOutputColorSpace(ClientPtr client)
{
    X_REQUEST_HEAD_STRUCT(xXpColorGetOutputColorSpaceReq);
    X_REQUEST_FIELD_CARD32(output);

    RROutputPtr output;
    int rc = dixLookupResourceByType((void **) &output, stuff->output, RROutputType,
                                     client, DixReadAccess);
    if (rc != Success) {
        client->errorValue = stuff->output;
        return rc;
    }

    const XpColorSpace *cs = colorOutput(output->pScreen);
    CARD8 primaries = XpColorPrimariesCustom;
    for (CARD8 p = XpColorPrimariesSRGB; p <= XpColorPrimariesLast; p++)
        if (!memcmp(&cs->primaries, namedPrimaries[p], sizeof(XpColorPrimaries)))
            primaries = p;
    xXpColorGetOutputColorSpaceReply reply = { .set = cs != &xpColorSpaceSRGB };
    colorToDesc(cs, primaries, &reply.desc);
    if (client->swapped)
        colorSwapDesc(&reply.desc);
    return X_SEND_REPLY_SIMPLE(client, reply);
}

static int
ProcXpColorClearWindowColorSpace(ClientPtr client)
{
    X_REQUEST_HEAD_STRUCT(xXpColorClearWindowColorSpaceReq);
    X_REQUEST_FIELD_CARD32(window);

    WindowPtr pWin;
    int rc = colorLookupWindow(client, stuff->window, DixSetAttrAccess, &pWin);
    if (rc != Success)
        return rc;
    if (colorWin(pWin)->set)
        colorSet(colorWin(pWin), false, 0, NULL);
    return Success;
}

static int
ProcXpColorDispatch(ClientPtr client)
{
    REQUEST(xReq);

    switch (stuff->data) {
    case X_XpColorQueryVersion:
        return ProcXpColorQueryVersion(client);
    case X_XpColorSetWindowColorSpace:
        return ProcXpColorSetWindowColorSpace(client);
    case X_XpColorGetWindowColorSpace:
        return ProcXpColorGetWindowColorSpace(client);
    case X_XpColorClearWindowColorSpace:
        return ProcXpColorClearWindowColorSpace(client);
    case X_XpColorGetOutputColorSpace:
        return ProcXpColorGetOutputColorSpace(client);
    default:
        return BadRequest;
    }
}

void
compColorExtensionInit(void)
{
    if (!AddExtension(XPRISM_COLOR_NAME, 0, 0, ProcXpColorDispatch,
                      ProcXpColorDispatch, NULL, StandardMinorOpcode))
        LogMessageVerb(X_WARNING, 1, "compositor: cannot add %s\n",
                       XPRISM_COLOR_NAME);
}
