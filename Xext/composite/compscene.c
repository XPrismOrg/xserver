/* SPDX-License-Identifier: MIT */
/*
 * XPrism scene compositor (AD-1, Phase III).
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
#include "dix/property_priv.h"
#include "propertyst.h"
#include "mi/mi_priv.h"
#include "miext/extinit_priv.h"

#include "compint.h"

Bool sceneCompositorEnabled = FALSE;

/* One node of the scene: a viewable top-level window. */
typedef struct {
    XID id;
    BoxRec box;                 /* bounds incl. border, screen coords */
    CARD32 opacity;             /* 0xffffffff = opaque */
} SceneNodeRec;

typedef struct {
    bool active;                /* we hold Manual redirection of root */
    bool yielded;               /* a legacy compositor holds it */
    bool fullDamage;            /* repaint everything next frame */
    SceneNodeRec *nodes;        /* previous frame */
    int numNodes;
    /* root background as last painted */
    int bgState;
    PixUnion bg;
} SceneScreenRec, *SceneScreenPtr;

static DevPrivateKeyRec sceneScreenKeyRec;
static DevPrivateKeyRec sceneWindowKeyRec;     /* DamagePtr per top-level */
static Atom atomOpacity;
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
            wins = reallocarray(wins, cap, sizeof(*wins));
            nodes = reallocarray(nodes, cap, sizeof(*nodes));
            if (!wins || !nodes) {
                free(wins);
                free(nodes);
                ss->fullDamage = true;
                *pWins = NULL;
                *pNodes = NULL;
                return 0;
            }
        }
        wins[n] = pWin;
        nodes[n].id = pWin->drawable.id;
        nodes[n].box = *RegionExtents(&pWin->borderSize);
        nodes[n].opacity = sceneWindowOpacity(pWin);

        /* content drawn into the window since the last frame */
        DamagePtr pWinDamage = sceneWindowDamage(pWin);
        if (pWinDamage) {
            RegionPtr r = DamageRegion(pWinDamage);
            if (RegionNotEmpty(r)) {
                RegionTranslate(r, pWin->drawable.x, pWin->drawable.y);
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
            memcmp(&old->box, &nodes[i].box, sizeof(BoxRec))) {
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

    /* root background (xsetroot, wallpaper setters) */
    if (pRoot->backgroundState != ss->bgState ||
        memcmp(&pRoot->background, &ss->bg, sizeof(PixUnion))) {
        ss->bgState = pRoot->backgroundState;
        ss->bg = pRoot->background;
        ss->fullDamage = true;
    }

    *pWins = wins;
    *pNodes = nodes;
    return n;
}

/* ---- painting -------------------------------------------------------- */

static void
scenePaint(ScreenPtr pScreen, WindowPtr *wins, const SceneNodeRec *nodes,
           int n, RegionPtr pDamage)
{
    WindowPtr pRoot = pScreen->root;
    int error;

    RegionIntersect(pDamage, pDamage, &pRoot->winSize);
    if (!RegionNotEmpty(pDamage))
        return;

    /* Composite inhibits the root background while Manual redirection
       is in place: painting it is our job now */
    pRoot->inhibitBGPaint = FALSE;
    miPaintWindow(pRoot, pDamage, PW_BACKGROUND);
    pRoot->inhibitBGPaint = TRUE;

    XID subwindowMode = IncludeInferiors;
    PicturePtr pDst = CreatePicture(0, &pRoot->drawable,
                                    PictureWindowFormat(pRoot),
                                    CPSubwindowMode, &subwindowMode,
                                    serverClient, &error);
    if (!pDst)
        return;

    RegionRec clip;
    RegionNull(&clip);

    for (int i = 0; i < n; i++) {
        WindowPtr pWin = wins[i];

        RegionIntersect(&clip, pDamage, &pWin->borderSize);
        if (!RegionNotEmpty(&clip))
            continue;

        PixmapPtr pPixmap = (*pScreen->GetWindowPixmap) (pWin);
        PicturePtr pSrc = CreatePicture(0, &pPixmap->drawable,
                                        PictureWindowFormat(pWin), 0, NULL,
                                        serverClient, &error);
        if (!pSrc)
            continue;

        PicturePtr pMask = NULL;
        if (nodes[i].opacity != 0xffffffff) {
            xRenderColor alpha = { .alpha = nodes[i].opacity >> 16 };
            pMask = CreateSolidPicture(0, &alpha, &error);
        }

        RegionTranslate(&clip, -pRoot->drawable.x, -pRoot->drawable.y);
        SetPictureClipRegion(pDst, 0, 0, &clip);
        CompositePicture(PictOpOver, pSrc, pMask, pDst,
                         0, 0, 0, 0,
                         pPixmap->screen_x - pRoot->drawable.x,
                         pPixmap->screen_y - pRoot->drawable.y,
                         pPixmap->drawable.width, pPixmap->drawable.height);
        if (pMask)
            FreePicture(pMask, 0);
        FreePicture(pSrc, 0);
    }

    RegionUninit(&clip);
    FreePicture(pDst, 0);
}

/* ---- activation ------------------------------------------------------ */

static void
sceneDeactivate(ScreenPtr pScreen, SceneScreenPtr ss)
{
    if (!ss->active)
        return;
    for (WindowPtr pWin = pScreen->root->firstChild; pWin; pWin = pWin->nextSib)
        sceneDropWindowDamage(pWin);
    ss->active = false;
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
    if (compRedirectSubwindows(serverClient, pScreen->root,
                               CompositeRedirectManual) != Success)
        return;
    ss->active = true;
    ss->fullDamage = true;
    ss->bgState = -1;
}

static void
sceneFrame(ScreenPtr pScreen, SceneScreenPtr ss)
{
    RegionRec damage;
    WindowPtr *wins;
    SceneNodeRec *nodes;

    RegionNull(&damage);
    int n = sceneSnapshot(pScreen, ss, &wins, &nodes, &damage);
    if (ss->fullDamage) {
        RegionCopy(&damage, &pScreen->root->winSize);
        ss->fullDamage = false;
    }
    if (RegionNotEmpty(&damage))
        scenePaint(pScreen, wins, nodes, n, &damage);
    RegionUninit(&damage);

    free(wins);
    free(ss->nodes);
    ss->nodes = nodes;
    ss->numNodes = n;
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
            sceneFrame(pScreen, ss);
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

/* ---- setup ------------------------------------------------------------ */

static void
sceneCloseScreen(CallbackListPtr *pcbl, ScreenPtr pScreen, void *unused)
{
    SceneScreenPtr ss = sceneScreen(pScreen);

    if (!ss)
        return;
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
        !dixRegisterPrivateKey(&sceneWindowKeyRec, PRIVATE_WINDOW, 0))
        return false;

    SceneScreenPtr ss = calloc(1, sizeof(SceneScreenRec));
    if (!ss)
        return false;
    dixSetPrivate(&pScreen->devPrivates, &sceneScreenKeyRec, ss);
    dixScreenHookClose(pScreen, sceneCloseScreen);

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
