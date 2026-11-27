/* SPDX-License-Identifier: X11 OR MIT OR AGPL-3.0-or-later */
/* Copyright (C) 2026 Enrico Weigelt, metux IT consult <info@metux.net> */
#define HOOK_NAME "clienstate"

#include <dix-config.h>

#include "dix/registry_priv.h"
#include "os/client_priv.h"
#include "os/auth.h"
#include "os/listener_priv.h"

#include "namespace.h"
#include "hooks.h"

void hookClientState(CallbackListPtr *pcbl, void *unused, void *calldata)
{
    XNS_HOOK_HEAD(NewClientInfoRec);

    switch (client->clientState) {
    case ClientStateInitial: {
        /* a client that came in through a namespace's own listening
           socket belongs to that namespace, whatever auth data it sends */
        struct XnsAppInfo *app = NULL;
        struct Xnamespace *bound = XnsListenerNamespace(ClientListenerLabel(client), &app);
        if (bound) {
            XnamespaceAssignClient(subj, bound);
            subj->socketBound = TRUE;
            subj->app = app;
            break;
        }
        // better assign *someting* than null -- clients can't do anything yet anyways
        XnamespaceAssignClient(subj, &ns_anon);
        break;
    }

    case ClientStateRunning:
        if (subj->socketBound)
            break;
        subj->authId = AuthorizationIDOfClient(client);

        short unsigned int name_len = 0, data_len = 0;
        const char * name = NULL;
        char * data = NULL;
        if (AuthorizationFromID(subj->authId, &name_len, &name, &data_len, &data)) {
            XnamespaceAssignClient(subj, XnsFindByAuth(name_len, name, data_len, data));
            XnsClassifyClient(client, subj);
        } else {
            XNS_HOOK_LOG("no auth data - assuming anon\n");
        }
        break;

    case ClientStateRetained:
        break;
    case ClientStateGone:
        break;
    default:
        XNS_HOOK_LOG("unknown state =%d\n", client->clientState);
        break;
    }
}

void hookClientDestroy(CallbackListPtr *pcbl, void *unused, void *calldata)
{
    ClientPtr client = calldata;
    struct XnamespaceClientPriv *subj = XnsClientPriv(client);

    if (!subj)
        return; /* no XNS devprivate assigned ? */

    XnsSelReplyDropClient(client);
    XnamespaceAssignClient(subj, NULL);
    XnsAppInfoUnref(subj->app);
    subj->app = NULL;
    /* the devprivate is embedded, so no free() necessary */
}
