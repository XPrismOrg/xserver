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
 *   audit    log what would be denied, deny nothing (default)
 *   enforce  deny with BadAccess (or drop the event); experimental
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

/** register the checks; once per server generation, before extensions */
void XpCapInit(void);

#endif /* XSERVER_DIX_CAPABILITY_PRIV_H */
