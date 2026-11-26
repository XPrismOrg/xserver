/* SPDX-License-Identifier: MIT OR X11
 *
 * XPrism capabilities: the permission model from idea.txt section 7.
 *
 * Each capability is checked at the place in the server where the
 * protocol actually does the thing (see dix/capability.c for the map). A
 * policy provider (today: Xnamespace) says which capabilities a client
 * holds. Clients a provider knows nothing about are unconfined and hold
 * all of them, which keeps plain X11 sessions working unchanged.
 *
 * The capability mode (-capmode) decides what a missing capability does:
 *   off      no checks at all
 *   audit    log what would be denied, deny nothing
 *   enforce  deny with BadAccess (or drop the event), or ask the permission
 *            agent (default)
 */
#ifndef XSERVER_DIX_CAPABILITY_PRIV_H
#define XSERVER_DIX_CAPABILITY_PRIV_H

#include <stdint.h>
#include <X11/Xdefs.h>

#include "include/dix.h"
#include "include/input.h"

enum xp_cap {
    XP_CAP_CAPTURE_SCREEN,      /* read the root window's contents */
    XP_CAP_CAPTURE_WINDOW,      /* read another client's window or pixmap */
    XP_CAP_INPUT_INJECT,        /* synthesize input (XTest) */
    XP_CAP_INPUT_MONITOR,       /* observe global input (raw events, RECORD) */
    XP_CAP_INPUT_VIRTUAL_SEAT,  /* create input devices / master devices */
    XP_CAP_WINDOW_ENUMERATE,    /* list other clients' windows */
    XP_CAP_WINDOW_INSPECT,      /* read other clients' window attributes/properties */
    XP_CAP_WINDOW_MANAGE,       /* manage other clients' windows (WM, compositor) */
    XP_CAP_CLIPBOARD_READ,      /* read PRIMARY / SECONDARY / CLIPBOARD */
    XP_CAP_CLIPBOARD_WRITE,     /* own PRIMARY / SECONDARY / CLIPBOARD */
    XP_CAP_DISPLAY_CONFIGURE,   /* change outputs / CRTCs (RandR) */
    XP_CAP_COMPOSITOR_LEGACY,   /* run an external compositor (AD-1);
                                   implies capture.screen and capture.window */
    XP_CAP_COUNT
};

#define XP_CAP_BIT(c)   ((uint32_t) 1 << (c))
#define XP_CAP_ALL      (XP_CAP_BIT(XP_CAP_COUNT) - 1)

enum xp_cap_mode {
    XP_CAP_MODE_OFF,
    XP_CAP_MODE_AUDIT,
    XP_CAP_MODE_ENFORCE,
};

extern enum xp_cap_mode xpCapMode;

/** "capture.screen" etc., or NULL for an invalid value */
const char *XpCapName(enum xp_cap cap);
/** capability for a name, or -1 */
int XpCapFromName(const char *name);

/**
 * Policy provider. grants() returns the capabilities @p client holds;
 * label() writes a short human-readable description of the client's
 * confinement (e.g. namespace and app id) for log messages.
 * Only one provider at a time; NULL procs remove it.
 */
typedef uint32_t (*XpCapGrantsProc)(ClientPtr client);
typedef void (*XpCapLabelProc)(ClientPtr client, char *buf, size_t len);
void XpCapSetProvider(XpCapGrantsProc grants, XpCapLabelProc label);

/**
 * Check that @p client holds @p cap, for an operation on @p target (an XID,
 * or 0). Returns Success, or BadAccess when the mode is "enforce" and the
 * capability is missing. In audit mode, a missing capability is logged,
 * once per client and capability.
 */
int XpCapCheck(ClientPtr client, enum xp_cap cap, XID target);

/**
 * Keyboard grabs (dix/events.c GrabDevice, dix/grabs.c
 * AddPassiveGrabToList): a client may grab the keyboard while it has the
 * focus (menus), and grab keys passively on its own windows, which only
 * fire while the focus is in them. Anything else sees keys meant for
 * other clients and needs input.monitor. Devices without keys pass.
 */
Bool XpCapHasFocus(ClientPtr client, DeviceIntPtr dev);
int XpCapCheckKeyboardGrab(ClientPtr client, DeviceIntPtr dev, WindowPtr pWin,
                           Bool active);

/*
 * Permission prompts. In enforce mode, when a client's request is refused
 * for a capability it lacks, a prompter (the permission agent of
 * X-NAMESPACE) may ask the user instead. The client is suspended; when
 * the answer comes (XpCapAnswer), the request runs again, and the answer
 * decides the check. Without an answer in XP_CAP_PROMPT_TIMEOUT ms, it is
 * refused.
 *
 * Only capabilities whose checks come before a request changes anything
 * are asked for (see prompt_cap_ok in dix/capability.c).
 */
#define XP_CAP_PROMPT_TIMEOUT   60000

enum xp_cap_answer {
    XP_CAP_DENY_ONCE    = 0,    /* refuse this request */
    XP_CAP_ALLOW_ONCE   = 1,    /* allow this request */
    XP_CAP_ALLOW_ALWAYS = 2,    /* allow for as long as the client runs */
    XP_CAP_DENY_ALWAYS  = 3,    /* refuse, and do not ask again */
};

/** ask about prompt @p id; FALSE when nobody can be asked */
typedef Bool (*XpCapPromptProc)(ClientPtr client, enum xp_cap cap, XID target,
                                uint32_t id);
void XpCapSetPrompter(XpCapPromptProc prompt);
/** answer prompt @p id; FALSE if there is no such prompt (any more) */
Bool XpCapAnswer(uint32_t id, enum xp_cap_answer answer);
/** refuse every open prompt (the agent went away) */
void XpCapRefuseAll(void);

/** the dispatcher, around each request (dix/dispatch.c). End may suspend
    the client for a prompt; it then sets *result to Success. */
void XpCapBeginRequest(ClientPtr client);
void XpCapEndRequest(ClientPtr client, int *result);

/** register the checks; once per server generation, before extensions */
void XpCapInit(void);

#endif /* XSERVER_DIX_CAPABILITY_PRIV_H */
