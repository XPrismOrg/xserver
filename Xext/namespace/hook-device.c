/* SPDX-License-Identifier: X11 OR MIT OR AGPL-3.0-or-later */
/* Copyright (C) 2026 Enrico Weigelt, metux IT consult <info@metux.net> */
#define HOOK_NAME "device"

#include <dix-config.h>

#include <X11/extensions/XIproto.h>
#include <X11/extensions/XI2proto.h>
#include <X11/extensions/XKB.h>

#include "dix/devices_priv.h"
#include "dix/dix_priv.h"
#include "dix/extension_priv.h"
#include "dix/registry_priv.h"

#include "dix/capability_priv.h"

#include "namespace.h"
#include "hooks.h"

void hookDevice(CallbackListPtr *pcbl, void *unused, void *calldata)
{
    XNS_HOOK_HEAD(DeviceAccessCallbackParam);

    if (subj->ns->superPower)
        goto pass;

    // should be safe to pass for anybody
    switch (client->majorOp) {
        case X_QueryPointer:
        case X_GetInputFocus:
        case X_GetKeyboardMapping:
        case X_GetModifierMapping:
        case X_GrabButton: // needed by xterm -- should be safe
            goto pass;
        // keyboard grabs and the keys held down: the capability checks
        // scope them to the client with the focus (dix/capability.c)
        case X_GrabKeyboard:
        case X_GrabKey:
        case X_QueryKeymap:
        case X_UngrabKeyboard:
        case X_UngrabKey:
        case X_AllowEvents:
            goto pass;
        // moving the focus among its windows, while it has it; taking
        // it is the window manager's
        case X_SetInputFocus:
            if (XpCapHasFocus(client, param->dev))
                goto pass;
            goto block;
        case EXTENSION_MAJOR_XKEYBOARD:
            switch(client->minorOp) {
                case X_kbSelectEvents:      // needed by xterm
                case X_kbGetMap:            // needed by xterm
                case X_kbBell:              // needed by GIMP
                case X_kbPerClientFlags:    // needed by firefox
                case X_kbGetState:          // needed by firefox
                case X_kbGetNames:          // needed by firefox
                case X_kbGetControls:       // needed by firefox
                    goto pass;
                default:
                    XNS_HOOK_LOG("BLOCKED unhandled XKEYBOARD %s\n", LookupRequestName(client->majorOp, client->minorOp));
                    goto block;
            }
        case EXTENSION_MAJOR_XINPUT:
            switch (client->minorOp) {
                case X_ListInputDevices:
                case X_XIQueryDevice:
                case X_XIGetFocus:              // as GetInputFocus
                case X_XIChangeCursor:          // GTK 3 sets cursors so; the
                                                // window is checked as usual
                case X_XIQueryPointer:          // as QueryPointer; the modifier
                                                // state is the focused client's
                case X_XIGrabDevice:            // menus (GTK, Qt)
                case X_XIUngrabDevice:
                case X_XIPassiveGrabDevice:
                case X_XIPassiveUngrabDevice:
                case X_XIAllowEvents:
                    goto pass;
                case X_XISetFocus:
                    if (XpCapHasFocus(client, param->dev))
                        goto pass;
                    goto block;
                default:
                    XNS_HOOK_LOG("BLOCKED unhandled Xinput request\n");
                    goto block;
            }
    }

block:
    param->status = BadAccess;
    return;

pass:
    /* leaves a denial by a hook that ran before (callbacks run newest
       first, so the capability checks run after this one) */
    return;
}
