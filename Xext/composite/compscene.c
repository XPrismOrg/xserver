/* SPDX-License-Identifier: MIT */
/*
 * XPrism scene compositor (doc/compositor.md).
 *
 * The server owns final composition. When enabled (-compositor on), the
 * server itself holds the Manual redirection of every top-level window,
 * so Composite keeps each one in its own pixmap and paints nothing. Once
 * per main-loop iteration (block handler) the compositor:
 *
 *   1. builds a snapshot of the scene: the viewable top-level windows,
 *      bottom to top, with their bounds and opacity;
 *   2. diffs it against the previous frame and adds the content damage
 *      of every window, giving the region of the screen that changed;
 *   3. repaints only that region: the root background, then every
 *      window with Render's Over operator (ARGB windows blend; the
 *      _NET_WM_WINDOW_OPACITY hint becomes a constant alpha mask).
 *
 * The default policy is plain stacking order. The XPrism-Scene protocol
 * will let the window manager attach effects to these nodes later.
 *
 * Legacy compositor mode: when a client (picom, KWin, ...) asks for
 * Manual redirection of a root window, the scene compositor steps aside
 * for that screen instead of failing the request with BadAccess, and
 * takes over again when the client's redirection goes away.
 *
 * Rendering goes through Render, so it is accelerated wherever the DDX
 * accelerates Render (glamor) and runs in software (fb) elsewhere.
 */

#include <dix-config.h>

#include <X11/Xatom.h>

#include "dix/dix_priv.h"
#include "dix/screen_hooks_priv.h"
#include "dix/window_priv.h"
#include "dix/property_priv.h"
#include "propertyst.h"
#include "mi/mi_priv.h"
#include "miext/extinit_priv.h"

#include "compint.h"
#include "compositeext_intern.h"
#include "compositeext_priv.h"
#include "dix/request_priv.h"
#include "dix/resource_priv.h"
#include "xprismsceneproto.h"
#include "randrstr.h"
#ifdef XINERAMA
#include "Xext/panoramiX/panoramiXsrv.h"
#include "Xext/panoramiX/panoramiX_priv.h"
#endif

#include <math.h>

Bool sceneCompositorEnabled = XPRISM_COMPOSITOR_DEFAULT;  /* -Dcompositor_default */

/* One node of the scene: a viewable top-level window. */
typedef struct {
    XID id;
    BoxRec box;                 /* shown bounds with shadow, screen coords */
    BoxRec shown;               /* shown bounds of the window itself */
    CARD32 opacity;             /* 0xffffffff = opaque */
    bool transformed;
    PictTransform xform;        /* window -> shown (XPRISM-SCENE) */
    CARD16 radius;              /* corner radius, 0 = square */
    bool shadow;                /* shadow drawn */
    BoxRec shadowBox;           /* screen coords, if shadow */
    CARD16 shadowBlur;
    CARD32 shadowColor;
    CARD16 bgBlur;              /* background blur radius, 0 = none */
    unsigned color;             /* compColorState */
} SceneNodeRec;

typedef struct {
    PicturePtr mask;
    int w, h, radius;
    CARD8 alpha;
} SceneCornerCache;

/* What the window manager set through XPRISM-SCENE (window private,
   zero-initialized storage: nothing set). */
typedef struct {
    CARD32 mask;                /* XpSceneNode* */
    CARD32 opacity;
    PictTransform xform;        /* as sent: window -> shown */
    PictTransform inverse;      /* shown -> window, for Render */
    ClientPtr owner;            /* the WM that set it; cleared when it goes */
    /* 0.2 decoration */
    CARD16 radius;
    CARD16 shadowBlur;
    INT16 shadowDx, shadowDy;
    CARD32 shadowColor;         /* ARGB, not premultiplied */
    /* 0.3 */
    CARD16 bgBlur;
    /* cached alpha masks, rebuilt when their key changes */
    SceneCornerCache corner;    /* with the window's opacity */
    SceneCornerCache backdrop;  /* opaque, for the blurred backdrop */
    PicturePtr shadowMask;
    int smW, smH, smRadius, smBlur;
} SceneAttrsRec, *SceneAttrsPtr;

#define SCENE_MAX_PACED 16

typedef struct {
    bool active;                /* we hold Manual redirection of root */
    bool yielded;               /* a legacy compositor holds it */
    bool fullDamage;            /* repaint everything next frame */
    bool rootHidden;            /* the root had no clip: frames skipped */
    /* frame pacing: each output is composed at most once per refresh */
    RegionRec deferred;         /* damage on outputs that are not due yet */
    CARD64 due;                 /* when the first of them is due */
    struct {
        RRCrtcPtr crtc;
        CARD64 last;            /* when it was last composed, microseconds */
    } paced[SCENE_MAX_PACED];
    SceneNodeRec *nodes;        /* previous frame */
    int numNodes;
    /* root background as last painted */
    int bgState;
    PixUnion bg;
    /* fullscreen window shown directly (unredirected), or None */
    XID direct;
    /* the root background's tile as the output shows it (XPRISM-COLOR);
       drawing into the tile makes the copy stale */
    PixmapPtr bgTileSrc, bgTile;
    DamagePtr bgTileDamage;
    bool bgTileStale;
    unsigned colorSerial;
    /* report drawing onto the root window, before and after it happens */
    DamagePtr rootDamage;
    DamagePtr rootDrawnDamage;
} SceneScreenRec, *SceneScreenPtr;

static DevPrivateKeyRec sceneScreenKeyRec;
static DevPrivateKeyRec sceneWindowKeyRec;     /* DamagePtr per top-level */
static DevPrivateKeyRec sceneAttrsKeyRec;      /* SceneAttrsRec per window */
static Atom atomOpacity;
static bool sceneInFrame;       /* painting a frame, or copying to windows */
int compSceneHold;              /* > 0: no frames on demand (compCopyWindow) */
static unsigned long handlersGeneration;

static SceneScreenPtr
sceneScreen(ScreenPtr pScreen)
{
    /* the key is only registered when the compositor is enabled; the
       Composite hooks call in here either way */
    if (!dixPrivateKeyRegistered(&sceneScreenKeyRec))
        return NULL;
    return dixLookupPrivate(&pScreen->devPrivates, &sceneScreenKeyRec);
}

/* ---- per-window content damage ------------------------------------- */

static void
sceneDamageReport(DamagePtr pDamage, RegionPtr pRegion, void *closure)
{
    /* nothing: the region accumulates and is read in the block handler */
}

static void
sceneDamageDestroy(DamagePtr pDamage, void *closure)
{
    WindowPtr pWin = closure;

    dixSetPrivate(&pWin->devPrivates, &sceneWindowKeyRec, NULL);
}

static DamagePtr
sceneWindowDamage(WindowPtr pWin)
{
    DamagePtr pDamage = dixLookupPrivate(&pWin->devPrivates, &sceneWindowKeyRec);

    if (pDamage)
        return pDamage;
    pDamage = DamageCreate(sceneDamageReport, sceneDamageDestroy,
                           DamageReportNonEmpty, TRUE,
                           pWin->drawable.pScreen, pWin);
    if (!pDamage)
        return NULL;
    DamageRegister(&pWin->drawable, pDamage);
    dixSetPrivate(&pWin->devPrivates, &sceneWindowKeyRec, pDamage);
    return pDamage;
}

static void
sceneDropWindowDamage(WindowPtr pWin)
{
    DamagePtr pDamage = dixLookupPrivate(&pWin->devPrivates, &sceneWindowKeyRec);

    if (!pDamage)
        return;
    dixSetPrivate(&pWin->devPrivates, &sceneWindowKeyRec, NULL);
    DamageUnregister(pDamage);
    DamageDestroy(pDamage);
}

/* ---- node attributes (XPRISM-SCENE) --------------------------------- */

static SceneAttrsPtr
sceneAttrs(WindowPtr pWin)
{
    return dixLookupPrivate(&pWin->devPrivates, &sceneAttrsKeyRec);
}

static double
fx(pixman_fixed_t v)
{
    return v / 65536.0;
}

/* Map window-local (x, y) through t. False if it lands at or behind the
   projection plane. */
static bool
sceneTransformPoint(const PictTransform *t, double x, double y,
                    double *ox, double *oy)
{
    double X = fx(t->matrix[0][0]) * x + fx(t->matrix[0][1]) * y + fx(t->matrix[0][2]);
    double Y = fx(t->matrix[1][0]) * x + fx(t->matrix[1][1]) * y + fx(t->matrix[1][2]);
    double W = fx(t->matrix[2][0]) * x + fx(t->matrix[2][1]) * y + fx(t->matrix[2][2]);

    if (W <= 1e-6)
        return false;
    *ox = X / W;
    *oy = Y / W;
    return true;
}

/* Bounds (window-local) of the window-local rectangle x1,y1 - x2,y2 shown
   through t. */
static bool
sceneTransformRectBounds(const PictTransform *t, int rx1, int ry1, int rx2, int ry2,
                         BoxPtr box)
{
    const double cx[4] = { rx1, rx2, rx1, rx2 }, cy[4] = { ry1, ry1, ry2, ry2 };
    double x1 = HUGE_VAL, y1 = HUGE_VAL, x2 = -HUGE_VAL, y2 = -HUGE_VAL;

    for (int i = 0; i < 4; i++) {
        double x, y;
        if (!sceneTransformPoint(t, cx[i], cy[i], &x, &y))
            return false;
        x1 = fmin(x1, x); y1 = fmin(y1, y);
        x2 = fmax(x2, x); y2 = fmax(y2, y);
    }
    /* one pixel of margin for bilinear filtering; clamp to sane coords */
    double bx1 = fmax(floor(x1) - 1, -32767), by1 = fmax(floor(y1) - 1, -32767);
    double bx2 = fmin(ceil(x2) + 1, 32767), by2 = fmin(ceil(y2) + 1, 32767);
    box->x1 = (short) bx1;
    box->y1 = (short) by1;
    box->x2 = (short) bx2;
    box->y2 = (short) by2;
    return true;
}

/* Bounds (window-local) of a w x h window shown through t. */
static bool
sceneTransformBounds(const PictTransform *t, int w, int h, BoxPtr box)
{
    return sceneTransformRectBounds(t, 0, 0, w, h, box);
}

/* ---- decoration masks (0.2) -------------------------------------------- */

/* An A8 picture with the given contents (rows padded to 4 bytes, as
   PutImage expects for ZPixmap at depth 8). */
static PicturePtr
sceneAlphaPicture(ScreenPtr pScreen, int w, int h, uint8_t *data)
{
    PictFormatPtr format = PictureMatchFormat(pScreen, 8, PICT_a8);
    if (!format || w <= 0 || h <= 0)
        return NULL;
    PixmapPtr pPixmap = (*pScreen->CreatePixmap) (pScreen, w, h, 8, 0);
    if (!pPixmap)
        return NULL;
    GCPtr pGC = GetScratchGC(8, pScreen);
    if (!pGC) {
        dixDestroyPixmap(pPixmap, 0);
        return NULL;
    }
    ValidateGC(&pPixmap->drawable, pGC);
    (*pGC->ops->PutImage) (&pPixmap->drawable, pGC, 8, 0, 0, w, h, 0,
                           ZPixmap, (char *) data);
    FreeScratchGC(pGC);
    int error;
    PicturePtr pPicture = CreatePicture(0, &pPixmap->drawable, format, 0, NULL,
                                        serverClient, &error);
    dixDestroyPixmap(pPixmap, 0);  /* the picture holds a reference */
    return pPicture;
}

/* Coverage of pixel (x, y) by a w x h rectangle with corner radius r,
   placed at (ox, oy) in a larger buffer. Antialiased at the corners. */
static double
sceneRoundRectCoverage(int x, int y, int ox, int oy, int w, int h, int r)
{
    double px = x - ox + 0.5, py = y - oy + 0.5;

    if (px < 0 || py < 0 || px > w || py > h)
        return 0;
    double cx = px < r ? r : (px > w - r ? w - r : px);
    double cy = py < r ? r : (py > h - r ? h - r : py);
    if (cx == px || cy == py)
        return 1;               /* not in a corner square */
    double d = sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy));
    double c = r - d + 0.5;
    return c <= 0 ? 0 : (c >= 1 ? 1 : c);
}

static int
sceneStride(int w)
{
    return (w + 3) & ~3;
}

static PicturePtr
sceneCornerMask(ScreenPtr pScreen, SceneCornerCache *cache, int radius,
                int w, int h, CARD8 alpha)
{
    int r = min(radius, min(w, h) / 2);

    if (cache->mask && cache->w == w && cache->h == h &&
        cache->radius == r && cache->alpha == alpha)
        return cache->mask;
    if (cache->mask)
        FreePicture(cache->mask, 0);
    cache->mask = NULL;

    int stride = sceneStride(w);
    uint8_t *buf = malloc((size_t) stride * h);
    if (!buf)
        return NULL;
    /* Coverage is 1 everywhere but in the four r x r corner squares */
    for (int y = 0; y < h; y++)
        memset(buf + y * stride, alpha, stride);
    for (int y = 0; y < h; y++) {
        if (y >= r && y < h - r)
            continue;
        for (int x = 0; x < w; x++) {
            if (x == r && w - r > r) {
                x = w - r - 1;      /* skip the straight part of the row */
                continue;
            }
            buf[y * stride + x] =
                (uint8_t) lround(alpha * sceneRoundRectCoverage(x, y, 0, 0, w, h, r));
        }
    }
    cache->mask = sceneAlphaPicture(pScreen, w, h, buf);
    free(buf);
    cache->w = w;
    cache->h = h;
    cache->radius = r;
    cache->alpha = alpha;
    return cache->mask;
}

/* One box-blur pass over n samples, step apart, using a running sum.
   Samples outside the buffer count as 0. */
static void
sceneBlurLine(uint8_t *p, int n, int step, int radius, uint8_t *tmp)
{
    int div = 2 * radius + 1;
    uint32_t sum = 0;

    for (int i = 0; i < n; i++)
        tmp[i] = p[i * step];
    for (int i = 0; i <= radius && i < n; i++)
        sum += tmp[i];
    for (int i = 0; i < n; i++) {
        p[i * step] = sum / div;
        if (i + radius + 1 < n)
            sum += tmp[i + radius + 1];
        if (i - radius >= 0)
            sum -= tmp[i - radius];
    }
}

/* Three passes of a box blur approximate a Gaussian; linear in the
   number of pixels whatever the radius. */
static void
sceneBoxBlur(uint8_t *buf, int w, int h, int stride, int radius)
{
    if (radius < 1)
        return;
    uint8_t *tmp = malloc(max(w, h));
    if (!tmp)
        return;
    for (int pass = 0; pass < 3; pass++) {
        for (int y = 0; y < h; y++)
            sceneBlurLine(buf + y * stride, w, 1, radius, tmp);
        for (int x = 0; x < w; x++)
            sceneBlurLine(buf + x, h, stride, radius, tmp);
    }
    free(tmp);
}

/* Shadow of a w x h rectangle with corner radius r, blurred by b, in a
   (w + 2b) x (h + 2b) buffer. */
static uint8_t *
sceneShadowBlurred(int w, int h, int r, int b)
{
    int sw = w + 2 * b, sh = h + 2 * b, stride = sceneStride(sw);
    uint8_t *buf = calloc((size_t) stride, sh);
    if (!buf)
        return NULL;
    for (int y = 0; y < sh; y++)
        for (int x = 0; x < sw; x++)
            buf[y * stride + x] =
                (uint8_t) lround(255 * sceneRoundRectCoverage(x, y, b, b, w, h, r));
    sceneBoxBlur(buf, sw, sh, stride, (b + 1) / 2);
    return buf;
}

/*
 * The same, without blurring the whole window. Away from the corners
 * every column of the top and bottom edges is the same, and so is every
 * row of the left and right edges: blur a rectangle just large enough
 * that its middle column and row are out of the blur's reach from the
 * corners, and repeat them. The result is identical to blurring the
 * whole rectangle, at a cost that depends on r and b, not on the size.
 */
static uint8_t *
sceneShadowPixels(int w, int h, int r, int b)
{
    int reach = 3 * ((b + 1) / 2);          /* three box-blur passes */
    int core = 2 * (r + reach) + 1;
    int smw = min(w, core), smh = min(h, core);

    if (smw == w && smh == h)
        return sceneShadowBlurred(w, h, r, b);
    uint8_t *small = sceneShadowBlurred(smw, smh, r, b);
    if (!small)
        return NULL;
    int SW = smw + 2 * b, SH = smh + 2 * b, ss = sceneStride(SW);
    int TW = w + 2 * b, TH = h + 2 * b, ts = sceneStride(TW);
    int mc = SW / 2, mr = SH / 2;           /* the repeated column and row */
    int right = SW - mc - 1, bottom = SH - mr - 1;
    uint8_t *buf = calloc((size_t) ts, TH);
    if (!buf) {
        free(small);
        return NULL;
    }
    for (int y = 0; y < TH; y++) {
        int my = y < mr ? y : (y >= TH - bottom ? y - (TH - SH) : mr);
        uint8_t *dst = buf + (size_t) y * ts;
        if (y > mr && my == mr) {
            memcpy(dst, buf + (size_t) mr * ts, TW);
            continue;
        }
        const uint8_t *src = small + (size_t) my * ss;
        memcpy(dst, src, mc);
        memset(dst + mc, src[mc], TW - right - mc);
        memcpy(dst + TW - right, src + mc + 1, right);
    }
    free(small);
    return buf;
}

static PicturePtr
sceneShadowMask(ScreenPtr pScreen, SceneAttrsPtr attrs, int w, int h)
{
    int r = min(attrs->radius, min(w, h) / 2);
    int b = attrs->shadowBlur;

    if (attrs->shadowMask && attrs->smW == w && attrs->smH == h &&
        attrs->smRadius == r && attrs->smBlur == b)
        return attrs->shadowMask;
    if (attrs->shadowMask)
        FreePicture(attrs->shadowMask, 0);
    attrs->shadowMask = NULL;

    uint8_t *buf = sceneShadowPixels(w, h, r, b);
    if (!buf)
        return NULL;
    attrs->shadowMask = sceneAlphaPicture(pScreen, w + 2 * b, h + 2 * b, buf);
    free(buf);
    attrs->smW = w;
    attrs->smH = h;
    attrs->smRadius = r;
    attrs->smBlur = b;
    return attrs->shadowMask;
}

/* Forget everything a window manager set, including cached masks. */
static void
sceneResetAttrs(SceneAttrsPtr attrs)
{
    if (attrs->corner.mask)
        FreePicture(attrs->corner.mask, 0);
    if (attrs->backdrop.mask)
        FreePicture(attrs->backdrop.mask, 0);
    if (attrs->shadowMask)
        FreePicture(attrs->shadowMask, 0);
    memset(attrs, 0, sizeof(*attrs));
}

static void
sceneWindowDestroy(CallbackListPtr *pcbl, ScreenPtr pScreen, WindowPtr pWin)
{
    sceneResetAttrs(sceneAttrs(pWin));
    compColorWindowDestroy(pWin);
}

/* ---- scene snapshot ------------------------------------------------- */

static CARD32
sceneWindowOpacity(WindowPtr pWin)
{
    PropertyPtr pProp;

    if (dixLookupProperty(&pProp, pWin, atomOpacity, serverClient,
                          DixReadAccess) != Success)
        return 0xffffffff;
    if (pProp->type != XA_CARDINAL || pProp->format != 32 || pProp->size < 1)
        return 0xffffffff;
    return *(CARD32 *) pProp->data;
}

/*
 * With Xinerama each window exists once per screen, and clients address
 * the copy on screen 0: that one holds the window's properties and what
 * the window manager set through XPRISM-SCENE. Copies on other screens
 * take their settings from it.
 */
static WindowPtr
sceneSettingsWindow(WindowPtr pWin)
{
#ifdef XINERAMA
    int scr = pWin->drawable.pScreen->myNum;
    WindowPtr pMaster;

    if (scr != 0 && PanoramiXIsEnabled()) {
        PanoramiXRes *res = PanoramiXFindIDByScrnum(XRT_WINDOW,
                                                    pWin->drawable.id, scr);
        if (res && dixLookupWindow(&pMaster, res->info[0].id, serverClient,
                                   DixReadAccess) == Success)
            return pMaster;
    }
#endif
    return pWin;
}

/* The settings, not the cached masks: those belong to one screen. */
/*
 * WindowPointMapHook: input follows what is shown. A point of the screen
 * over a transformed top-level window is mapped back through the
 * window's inverse transform into its untransformed space, where its
 * geometry, shapes and event coordinates are. Only while this compositor
 * draws the screen: otherwise the transform is not shown.
 */
static void
sceneMapPoint(WindowPtr top, double *x, double *y)
{
    SceneScreenPtr ss = sceneScreen(top->drawable.pScreen);
    SceneAttrsPtr attrs;

    if (!ss || !ss->active || !top->viewable)
        return;
    attrs = sceneAttrs(sceneSettingsWindow(top));
    if (!attrs || !(attrs->mask & XpSceneNodeTransform))
        return;

    const PictTransform *m = &attrs->inverse;
    int bw = wBorderWidth(top);
    double ox = top->drawable.x - bw, oy = top->drawable.y - bw;
    double lx = *x - ox, ly = *y - oy;
    double wx = fx(m->matrix[0][0]) * lx + fx(m->matrix[0][1]) * ly + fx(m->matrix[0][2]);
    double wy = fx(m->matrix[1][0]) * lx + fx(m->matrix[1][1]) * ly + fx(m->matrix[1][2]);
    double w = fx(m->matrix[2][0]) * lx + fx(m->matrix[2][1]) * ly + fx(m->matrix[2][2]);

    if (w <= 0) {
        /* beyond the horizon of a perspective transform: nowhere on the
           window */
        *x = *y = -65536;
        return;
    }
    *x = ox + wx / w;
    *y = oy + wy / w;
}

static void
sceneCopySettings(SceneAttrsPtr to, const SceneAttrsRec *from)
{
    to->mask = from->mask;
    to->opacity = from->opacity;
    to->xform = from->xform;
    to->inverse = from->inverse;
    to->radius = from->radius;
    to->shadowBlur = from->shadowBlur;
    to->shadowDx = from->shadowDx;
    to->shadowDy = from->shadowDy;
    to->shadowColor = from->shadowColor;
    to->bgBlur = from->bgBlur;
}

static bool
sceneWindowIsNode(WindowPtr pWin)
{
    return pWin->viewable && pWin->drawable.class == InputOutput &&
        pWin->redirectDraw != RedirectDrawNone;
}

static void
sceneUnionBox(RegionPtr pRegion, const BoxRec *box)
{
    RegionRec r;

    RegionInit(&r, (BoxPtr) box, 1);
    RegionUnion(pRegion, pRegion, &r);
    RegionUninit(&r);
}

static const SceneNodeRec *
sceneFindNode(const SceneNodeRec *nodes, int n, XID id, int *index)
{
    for (int i = 0; i < n; i++)
        if (nodes[i].id == id) {
            *index = i;
            return &nodes[i];
        }
    return NULL;
}

/*
 * Build this frame's node list (bottom to top), and add everything that
 * changed since the last frame to pDamage (screen coordinates).
 */
static int
sceneSnapshot(ScreenPtr pScreen, SceneScreenPtr ss, WindowPtr **pWins,
              SceneNodeRec **pNodes, RegionPtr pDamage)
{
    WindowPtr pRoot = pScreen->root;
    int n = 0, cap = 0;
    WindowPtr *wins = NULL;
    SceneNodeRec *nodes = NULL;

    for (WindowPtr pWin = pRoot->lastChild; pWin; pWin = pWin->prevSib) {
        if (!sceneWindowIsNode(pWin))
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            WindowPtr *w2 = reallocarray(wins, cap, sizeof(*wins));
            if (w2)
                wins = w2;
            SceneNodeRec *n2 = reallocarray(nodes, cap, sizeof(*nodes));
            if (n2)
                nodes = n2;
            if (!w2 || !n2) {
                free(wins);
                free(nodes);
                ss->fullDamage = true;
                *pWins = NULL;
                *pNodes = NULL;
                return 0;
            }
        }
        WindowPtr pSettings = sceneSettingsWindow(pWin);
        SceneAttrsPtr attrs = sceneAttrs(pWin);
        SceneNodeRec *node = &nodes[n];

        if (pSettings != pWin)
            sceneCopySettings(attrs, sceneAttrs(pSettings));
        wins[n] = pWin;
        memset(node, 0, sizeof(*node));
        node->id = pWin->drawable.id;
        node->box = *RegionExtents(&pWin->borderSize);
        node->shown = node->box;
        node->opacity = (attrs->mask & XpSceneNodeOpacity) ?
            attrs->opacity : sceneWindowOpacity(pSettings);
        if (attrs->mask & XpSceneNodeTransform) {
            int bw = wBorderWidth(pWin);
            BoxRec local;
            if (sceneTransformBounds(&attrs->xform,
                                     pWin->drawable.width + 2 * bw,
                                     pWin->drawable.height + 2 * bw, &local)) {
                int ox = pWin->drawable.x - bw, oy = pWin->drawable.y - bw;
                node->transformed = true;
                node->xform = attrs->xform;
                node->box = (BoxRec) {
                    .x1 = max(local.x1 + ox, -32767), .y1 = max(local.y1 + oy, -32767),
                    .x2 = min(local.x2 + ox, 32767), .y2 = min(local.y2 + oy, 32767),
                };
                node->shown = node->box;
            }
        }

        if (attrs->mask & XpSceneNodeCornerRadius)
            node->radius = attrs->radius;
        if ((attrs->mask & XpSceneNodeBackgroundBlur) && attrs->bgBlur) {
            /* only what shows through the window is blurred */
            PictFormatPtr format = PictureWindowFormat(pWin);
            if (node->opacity != 0xffffffff ||
                (format && format->direct.alphaMask))
                node->bgBlur = attrs->bgBlur;
        }
        if (attrs->mask & XpSceneNodeShadow) {
            int bw = wBorderWidth(pWin), b = attrs->shadowBlur;
            int ow = pWin->drawable.width + 2 * bw, oh = pWin->drawable.height + 2 * bw;
            int x = pWin->drawable.x - bw + attrs->shadowDx - b;
            int y = pWin->drawable.y - bw + attrs->shadowDy - b;
            BoxRec local = { .x1 = -b, .y1 = -b, .x2 = ow + b, .y2 = oh + b };
            /* transformed: the shadow is the window's shape shown through
               the transform, then moved by dx, dy on the screen */
            if (node->transformed) {
                node->shadow = sceneTransformRectBounds(&attrs->xform, -b, -b,
                                                        ow + b, oh + b, &local);
                x = pWin->drawable.x - bw + attrs->shadowDx + local.x1;
                y = pWin->drawable.y - bw + attrs->shadowDy + local.y1;
            } else {
                node->shadow = true;
            }
            if (node->shadow) {
                node->shadowBlur = attrs->shadowBlur;
                node->shadowColor = attrs->shadowColor;
                /* in linear light, converted when painted */
                if (!compColorOutputIsSRGB(pScreen) && !compColorLinear(pScreen))
                    node->shadowColor = (attrs->shadowColor & 0xff000000) |
                        (compColorConvertPixel(pScreen,
                                               PictureMatchFormat(pScreen, 24, PICT_x8r8g8b8),
                                               attrs->shadowColor & 0xffffff) & 0xffffff);
                node->shadowBox = (BoxRec) {
                    .x1 = max(x, -32767), .y1 = max(y, -32767),
                    .x2 = min(x + (local.x2 - local.x1), 32767),
                    .y2 = min(y + (local.y2 - local.y1), 32767),
                };
                node->box.x1 = min(node->box.x1, node->shadowBox.x1);
                node->box.y1 = min(node->box.y1, node->shadowBox.y1);
                node->box.x2 = max(node->box.x2, node->shadowBox.x2);
                node->box.y2 = max(node->box.y2, node->shadowBox.y2);
            }
        }

        node->color = compColorState(pWin);

        /* content drawn into the window since the last frame */
        DamagePtr pWinDamage = sceneWindowDamage(pWin);
        if (pWinDamage) {
            RegionPtr r = DamageRegion(pWinDamage);
            if (RegionNotEmpty(r)) {
                RegionTranslate(r, pWin->drawable.x, pWin->drawable.y);
                compColorDamage(pWin, r);
                if (node->transformed)
                    sceneUnionBox(pDamage, &node->box);
                else
                    RegionUnion(pDamage, pDamage, r);
                DamageEmpty(pWinDamage);
            }
        }
        n++;
    }

    /* geometry, opacity, map state; remember where each node was */
    int *oldIndex = n ? calloc(n, sizeof(int)) : NULL;
    for (int i = 0; i < n; i++) {
        int oi;
        const SceneNodeRec *old = sceneFindNode(ss->nodes, ss->numNodes,
                                                nodes[i].id, &oi);
        if (oldIndex)
            oldIndex[i] = -1;
        if (!old) {
            sceneUnionBox(pDamage, &nodes[i].box);
            continue;
        }
        if (old->opacity != nodes[i].opacity ||
            memcmp(&old->box, &nodes[i].box, sizeof(BoxRec)) ||
            old->transformed != nodes[i].transformed ||
            memcmp(&old->xform, &nodes[i].xform, sizeof(PictTransform)) ||
            old->radius != nodes[i].radius || old->shadow != nodes[i].shadow ||
            old->shadowBlur != nodes[i].shadowBlur ||
            /* a shadow has the window's size, so box covers its position */
            old->shadowColor != nodes[i].shadowColor ||
            old->bgBlur != nodes[i].bgBlur || old->color != nodes[i].color) {
            sceneUnionBox(pDamage, &old->box);
            sceneUnionBox(pDamage, &nodes[i].box);
            continue;           /* fully damaged already */
        }
        if (oldIndex)
            oldIndex[i] = oi;
    }

    /* stacking: where two unchanged windows swapped order, only their
       overlap can look different. Windows mapped or unmapped in between
       shift indices without changing anyone's relative order. */
    if (oldIndex) {
        for (int i = 0; i < n; i++) {
            if (oldIndex[i] < 0)
                continue;
            for (int j = i + 1; j < n; j++) {
                if (oldIndex[j] < 0 || oldIndex[j] > oldIndex[i])
                    continue;
                BoxRec both = {
                    .x1 = max(nodes[i].box.x1, nodes[j].box.x1),
                    .y1 = max(nodes[i].box.y1, nodes[j].box.y1),
                    .x2 = min(nodes[i].box.x2, nodes[j].box.x2),
                    .y2 = min(nodes[i].box.y2, nodes[j].box.y2),
                };
                if (both.x1 < both.x2 && both.y1 < both.y2)
                    sceneUnionBox(pDamage, &both);
            }
        }
        free(oldIndex);
    } else if (n) {
        ss->fullDamage = true;  /* out of memory: repaint everything */
    }
    for (int i = 0; i < ss->numNodes; i++) {
        int ni;
        if (!sceneFindNode(nodes, n, ss->nodes[i].id, &ni))
            sceneUnionBox(pDamage, &ss->nodes[i].box);
    }

    /* colour descriptions or the output's colour space changed: the root
       background's conversion with them */
    if (compColorCrtcsChanged(pScreen))
        ss->fullDamage = true;  /* CRTCs showing other spaces moved */
    if (ss->colorSerial != compColorSerial()) {
        ss->colorSerial = compColorSerial();
        ss->bgTileSrc = NULL;
        ss->fullDamage = true;
    }

    /* drawn into the root's tile: its converted copy is stale, paint it
       again (on sRGB outputs that shows when the root is next painted) */
    if (ss->bgTileStale)
        ss->fullDamage = true;

    /* root background (xsetroot, wallpaper setters) */
    if (pRoot->backgroundState != ss->bgState ||
        memcmp(&pRoot->background, &ss->bg, sizeof(PixUnion))) {
        ss->bgState = pRoot->backgroundState;
        ss->bg = pRoot->background;
        ss->bgTileSrc = NULL;   /* a new pixmap may have the old address */
        ss->fullDamage = true;
    }

    *pWins = wins;
    *pNodes = nodes;
    return n;
}

/* ---- painting -------------------------------------------------------- */

static void
sceneTileDamaged(DamagePtr pDamage, RegionPtr pRegion, void *closure)
{
    ((SceneScreenPtr) closure)->bgTileStale = true;
}

static void
sceneTileDamageDestroy(DamagePtr pDamage, void *closure)
{
    SceneScreenPtr ss = closure;

    if (ss->bgTileDamage == pDamage)
        ss->bgTileDamage = NULL;
}

static void
sceneUntrackTile(SceneScreenPtr ss)
{
    DamagePtr d = ss->bgTileDamage;

    ss->bgTileDamage = NULL;
    if (d) {
        DamageUnregister(d);
        DamageDestroy(d);
    }
}

/* The root's tile as the output shows it, converted again when it was
   drawn into; NULL if it cannot. */
static PixmapPtr
sceneConvertedTile(ScreenPtr pScreen, SceneScreenPtr ss, PixmapPtr pTile)
{
    if (ss->bgTileSrc == pTile && !ss->bgTileStale)
        return ss->bgTile;
    sceneUntrackTile(ss);
    if (ss->bgTile)
        dixDestroyPixmap(ss->bgTile, 0);
    ss->bgTile = compColorConvertedCopy(pScreen, pTile,
                                        PictureWindowFormat(pScreen->root));
    ss->bgTileSrc = pTile;
    ss->bgTileStale = false;
    ss->bgTileDamage = DamageCreate(sceneTileDamaged, sceneTileDamageDestroy,
                                    DamageReportRawRegion, FALSE, pScreen, ss);
    if (ss->bgTileDamage)
        DamageRegister(&pTile->drawable, ss->bgTileDamage);
    return ss->bgTile;
}

/*
 * Where layers are painted: the screen, or a scratch picture covering part
 * of it (background blur renders the layers below a window there). ox, oy
 * are the screen coordinates of the picture's origin. linear: a floating
 * point picture in the output's linear light (compColorLinear), and so
 * are the scratch pictures painted for it.
 */
typedef struct {
    PicturePtr pict;
    int ox, oy;
    bool screen;
    bool linear;
} SceneTarget;

/* Clip the target to pRegion (screen coordinates). */
static void
sceneTargetClip(const SceneTarget *t, RegionPtr pRegion)
{
    RegionTranslate(pRegion, -t->ox, -t->oy);
    SetPictureClipRegion(t->pict, 0, 0, pRegion);
    RegionTranslate(pRegion, t->ox, t->oy);
}

/* The outer box of a window, border included, in screen coordinates. */
static BoxRec
sceneOuterBox(WindowPtr pWin)
{
    int bw = wBorderWidth(pWin);
    return (BoxRec) {
        .x1 = pWin->drawable.x - bw, .y1 = pWin->drawable.y - bw,
        .x2 = pWin->drawable.x + pWin->drawable.width + bw,
        .y2 = pWin->drawable.y + pWin->drawable.height + bw,
    };
}

static void
scenePaintRootBackground(ScreenPtr pScreen, const SceneTarget *t, RegionPtr pRegion)
{
    WindowPtr pRoot = pScreen->root;

    if (t->linear && pRoot->backgroundState != BackgroundPixmap) {
        /* a GC's pixel would be 8 bits of linear light: too coarse */
        CARD32 pixel = pRoot->backgroundState == BackgroundPixel ?
            pRoot->background.pixel : pScreen->blackPixel;
        PicturePtr pSrc = compColorLinearSolid(pScreen, PictureWindowFormat(pRoot),
                                               pixel, 1.0);
        if (pSrc) {
            sceneTargetClip(t, pRegion);
            const BoxRec *e = RegionExtents(pRegion);
            CompositePicture(PictOpSrc, pSrc, NULL, t->pict, 0, 0, 0, 0,
                             e->x1 - t->ox, e->y1 - t->oy,
                             e->x2 - e->x1, e->y2 - e->y1);
            FreePicture(pSrc, 0);
        }
        return;
    }

    /* On the screen, the root window's children clip it: draw through
       them, they are painted afterwards. */
    DrawablePtr pDrawable = t->screen ? &pRoot->drawable : t->pict->pDrawable;
    GCPtr pGC = GetScratchGC(pDrawable->depth, pScreen);
    if (!pGC)
        return;
    ChangeGCVal vals[5];
    BITS32 mask;
    if (pRoot->backgroundState == BackgroundPixmap) {
        PixmapPtr pTile = pRoot->background.pixmap;
        SceneScreenPtr ss = sceneScreen(pScreen);
        /* on outputs other than sRGB, a converted copy */
        if (!compColorOutputIsSRGB(pScreen)) {
            if (sceneConvertedTile(pScreen, ss, pTile)) {
                pTile = ss->bgTile;
            } else if (t->linear) {
                FreeScratchGC(pGC);     /* not of the buffer's depth */
                return;
            }
        }
        mask = GCFillStyle | GCTile | GCTileStipXOrigin | GCTileStipYOrigin;
        vals[0].val = FillTiled;
        vals[1].ptr = pTile;
        vals[2].val = pRoot->drawable.x - t->ox;
        vals[3].val = pRoot->drawable.y - t->oy;
    } else {
        mask = GCForeground;
        vals[0].val = compColorConvertPixel(pScreen, PictureWindowFormat(pRoot),
                                            pRoot->backgroundState == BackgroundPixel ?
                                            pRoot->background.pixel : pScreen->blackPixel);
    }
    ChangeGC(NULL, pGC, mask, vals);
    if (t->screen) {
        vals[0].val = IncludeInferiors;
        ChangeGC(NULL, pGC, GCSubwindowMode, vals);
    }
    ValidateGC(pDrawable, pGC);

    int n = RegionNumRects(pRegion);
    BoxPtr boxes = RegionRects(pRegion);
    xRectangle *rects = calloc(n ? n : 1, sizeof(xRectangle));
    if (rects) {
        for (int k = 0; k < n; k++)
            rects[k] = (xRectangle) {
                .x = boxes[k].x1 - t->ox, .y = boxes[k].y1 - t->oy,
                .width = boxes[k].x2 - boxes[k].x1,
                .height = boxes[k].y2 - boxes[k].y1,
            };
        (*pGC->ops->PolyFillRect) (pDrawable, pGC, n, rects);
        free(rects);
    }
    FreeScratchGC(pGC);
}

static void scenePaintShadowTransformed(ScreenPtr pScreen, const SceneTarget *t,
                                        WindowPtr pWin, SceneAttrsPtr attrs,
                                        const SceneNodeRec *node, RegionPtr pRegion);

/* A premultiplied solid picture of an ARGB colour (not premultiplied). */
static PicturePtr
sceneSolidARGB(CARD32 c)
{
    int error;
    CARD16 a = (c >> 24) * 0x101;
    xRenderColor color = {
        .red = ((c >> 16) & 0xff) * a / 0xff,
        .green = ((c >> 8) & 0xff) * a / 0xff,
        .blue = (c & 0xff) * a / 0xff,
        .alpha = a,
    };
    return CreateSolidPicture(0, &color, &error);
}

/* The source of a shadow of colour c (ARGB, not premultiplied) on t */
static PicturePtr
sceneShadowSource(ScreenPtr pScreen, const SceneTarget *t, CARD32 c)
{
    if (t->linear)
        return compColorLinearSolid(pScreen, PictureMatchFormat(pScreen, 24, PICT_x8r8g8b8),
                                    c & 0xffffff, (c >> 24) / 255.0);
    return sceneSolidARGB(c);
}

static void
scenePaintShadow(ScreenPtr pScreen, const SceneTarget *t, WindowPtr pWin,
                 SceneAttrsPtr attrs, const SceneNodeRec *node, RegionPtr pRegion)
{
    if (node->transformed) {
        scenePaintShadowTransformed(pScreen, t, pWin, attrs, node, pRegion);
        return;
    }

    int bw = wBorderWidth(pWin);
    PicturePtr pMask = sceneShadowMask(pScreen, attrs,
                                       pWin->drawable.width + 2 * bw,
                                       pWin->drawable.height + 2 * bw);
    if (!pMask)
        return;

    RegionRec clip, box;
    RegionInit(&box, (BoxPtr) &node->shadowBox, 1);
    RegionNull(&clip);
    RegionIntersect(&clip, pRegion, &box);

    /* Nothing of the shadow shows through the window itself, even when
       it is translucent. Only the corner squares keep it, where rounding
       cuts the window away. */
    BoxRec o = sceneOuterBox(pWin);
    int r = min(node->radius, min(o.x2 - o.x1, o.y2 - o.y1) / 2);
    BoxRec cross[2] = {
        { .x1 = o.x1 + r, .y1 = o.y1, .x2 = o.x2 - r, .y2 = o.y2 },
        { .x1 = o.x1, .y1 = o.y1 + r, .x2 = o.x2, .y2 = o.y2 - r },
    };
    RegionPtr pBody = RegionCreate(NullBox, 0);
    if (pBody) {
        RegionRec part;
        for (int k = 0; k < 2; k++) {
            if (cross[k].x1 >= cross[k].x2 || cross[k].y1 >= cross[k].y2)
                continue;
            RegionInit(&part, &cross[k], 1);
            RegionUnion(pBody, pBody, &part);
            RegionUninit(&part);
        }
        RegionIntersect(pBody, pBody, &pWin->borderSize);
        RegionSubtract(&clip, &clip, pBody);
        RegionDestroy(pBody);
    }
    if (RegionNotEmpty(&clip)) {
        PicturePtr pSrc = sceneShadowSource(pScreen, t, node->shadowColor);
        if (pSrc) {
            sceneTargetClip(t, &clip);
            const BoxRec *b = &node->shadowBox;
            CompositePicture(PictOpOver, pSrc, pMask, t->pict, 0, 0, 0, 0,
                             b->x1 - t->ox, b->y1 - t->oy,
                             b->x2 - b->x1, b->y2 - b->y1);
            FreePicture(pSrc, 0);
        }
    }
    RegionUninit(&clip);
    RegionUninit(&box);
}

static void
scenePaintWindow(ScreenPtr pScreen, const SceneTarget *t, WindowPtr pWin,
                 SceneAttrsPtr attrs, const SceneNodeRec *node, RegionPtr pRegion)
{
    int error;
    RegionRec clip;

    RegionNull(&clip);
    if (node->transformed) {
        RegionRec shown;
        RegionInit(&shown, (BoxPtr) &node->box, 1);
        RegionIntersect(&clip, pRegion, &shown);
        RegionUninit(&shown);
    } else {
        RegionIntersect(&clip, pRegion, &pWin->borderSize);
    }
    if (!RegionNotEmpty(&clip)) {
        RegionUninit(&clip);
        return;
    }

    PixmapPtr pPixmap = (*pScreen->GetWindowPixmap) (pWin);
    /* converted for the output, if its colour space asks for it */
    PixmapPtr pShown = compColorSource(pWin, pPixmap);
    /* in linear light, a floating point copy */
    PictFormatPtr format = pShown != pPixmap && t->linear ?
        compColorFloatFormat(pScreen) : PictureWindowFormat(pWin);
    PicturePtr pSrc = CreatePicture(0, &pShown->drawable, format, 0, NULL,
                                    serverClient, &error);
    if (!pSrc) {
        RegionUninit(&clip);
        return;
    }

    PicturePtr pMask = NULL;
    bool ownMask = false;
    if (node->radius) {
        /* cached; owned by attrs */
        pMask = sceneCornerMask(pScreen, &attrs->corner, node->radius,
                                pPixmap->drawable.width,
                                pPixmap->drawable.height,
                                node->opacity >> 24);
        if (pMask && node->transformed) {
            SetPictureTransform(pMask, &attrs->inverse);
            SetPictureFilter(pMask, (char *) "bilinear", 8, NULL, 0);
        } else if (pMask) {
            SetPictureTransform(pMask, NULL);
        }
    } else if (node->opacity != 0xffffffff) {
        xRenderColor alpha = { .alpha = node->opacity >> 16 };
        pMask = CreateSolidPicture(0, &alpha, &error);
        ownMask = true;
    }

    sceneTargetClip(t, &clip);
    if (node->transformed) {
        /* Render maps destination to source: give it the inverse */
        SetPictureTransform(pSrc, &attrs->inverse);
        SetPictureFilter(pSrc, (char *) "bilinear", 8, NULL, 0);
        const BoxRec *b = &node->box;
        CompositePicture(PictOpOver, pSrc, pMask, t->pict,
                         b->x1 - pPixmap->screen_x, b->y1 - pPixmap->screen_y,
                         b->x1 - pPixmap->screen_x, b->y1 - pPixmap->screen_y,
                         b->x1 - t->ox, b->y1 - t->oy,
                         b->x2 - b->x1, b->y2 - b->y1);
    } else {
        CompositePicture(PictOpOver, pSrc, pMask, t->pict,
                         0, 0, 0, 0,
                         pPixmap->screen_x - t->ox, pPixmap->screen_y - t->oy,
                         pPixmap->drawable.width, pPixmap->drawable.height);
    }
    if (pMask && ownMask)
        FreePicture(pMask, 0);
    FreePicture(pSrc, 0);
    RegionUninit(&clip);
}

/* ---- background blur (0.3) -------------------------------------------- */

/* Every box of pRegion grown by e on each side, added to pRegion. */
static void
sceneGrowRegion(RegionPtr pRegion, int e)
{
    int n = RegionNumRects(pRegion);
    BoxPtr rects = RegionRects(pRegion);
    BoxPtr grown = calloc(n ? n : 1, sizeof(BoxRec));

    if (!grown)
        return;
    for (int k = 0; k < n; k++)
        grown[k] = (BoxRec) {
            .x1 = max(rects[k].x1 - e, -32767), .y1 = max(rects[k].y1 - e, -32767),
            .x2 = min(rects[k].x2 + e, 32767), .y2 = min(rects[k].y2 + e, 32767),
        };
    for (int k = 0; k < n; k++)
        sceneUnionBox(pRegion, &grown[k]);
    free(grown);
}

/*
 * A blurred backdrop pixel depends on everything below the window within
 * the blur radius. So where anything below changed within the radius of a
 * blurring window, its blur changes too, within the radius of the change.
 * Going up the stack, a window's changed blur is in turn a change below
 * the windows above it.
 */
static void
sceneExpandForBlur(const SceneNodeRec *nodes, WindowPtr *wins, int n,
                   RegionPtr pDamage)
{
    for (int i = 0; i < n; i++) {
        if (!nodes[i].bgBlur)
            continue;
        int r = nodes[i].bgBlur;
        BoxRec box = sceneOuterBox(wins[i]);
        BoxRec reach = { box.x1 - r, box.y1 - r, box.x2 + r, box.y2 + r };
        RegionRec part, win;
        RegionInit(&part, &reach, 1);
        RegionIntersect(&part, &part, pDamage);
        if (RegionNotEmpty(&part)) {
            sceneGrowRegion(&part, r);
            RegionInit(&win, &box, 1);
            RegionIntersect(&part, &part, &win);
            RegionUnion(pDamage, pDamage, &part);
            RegionUninit(&win);
        }
        RegionUninit(&part);
    }
}

/* Separable Gaussian kernel for Render's convolution filter. */
static xFixed *
sceneGaussKernel(int radius, bool horizontal, int *nparams)
{
    int size = 2 * radius + 1;
    xFixed *params = calloc(size + 2, sizeof(xFixed));
    double *w = calloc(size, sizeof(double));
    if (!params || !w) {
        free(params);
        free(w);
        return NULL;
    }
    double sigma = radius / 2.0, sum = 0;
    for (int k = 0; k < size; k++) {
        double d = k - radius;
        w[k] = exp(-d * d / (2 * sigma * sigma));
        sum += w[k];
    }
    params[0] = IntToxFixed(horizontal ? size : 1);
    params[1] = IntToxFixed(horizontal ? 1 : size);
    for (int k = 0; k < size; k++)
        params[2 + k] = pixman_double_to_fixed(w[k] / sum);
    free(w);
    *nparams = size + 2;
    return params;
}

/* linear: a floating point picture, format ignored */
static PicturePtr
sceneScratchPicture(ScreenPtr pScreen, int w, int h, PictFormatPtr format, bool linear)
{
    int error;
    if (linear)
        format = compColorFloatFormat(pScreen);
    PixmapPtr pPixmap = linear ? compColorFloatPixmap(pScreen, w, h) :
        (*pScreen->CreatePixmap) (pScreen, max(w, 1), max(h, 1), format->depth, 0);
    if (!pPixmap)
        return NULL;
    XID repeat = RepeatPad;
    PicturePtr pPicture = CreatePicture(0, &pPixmap->drawable, format,
                                        CPRepeat, &repeat, serverClient, &error);
    dixDestroyPixmap(pPixmap, 0);
    return pPicture;
}

/*
 * The shadow of a transformed window: its shadow shape shown through the
 * window's transform and moved by dx, dy on the screen, minus the
 * window's own shape (nothing of the shadow shows through the window,
 * as for untransformed windows), in the shadow colour.
 */
static void
scenePaintShadowTransformed(ScreenPtr pScreen, const SceneTarget *t, WindowPtr pWin,
                            SceneAttrsPtr attrs, const SceneNodeRec *node,
                            RegionPtr pRegion)
{
    int bw = wBorderWidth(pWin);
    int ow = pWin->drawable.width + 2 * bw, oh = pWin->drawable.height + 2 * bw;
    int b = attrs->shadowBlur;
    const BoxRec *sb = &node->shadowBox;
    int sw = sb->x2 - sb->x1, sh = sb->y2 - sb->y1;
    int ox = pWin->drawable.x - bw, oy = pWin->drawable.y - bw;

    RegionRec clip, box;
    RegionInit(&box, (BoxPtr) sb, 1);
    RegionNull(&clip);
    RegionIntersect(&clip, pRegion, &box);
    RegionUninit(&box);
    if (!RegionNotEmpty(&clip) || sw <= 0 || sh <= 0) {
        RegionUninit(&clip);
        return;
    }

    PicturePtr pShape = sceneShadowMask(pScreen, attrs, ow, oh);
    PicturePtr pBody = sceneCornerMask(pScreen, &attrs->backdrop, node->radius,
                                       ow, oh, 0xff);
    PictFormatPtr a8 = PictureMatchFormat(pScreen, 8, PICT_a8);
    PicturePtr pAlpha = a8 ? sceneScratchPicture(pScreen, sw, sh, a8, false) : NULL;
    PicturePtr pSrc = sceneShadowSource(pScreen, t, node->shadowColor);

    if (pShape && pBody && pAlpha && pSrc) {
        /* screen, relative to the window's outer corner -> shadow mask:
           undo dx, dy, undo the transform, then the mask's blur margin */
        PictTransform shift, unshift, tmp, toShape;
        pixman_transform_init_translate(&shift, IntToxFixed(-attrs->shadowDx),
                                        IntToxFixed(-attrs->shadowDy));
        pixman_transform_init_translate(&unshift, IntToxFixed(b), IntToxFixed(b));
        pixman_transform_multiply(&tmp, &attrs->inverse, &shift);
        pixman_transform_multiply(&toShape, &unshift, &tmp);

        SetPictureTransform(pShape, &toShape);
        SetPictureFilter(pShape, (char *) "bilinear", 8, NULL, 0);
        CompositePicture(PictOpSrc, pShape, NULL, pAlpha,
                         sb->x1 - ox, sb->y1 - oy, 0, 0, 0, 0, sw, sh);
        SetPictureTransform(pShape, NULL);
        SetPictureFilter(pShape, (char *) "nearest", 7, NULL, 0);

        SetPictureTransform(pBody, &attrs->inverse);
        SetPictureFilter(pBody, (char *) "bilinear", 8, NULL, 0);
        CompositePicture(PictOpOutReverse, pBody, NULL, pAlpha,
                         sb->x1 - ox, sb->y1 - oy, 0, 0, 0, 0, sw, sh);
        SetPictureTransform(pBody, NULL);
        SetPictureFilter(pBody, (char *) "nearest", 7, NULL, 0);

        sceneTargetClip(t, &clip);
        CompositePicture(PictOpOver, pSrc, pAlpha, t->pict, 0, 0, 0, 0,
                         sb->x1 - t->ox, sb->y1 - t->oy, sw, sh);
    }
    if (pAlpha)
        FreePicture(pAlpha, 0);
    if (pSrc)
        FreePicture(pSrc, 0);
    RegionUninit(&clip);
}

static void
sceneScaleTransform(PictTransform *t, xFixed scale)
{
    memset(t, 0, sizeof(*t));
    t->matrix[0][0] = scale;
    t->matrix[1][1] = scale;
    t->matrix[2][2] = xFixed1;
}

/* Gaussian blur of pPict (w x h, linear: floating point) by radius r, in
   place. */
static void
sceneBlurPicture(ScreenPtr pScreen, PicturePtr pPict, int w, int h, int r, bool linear)
{
    int nh = 0, nv = 0;
    xFixed *kh = sceneGaussKernel(r, true, &nh);
    xFixed *kv = sceneGaussKernel(r, false, &nv);
    PicturePtr pTmp = sceneScratchPicture(pScreen, w, h, pPict->pFormat, linear);

    if (kh && kv && pTmp) {
        SetPictureFilter(pPict, (char *) "convolution", 11, kh, nh);
        CompositePicture(PictOpSrc, pPict, NULL, pTmp, 0, 0, 0, 0, 0, 0, w, h);
        SetPictureFilter(pTmp, (char *) "convolution", 11, kv, nv);
        CompositePicture(PictOpSrc, pTmp, NULL, pPict, 0, 0, 0, 0, 0, 0, w, h);
        SetPictureFilter(pPict, (char *) "nearest", 7, NULL, 0);
    }
    free(kh);
    free(kv);
    if (pTmp)
        FreePicture(pTmp, 0);
}

static void sceneRenderLayers(ScreenPtr pScreen, const SceneTarget *t,
                              WindowPtr *wins, const SceneNodeRec *nodes,
                              int upto, RegionPtr pRegion, int depth);

/* Larger radii are blurred at a reduced scale: each pass then needs at
   most this many taps on either side. */
#define SCENE_BLUR_TAPS 4
/* Blurring windows seen through blurring windows: how deep to recurse. */
#define SCENE_BLUR_DEPTH 3

/*
 * Draw window i's backdrop, blurred, within pRegion: the layers below it
 * are rendered into a scratch picture (never read back from the screen,
 * which also holds the layers above), reduced by halving for large radii,
 * blurred, and scaled back up onto the target.
 */
static void
scenePaintBackdrop(ScreenPtr pScreen, const SceneTarget *t, WindowPtr *wins,
                   const SceneNodeRec *nodes, int i, RegionPtr pRegion, int depth)
{
    WindowPtr pWin = wins[i], pRoot = pScreen->root;
    SceneAttrsPtr attrs = sceneAttrs(pWin);
    const SceneNodeRec *node = &nodes[i];
    BoxRec o = sceneOuterBox(pWin);
    /* where the blurred backdrop goes: a transformed window's shown
       bounds, cut to its shape by the mask below */
    BoxRec b = node->transformed ? node->shown : o;
    const BoxRec *scr = RegionExtents(&pRoot->winSize);
    int r = node->bgBlur;

    RegionRec clip;
    RegionNull(&clip);
    if (node->transformed) {
        RegionRec shown;
        RegionInit(&shown, &b, 1);
        RegionIntersect(&clip, pRegion, &shown);
        RegionUninit(&shown);
    } else {
        RegionIntersect(&clip, pRegion, &pWin->borderSize);
    }
    if (!RegionNotEmpty(&clip)) {
        RegionUninit(&clip);
        return;
    }

    /* what the blur reads: the clip and the radius around it */
    const BoxRec *ext = RegionExtents(&clip);
    BoxRec e = {
        .x1 = max(ext->x1 - r, scr->x1), .y1 = max(ext->y1 - r, scr->y1),
        .x2 = min(ext->x2 + r, scr->x2), .y2 = min(ext->y2 + r, scr->y2),
    };
    int ew = e.x2 - e.x1, eh = e.y2 - e.y1;
    PictFormatPtr format = PictureWindowFormat(pRoot);
    PicturePtr levels[8] = { 0 };
    int lw[8], lh[8], nlevels = 0;

    levels[0] = sceneScratchPicture(pScreen, ew, eh, format, t->linear);
    if (!levels[0])
        goto out;
    lw[0] = ew;
    lh[0] = eh;
    nlevels = 1;

    SceneTarget below = {
        .pict = levels[0], .ox = e.x1, .oy = e.y1, .linear = t->linear,
    };
    RegionRec area;
    RegionInit(&area, &e, 1);
    sceneRenderLayers(pScreen, &below, wins, nodes, i, &area, depth + 1);
    RegionUninit(&area);
    SetPictureClipRegion(levels[0], 0, 0, NULL);

    /* halve until the radius fits; bilinear sampling at 2x is an exact
       2x2 average */
    int scale = 1;
    PictTransform half;
    sceneScaleTransform(&half, IntToxFixed(2));
    while (r > SCENE_BLUR_TAPS * scale && nlevels < 8) {
        int k = nlevels;
        lw[k] = (lw[k - 1] + 1) / 2;
        lh[k] = (lh[k - 1] + 1) / 2;
        levels[k] = sceneScratchPicture(pScreen, lw[k], lh[k], format, t->linear);
        if (!levels[k])
            goto out;
        SetPictureTransform(levels[k - 1], &half);
        SetPictureFilter(levels[k - 1], (char *) "bilinear", 8, NULL, 0);
        CompositePicture(PictOpSrc, levels[k - 1], NULL, levels[k],
                         0, 0, 0, 0, 0, 0, lw[k], lh[k]);
        nlevels++;
        scale *= 2;
    }
    PicturePtr pSmall = levels[nlevels - 1];
    sceneBlurPicture(pScreen, pSmall, lw[nlevels - 1], lh[nlevels - 1],
                     max(1, (r + scale / 2) / scale), t->linear);

    PicturePtr pMask = NULL;
    if (node->radius || node->transformed) {
        pMask = sceneCornerMask(pScreen, &attrs->backdrop, node->radius,
                                o.x2 - o.x1, o.y2 - o.y1, 0xff);
        if (!pMask)
            goto out;
        if (node->transformed) {
            SetPictureTransform(pMask, &attrs->inverse);
            SetPictureFilter(pMask, (char *) "bilinear", 8, NULL, 0);
        }
    }
    PictTransform up;
    sceneScaleTransform(&up, xFixed1 / scale);
    SetPictureTransform(pSmall, scale > 1 ? &up : NULL);
    SetPictureFilter(pSmall, (char *) "bilinear", 8, NULL, 0);
    sceneTargetClip(t, &clip);
    CompositePicture(pMask ? PictOpOver : PictOpSrc, pSmall, pMask, t->pict,
                     b.x1 - e.x1, b.y1 - e.y1, b.x1 - o.x1, b.y1 - o.y1,
                     b.x1 - t->ox, b.y1 - t->oy, b.x2 - b.x1, b.y2 - b.y1);
    if (pMask && node->transformed) {
        SetPictureTransform(pMask, NULL);
        SetPictureFilter(pMask, (char *) "nearest", 7, NULL, 0);
    }
out:
    for (int k = 0; k < nlevels; k++)
        FreePicture(levels[k], 0);
    RegionUninit(&clip);
}

/* Paint the root background and windows [0, upto) within pRegion. */
static void
sceneRenderLayers(ScreenPtr pScreen, const SceneTarget *t, WindowPtr *wins,
                  const SceneNodeRec *nodes, int upto, RegionPtr pRegion,
                  int depth)
{
    const BoxRec *ext = RegionExtents(pRegion);

    scenePaintRootBackground(pScreen, t, pRegion);
    for (int i = 0; i < upto; i++) {
        WindowPtr pWin = wins[i];
        SceneAttrsPtr attrs = sceneAttrs(pWin);
        const BoxRec *nb = &nodes[i].box;

        if (nb->x2 <= ext->x1 || nb->x1 >= ext->x2 ||
            nb->y2 <= ext->y1 || nb->y1 >= ext->y2)
            continue;
        if (nodes[i].shadow)
            scenePaintShadow(pScreen, t, pWin, attrs, &nodes[i], pRegion);
        if (nodes[i].bgBlur && depth < SCENE_BLUR_DEPTH)
            scenePaintBackdrop(pScreen, t, wins, nodes, i, pRegion, depth);
        scenePaintWindow(pScreen, t, pWin, attrs, &nodes[i], pRegion);
    }
}

static void
scenePaint(ScreenPtr pScreen, WindowPtr *wins, const SceneNodeRec *nodes,
           int n, RegionPtr pDamage)
{
    WindowPtr pRoot = pScreen->root;
    int error;

    RegionIntersect(pDamage, pDamage, &pRoot->winSize);
    if (!RegionNotEmpty(pDamage))
        return;

    XID subwindowMode = IncludeInferiors;
    PicturePtr pDst = CreatePicture(0, &pRoot->drawable,
                                    PictureWindowFormat(pRoot),
                                    CPSubwindowMode, &subwindowMode,
                                    serverClient, &error);
    if (!pDst)
        return;
    SceneTarget screen = {
        .pict = pDst, .ox = pRoot->drawable.x, .oy = pRoot->drawable.y,
        .screen = true,
    };

    /* in linear light: blend in a floating point buffer over the damage,
       then encode it for the output */
    const BoxRec *e = RegionExtents(pDamage);
    PixmapPtr pLin = compColorLinear(pScreen) ?
        compColorFloatPixmap(pScreen, e->x2 - e->x1, e->y2 - e->y1) : NULL;
    PicturePtr pLinPict = pLin ?
        CreatePicture(0, &pLin->drawable, compColorFloatFormat(pScreen), 0, NULL,
                      serverClient, &error) : NULL;
    if (pLinPict) {
        SceneTarget lin = {
            .pict = pLinPict, .ox = e->x1, .oy = e->y1, .linear = true,
        };
        sceneRenderLayers(pScreen, &lin, wins, nodes, n, pDamage, 0);
        compColorEncodeLinear(pScreen, pLin, e->x1, e->y1, pDst,
                              pRoot->drawable.x, pRoot->drawable.y, pDamage);
        FreePicture(pLinPict, 0);
    } else {
        sceneRenderLayers(pScreen, &screen, wins, nodes, n, pDamage, 0);
        compColorEncodeCrtcs(pScreen, pDst, pRoot->drawable.x, pRoot->drawable.y,
                             pDamage);
    }
    if (pLin)
        dixDestroyPixmap(pLin, 0);
    FreePicture(pDst, 0);
}

/* ---- activation ------------------------------------------------------ */

/*
 * A client drawing onto the root window (IncludeInferiors draws over the
 * top-level windows too) expects the windows it draws over to be on the
 * screen already: compose pending changes first, as before reads. Damage
 * reports before the drawing is done. Painting a frame reports here too;
 * sceneFrame ignores that.
 */
static void
sceneRootDrawReport(DamagePtr pDamage, RegionPtr pRegion, void *closure)
{
    compSceneValidate((ScreenPtr) closure);
}

/*
 * Drawing on the root window with IncludeInferiors draws over its
 * children. Unredirected, that lands in the children's contents (they
 * are on the screen); here their contents are in their own pixmaps. So
 * copy what was drawn over each top-level window's visible part into its
 * pixmap, as if it had been drawn there. With ClipByChildren nothing is
 * drawn over the children: they clip the root (compSceneOwnsRedirection).
 */
static void
sceneRootDrawnReport(DamagePtr pDamage, RegionPtr pRegion, void *closure)
{
    ScreenPtr pScreen = closure;
    WindowPtr pRoot = pScreen->root;
    PixmapPtr pScreenPixmap = (*pScreen->GetScreenPixmap) (pScreen);

    if (sceneInFrame || !RegionNotEmpty(pRegion))
        return;
    /* reading the screen below must not compose a frame first: that would
       repaint the windows over what was just drawn */
    sceneInFrame = true;

    RegionRec covered, part;
    RegionNull(&covered);
    RegionNull(&part);
    for (WindowPtr pWin = pRoot->firstChild; pWin; pWin = pWin->nextSib) {
        if (!pWin->viewable || pWin->drawable.class == InputOnly)
            continue;
        RegionSubtract(&part, pRegion, &covered);
        RegionIntersect(&part, &part, &pWin->borderSize);
        RegionUnion(&covered, &covered, &pWin->borderSize);
        PixmapPtr pPixmap = (*pScreen->GetWindowPixmap) (pWin);
        if (!RegionNotEmpty(&part) || pPixmap == pScreenPixmap)
            continue;

        GCPtr pGC = GetScratchGC(pPixmap->drawable.depth, pScreen);
        if (!pGC)
            continue;
        ChangeGCVal val = { .val = IncludeInferiors };
        ChangeGC(NULL, pGC, GCSubwindowMode, &val);
        ValidateGC(&pPixmap->drawable, pGC);
        int n = RegionNumRects(&part);
        BoxPtr b = RegionRects(&part);
        for (int k = 0; k < n; k++)
            (*pGC->ops->CopyArea) (&pRoot->drawable, &pPixmap->drawable, pGC,
                                   b[k].x1 - pRoot->drawable.x,
                                   b[k].y1 - pRoot->drawable.y,
                                   b[k].x2 - b[k].x1, b[k].y2 - b[k].y1,
                                   b[k].x1 - pPixmap->screen_x,
                                   b[k].y1 - pPixmap->screen_y);
        FreeScratchGC(pGC);
    }
    RegionUninit(&part);
    RegionUninit(&covered);
    sceneInFrame = false;
}

static void
sceneRootDamageDestroy(DamagePtr pDamage, void *closure)
{
    SceneScreenPtr ss = sceneScreen((ScreenPtr) closure);

    if (ss && ss->rootDamage == pDamage)
        ss->rootDamage = NULL;
    if (ss && ss->rootDrawnDamage == pDamage)
        ss->rootDrawnDamage = NULL;
}

static void
sceneDeactivate(ScreenPtr pScreen, SceneScreenPtr ss)
{
    if (!ss->active)
        return;
    DamagePtr damages[2] = { ss->rootDamage, ss->rootDrawnDamage };
    ss->rootDamage = ss->rootDrawnDamage = NULL;
    for (int k = 0; k < 2; k++) {
        if (damages[k]) {
            DamageUnregister(damages[k]);
            DamageDestroy(damages[k]);
        }
    }
    for (WindowPtr pWin = pScreen->root->firstChild; pWin; pWin = pWin->nextSib)
        sceneDropWindowDamage(pWin);
    ss->active = false;
    ss->direct = None;
    free(ss->nodes);
    ss->nodes = NULL;
    ss->numNodes = 0;
    compUnredirectSubwindows(serverClient, pScreen->root, CompositeRedirectManual);
}

static void
sceneActivate(ScreenPtr pScreen, SceneScreenPtr ss)
{
    if (ss->active || ss->yielded || !pScreen->root)
        return;
    /* active first: the clips computed while redirecting depend on it
       (compSceneOwnsRedirection) */
    ss->active = true;
    if (compRedirectSubwindows(serverClient, pScreen->root,
                               CompositeRedirectManual) != Success) {
        ss->active = false;
        return;
    }
    ss->fullDamage = true;
    ss->bgState = -1;
    ss->rootDamage = DamageCreate(sceneRootDrawReport, sceneRootDamageDestroy,
                                  DamageReportRawRegion, TRUE, pScreen, pScreen);
    if (ss->rootDamage)
        DamageRegister(&pScreen->root->drawable, ss->rootDamage);
    /* not internal: the software cursor (misprite draws with
       DamageDrawInternal) is not drawing over the windows */
    ss->rootDrawnDamage = DamageCreate(sceneRootDrawnReport, sceneRootDamageDestroy,
                                       DamageReportRawRegion, FALSE, pScreen,
                                       pScreen);
    if (ss->rootDrawnDamage) {
        DamageSetReportAfterOp(ss->rootDrawnDamage, TRUE);
        DamageRegister(&pScreen->root->drawable, ss->rootDrawnDamage);
    }
}

/* ---- direct scanout ----------------------------------------------------- */

/*
 * An opaque, untransformed window that covers the whole screen and has
 * nothing above it needs no composition: unredirect it so it renders
 * straight to the screen, which on real hardware also lets Present flip
 * its buffers. It goes back to being composited as soon as that stops
 * being true.
 */
static bool
sceneCanScanOut(WindowPtr pWin)
{
    WindowPtr pRoot = pWin->drawable.pScreen->root;
    SceneAttrsPtr attrs = sceneAttrs(pWin);

    if (attrs->mask & (XpSceneNodeTransform | XpSceneNodeDecoration))
        return false;
    if (!compColorShownAsIs(pWin))
        return false;           /* its pixels need converting */
    CARD32 opacity = (attrs->mask & XpSceneNodeOpacity) ?
        attrs->opacity : sceneWindowOpacity(pWin);
    if (opacity != 0xffffffff)
        return false;
    PictFormatPtr format = PictureWindowFormat(pWin);
    if (!format || format->direct.alphaMask)
        return false;
    if (RegionNumRects(&pWin->borderSize) != 1)
        return false;           /* shaped */
    const BoxRec *b = RegionExtents(&pWin->borderSize);
    const BoxRec *r = RegionExtents(&pRoot->winSize);
    return b->x1 <= r->x1 && b->y1 <= r->y1 && b->x2 >= r->x2 && b->y2 >= r->y2;
}

/* The window that should be shown directly now, or NULL. */
static WindowPtr
sceneWantDirect(ScreenPtr pScreen)
{
    for (WindowPtr pWin = pScreen->root->firstChild; pWin; pWin = pWin->nextSib)
        if (pWin->viewable && pWin->drawable.class == InputOutput)
            return sceneCanScanOut(pWin) ? pWin : NULL;
    return NULL;
}

/* Start or end direct scanout. Starting it frees the window's pixmap, so
   only the block handler does that (allowStart); ending it allocates one
   and is also done before composing on demand. */
static void
sceneUpdateDirect(ScreenPtr pScreen, SceneScreenPtr ss, bool allowStart)
{
    WindowPtr want = sceneWantDirect(pScreen);

    if (ss->direct != None && (!want || want->drawable.id != ss->direct)) {
        WindowPtr old;
        if (dixLookupWindow(&old, ss->direct, serverClient, DixUnknownAccess) == Success &&
            old->parent == pScreen->root)
            compRedirectWindow(serverClient, old, CompositeRedirectManual);
        ss->direct = None;
        ss->fullDamage = true;
    }
    if (allowStart && want && ss->direct == None &&
        want->redirectDraw != RedirectDrawNone &&
        compUnredirectWindow(serverClient, want, CompositeRedirectManual) == Success &&
        want->redirectDraw == RedirectDrawNone)
        ss->direct = want->drawable.id;
}

/* ---- frame pacing ----------------------------------------------------- */

static bool scenePacingOff;     /* XPRISM_NO_FRAME_PACING */

/* An output's refresh period in microseconds; 60 Hz if its mode carries
   no timing (Xvfb's modes, for one). */
static CARD64
scenePeriod(RRCrtcPtr crtc)
{
    xRRModeInfo *m = &crtc->mode->mode;

    if (m->dotClock && m->hTotal && m->vTotal) {
        CARD64 period = (CARD64) m->hTotal * m->vTotal * 1000000 / m->dotClock;
        if (m->modeFlags & RR_DoubleScan)
            period *= 2;
        if (m->modeFlags & RR_Interlace)
            period /= 2;
        if (period >= 1000)     /* up to 1000 Hz */
            return period;
    }
    return 1000000 / 60;
}

static CARD64 *
scenePacedLast(SceneScreenPtr ss, RRCrtcPtr crtc)
{
    int free_slot = -1;

    for (int i = 0; i < SCENE_MAX_PACED; i++) {
        if (ss->paced[i].crtc == crtc)
            return &ss->paced[i].last;
        if (!ss->paced[i].crtc && free_slot < 0)
            free_slot = i;
    }
    if (free_slot < 0)
        return NULL;
    ss->paced[free_slot].crtc = crtc;
    ss->paced[free_slot].last = 0;
    return &ss->paced[free_slot].last;
}

static bool
sceneCrtcBox(RRCrtcPtr crtc, BoxPtr box)
{
    int w, h;

    if (!crtc->mode || !crtc->numOutputs)
        return false;
    RRCrtcGetScanoutSize(crtc, &w, &h);
    *box = (BoxRec) { crtc->x, crtc->y, crtc->x + w, crtc->y + h };
    return true;
}

/*
 * Split a frame's damage by output. Damage on an output that was composed
 * less than a refresh period ago is kept for later, and the block handler
 * makes the server wake up when that output is due; the rest is composed
 * now. Damage that
 * no output shows, or that an output which is due also shows, is never
 * held back. Frames composed on demand (all) compose everything: a client
 * reads or draws on the root window right after.
 */
static void
scenePace(ScreenPtr pScreen, SceneScreenPtr ss, RegionPtr damage, bool all)
{
    rrScrPrivPtr pScrPriv = dixPrivateKeyRegistered(rrPrivKey) ?
        rrGetScrPriv(pScreen) : NULL;
    CARD64 now = GetTimeInMicros(), due = 0;
    RegionRec waiting, part;
    BoxRec box;

    RegionUnion(damage, damage, &ss->deferred);
    RegionEmpty(&ss->deferred);
    if (!pScrPriv || !RegionNotEmpty(damage))
        return;

    RegionNull(&waiting);
    RegionNull(&part);
    for (int i = 0; i < pScrPriv->numCrtcs; i++) {
        RRCrtcPtr crtc = pScrPriv->crtcs[i];

        if (!sceneCrtcBox(crtc, &box))
            continue;
        RegionReset(&part, &box);
        RegionIntersect(&part, &part, damage);
        if (!RegionNotEmpty(&part))
            continue;

        CARD64 *last = scenePacedLast(ss, crtc);
        CARD64 period = scenePeriod(crtc);
        /* a little early is fine: the timer has millisecond precision */
        if (!last || all || scenePacingOff || now - *last + 1000 >= period) {
            if (last)
                *last = now;
            continue;
        }
        RegionUnion(&waiting, &waiting, &part);
        if (!due || *last + period < due)
            due = *last + period;
    }
    /* what an output composed now also shows is composed with it */
    for (int i = 0; i < pScrPriv->numCrtcs && RegionNotEmpty(&waiting); i++) {
        RRCrtcPtr crtc = pScrPriv->crtcs[i];
        CARD64 *last;

        if (!sceneCrtcBox(crtc, &box) || !(last = scenePacedLast(ss, crtc)) ||
            *last != now)
            continue;
        RegionReset(&part, &box);
        RegionSubtract(&waiting, &waiting, &part);
    }
    RegionSubtract(damage, damage, &waiting);
    RegionCopy(&ss->deferred, &waiting);
    RegionUninit(&waiting);
    RegionUninit(&part);
    ss->due = due;
}

/*
 * Compose one frame. A frame composed on demand (fromHook: before the
 * root window is read from or drawn on) runs in the middle of a request
 * or a drawing operation. It may end direct scanout, which gives the
 * window a new pixmap, but not start it, which frees one that the
 * operation may still use; and none runs while compCopyWindow copies a
 * window. Starting direct scanout is left to the block handler.
 */
static void
sceneFrame(ScreenPtr pScreen, SceneScreenPtr ss, bool fromHook)
{
    RegionRec damage;
    WindowPtr *wins;
    SceneNodeRec *nodes;

    /* Painting draws on the root window, and drawing on or reading from
       it composes pending changes first: a frame must not start another */
    if (sceneInFrame || (fromHook && compSceneHold))
        return;

    /* While the screen isn't ours (a DDX on another VT empties the root
       window's clip), painting would be clipped away yet the damage
       counted as shown. Keep the damage, and repaint everything when the
       clip comes back: what was on the screen meanwhile is unknown. */
    if (!RegionNotEmpty(&pScreen->root->borderClip)) {
        ss->rootHidden = true;
        return;
    }
    if (ss->rootHidden) {
        ss->rootHidden = false;
        ss->fullDamage = true;
    }
    sceneInFrame = true;
    sceneUpdateDirect(pScreen, ss, !fromHook);

    /* Composed before a drawing operation on the root window, this frame
       paints on the root itself. Each of its operations ends by
       reporting rootDrawnDamage's pending region, which already holds
       the outer operation's area: sceneRootDrawnReport ignores reports
       during a frame, and the outer operation would find nothing left
       to report, so it would never be copied to the windows. Keep that
       pending area aside and give it back afterwards. */
    RegionRec pending;
    RegionNull(&pending);
    if (fromHook && ss->rootDrawnDamage)
        RegionCopy(&pending, DamagePendingRegion(ss->rootDrawnDamage));

    RegionNull(&damage);
    int n = sceneSnapshot(pScreen, ss, &wins, &nodes, &damage);
    if (ss->fullDamage) {
        RegionCopy(&damage, &pScreen->root->winSize);
        ss->fullDamage = false;
    }
    scenePace(pScreen, ss, &damage, fromHook);
    sceneExpandForBlur(nodes, wins, n, &damage);
    /* a window shown directly draws itself: never paint over it */
    WindowPtr pDirect;
    if (ss->direct != None &&
        dixLookupWindow(&pDirect, ss->direct, serverClient, DixUnknownAccess) == Success)
        RegionSubtract(&damage, &damage, &pDirect->borderSize);
    if (RegionNotEmpty(&damage))
        scenePaint(pScreen, wins, nodes, n, &damage);
    RegionUninit(&damage);

    free(wins);
    free(ss->nodes);
    ss->nodes = nodes;
    ss->numNodes = n;
    if (ss->rootDrawnDamage && RegionNotEmpty(&pending))
        RegionUnion(DamagePendingRegion(ss->rootDrawnDamage),
                    DamagePendingRegion(ss->rootDrawnDamage), &pending);
    RegionUninit(&pending);
    sceneInFrame = false;
}

/*
 * Whether pWin is drawn into a pixmap only because the built-in
 * compositor redirects its top-level window. Clients cannot see that
 * redirection, so the server must keep the semantics of an unredirected
 * window for them (GetImage bounds, for one). A redirection requested by
 * any client, of the top-level window or of a window inside it, counts
 * as visible.
 */
bool
compSceneOwnsRedirection(WindowPtr pWin)
{
    SceneScreenPtr ss = sceneScreen(pWin->drawable.pScreen);

    if (!ss || !ss->active)
        return false;
    WindowPtr pRedirected = pWin;
    while (pRedirected && pRedirected->redirectDraw == RedirectDrawNone)
        pRedirected = pRedirected->parent;
    if (!pRedirected || !pRedirected->parent || pRedirected->parent->parent)
        return false;           /* not redirected, or not a top-level */
    CompWindowPtr cw = GetCompWindow(pRedirected);
    if (!cw)
        return false;
    for (CompClientWindowPtr ccw = cw->clients; ccw; ccw = ccw->next)
        if (dixClientIdForXID(ccw->id) != serverClient->index)
            return false;
    return true;
}

/* Bring the screen up to date before its contents are read. */
void
compSceneValidate(ScreenPtr pScreen)
{
    SceneScreenPtr ss = sceneScreen(pScreen);

    if (ss && ss->active)
        sceneFrame(pScreen, ss, true);
}

static void
sceneBlockHandler(void *data, void *timeout)
{
    for (unsigned i = 0; i < screenInfo.numScreens; i++) {
        ScreenPtr pScreen = screenInfo.screens[i];
        SceneScreenPtr ss = sceneScreen(pScreen);

        if (!ss)
            continue;
        sceneActivate(pScreen, ss);
        if (ss->active)
            sceneFrame(pScreen, ss, false);
        compColorCheckOutputs(pScreen);
        /* wake up when held-back damage is due: the main loop computed its
           timeout before calling us, so a timer set now would not count */
        if (ss->active && RegionNotEmpty(&ss->deferred)) {
            CARD64 now = GetTimeInMicros();
            AdjustWaitForDelay(timeout, ss->due > now ?
                               (int) ((ss->due - now + 999) / 1000) : 0);
        }
    }
}

static void
sceneWakeupHandler(void *data, int result)
{
}

/* ---- legacy compositor mode ----------------------------------------- */

/*
 * A client asks for Manual redirection of pWin's subwindows. If pWin is
 * a root window whose Manual redirection the scene compositor holds,
 * step aside so the request can succeed.
 */
void
compSceneYield(ClientPtr pClient, WindowPtr pWin)
{
    if (pClient == serverClient || pWin->parent)
        return;
    SceneScreenPtr ss = sceneScreen(pWin->drawable.pScreen);
    if (!ss || !ss->active)
        return;
    LogMessageVerb(X_INFO, 1, "compositor: a legacy compositor took over "
                   "screen %d\n", pWin->drawable.pScreen->myNum);
    sceneDeactivate(pWin->drawable.pScreen, ss);
    ss->yielded = true;
}

/*
 * A client's Manual redirection of pWin's subwindows went away. Take
 * the screen back on the next frame.
 */
void
compSceneResume(ClientPtr pClient, WindowPtr pWin)
{
    if (pClient == serverClient || pWin->parent)
        return;
    SceneScreenPtr ss = sceneScreen(pWin->drawable.pScreen);
    if (!ss || !ss->yielded)
        return;
    LogMessageVerb(X_INFO, 1, "compositor: taking screen %d back\n",
                   pWin->drawable.pScreen->myNum);
    ss->yielded = false;
}

/* ---- XPRISM-SCENE protocol -------------------------------------------- */

/* The window manager: the client holding SubstructureRedirect on the
   window's root. Only it may describe the scene. */
static bool
sceneClientIsWM(ClientPtr client, WindowPtr pWin)
{
    WindowPtr pRoot = pWin->drawable.pScreen->root;

    for (OtherClientsPtr oc = wOtherClients(pRoot); oc; oc = oc->next)
        if ((oc->mask & SubstructureRedirectMask) &&
            dixClientForXID(oc->resource) == client)
            return true;
    return false;
}

/* Look up a top-level window the calling WM may describe. */
static int
sceneLookupNode(ClientPtr client, XID id, WindowPtr *ppWin)
{
    WindowPtr pWin;
    int rc = dixLookupWindow(&pWin, id, client, DixSetAttrAccess);

    if (rc != Success) {
        client->errorValue = id;
        return rc;
    }
    if (!pWin->parent || pWin->parent->parent) {
        client->errorValue = id;
        return BadMatch;        /* scene nodes are top-level windows */
    }
    if (!sceneClientIsWM(client, pWin))
        return BadAccess;
    *ppWin = pWin;
    return Success;
}

static int
ProcXpSceneQueryVersion(ClientPtr client)
{
    X_REQUEST_HEAD_STRUCT(xXpSceneQueryVersionReq);
    X_REQUEST_FIELD_CARD32(majorVersion);
    X_REQUEST_FIELD_CARD32(minorVersion);

    xXpSceneQueryVersionReply reply = {
        .majorVersion = XPRISM_SCENE_MAJOR,
        .minorVersion = XPRISM_SCENE_MINOR,
    };
    if (client->swapped) {
        swapl(&reply.majorVersion);
        swapl(&reply.minorVersion);
    }
    return X_SEND_REPLY_SIMPLE(client, reply);
}

static int
ProcXpSceneSetNodeAttributes(ClientPtr client)
{
    X_REQUEST_HEAD_STRUCT(xXpSceneSetNodeAttributesReq);
    X_REQUEST_FIELD_CARD32(window);
    X_REQUEST_FIELD_CARD32(mask);
    X_REQUEST_FIELD_CARD32(opacity);
    if (client->swapped)
        for (int i = 0; i < 9; i++)
            swapl(&stuff->transform[i]);

    if (stuff->mask & ~XpSceneNodeAttributes) {
        client->errorValue = stuff->mask;
        return BadValue;
    }

    WindowPtr pWin;
    int rc = sceneLookupNode(client, stuff->window, &pWin);
    if (rc != Success)
        return rc;

    PictTransform xform, inverse;
    if (stuff->mask & XpSceneNodeTransform) {
        for (int i = 0; i < 9; i++)
            xform.matrix[i / 3][i % 3] = stuff->transform[i];
        BoxRec unused;
        int bw = wBorderWidth(pWin);
        /* must be invertible and keep the window in front of the viewer */
        if (!pixman_transform_invert(&inverse, &xform) ||
            !sceneTransformBounds(&xform, pWin->drawable.width + 2 * bw,
                                  pWin->drawable.height + 2 * bw, &unused))
            return BadMatch;
    }

    SceneAttrsPtr attrs = sceneAttrs(pWin);
    if (stuff->mask & XpSceneNodeOpacity)
        attrs->opacity = stuff->opacity;
    if (stuff->mask & XpSceneNodeTransform) {
        attrs->xform = xform;
        attrs->inverse = inverse;
    }
    attrs->mask |= stuff->mask;
    if (attrs->mask)
        attrs->owner = client;
    return Success;         /* the next frame's diff sees the change */
}

static int
ProcXpSceneGetNodeAttributes(ClientPtr client)
{
    X_REQUEST_HEAD_STRUCT(xXpSceneGetNodeAttributesReq);
    X_REQUEST_FIELD_CARD32(window);

    WindowPtr pWin;
    int rc = sceneLookupNode(client, stuff->window, &pWin);
    if (rc != Success)
        return rc;

    SceneAttrsPtr attrs = sceneAttrs(pWin);
    xXpSceneGetNodeAttributesReply reply = {
        .mask = attrs->mask & XpSceneNodeAttributes,
        .opacity = (attrs->mask & XpSceneNodeOpacity) ? attrs->opacity : 0xffffffff,
    };
    for (int i = 0; i < 9; i++)
        reply.transform[i] = (attrs->mask & XpSceneNodeTransform) ?
            attrs->xform.matrix[i / 3][i % 3] :
            (i % 4 == 0 ? pixman_fixed_1 : 0);      /* identity */
    if (client->swapped) {
        swapl(&reply.mask);
        swapl(&reply.opacity);
        for (int i = 0; i < 9; i++)
            swapl(&reply.transform[i]);
    }
    return X_SEND_REPLY_SIMPLE(client, reply);
}

static int
ProcXpSceneClearNodeAttributes(ClientPtr client)
{
    X_REQUEST_HEAD_STRUCT(xXpSceneClearNodeAttributesReq);
    X_REQUEST_FIELD_CARD32(window);
    X_REQUEST_FIELD_CARD32(mask);

    if (stuff->mask & ~XpSceneNodeAll) {
        client->errorValue = stuff->mask;
        return BadValue;
    }

    WindowPtr pWin;
    int rc = sceneLookupNode(client, stuff->window, &pWin);
    if (rc != Success)
        return rc;

    SceneAttrsPtr attrs = sceneAttrs(pWin);
    attrs->mask &= ~stuff->mask;
    if (!attrs->mask)
        sceneResetAttrs(attrs);
    return Success;         /* the next frame's diff sees the change */
}

static int
ProcXpSceneSetNodeDecoration(ClientPtr client)
{
    /* 0.2 clients send the request without backgroundBlur */
    X_REQUEST_HEAD_NO_CHECK(xXpSceneSetNodeDecorationReq);
    bool hasBlur = client->req_len == bytes_to_int32(sz_xXpSceneSetNodeDecorationReq);
    if (!hasBlur &&
        client->req_len != bytes_to_int32(sz_xXpSceneSetNodeDecorationReq02))
        return BadLength;
    if (hasBlur)
        X_REQUEST_FIELD_CARD16(backgroundBlur);
    CARD16 bgBlur = hasBlur ? stuff->backgroundBlur : 0;
    X_REQUEST_FIELD_CARD32(window);
    X_REQUEST_FIELD_CARD32(mask);
    X_REQUEST_FIELD_CARD16(cornerRadius);
    X_REQUEST_FIELD_CARD16(shadowBlur);
    X_REQUEST_FIELD_CARD16(shadowDx);
    X_REQUEST_FIELD_CARD16(shadowDy);
    X_REQUEST_FIELD_CARD32(shadowColor);

    if (stuff->mask & ~XpSceneNodeDecoration) {
        client->errorValue = stuff->mask;
        return BadValue;
    }
    if ((stuff->mask & XpSceneNodeCornerRadius) &&
        stuff->cornerRadius > XpSceneMaxCornerRadius) {
        client->errorValue = stuff->cornerRadius;
        return BadValue;
    }
    if ((stuff->mask & XpSceneNodeShadow) &&
        stuff->shadowBlur > XpSceneMaxShadowBlur) {
        client->errorValue = stuff->shadowBlur;
        return BadValue;
    }
    if (stuff->mask & XpSceneNodeBackgroundBlur) {
        if (!hasBlur) {
            client->errorValue = stuff->mask;
            return BadValue;
        }
        if (bgBlur > XpSceneMaxBackgroundBlur) {
            client->errorValue = bgBlur;
            return BadValue;
        }
    }

    WindowPtr pWin;
    int rc = sceneLookupNode(client, stuff->window, &pWin);
    if (rc != Success)
        return rc;

    SceneAttrsPtr attrs = sceneAttrs(pWin);
    if (stuff->mask & XpSceneNodeCornerRadius)
        attrs->radius = stuff->cornerRadius;
    if (stuff->mask & XpSceneNodeShadow) {
        attrs->shadowBlur = stuff->shadowBlur;
        attrs->shadowDx = stuff->shadowDx;
        attrs->shadowDy = stuff->shadowDy;
        attrs->shadowColor = stuff->shadowColor;
    }
    if (stuff->mask & XpSceneNodeBackgroundBlur)
        attrs->bgBlur = bgBlur;
    attrs->mask |= stuff->mask;
    if (attrs->mask)
        attrs->owner = client;
    return Success;
}

static int
ProcXpSceneGetNodeDecoration(ClientPtr client)
{
    X_REQUEST_HEAD_STRUCT(xXpSceneGetNodeDecorationReq);
    X_REQUEST_FIELD_CARD32(window);

    WindowPtr pWin;
    int rc = sceneLookupNode(client, stuff->window, &pWin);
    if (rc != Success)
        return rc;

    SceneAttrsPtr attrs = sceneAttrs(pWin);
    bool radius = attrs->mask & XpSceneNodeCornerRadius;
    bool shadow = attrs->mask & XpSceneNodeShadow;
    xXpSceneGetNodeDecorationReply reply = {
        .mask = attrs->mask & XpSceneNodeDecoration,
        .cornerRadius = radius ? attrs->radius : 0,
        .shadowBlur = shadow ? attrs->shadowBlur : 0,
        .shadowDx = shadow ? attrs->shadowDx : 0,
        .shadowDy = shadow ? attrs->shadowDy : 0,
        .shadowColor = shadow ? attrs->shadowColor : 0,
        .backgroundBlur = (attrs->mask & XpSceneNodeBackgroundBlur) ?
            attrs->bgBlur : 0,
    };
    if (client->swapped) {
        swapl(&reply.mask);
        swaps(&reply.cornerRadius);
        swaps(&reply.shadowBlur);
        swaps(&reply.shadowDx);
        swaps(&reply.shadowDy);
        swapl(&reply.shadowColor);
        swaps(&reply.backgroundBlur);
    }
    return X_SEND_REPLY_SIMPLE(client, reply);
}

static int
ProcXpSceneDispatch(ClientPtr client)
{
    REQUEST(xReq);

    switch (stuff->data) {
    case X_XpSceneQueryVersion:
        return ProcXpSceneQueryVersion(client);
    case X_XpSceneSetNodeAttributes:
        return ProcXpSceneSetNodeAttributes(client);
    case X_XpSceneGetNodeAttributes:
        return ProcXpSceneGetNodeAttributes(client);
    case X_XpSceneClearNodeAttributes:
        return ProcXpSceneClearNodeAttributes(client);
    case X_XpSceneSetNodeDecoration:
        return ProcXpSceneSetNodeDecoration(client);
    case X_XpSceneGetNodeDecoration:
        return ProcXpSceneGetNodeDecoration(client);
    default:
        return BadRequest;
    }
}

/* A window manager that goes away takes its scene description with it:
   a crashed WM must not leave windows invisible or shrunk. */
static void
sceneClientStateCallback(CallbackListPtr *list, void *closure, void *data)
{
    NewClientInfoRec *ci = data;
    ClientPtr gone = ci->client;

    if (gone->clientState != ClientStateGone)
        return;
    for (unsigned s = 0; s < screenInfo.numScreens; s++) {
        WindowPtr pRoot = screenInfo.screens[s]->root;
        if (!pRoot)
            continue;
        for (WindowPtr pWin = pRoot->firstChild; pWin; pWin = pWin->nextSib) {
            SceneAttrsPtr attrs = sceneAttrs(pWin);
            if (attrs->owner == gone)
                sceneResetAttrs(attrs);
        }
    }
}

void
compSceneExtensionInit(void)
{
    if (!sceneCompositorEnabled)
        return;
    if (!AddCallback(&ClientStateCallback, sceneClientStateCallback, NULL))
        return;
    if (!AddExtension(XPRISM_SCENE_NAME, 0, 0, ProcXpSceneDispatch,
                      ProcXpSceneDispatch, NULL, StandardMinorOpcode))
        LogMessageVerb(X_WARNING, 1, "compositor: cannot add %s\n",
                       XPRISM_SCENE_NAME);
    compColorExtensionInit();
}

/* ---- setup ------------------------------------------------------------ */

static void
sceneCloseScreen(CallbackListPtr *pcbl, ScreenPtr pScreen, void *unused)
{
    SceneScreenPtr ss = sceneScreen(pScreen);

    compColorCloseScreen(pScreen);
    if (!ss)
        return;
    sceneUntrackTile(ss);
    RegionUninit(&ss->deferred);
    if (ss->bgTile)
        dixDestroyPixmap(ss->bgTile, 0);
    free(ss->nodes);
    free(ss);
    dixSetPrivate(&pScreen->devPrivates, &sceneScreenKeyRec, NULL);
}

bool
compSceneScreenInit(ScreenPtr pScreen)
{
    if (!sceneCompositorEnabled)
        return true;

    if (!dixRegisterPrivateKey(&sceneScreenKeyRec, PRIVATE_SCREEN, 0) ||
        !dixRegisterPrivateKey(&sceneWindowKeyRec, PRIVATE_WINDOW, 0) ||
        !dixRegisterPrivateKey(&sceneAttrsKeyRec, PRIVATE_WINDOW,
                               sizeof(SceneAttrsRec)) ||
        !compColorScreenInit(pScreen))
        return false;

    SceneScreenPtr ss = calloc(1, sizeof(SceneScreenRec));
    if (!ss)
        return false;
    RegionNull(&ss->deferred);
    scenePacingOff = getenv("XPRISM_NO_FRAME_PACING") != NULL;
    dixSetPrivate(&pScreen->devPrivates, &sceneScreenKeyRec, ss);
    dixScreenHookClose(pScreen, sceneCloseScreen);
    WindowPointMapHook = sceneMapPoint;
    dixScreenHookWindowDestroy(pScreen, sceneWindowDestroy);

    atomOpacity = MakeAtom("_NET_WM_WINDOW_OPACITY",
                           strlen("_NET_WM_WINDOW_OPACITY"), TRUE);

    /* the handler list is reset with every server generation */
    if (handlersGeneration != serverGeneration) {
        if (!RegisterBlockAndWakeupHandlers(sceneBlockHandler,
                                            sceneWakeupHandler, NULL))
            return false;
        handlersGeneration = serverGeneration;
    }
    LogMessageVerb(X_INFO, 1, "compositor: scene compositor enabled on "
                   "screen %d\n", pScreen->myNum);
    return true;
}
