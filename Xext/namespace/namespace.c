/* SPDX-License-Identifier: X11 OR MIT OR AGPL-3.0-or-later */
/* Copyright (C) 2026 Enrico Weigelt, metux IT consult <info@metux.net> */
#include <dix-config.h>

#include <stdio.h>
#include <X11/Xmd.h>

#include "dix/capability_priv.h"
#include "dix/client_priv.h"
#include "dix/devices_priv.h"
#include "dix/dix_priv.h"
#include "dix/extension_priv.h"
#include "dix/property_priv.h"
#include "dix/selection_priv.h"
#include "dix/server_priv.h"
#include "include/os.h"
#include "miext/extinit_priv.h"
#include "Xext/xacestr.h"

#include "namespace.h"
#include "hooks.h"

Bool noNamespaceExtension = TRUE;

DevPrivateKeyRec namespaceClientPrivKeyRec = { 0 };

/* capability policy provider (dix/capability_priv.h) */
uint32_t
XnsClientGrants(struct XnamespaceClientPriv *priv)
{
    if (!priv || priv->isServer)
        return XP_CAP_ALL;
    if (!priv->ns)
        return 0;           /* namespace already deleted */

    uint32_t grants = priv->ns->superPower ? XP_CAP_ALL : priv->ns->xpcaps;
    if (priv->app)
        grants &= priv->app->ceiling;   /* the launcher's limit */
    return grants;
}

static uint32_t
XnsCapGrants(ClientPtr client)
{
    return XnsClientGrants(XnsClientPriv(client));
}

/* scoped automation: may injector inject into target's windows (target
   NULL: may it inject at all)? Its launcher's ceiling must allow
   input.inject. */
static Bool
XnsInjectScope(ClientPtr injector, ClientPtr target)
{
    struct XnamespaceClientPriv *priv = XnsClientPriv(injector);

    if (!priv || !priv->ns || !priv->ns->injectInto)
        return FALSE;
    if (priv->app && !(priv->app->ceiling & XP_CAP_BIT(XP_CAP_INPUT_INJECT)))
        return FALSE;
    if (!target)
        return TRUE;
    struct XnamespaceClientPriv *t = XnsClientPriv(target);
    return t && t->ns && strcmp(t->ns->name, priv->ns->injectInto) == 0;
}

/* Extensions without a fixed major opcode that were reviewed for
   confinement: XPrism's own (XPRISM-COLOR goes by window access,
   XPRISM-SCENE is the window manager's only), and NV-GLX, which carries
   NVIDIA's OpenGL. NV-CONTROL, which changes GPU settings, is not. */
static const char *const reviewed_extensions[] = {
    "XPRISM-COLOR", "XPRISM-SCENE", "NV-GLX",
};

/* Extensions not reviewed (driver extensions, new ones) are hidden from
   confined clients unless their namespace allows them. */
Bool
XnsAllowsExtension(struct Xnamespace *ns, const char *name)
{
    for (size_t i = 0; i < sizeof(reviewed_extensions) / sizeof(reviewed_extensions[0]); i++)
        if (strcmp(name, reviewed_extensions[i]) == 0)
            return TRUE;
    if (!ns->allowExtensions)
        return FALSE;
    size_t n = strlen(name);
    for (const char *p = ns->allowExtensions; (p = strstr(p, name)) != NULL; p++)
        if (p[-1] == ' ' && p[n] == ' ')
            return TRUE;
    return FALSE;
}

/* XTest is for namespaces that may inject */
Bool
XnsMayInject(struct Xnamespace *ns)
{
    return ns->superPower || (ns->xpcaps & XP_CAP_BIT(XP_CAP_INPUT_INJECT)) ||
           ns->injectInto != NULL;
}

static void
XnsCapLabel(ClientPtr client, char *buf, size_t len)
{
    struct XnamespaceClientPriv *priv = XnsClientPriv(client);

    if (!priv || !priv->ns) {
        snprintf(buf, len, "no namespace");
        return;
    }
    if (priv->app && priv->app->appId[0])
        snprintf(buf, len, "namespace \"%s\", app \"%s\"",
                 priv->ns->name, priv->app->appId);
    else
        snprintf(buf, len, "namespace \"%s\"", priv->ns->name);
}

void
NamespaceExtensionInit(void)
{
    XNS_LOG("initializing namespace extension ...\n");

    /* load configuration */
    if (!XnsLoadConfig()) {
        XNS_LOG("No config file. disabling Xns extension\n");
        return;
    }

    if (!(dixRegisterPrivateKey(&namespaceClientPrivKeyRec, PRIVATE_CLIENT,
            sizeof(struct XnamespaceClientPriv)) &&
          AddCallback(&ClientStateCallback, hookClientState, NULL) &&
          AddCallback(&PostInitRootWindowCallback, hookInitRootWindow, NULL) &&
          AddCallback(&PropertyFilterCallback, hookWindowProperty, NULL) &&
          AddCallback(&SelectionFilterCallback, hookSelectionFilter, NULL) &&
          AddCallback(&ExtensionAccessCallback, hookExtAccess, NULL) &&
          AddCallback(&ExtensionDispatchCallback, hookExtDispatch, NULL) &&
          AddCallback(&ServerAccessCallback, hookServerAccess, NULL) &&
          AddCallback(&ClientDestroyCallback, hookClientDestroy, NULL) &&
          AddCallback(&ClientAccessCallback, hookClient, NULL) &&
          AddCallback(&DeviceAccessCallback, hookDevice, NULL) &&
          XaceRegisterCallback(XACE_PROPERTY_ACCESS, hookPropertyAccess, NULL) &&
          XaceRegisterCallback(XACE_RECEIVE_ACCESS, hookReceive, NULL) &&
          XaceRegisterCallback(XACE_RESOURCE_ACCESS, hookResourceAccess, NULL) &&
          XaceRegisterCallback(XACE_SEND_ACCESS, hookSend, NULL)))
        FatalError("NamespaceExtensionInit: allocation failure\n");

    XpCapSetProvider(XnsCapGrants, XnsCapLabel);
    XpCapSetInjectScope(XnsInjectScope);

    /* Do the serverClient */
    struct XnamespaceClientPriv *srv = XnsClientPriv(serverClient);
    *srv = (struct XnamespaceClientPriv) { .isServer = TRUE };
    XnamespaceAssignClient(srv, &ns_root);

    /* register the runtime management protocol extension. It is gated to
       superPower clients in hookExtAccess()/hookExtDispatch(), so it stays
       invisible and unreachable to namespaced clients. */
    XnsProtoExtensionInit();
}

/**
 * @brief Assign a client to a namespace, maintaining reference counts.
 *
 * Decrements the old namespace's refcount and increments the new one's. If the
 * old namespace is transient (XNS_ATTR_TRANSIENT) and its last client just
 * left, it is destroyed.
 *
 * @param priv  the client's namespace private (may already reference a namespace)
 * @param newns the namespace to move the client into (NULL to detach)
 */
void XnamespaceAssignClient(struct XnamespaceClientPriv *priv, struct Xnamespace *newns)
{
    struct Xnamespace *oldns = priv->ns;

    if (oldns != NULL)
        oldns->refcnt--;

    priv->ns = newns;

    if (newns != NULL)
        newns->refcnt++;

    /* a transient namespace vanishes once its last client has left */
    if (oldns != NULL && oldns != newns && oldns->refcnt == 0 &&
        oldns->autoRemove && !oldns->builtin)
        XnsDestroyNamespace(oldns);
}

void XnamespaceAssignClientByName(struct XnamespaceClientPriv *priv, const char *name)
{
    struct Xnamespace *newns = XnsFindByName(name);

    if (newns == NULL)
        newns = &ns_anon;

    XnamespaceAssignClient(priv, newns);
}

struct Xnamespace* XnsFindByAuth(size_t szAuthProto, const char* authProto, size_t szAuthToken, const char* authToken)
{
    struct Xnamespace *walk;
    xorg_list_for_each_entry(walk, &ns_list, entry) {
        struct auth_token *at;
        xorg_list_for_each_entry(at, &walk->auth_tokens, entry) {
            int protoLen = at->authProto ? strlen(at->authProto) : 0;
            if ((protoLen == szAuthProto) &&
                (at->authTokenLen == szAuthToken) &&
                (memcmp(at->authTokenData, authToken, szAuthToken)==0) &&
                (memcmp(at->authProto, authProto, szAuthProto)==0))
                return walk;
        }
    }

    // default to anonymous if credentials aren't assigned to specific NS
    return &ns_anon;
}
