/* SPDX-License-Identifier: MIT OR X11
 *
 * XPrism capabilities, see capability_priv.h.
 *
 * Where each capability is checked:
 *
 *   capture.screen       resource hook: DixReadAccess on the root window
 *                        (GetImage, ShmGetImage, CopyArea/CopyPlane source)
 *   capture.window       resource hook: DixReadAccess on another client's
 *                        window or pixmap
 *   input.inject         extension dispatch: XTest (except GetVersion)
 *   input.monitor        receive hook: XI2 raw key/button/touch events;
 *                        extension dispatch: RECORD (except QueryVersion)
 *   input.virtual_seat   device hook: a client creating a device
 *                        (XIChangeHierarchy AddMaster)
 *   window.enumerate     resource hook: DixListAccess on another client's
 *                        or the root window (QueryTree)
 *   window.inspect       resource hook: DixGetAttrAccess on another
 *                        client's window (GetWindowAttributes, GetGeometry);
 *                        property hook: reading or listing properties of
 *                        another client's window. The root window's
 *                        attributes and properties are public.
 *   window.manage        resource hook: DixManageAccess on another client's
 *                        or the root window (Reparent, SaveSet, Composite
 *                        redirect, and selecting Substructure/Resize
 *                        Redirect, which EventSelectForWindow checks as
 *                        DixManageAccess)
 *   clipboard.read       selection hook: ConvertSelection (DixReadAccess)
 *   clipboard.write      selection hook: SetSelectionOwner (DixSetAttrAccess)
 *                        both only for the session's PRIMARY, SECONDARY and
 *                        CLIPBOARD. Xnamespace gives each namespace private
 *                        copies ("<ns>CLIPBOARD"), which aren't checked:
 *                        confined clients never reach the session clipboard.
 *                        (Letting a grant open it to them is still to do.)
 *   display.configure    extension dispatch: the RandR requests that change
 *                        the configuration. Not the resource hook: RandR
 *                        looks up outputs and CRTCs with DixReadAccess even
 *                        in most requests that change them, and
 *                        SetScreenSize looks up no RandR resource at all.
 *
 * Known gaps, see docs/audit-phase1.md: CopyArea with IncludeInferiors from
 * one's own window (G1), core/XI grabs and QueryKeymap (input.monitor).
 */
#include <dix-config.h>

#include <stdio.h>
#include <string.h>

#include <X11/X.h>
#include <X11/Xproto.h>
#include <X11/Xatom.h>
#include <X11/extensions/XI2.h>
#include <X11/extensions/XI2proto.h>
#include <X11/extensions/randrproto.h>

#include "dix/capability_priv.h"
#include "dix/devices_priv.h"
#include "dix/dix_priv.h"
#include "dix/extension_priv.h"
#include "dix/registry_priv.h"
#include "dix/resource_priv.h"
#include "dix/selection_priv.h"
#include "include/dixstruct.h"
#include "include/privates.h"
#include "include/propertyst.h"
#include "include/windowstr.h"
#include "os/log_priv.h"
#include "os/pal/identity.h"
#include "Xext/xace.h"
#include "Xext/xacestr.h"

enum xp_cap_mode xpCapMode = XP_CAP_MODE_AUDIT;

static const char *const cap_names[XP_CAP_COUNT] = {
    [XP_CAP_CAPTURE_SCREEN]     = "capture.screen",
    [XP_CAP_CAPTURE_WINDOW]     = "capture.window",
    [XP_CAP_INPUT_INJECT]       = "input.inject",
    [XP_CAP_INPUT_MONITOR]      = "input.monitor",
    [XP_CAP_INPUT_VIRTUAL_SEAT] = "input.virtual_seat",
    [XP_CAP_WINDOW_ENUMERATE]   = "window.enumerate",
    [XP_CAP_WINDOW_INSPECT]     = "window.inspect",
    [XP_CAP_WINDOW_MANAGE]      = "window.manage",
    [XP_CAP_CLIPBOARD_READ]     = "clipboard.read",
    [XP_CAP_CLIPBOARD_WRITE]    = "clipboard.write",
    [XP_CAP_DISPLAY_CONFIGURE]  = "display.configure",
};

const char *
XpCapName(enum xp_cap cap)
{
    return (cap >= 0 && cap < XP_CAP_COUNT) ? cap_names[cap] : NULL;
}

int
XpCapFromName(const char *name)
{
    for (int i = 0; i < XP_CAP_COUNT; i++)
        if (strcmp(name, cap_names[i]) == 0)
            return i;
    return -1;
}

/* ------------------------------------------------------------------ */

static XpCapGrantsProc provider_grants;
static XpCapLabelProc provider_label;

void
XpCapSetProvider(XpCapGrantsProc grants, XpCapLabelProc label)
{
    provider_grants = grants;
    provider_label = label;
}

/* per client: which missing capabilities were already logged */
static DevPrivateKeyRec capClientPrivKeyRec;

static uint32_t *
logged_mask(ClientPtr client)
{
    if (!dixPrivateKeyRegistered(&capClientPrivKeyRec))
        return NULL;
    return dixLookupPrivate(&client->devPrivates, &capClientPrivKeyRec);
}

static void
log_missing(ClientPtr client, enum xp_cap cap, XID target, Bool denied)
{
    uint32_t *logged = logged_mask(client);
    if (logged) {
        if (*logged & XP_CAP_BIT(cap))
            return;
        *logged |= XP_CAP_BIT(cap);
    }

    char label[160] = "";
    if (provider_label)
        provider_label(client, label, sizeof(label));

    char who[48] = "";
    const struct peer_identity *id = ClientPeerIdentity(client);
    if (id && (id->fields & PEER_ID_PID))
        snprintf(who, sizeof(who), ", pid %ld", (long) id->pid);

    char reqbuf[24];
    const char *req = LookupRequestName(client->majorOp, client->minorOp);
    if (!req || req[0] == '<') {    /* no request names in this build */
        snprintf(reqbuf, sizeof(reqbuf), "request %d.%d",
                 client->majorOp, client->minorOp);
        req = reqbuf;
    }

    LogMessageVerb(X_WARNING, 0,
                   "capability: %s %s to client %d (%s%s) for %s on 0x%lx%s\n",
                   denied ? "denied" : "would deny", cap_names[cap],
                   client->index, label[0] ? label : "unlabeled", who,
                   req, (unsigned long) target,
                   denied ? "" : " [audit]");
}

int
XpCapCheck(ClientPtr client, enum xp_cap cap, XID target)
{
    if (xpCapMode == XP_CAP_MODE_OFF || !provider_grants)
        return Success;
    if (!client || client == serverClient || cap < 0 || cap >= XP_CAP_COUNT)
        return Success;
    if (provider_grants(client) & XP_CAP_BIT(cap))
        return Success;

    Bool deny = (xpCapMode == XP_CAP_MODE_ENFORCE);
    log_missing(client, cap, target, deny);
    return deny ? BadAccess : Success;
}

/* ------------------------------------------------------------------ *
 *  Checks
 * ------------------------------------------------------------------ */

/* apply a check result to a hook's status, without undoing another
   policy's denial */
static void
apply(int *status, int rc)
{
    if (rc != Success && *status == Success)
        *status = rc;
}

static Bool
is_root(WindowPtr pWin)
{
    return pWin && pWin->parent == NullWindow;
}

/* does @p id belong to some other client (or the server, e.g. the root)? */
static Bool
not_own(ClientPtr client, XID id)
{
    return dixClientIdForXID(id) != client->index;
}

static void
cap_resource(CallbackListPtr *pcbl, void *unused, void *calldata)
{
    XaceResourceAccessRec *rec = calldata;
    ClientPtr client = rec->client;
    Mask mode = rec->access_mode;

    if (!client || client == serverClient || !not_own(client, rec->id))
        return;

    if (rec->rtype == RT_WINDOW || rec->rtype == RT_PIXMAP) {
        WindowPtr pWin = rec->rtype == RT_WINDOW ? rec->res : NULL;
        Bool root = is_root(pWin);

        if (mode & DixReadAccess)
            apply(&rec->status, XpCapCheck(client, root ? XP_CAP_CAPTURE_SCREEN
                                                       : XP_CAP_CAPTURE_WINDOW, rec->id));
        if (pWin && (mode & DixManageAccess))
            apply(&rec->status, XpCapCheck(client, XP_CAP_WINDOW_MANAGE, rec->id));
        if (pWin && (mode & DixListAccess))
            apply(&rec->status, XpCapCheck(client, XP_CAP_WINDOW_ENUMERATE, rec->id));
        if (pWin && !root && (mode & DixGetAttrAccess))
            apply(&rec->status, XpCapCheck(client, XP_CAP_WINDOW_INSPECT, rec->id));
        return;
    }
}

static void
cap_property(CallbackListPtr *pcbl, void *unused, void *calldata)
{
    XacePropertyAccessRec *rec = calldata;
    ClientPtr client = rec->client;
    WindowPtr pWin = rec->pWin;

    if (!client || client == serverClient || !pWin || is_root(pWin) ||
        !not_own(client, pWin->drawable.id))
        return;
    if (rec->access_mode & (DixReadAccess | DixListPropAccess))
        apply(&rec->status, XpCapCheck(client, XP_CAP_WINDOW_INSPECT, pWin->drawable.id));
}

static Bool
is_clipboard_selection(Atom a)
{
    static Atom clipboard;

    if (a == XA_PRIMARY || a == XA_SECONDARY)
        return TRUE;
    if (!clipboard)
        clipboard = MakeAtom("CLIPBOARD", 9, TRUE);
    return a == clipboard;
}

static void
cap_selection(CallbackListPtr *pcbl, void *unused, void *calldata)
{
    XaceSelectionAccessRec *rec = calldata;
    Selection *sel = rec->ppSel ? *rec->ppSel : NULL;

    if (!rec->client || rec->client == serverClient || !sel ||
        !is_clipboard_selection(sel->selection))
        return;
    if (rec->access_mode & DixReadAccess)
        apply(&rec->status, XpCapCheck(rec->client, XP_CAP_CLIPBOARD_READ, sel->selection));
    if (rec->access_mode & DixSetAttrAccess)
        apply(&rec->status, XpCapCheck(rec->client, XP_CAP_CLIPBOARD_WRITE, sel->selection));
}

/* RandR requests that change the display configuration */
static Bool
randr_request_configures(int minor)
{
    switch (minor) {
    case X_RRSetScreenConfig:
    case X_RRSetScreenSize:
    case X_RRConfigureOutputProperty:
    case X_RRChangeOutputProperty:
    case X_RRDeleteOutputProperty:
    case X_RRCreateMode:
    case X_RRDestroyMode:
    case X_RRAddOutputMode:
    case X_RRDeleteOutputMode:
    case X_RRSetCrtcConfig:
    case X_RRSetCrtcGamma:
    case X_RRSetCrtcTransform:
    case X_RRSetPanning:
    case X_RRSetOutputPrimary:
    case X_RRSetProviderOffloadSink:
    case X_RRSetProviderOutputSource:
    case X_RRConfigureProviderProperty:
    case X_RRChangeProviderProperty:
    case X_RRDeleteProviderProperty:
    case X_RRSetMonitor:
    case X_RRDeleteMonitor:
    case X_RRCreateLease:           /* hands out the display hardware */
    case X_RRFreeLease:
        return TRUE;
    default:
        return FALSE;
    }
}

static void
cap_ext_dispatch(CallbackListPtr *pcbl, void *unused, void *calldata)
{
    ExtensionAccessCallbackParam *rec = calldata;
    ClientPtr client = rec->client;

    switch (client->majorOp) {
    case EXTENSION_MAJOR_XTEST:
        if (client->minorOp != 0)               /* GetVersion */
            apply(&rec->status, XpCapCheck(client, XP_CAP_INPUT_INJECT, 0));
        break;
    case EXTENSION_MAJOR_RECORD:
        if (client->minorOp != 0)               /* QueryVersion */
            apply(&rec->status, XpCapCheck(client, XP_CAP_INPUT_MONITOR, 0));
        break;
    case EXTENSION_MAJOR_RANDR:
        if (randr_request_configures(client->minorOp))
            apply(&rec->status, XpCapCheck(client, XP_CAP_DISPLAY_CONFIGURE, 0));
        break;
    }
}

static void
cap_receive(CallbackListPtr *pcbl, void *unused, void *calldata)
{
    XaceReceiveAccessRec *rec = calldata;

    for (int i = 0; i < rec->count; i++) {
        if (rec->events[i].u.u.type != GenericEvent)
            continue;
        xGenericEvent *gev = (xGenericEvent *) &rec->events[i];
        if (gev->extension != EXTENSION_MAJOR_XINPUT)
            continue;
        switch (gev->evtype) {
        /* raw motion is left out: the pointer position is public anyway
           (QueryPointer), and apps like xeyes rely on it */
        case XI_RawKeyPress:
        case XI_RawKeyRelease:
        case XI_RawButtonPress:
        case XI_RawButtonRelease:
        case XI_RawTouchBegin:
        case XI_RawTouchUpdate:
        case XI_RawTouchEnd:
            apply(&rec->status, XpCapCheck(rec->client, XP_CAP_INPUT_MONITOR,
                                           rec->pWin ? rec->pWin->drawable.id : 0));
            return;
        }
    }
}

static void
cap_device(CallbackListPtr *pcbl, void *unused, void *calldata)
{
    DeviceAccessCallbackParam *rec = calldata;

    /* the server creates the real and XTest devices itself; a client
       creating one means a new (master) device */
    if (rec->client && rec->client != serverClient &&
        (rec->access_mode & DixCreateAccess))
        apply(&rec->status, XpCapCheck(rec->client, XP_CAP_INPUT_VIRTUAL_SEAT, 0));
}

void
XpCapInit(void)
{
    if (xpCapMode == XP_CAP_MODE_OFF)
        return;

    if (!dixRegisterPrivateKey(&capClientPrivKeyRec, PRIVATE_CLIENT, sizeof(uint32_t)))
        FatalError("capability: cannot register client private\n");

    if (!(XaceRegisterCallback(XACE_RESOURCE_ACCESS, cap_resource, NULL) &&
          XaceRegisterCallback(XACE_PROPERTY_ACCESS, cap_property, NULL) &&
          XaceRegisterCallback(XACE_SELECTION_ACCESS, cap_selection, NULL) &&
          XaceRegisterCallback(XACE_RECEIVE_ACCESS, cap_receive, NULL) &&
          AddCallback(&ExtensionDispatchCallback, cap_ext_dispatch, NULL) &&
          AddCallback(&DeviceAccessCallback, cap_device, NULL)))
        FatalError("capability: cannot register checks\n");
}
