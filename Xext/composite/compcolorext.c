/* SPDX-License-Identifier: MIT */
/*
 * XPRISM-COLOR (Phase IV, doc/color.md): applications describe the colour
 * space of their windows' pixels, and the scene compositor shows them
 * converted for the output.
 *
 * Outputs are sRGB for now. A top-level window whose contents are, in
 * whole or in part, described as anything else gets a second pixmap
 * holding its contents converted to sRGB, kept up to date where the
 * window is drawn on; the compositor shows that one instead. Each pixel
 * is in the colour space of the innermost window that covers it and has
 * a description (child windows inherit their parent's), sRGB without
 * one.
 *
 * Conversion: decode the transfer function to linear light, convert the
 * primaries (3x3 matrix), tone map HDR content down to SDR white,
 * bring colours outside the sRGB gamut into it by desaturating towards
 * their luminance (which keeps the hue), encode sRGB. The pixels are
 * converted by the CPU, through GetImage and PutImage, so this works with
 * every DDX; GPU conversion comes with HDR composition.
 *
 * Windows without a description cost nothing, and as long as no window
 * has one the compositor does exactly what it did before.
 */

#include <dix-config.h>

#include <math.h>

#include "dix/dix_priv.h"
#include "dix/request_priv.h"
#include "miext/extinit_priv.h"

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

static bool
colorDescribed(WindowPtr pWin)
{
    ColorWinPtr cw = colorSettings(pWin);
    return cw->set && !colorIsSRGB(&cw->cs);
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

static bool
colorNeedsConversion(WindowPtr top)
{
    if (!colorTagged)
        return false;
    for (WindowPtr pWin = top; pWin; pWin = colorNext(top, pWin))
        if (colorDescribed(pWin))
            return true;
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

typedef struct {
    const XpColorSpace *cs;
    XpColorMatrix m;            /* linear, the space's primaries to sRGB */
    bool hlg;
    float decode[DECODE_LUT + 1];
    /* HDR: luminance above SDR white is tone mapped */
    bool tone;
    double knee, peak;          /* relative to reference white */
    float toneLut[TONE_LUT + 1];
    /* converted pixels: UI contents repeat the same few colours */
    CARD32 cacheKey[COLOR_CACHE], cacheVal[COLOR_CACHE];
    bool cacheSet[COLOR_CACHE];
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

static bool
colorConvInit(ColorConv *cv, const XpColorSpace *cs)
{
    memset(cv, 0, sizeof(*cv));
    cv->cs = cs;
    if (!xpColorConversion(&cs->primaries, &xpColorPrimariesSRGB, cv->m))
        return false;
    cv->hlg = cs->transfer == XpColorTransferHLG;
    for (int i = 0; i <= DECODE_LUT; i++)
        cv->decode[i] = (float) xpColorDecode(cs, (double) i / DECODE_LUT);

    if ((cs->transfer == XpColorTransferPQ || cv->hlg) &&
        cs->maxLum > cs->refWhite) {
        /* the output's peak is SDR white */
        cv->tone = true;
        cv->knee = xpColorToneMapKnee(cs->maxLum, cs->refWhite) / cs->refWhite;
        cv->peak = cs->maxLum / cs->refWhite;
        for (int i = 0; i <= TONE_LUT; i++) {
            double y = cv->knee + (cv->peak - cv->knee) * i / TONE_LUT;
            cv->toneLut[i] = (float) (xpColorToneMap(y * cs->refWhite, cs->maxLum,
                                                     cs->refWhite) / cs->refWhite);
        }
    }
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

    /* luminance, from the sRGB primaries */
    double y = 0.2126 * out[0] + 0.7152 * out[1] + 0.0722 * out[2];
    if (cv->tone && y > cv->knee) {
        double t = (y - cv->knee) / (cv->peak - cv->knee);
        double mapped = lutGet(cv->toneLut, TONE_LUT, t);
        for (int c = 0; c < 3; c++)
            out[c] *= mapped / y;
        y = mapped;
    }

    /* gamut: move towards the luminance until every channel fits */
    y = y < 0 ? 0 : y > 1 ? 1 : y;
    double s = 1;
    for (int c = 0; c < 3; c++) {
        if (out[c] > 1 && out[c] - y > 0)
            s = fmin(s, (1 - y) / (out[c] - y));
        else if (out[c] < 0)
            s = fmin(s, y / (y - out[c]));
    }
    for (int c = 0; c < 3; c++) {
        double v = y + s * (out[c] - y);
        out[c] = lutGet(encodeLut, ENCODE_LUT, v);
    }
}

typedef struct {
    int shift[4];               /* r, g, b, a */
    CARD32 mask[4];
} ColorLayout;

static void
colorConvertSpan(ColorConv *cv, const ColorLayout *l, CARD32 *px, int n)
{
    for (int i = 0; i < n; i++) {
        CARD32 p = px[i];
        double a = 1, in[3], out[3];
        unsigned h = (p * 2654435761u) >> 20 & (COLOR_CACHE - 1);

        if (cv->cacheSet[h] && cv->cacheKey[h] == p) {
            px[i] = cv->cacheVal[h];
            continue;
        }

        if (l->mask[3]) {
            CARD32 av = (p >> l->shift[3]) & l->mask[3];
            if (!av)
                continue;       /* premultiplied: all zero */
            a = (double) av / l->mask[3];
        }
        for (int c = 0; c < 3; c++) {
            double v = (double) ((p >> l->shift[c]) & l->mask[c]) / l->mask[c];
            in[c] = fmin(v / a, 1);
        }
        colorConvertPixel(cv, in, out);
        CARD32 q = l->mask[3] ? p & (l->mask[3] << l->shift[3]) : 0;
        for (int c = 0; c < 3; c++)
            q |= (CARD32) lrint(out[c] * a * l->mask[c]) << l->shift[c];
        px[i] = q;
        cv->cacheSet[h] = true;
        cv->cacheKey[h] = p;
        cv->cacheVal[h] = q;
    }
}

/* Convert pRegion (pixmap coordinates) of src into dst. cs NULL: copy. */
static void
colorConvertRegion(WindowPtr top, PixmapPtr src, PixmapPtr dst,
                   const XpColorSpace *cs, RegionPtr pRegion)
{
    ScreenPtr pScreen = src->drawable.pScreen;
    int n = RegionNumRects(pRegion);
    BoxPtr b = RegionRects(pRegion);
    GCPtr pGC;
    ColorConv *cv = NULL;
    ColorLayout layout;

    if (!n)
        return;
    pGC = GetScratchGC(dst->drawable.depth, pScreen);
    if (!pGC)
        return;
    ValidateGC(&dst->drawable, pGC);

    if (cs) {
        PictFormatPtr format = PictureWindowFormat(top);
        cv = calloc(1, sizeof(*cv));
        if (!format || format->type != PictTypeDirect || !cv ||
            !colorConvInit(cv, cs)) {
            free(cv);
            cv = NULL;          /* shown as it is */
        } else {
            const DirectFormatRec *d = &format->direct;
            layout = (ColorLayout) {
                .shift = { d->red, d->green, d->blue, d->alpha },
                .mask = { d->redMask, d->greenMask, d->blueMask, d->alphaMask },
            };
        }
    }

    for (int k = 0; k < n; k++) {
        int w = b[k].x2 - b[k].x1, h = b[k].y2 - b[k].y1;
        if (!cv) {
            (*pGC->ops->CopyArea) (&src->drawable, &dst->drawable, pGC,
                                   b[k].x1, b[k].y1, w, h, b[k].x1, b[k].y1);
            continue;
        }
        /* in strips, to bound the buffer */
        int rows = max(1, min(h, (1 << 20) / max(w, 1)));
        CARD32 *buf = calloc((size_t) w * rows, sizeof(CARD32));
        if (!buf)
            continue;
        for (int y = b[k].y1; y < b[k].y2; y += rows) {
            int sh = min(rows, b[k].y2 - y);
            (*pScreen->GetImage) (&src->drawable, b[k].x1, y, w, sh, ZPixmap,
                                  ~0UL, (char *) buf);
            colorConvertSpan(cv, &layout, buf, w * sh);
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
    return colorNeedsConversion(top) ? colorSerial : 0;
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

    if (!colorNeedsConversion(top) || content->drawable.bitsPerPixel != 32) {
        colorRelease(cw);
        return content;
    }

    if (cw->content != content ||
        cw->converted->drawable.width != content->drawable.width ||
        cw->converted->drawable.height != content->drawable.height ||
        cw->converted->drawable.depth != content->drawable.depth) {
        colorRelease(cw);
        cw->converted = (*pScreen->CreatePixmap) (pScreen, content->drawable.width,
                                                  content->drawable.height,
                                                  content->drawable.depth, 0);
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
    ColorWinPtr ts = colorSettings(top);
    colorConvertRegion(top, content, cw->converted,
                       ts->set && !colorIsSRGB(&ts->cs) ? &ts->cs : NULL,
                       &cw->dirty);
    RegionRec part;
    RegionNull(&part);
    for (WindowPtr pWin = colorNext(top, top); pWin; pWin = colorNext(top, pWin)) {
        ColorWinPtr s = colorSettings(pWin);
        if (!s->set)
            continue;
        RegionCopy(&part, &pWin->borderClip);
        RegionTranslate(&part, -content->screen_x, -content->screen_y);
        RegionIntersect(&part, &part, &cw->dirty);
        colorConvertRegion(top, content, cw->converted,
                           colorIsSRGB(&s->cs) ? NULL : &s->cs, &part);
    }
    RegionUninit(&part);
    RegionEmpty(&cw->dirty);
    return cw->converted;
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
