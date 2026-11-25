/* SPDX-License-Identifier: X11 OR MIT OR AGPL-3.0-or-later */
/* Copyright (C) 2026 Enrico Weigelt, metux IT consult <info@metux.net> */
#ifndef __XSERVER_NAMESPACE_H
#define __XSERVER_NAMESPACE_H

#include <stdio.h>
#include <X11/Xmd.h>

#include "include/dixstruct.h"
#include "include/list.h"
#include "include/privates.h"
#include "include/window.h"
#include "include/windowstr.h"

struct auth_token {
    struct xorg_list entry;
    char *authProto;
    char *authTokenData;
    size_t authTokenLen;
    XID authId;
    CARD32 handle;              /* per-namespace token handle */
};

struct Xnamespace {
    struct xorg_list entry;
    const char *name;
    Bool builtin;
    Bool allowMouseMotion;
    Bool allowShape;
    Bool allowTransparency;
    Bool allowXInput;
    Bool allowXKeyboard;
    Bool superPower;
    uint32_t xpcaps;            /* XPrism capabilities (dix/capability_priv.h);
                                   superPower implies all of them */
    Bool autoRemove;           /* destroy when the last client exits */
    struct xorg_list auth_tokens;
    CARD32 tokenHandleSeq;      /* monotonic per-namespace handle counter */
    size_t refcnt;
    WindowPtr rootWindow;
};

extern struct xorg_list ns_list;
extern struct Xnamespace ns_root;
extern struct Xnamespace ns_anon;

/* What the launcher said about the app behind a listener (protocol 1.1).
   Shared by the listener and its clients; refcounted, so clients keep it
   after the listener is gone. Strings are never NULL (maybe empty). */
struct XnsAppInfo {
    int refcnt;
    char *appId;                /* e.g. "org.example.Editor" */
    char *instanceId;           /* e.g. Flatpak instance id */
    char *engine;               /* e.g. "flatpak", "bubblewrap" */
    uint32_t ceiling;           /* most XPrism capabilities its clients may
                                   hold (XP_CAP_ALL: no limit) */
};

struct XnamespaceClientPriv;
struct XnsAppInfo *XnsAppInfoNew(const char *appId, size_t appIdLen,
                                 const char *instanceId, size_t instanceIdLen,
                                 const char *engine, size_t engineLen);
struct XnsAppInfo *XnsAppInfoRef(struct XnsAppInfo *app);

/* XPrism capabilities a client holds: its namespace's grants, limited by
   its listener's ceiling (namespace.c) */
uint32_t XnsClientGrants(struct XnamespaceClientPriv *priv);
void XnsAppInfoUnref(struct XnsAppInfo *app);

struct XnamespaceClientPriv {
    Bool isServer;
    XID authId;
    struct Xnamespace* ns;
    Bool socketBound;           /* placed by its listener; auth data ignored */
    struct XnsAppInfo *app;     /* set for socket-bound clients, else NULL */
};

#define NS_NAME_ROOT      "root"
#define NS_NAME_ANONYMOUS "anon"

extern DevPrivateKeyRec namespaceClientPrivKeyRec;

Bool XnsLoadConfig(void);
void XnsProtoExtensionInit(void);
WindowPtr XnsCreateVirtualRoot(WindowPtr realRoot, const char *name);
struct Xnamespace *XnsFindByName(const char* name);
struct Xnamespace *XnsLookup(const char *name, size_t namelen);
int XnsAddToken(struct Xnamespace *ns, const char *proto, size_t protolen,
                const char *data, size_t datalen, CARD32 *handleOut);

/* namespace-model setter layer (config.c), shared with the protocol handlers */
struct Xnamespace *XnsCreate(const char *name, size_t namelen,
                             CARD32 caps, CARD32 attrs, int *err);
void XnsDestroyNamespace(struct Xnamespace *ns);
int  XnsDelete(struct Xnamespace *ns, CARD8 onClients);
int  XnsSetCaps(struct Xnamespace *ns, CARD32 mask, CARD32 values);
int  XnsRemoveToken(struct Xnamespace *ns, CARD32 handle);
CARD32 XnsCountTokens(struct Xnamespace *ns);

/* listening sockets bound to a namespace (listener.c) */
int XnsAddListener(struct Xnamespace *ns, struct XnsAppInfo *app,
                   int listen_fd, int close_fd, CARD32 *handleOut);
int XnsRemoveListener(CARD32 handle);
void XnsRemoveListenersOf(struct Xnamespace *ns);
/* namespace (and, if @p app is given, a new reference to the app info) of
   the listener behind @p label, or NULL if it isn't one of ours */
struct Xnamespace *XnsListenerNamespace(void *label, struct XnsAppInfo **app);
CARD32 XnsCaps(const struct Xnamespace *ns);
CARD32 XnsAttrs(const struct Xnamespace *ns);
struct Xnamespace* XnsFindByAuth(size_t szAuthProto, const char* authProto, size_t szAuthToken, const char* authToken);
void XnamespaceAssignClient(struct XnamespaceClientPriv *priv, struct Xnamespace *ns);
void XnamespaceAssignClientByName(struct XnamespaceClientPriv *priv, const char *name);

static inline struct XnamespaceClientPriv *XnsClientPriv(ClientPtr client) {
    if (client == NULL) return NULL;
    return dixLookupPrivate(&client->devPrivates, &namespaceClientPrivKeyRec);
}

static inline Bool XnsClientSameNS(struct XnamespaceClientPriv *p1, struct XnamespaceClientPriv *p2)
{
    if (!p1 && !p2)
        return TRUE;
    if (!p1 || !p2)
        return FALSE;
    return (p1->ns == p2->ns);
}

#define XNS_LOG(...) do { printf("XNS "); printf(__VA_ARGS__); } while (0)

static inline Bool streq(const char *a, const char *b)
{
    if (!a && !b)
        return TRUE;
    if (!a || !b)
        return FALSE;
    return (strcmp(a,b) == 0);
}

#endif /* __XSERVER_NAMESPACE_H */
