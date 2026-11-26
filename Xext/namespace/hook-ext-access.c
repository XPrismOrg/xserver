/* SPDX-License-Identifier: X11 OR MIT OR AGPL-3.0-or-later */
/* Copyright (C) 2026 Enrico Weigelt, metux IT consult <info@metux.net> */
#define HOOK_NAME "ext-access"

#include <dix-config.h>

#include "dix/dix_priv.h"
#include "dix/extension_priv.h"
#include "dix/registry_priv.h"
#include "Xext/xacestr.h"

#include "namespace.h"
#include "namespaceproto.h"
#include "hooks.h"

/* called on X_QueryExtension */
void hookExtAccess(CallbackListPtr *pcbl, void *unused, void *calldata)
{
    XNS_HOOK_HEAD(ExtensionAccessCallbackParam);

    /* root NS has super powers */
    if (subj->ns->superPower)
        goto pass;

    /* the namespace management extension is invisible to non-superPower
       clients - they must not even learn it exists */
    if (streq(param->ext->name, XNS_EXTENSION_NAME))
        goto reject;

    switch (param->ext->index + EXTENSION_BASE) {
        /* unrestricted access */
        case EXTENSION_MAJOR_BIG_REQUESTS:
        case EXTENSION_MAJOR_DAMAGE:
        case EXTENSION_MAJOR_DOUBLE_BUFFER:
        case EXTENSION_MAJOR_GENERIC_EVENT:
        case EXTENSION_MAJOR_PRESENT:
        case EXTENSION_MAJOR_SYNC:
        case EXTENSION_MAJOR_XC_MISC:
        case EXTENSION_MAJOR_XFIXES:
        case EXTENSION_MAJOR_XKEYBOARD:
        case EXTENSION_MAJOR_XRESOURCE:
            goto pass;

        /* reviewed: their requests reach other clients' resources only
           through the resource hooks, and the capability checks gate
           what changes the display (doc/capabilities.md, "Extensions") */
        case EXTENSION_MAJOR_COMPOSITE:
        case EXTENSION_MAJOR_DPMS:
        case EXTENSION_MAJOR_DRI2:
        case EXTENSION_MAJOR_DRI3:
        case EXTENSION_MAJOR_GLX:
        case EXTENSION_MAJOR_RANDR:
        case EXTENSION_MAJOR_RENDER:
        case EXTENSION_MAJOR_SHM:
        case EXTENSION_MAJOR_XINERAMA:
        case EXTENSION_MAJOR_XF86_VIDMODE:
            goto pass;

        /* really blacklisted */
        case EXTENSION_MAJOR_MIT_SCREEN_SAVER:
        case EXTENSION_MAJOR_RECORD:
        case EXTENSION_MAJOR_SECURITY:
        case EXTENSION_MAJOR_XVIDEO:
            goto reject;

        /* granted input.inject, or scoped automation (the capability
           checks decide the rest) */
        case EXTENSION_MAJOR_XTEST:
            if (XnsMayInject(subj->ns))
                goto pass;
            goto reject;

        /* only allowed if namespace has flag set */
        case EXTENSION_MAJOR_SHAPE:
            if (subj->ns->allowShape)
                goto pass;
            goto reject;

        /* only allowed if namespace has flag set */
        case EXTENSION_MAJOR_XINPUT:
            if (subj->ns->allowXInput)
                goto pass;
            goto reject;
    }

    /* not reviewed (DGA, driver extensions, new ones): hidden unless the
       namespace allows it */
    if (XnsAllowsExtension(subj->ns, param->ext->name))
        goto pass;
    XNS_HOOK_LOG("hiding unreviewed extension: %s (%d)\n", param->ext->name, param->ext->index);
    goto reject;

reject:
    param->status = BadAccess;
    return;

pass:
    param->status = Success;
    return;
}
