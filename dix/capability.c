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
 *                        the configuration (all in one place; RandR's own
 *                        lookups also use write access since 4e749a551),
 *                        DPMS and VidMode writes; SetScreenSaver
 *                        (cap_screensaver)
 *   compositor.legacy    Composite: Manual redirection of a root window's
 *                        subwindows (compRedirectSubwindows), i.e. taking
 *                        the screen over from the scene compositor. A
 *                        compositor sees every window, so holding it also
 *                        grants capture.screen and capture.window.
 *
 *   input.monitor (keys) keyboard grabs and QueryKeymap by a client
 *                        without the focus, passive key grabs on others'
 *                        windows (XpCapCheckKeyboardGrab, cap_device)
 *
 *   input.monitor (state) the modifiers held and latched, seen by a
 *                        client without the focus: XKB StateNotify and
 *                        GetState, QueryPointer, XIQueryPointer show it
 *                        only the locked ones (XpCapSeesKeyState)
 *
 *   window.inspect (focus) the focus window, if another client's: None
 *                        to others (GetInputFocus, XIGetFocus;
 *                        XpCapSeesWindow)
 *
 *   capture.window (inferiors) other clients' windows inside one's own,
 *                        read with GetImage or IncludeInferiors copies
 *                        (XpCapCheckInferiors)
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
#include <X11/extensions/xtestproto.h>
#include <X11/extensions/dpmsproto.h>
#include <X11/extensions/xf86vmproto.h>

#include "dix/capability_priv.h"
#include "dix/screensaver_priv.h"
#include "dix/devices_priv.h"
#include "dix/input_priv.h"
#include "dix/dix_priv.h"
#include "dix/extension_priv.h"
#include "dix/registry_priv.h"
#include "dix/resource_priv.h"
#include "dix/selection_priv.h"
#include "include/dixstruct.h"
#include "include/privates.h"
#include "include/propertyst.h"
#include "include/windowstr.h"
#include "os/client_priv.h"
#include "os/log_priv.h"
#include "os/pal/identity.h"
#include "Xext/xace.h"
#include "Xext/xacestr.h"
#include "Xext/xinput/xichangehierarchy.h"

enum xp_cap_mode xpCapMode = XP_CAP_MODE_ENFORCE;

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
    [XP_CAP_COMPOSITOR_LEGACY]  = "compositor.legacy",
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

static Bool prompt_cap_ok(enum xp_cap cap);
static Bool is_root(WindowPtr pWin);
static Bool not_own(ClientPtr client, XID id);
static Bool holds(ClientPtr client, enum xp_cap cap);
static Bool in_own_window(ClientPtr client, WindowPtr pWin);

/* per client */
typedef struct {
    uint32_t logged;        /* missing capabilities already logged */
    uint32_t allowed;       /* answered "allow always" */
    uint32_t denied;        /* answered "deny always" */
    uint32_t allow_once;    /* answered for the request that runs again */
    uint32_t deny_once;
    Bool in_request;        /* a request of this client is being run */
    int ask;                /* capability to ask for after it, or -1 */
    XID ask_target;
    uint32_t prompt;        /* open prompt's id, or 0 */
    OsTimerPtr timer;
    int seat;               /* id of its automation seat's master pointer */
} CapClientRec;

static DevPrivateKeyRec capClientPrivKeyRec;
static XpCapPromptProc prompter;

static CapClientRec *
cap_client(ClientPtr client)
{
    if (!client || !dixPrivateKeyRegistered(&capClientPrivKeyRec))
        return NULL;
    return dixLookupPrivate(&client->devPrivates, &capClientPrivKeyRec);
}

static uint32_t *
logged_mask(ClientPtr client)
{
    CapClientRec *c = cap_client(client);
    return c ? &c->logged : NULL;
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
    uint32_t grants = provider_grants(client);
    if (grants & XP_CAP_BIT(XP_CAP_COMPOSITOR_LEGACY))
        grants |= XP_CAP_BIT(XP_CAP_CAPTURE_SCREEN) | XP_CAP_BIT(XP_CAP_CAPTURE_WINDOW);
    if (grants & XP_CAP_BIT(cap))
        return Success;

    CapClientRec *c = cap_client(client);
    if (c && ((c->allowed | c->allow_once) & XP_CAP_BIT(cap)))
        return Success;

    Bool deny = (xpCapMode == XP_CAP_MODE_ENFORCE);
    if (deny && c && c->in_request && c->ask < 0 && prompter &&
        prompt_cap_ok(cap) && !((c->denied | c->deny_once) & XP_CAP_BIT(cap))) {
        /* refused for now; XpCapEndRequest asks, if the request fails */
        c->ask = cap;
        c->ask_target = target;
        return BadAccess;
    }
    log_missing(client, cap, target, deny);
    return deny ? BadAccess : Success;
}

/* ------------------------------------------------------------------ *
 *  Permission prompts
 * ------------------------------------------------------------------ */

static uint32_t last_prompt_id;

void
XpCapSetPrompter(XpCapPromptProc prompt)
{
    prompter = prompt;
}

/* checks made before the request changes anything, so that it can run
   again: lookups, and the extension dispatch hook */
static Bool
prompt_cap_ok(enum xp_cap cap)
{
    switch (cap) {
    case XP_CAP_CAPTURE_SCREEN:
    case XP_CAP_CAPTURE_WINDOW:
    case XP_CAP_INPUT_INJECT:
    case XP_CAP_INPUT_MONITOR:
    case XP_CAP_WINDOW_ENUMERATE:
    case XP_CAP_WINDOW_INSPECT:
    case XP_CAP_DISPLAY_CONFIGURE:
        return TRUE;
    default:
        /* window.manage and input.virtual_seat are checked part way
           through requests that change things; clipboard checks choose
           a selection rather than refuse */
        return FALSE;
    }
}

void
XpCapBeginRequest(ClientPtr client)
{
    CapClientRec *c = cap_client(client);
    if (!c)
        return;
    c->in_request = TRUE;
    c->ask = -1;
}

static ClientPtr
prompt_client(uint32_t id, CapClientRec **pc)
{
    for (int i = 1; i < currentMaxClients; i++) {
        CapClientRec *c = clients[i] ? cap_client(clients[i]) : NULL;
        if (c && id && c->prompt == id) {
            *pc = c;
            return clients[i];
        }
    }
    return NULL;
}

static void
end_prompt(ClientPtr client, CapClientRec *c)
{
    c->prompt = 0;
    TimerFree(c->timer);
    c->timer = NULL;
    if (!client->clientGone)
        AttendClient(client);
}

static CARD32
prompt_timeout(OsTimerPtr timer, CARD32 time, void *arg)
{
    XpCapAnswer((uint32_t) (uintptr_t) arg, XP_CAP_DENY_ONCE);
    return 0;
}

void
XpCapEndRequest(ClientPtr client, int *result)
{
    CapClientRec *c = cap_client(client);
    if (!c)
        return;
    int cap = c->ask;
    c->in_request = FALSE;
    c->ask = -1;
    c->allow_once = c->deny_once = 0;
    if (cap < 0)
        return;
    if (*result != BadAccess || client->clientGone || !prompter) {
        /* refused without an error (QueryKeymap's empty map), or the
           client went: nothing to ask */
        log_missing(client, cap, c->ask_target, TRUE);
        return;
    }

    uint32_t id = ++last_prompt_id;
    if (!id)
        id = ++last_prompt_id;
    if (!prompter(client, cap, c->ask_target, id)) {
        log_missing(client, cap, c->ask_target, TRUE);
        return;
    }
    LogMessageVerb(X_INFO, 1, "capability: asking about %s for client %d (prompt %u)\n",
                   cap_names[cap], client->index, id);

    /* run the request again once answered (as ProcGrabServer waits) */
    c->prompt = id;
    c->ask = cap;           /* remembered for the answer */
    ResetCurrentRequest(client);
    client->sequence--;
    IgnoreClient(client);
    c->timer = TimerSet(NULL, 0, XP_CAP_PROMPT_TIMEOUT, prompt_timeout,
                        (void *) (uintptr_t) id);
    *result = Success;
}

Bool
XpCapAnswer(uint32_t id, enum xp_cap_answer answer)
{
    CapClientRec *c;
    ClientPtr client = prompt_client(id, &c);
    if (!client)
        return FALSE;
    uint32_t bit = XP_CAP_BIT(c->ask);

    switch (answer) {
    case XP_CAP_ALLOW_ALWAYS:   c->allowed |= bit; break;
    case XP_CAP_ALLOW_ONCE:     c->allow_once |= bit; break;
    case XP_CAP_DENY_ALWAYS:    c->denied |= bit; c->deny_once |= bit; break;
    default:                    c->deny_once |= bit; break;
    }
    LogMessageVerb(X_INFO, 1, "capability: prompt %u for %s of client %d: %s\n",
                   id, cap_names[c->ask], client->index,
                   answer == XP_CAP_ALLOW_ALWAYS ? "allowed" :
                   answer == XP_CAP_ALLOW_ONCE ? "allowed once" :
                   answer == XP_CAP_DENY_ALWAYS ? "denied" : "denied once");
    c->ask = -1;
    end_prompt(client, c);
    return TRUE;
}

void
XpCapRefuseAll(void)
{
    for (int i = 1; i < currentMaxClients; i++) {
        CapClientRec *c = clients[i] ? cap_client(clients[i]) : NULL;
        if (c && c->prompt)
            XpCapAnswer(c->prompt, XP_CAP_DENY_ONCE);
    }
}

/* a viewable window of another client inside pWin, or NULL */
static WindowPtr
foreign_inferior(ClientPtr client, WindowPtr pWin)
{
    for (WindowPtr child = pWin->firstChild; child; child = child->nextSib) {
        if (!child->viewable)
            continue;
        int owner = dixClientIdForXID(child->drawable.id);
        if (owner != 0 && owner != client->index)
            return child;
        WindowPtr found = foreign_inferior(client, child);
        if (found)
            return found;
    }
    return NULL;
}

int
XpCapCheckInferiors(ClientPtr client, DrawablePtr pDraw)
{
    if (!client || client == serverClient || !pDraw || pDraw->type != DRAWABLE_WINDOW ||
        xpCapMode == XP_CAP_MODE_OFF || !provider_grants)
        return Success;
    WindowPtr pWin = (WindowPtr) pDraw;
    if (is_root(pWin) || not_own(client, pWin->drawable.id))
        return Success;             /* checked at the lookup */
    WindowPtr other = foreign_inferior(client, pWin);
    return other ? XpCapCheck(client, XP_CAP_CAPTURE_WINDOW, other->drawable.id) : Success;
}

Bool
XpCapIsActive(ClientPtr client)
{
    /* a fact, not a check: the same in every mode, since Xnamespace's
       rules go by it too */
    if (!client || client == serverClient)
        return TRUE;
    DeviceIntPtr ptr = PickPointer(client);
    if (!ptr)
        return FALSE;
    DeviceIntPtr kbd = GetMaster(ptr, MASTER_KEYBOARD);
    return kbd && XpCapHasFocus(client, kbd) &&
           in_own_window(client, InputDevSpriteWindow(ptr));
}

Bool
XpCapSeesWindow(ClientPtr client, WindowPtr pWin)
{
    if (xpCapMode != XP_CAP_MODE_ENFORCE || !provider_grants)
        return TRUE;
    if (!client || client == serverClient || !pWin)
        return TRUE;
    return holds(client, XP_CAP_WINDOW_INSPECT) || in_own_window(client, pWin);
}

Bool
XpCapSeesKeyState(ClientPtr client, DeviceIntPtr kbd)
{
    if (xpCapMode != XP_CAP_MODE_ENFORCE || !provider_grants)
        return TRUE;
    if (!client || client == serverClient || client->clientGone)
        return TRUE;
    return holds(client, XP_CAP_INPUT_MONITOR) || XpCapHasFocus(client, kbd);
}

/* ------------------------------------------------------------------ *
 *  Scoped automation
 * ------------------------------------------------------------------ */

static XpCapInjectScopeProc inject_scope;
static Bool seat_dev[MAXDEVICES];       /* devices of automation seats */
static CARD32 last_user_input;
static Bool had_user_input;

void
XpCapSetInjectScope(XpCapInjectScopeProc scope)
{
    inject_scope = scope;
}

static Bool
holds(ClientPtr client, enum xp_cap cap)
{
    uint32_t grants = provider_grants(client);
    CapClientRec *c = cap_client(client);

    if (grants & XP_CAP_BIT(XP_CAP_COMPOSITOR_LEGACY))
        grants |= XP_CAP_BIT(XP_CAP_CAPTURE_SCREEN) | XP_CAP_BIT(XP_CAP_CAPTURE_WINDOW);
    if (c)
        grants |= c->allowed;
    return (grants & XP_CAP_BIT(cap)) != 0;
}

Bool
XpCapInjectScoped(ClientPtr client)
{
    return xpCapMode == XP_CAP_MODE_ENFORCE && provider_grants && inject_scope &&
           client && client != serverClient && !holds(client, XP_CAP_INPUT_INJECT) &&
           inject_scope(client, NULL);
}

static void
mark_seat(DeviceIntPtr ptr, Bool on)
{
    DeviceIntPtr kbd = GetMaster(ptr, MASTER_KEYBOARD);
    DeviceIntPtr devs[] = { ptr, kbd, GetXTestDevice(ptr), kbd ? GetXTestDevice(kbd) : NULL };

    for (size_t i = 0; i < ARRAY_SIZE(devs); i++)
        if (devs[i])
            seat_dev[devs[i]->id] = on;
}

/* the client's seat, if it still exists */
static DeviceIntPtr
seat_of(CapClientRec *c)
{
    DeviceIntPtr d;

    if (!c->seat)
        return NULL;
    if (dixLookupDevice(&d, c->seat, serverClient, DixReadAccess) != Success ||
        !InputDevIsMaster(d) || !seat_dev[d->id]) {
        c->seat = 0;
        return NULL;
    }
    return d;
}

int
XpCapInjectDevice(ClientPtr client, DeviceIntPtr *dev)
{
    CapClientRec *c = cap_client(client);

    if (!c || !XpCapInjectScoped(client))
        return Success;

    DeviceIntPtr ptr = seat_of(c);
    if (!ptr) {
        char name[64];
        snprintf(name, sizeof(name), "XPrism automation seat %d", client->index);
        ptr = XIAddSeat(name);
        if (!ptr)
            return BadAlloc;
        c->seat = ptr->id;
        mark_seat(ptr, TRUE);
        LogMessageVerb(X_INFO, 1, "capability: automation seat %d for client %d\n",
                       ptr->id, client->index);
    }
    DeviceIntPtr kbd = GetMaster(ptr, MASTER_KEYBOARD);
    DeviceIntPtr xptr = GetXTestDevice(ptr), xkbd = kbd ? GetXTestDevice(kbd) : NULL;
    DeviceIntPtr d = *dev;

    if (d == ptr || d == kbd || d == xptr || d == xkbd)
        return Success;
    /* the core XTest devices: the user's pointer and keyboard */
    if (d && IsXTestDevice(d, NULL) && !seat_dev[d->id]) {
        DeviceIntPtr master = GetMaster(d, MASTER_ATTACHED);
        if (master == inputInfo.pointer || master == inputInfo.keyboard) {
            *dev = d->key ? xkbd : xptr;
            return *dev ? Success : BadAlloc;
        }
    }
    log_missing(client, XP_CAP_INPUT_INJECT, 0, TRUE);
    return BadAccess;
}

int
XpCapInjectTarget(ClientPtr client, DeviceIntPtr dev, int type, CARD32 *wait_ms)
{
    *wait_ms = 0;
    if (!XpCapInjectScoped(client) || (type != KeyPress && type != KeyRelease &&
                                       type != ButtonPress && type != ButtonRelease))
        return Success;

    /* the user first */
    CARD32 now = GetTimeInMillis();
    if (had_user_input && now - last_user_input < XP_CAP_USER_PRIORITY_MS) {
        *wait_ms = XP_CAP_USER_PRIORITY_MS - (now - last_user_input);
        return Success;
    }

    DeviceIntPtr m = InputDevIsMaster(dev) ? dev : GetMaster(dev, MASTER_ATTACHED);
    WindowPtr win = NULL;
    if (!m)
        return BadAccess;
    if (m->deviceGrab.grab)
        win = m->deviceGrab.grab->window;
    else if (type == KeyPress || type == KeyRelease) {
        win = m->focus ? m->focus->win : NoneWin;
        if (win == PointerRootWin) {
            DeviceIntPtr ptr = GetMaster(m, MASTER_POINTER);
            win = ptr ? InputDevSpriteWindow(ptr) : NULL;
        }
        else if (win == NoneWin || win == FollowKeyboardWin)
            win = NULL;
    }
    else
        win = InputDevSpriteWindow(m);

    int owner = win ? dixClientIdForXID(win->drawable.id) : 0;
    ClientPtr target = owner > 0 && owner < currentMaxClients ? clients[owner] : NULL;
    if (target && (target == client || inject_scope(client, target)))
        return Success;
    log_missing(client, XP_CAP_INPUT_INJECT, win ? win->drawable.id : 0, TRUE);
    return BadAccess;
}

/* the user's input: events of devices that are not automation seats'.
   XTest through the core devices counts (on-screen keyboards). */
static void
cap_device_event(CallbackListPtr *pcbl, void *unused, void *calldata)
{
    DeviceEventInfoRec *info = calldata;
    DeviceIntPtr d = info->device;
    DeviceIntPtr m = d && !InputDevIsMaster(d) ? GetMaster(d, MASTER_ATTACHED) : d;

    if (!d || seat_dev[d->id] || (m && seat_dev[m->id]))
        return;
    last_user_input = GetTimeInMillis();
    had_user_input = TRUE;
}

static Bool
remove_seat(ClientPtr unused, void *closure)
{
    int id = (int) (intptr_t) closure;
    DeviceIntPtr d;

    if (dixLookupDevice(&d, id, serverClient, DixReadAccess) == Success &&
        InputDevIsMaster(d) && seat_dev[id]) {
        mark_seat(d, FALSE);
        XIRemoveSeat(d);
    }
    return TRUE;
}

/* legacy compatibility (idea.txt section 7) is explicit, never silent:
   every client with unrestricted access is on record, and so is a server
   with no policy at all */
static void
note_unconfined(ClientPtr client)
{
    static unsigned long warned_generation;

    if (xpCapMode == XP_CAP_MODE_OFF || client == serverClient)
        return;
    if (!provider_grants) {
        if (warned_generation != serverGeneration) {
            warned_generation = serverGeneration;
            LogMessageVerb(X_WARNING, 0,
                           "capability: no namespace configuration (-namespace), so every "
                           "client has unrestricted X11 access (legacy mode)\n");
        }
        return;
    }
    if (provider_grants(client) != XP_CAP_ALL)
        return;

    char label[160] = "";
    if (provider_label)
        provider_label(client, label, sizeof(label));
    pid_t pid = GetClientPid(client);
    const char *cmd = GetClientCmdName(client);
    Bool local = ClientIsLocal(client);
    LogMessageVerb(local ? X_INFO : X_WARNING, 0,
                   "capability: client %d (%s, pid %ld, %s) is unconfined: unrestricted "
                   "X11 access%s\n", client->index, label[0] ? label : "unlabeled",
                   (long) pid, cmd ? cmd : "unknown command",
                   local ? "" : ", from another host");
}

static void
cap_client_state(CallbackListPtr *pcbl, void *unused, void *calldata)
{
    NewClientInfoRec *info = calldata;
    ClientPtr client = info->client;
    CapClientRec *c = cap_client(client);

    /* after Xnamespace placed it (callbacks run newest first) */
    if (client->clientState == ClientStateRunning) {
        note_unconfined(client);
        return;
    }
    if (!c || client->clientState != ClientStateGone)
        return;
    if (c->prompt) {
        c->prompt = 0;
        TimerFree(c->timer);
        c->timer = NULL;
    }
    if (c->seat) {
        /* not from inside CloseDownClient */
        QueueWorkProc(remove_seat, NULL, (void *) (intptr_t) c->seat);
        c->seat = 0;
    }
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
        /* GetVersion is free; scoped clients may inject (checked in
           FakeInput) and compare cursors, not ignore grabs */
        if (client->minorOp == 0 ||
            (client->minorOp != X_XTestGrabControl && XpCapInjectScoped(client)))
            break;
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
    case EXTENSION_MAJOR_DPMS:
        /* the monitors' power: turning them off, or never */
        switch (client->minorOp) {
        case X_DPMSSetTimeouts:
        case X_DPMSEnable:
        case X_DPMSDisable:
        case X_DPMSForceLevel:
            apply(&rec->status, XpCapCheck(client, XP_CAP_DISPLAY_CONFIGURE, 0));
        }
        break;
    case EXTENSION_MAJOR_XF86_VIDMODE:
        /* video modes, viewport and gamma of every output */
        switch (client->minorOp) {
        case X_XF86VidModeModModeLine:
        case X_XF86VidModeSwitchMode:
        case X_XF86VidModeLockModeSwitch:
        case X_XF86VidModeAddModeLine:
        case X_XF86VidModeDeleteModeLine:
        case X_XF86VidModeSwitchToMode:
        case X_XF86VidModeSetViewPort:
        case X_XF86VidModeSetGamma:
        case X_XF86VidModeSetGammaRamp:
            apply(&rec->status, XpCapCheck(client, XP_CAP_DISPLAY_CONFIGURE, 0));
        }
        break;
    }
}

/* SetScreenSaver: blanking off, or its timeout, for the whole session
   (and the screen lockers going by it). ForceScreenSaver is free: video
   players reset the timer, and blanking now is harmless. */
static void
cap_screensaver(CallbackListPtr *pcbl, void *unused, void *calldata)
{
    ScreenSaverAccessCallbackParam *rec = calldata;

    if (rec->access_mode & DixSetAttrAccess)
        apply(&rec->status, XpCapCheck(rec->client, XP_CAP_DISPLAY_CONFIGURE, 0));
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
           (QueryPointer), and apps like xeyes rely on it. The active
           client gets the input anyway (games use raw events). */
        case XI_RawKeyPress:
        case XI_RawKeyRelease:
        case XI_RawButtonPress:
        case XI_RawButtonRelease:
        case XI_RawTouchBegin:
        case XI_RawTouchUpdate:
        case XI_RawTouchEnd:
            if (XpCapIsActive(rec->client))
                return;
            apply(&rec->status, XpCapCheck(rec->client, XP_CAP_INPUT_MONITOR,
                                           rec->pWin ? rec->pWin->drawable.id : 0));
            return;
        }
    }
}

/* is @p pWin, or one of its ancestors, a window of @p client? */
static Bool
in_own_window(ClientPtr client, WindowPtr pWin)
{
    for (; pWin && pWin->parent; pWin = pWin->parent)
        if (dixClientIdForXID(pWin->drawable.id) == client->index)
            return TRUE;
    return FALSE;
}

/* does @p client have the focus of keyboard @p dev: is the focus window,
   or with PointerRoot the window under the pointer, in one of its
   windows? */
Bool
XpCapHasFocus(ClientPtr client, DeviceIntPtr dev)
{
    DeviceIntPtr kbd = InputDevIsMaster(dev) ? dev : GetMaster(dev, MASTER_KEYBOARD);

    if (!kbd || !kbd->focus)
        return FALSE;
    WindowPtr focus = kbd->focus->win;
    if (focus == NoneWin || focus == FollowKeyboardWin)
        return FALSE;
    if (focus == PointerRootWin) {
        DeviceIntPtr ptr = GetMaster(kbd, MASTER_POINTER);
        focus = ptr ? InputDevSpriteWindow(ptr) : NULL;
    }
    return in_own_window(client, focus);
}

int
XpCapCheckKeyboardGrab(ClientPtr client, DeviceIntPtr dev, WindowPtr pWin, Bool active)
{
    if (!client || client == serverClient || !dev || !dev->key)
        return Success;
    if (active ? XpCapHasFocus(client, dev) : in_own_window(client, pWin))
        return Success;
    return XpCapCheck(client, XP_CAP_INPUT_MONITOR, pWin ? pWin->drawable.id : 0);
}

static void
cap_device(CallbackListPtr *pcbl, void *unused, void *calldata)
{
    DeviceAccessCallbackParam *rec = calldata;

    /* the keys held down, polled, are a key logger; the focused client
       may ask (menus, games) */
    if (rec->client && rec->client != serverClient &&
        rec->client->majorOp == X_QueryKeymap && !XpCapHasFocus(rec->client, rec->dev))
        apply(&rec->status, XpCapCheck(rec->client, XP_CAP_INPUT_MONITOR, 0));

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

    if (!dixRegisterPrivateKey(&capClientPrivKeyRec, PRIVATE_CLIENT, sizeof(CapClientRec)))
        FatalError("capability: cannot register client private\n");

    if (!(XaceRegisterCallback(XACE_RESOURCE_ACCESS, cap_resource, NULL) &&
          XaceRegisterCallback(XACE_PROPERTY_ACCESS, cap_property, NULL) &&
          XaceRegisterCallback(XACE_SELECTION_ACCESS, cap_selection, NULL) &&
          XaceRegisterCallback(XACE_RECEIVE_ACCESS, cap_receive, NULL) &&
          AddCallback(&ExtensionDispatchCallback, cap_ext_dispatch, NULL) &&
          AddCallback(&DeviceAccessCallback, cap_device, NULL) &&
          AddCallback(&ClientStateCallback, cap_client_state, NULL) &&
          AddCallback(&DeviceEventCallback, cap_device_event, NULL) &&
          AddCallback(&ScreenSaverAccessCallback, cap_screensaver, NULL)))
        FatalError("capability: cannot register checks\n");
}
