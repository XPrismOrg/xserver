/* SPDX-License-Identifier: MIT OR X11
 *
 * Xnamespace management extension - protocol dispatch (DRAFT SKELETON)
 *
 * See doc/Xnamespace-protocol.md. Request parsing uses the X_REQUEST_*
 * macros (including the X_REQUEST_VAR_* helpers); the namespace-model
 * mutations go through the shared setter layer in config.c, which is also
 * used by the config loader so both share one code path.
 *
 * Access control is NOT done here: the extension is made unreachable to
 * non-superPower clients in hook-ext-access.c / hook-ext-dispatch.c, so
 * every handler below may assume its caller is superPower.
 */
#include <dix-config.h>

#include <unistd.h>

#include <X11/Xmd.h>
#include <X11/Xproto.h>

#include "dix/dix_priv.h"
#include "dix/dixstruct_priv.h"
#include "dix/request_priv.h"
#include "dix/rpcbuf_priv.h"
#include "include/dixstruct.h"
#include "include/os.h"
#include "os/pal/identity.h"
#include "dix/capability_priv.h"
#include "miext/extinit_priv.h"

#include "namespace.h"
#include "namespaceproto.h"

/* The namespace-model setters (XnsCreate/XnsDelete/XnsSetCaps/XnsAddToken/
   XnsRemoveToken/XnsLookup/XnsCaps/XnsAttrs/XnsCountTokens) live in config.c
   and are shared with the config loader - declared in namespace.h. */

/* ------------------------------------------------------------------ *
 *  Requests
 * ------------------------------------------------------------------ */

/** @brief Negotiate the extension (major, minor) version. */
static int
ProcXnsQueryVersion(ClientPtr client)
{
    X_REQUEST_HEAD_STRUCT(xXnsQueryVersionReq);
    X_REQUEST_FIELD_CARD16(clientMajorVersion);
    X_REQUEST_FIELD_CARD16(clientMinorVersion);

    xXnsQueryVersionReply reply = {
        .majorVersion = XNS_MAJOR_VERSION,
        .minorVersion = (stuff->clientMajorVersion < XNS_MAJOR_VERSION)
                            ? stuff->clientMinorVersion : XNS_MINOR_VERSION,
    };
    X_REPLY_FIELD_CARD16(majorVersion);
    X_REPLY_FIELD_CARD16(minorVersion);
    return X_SEND_REPLY_SIMPLE(client, reply);
}

/** @brief Create a namespace with the requested capabilities/attributes. */
static int
ProcXnsCreateNamespace(ClientPtr client)
{
    X_REQUEST_HEAD_AT_LEAST(xXnsCreateNamespaceReq);
    X_REQUEST_FIELD_CARD32(capabilities);
    X_REQUEST_FIELD_CARD32(attributes);
    X_REQUEST_FIELD_CARD16(nameLen);

    const char *name;
    X_REQUEST_VAR_BEGIN();
    X_REQUEST_VAR_FIELD(name, stuff->nameLen);
    X_REQUEST_VAR_END();

    if (stuff->capabilities & ~XNS_CAPABILITY_ALL)
        return BadValue;
    if (stuff->attributes & (~XNS_ATTR_ALL | XNS_ATTR_IMMUTABLE))
        return BadValue;       /* IMMUTABLE is server-set only */

    int err = Success;
    if (!XnsCreate(name, stuff->nameLen, stuff->capabilities,
                   stuff->attributes, &err))
        return err;            /* BadName / BadAlloc */

    xXnsAckReply reply = { 0 };
    return X_SEND_REPLY_SIMPLE(client, reply);
}

/** @brief Delete a namespace, optionally killing its clients first. */
static int
ProcXnsDeleteNamespace(ClientPtr client)
{
    X_REQUEST_HEAD_AT_LEAST(xXnsDeleteNamespaceReq);
    X_REQUEST_FIELD_CARD16(nameLen);

    const char *name;
    X_REQUEST_VAR_BEGIN();
    X_REQUEST_VAR_FIELD(name, stuff->nameLen);
    X_REQUEST_VAR_END();

    if (stuff->onClients != XNS_DELETE_FAIL_IF_BUSY &&
        stuff->onClients != XNS_DELETE_KILL_CLIENTS)
        return BadValue;

    struct Xnamespace *ns = XnsLookup(name, stuff->nameLen);
    if (!ns)
        return BadName;
    if (XnsAttrs(ns) & XNS_ATTR_IMMUTABLE)
        return BadAccess;

    /* refuse to delete the namespace the caller itself belongs to: killing
       our own client mid-request would free `client` under us (UAF on the
       reply path). A manager lives in a separate superPower namespace. */
    struct XnamespaceClientPriv *self = XnsClientPriv(client);
    if (self && self->ns == ns)
        return BadAccess;

    int rc = XnsDelete(ns, stuff->onClients);
    if (rc != Success)
        return rc;             /* BadAccess if busy && FAIL_IF_BUSY */

    xXnsAckReply reply = { 0 };
    return X_SEND_REPLY_SIMPLE(client, reply);
}

/** @brief Report a namespace's capabilities, attributes, refcount and token count. */
static int
ProcXnsQueryNamespace(ClientPtr client)
{
    X_REQUEST_HEAD_AT_LEAST(xXnsQueryNamespaceReq);
    X_REQUEST_FIELD_CARD16(nameLen);

    const char *name;
    X_REQUEST_VAR_BEGIN();
    X_REQUEST_VAR_FIELD(name, stuff->nameLen);
    X_REQUEST_VAR_END();

    struct Xnamespace *ns = XnsLookup(name, stuff->nameLen);
    if (!ns)
        return BadName;

    xXnsQueryNamespaceReply reply = {
        .capabilities = XnsCaps(ns),
        .attributes   = XnsAttrs(ns),
        .refcnt       = (CARD32) ns->refcnt,
        .numTokens    = XnsCountTokens(ns),
        .grants       = ns->superPower ? XP_CAP_ALL : ns->xpcaps,
    };
    X_REPLY_FIELD_CARD32(capabilities);
    X_REPLY_FIELD_CARD32(attributes);
    X_REPLY_FIELD_CARD32(refcnt);
    X_REPLY_FIELD_CARD32(numTokens);
    X_REPLY_FIELD_CARD32(grants);
    return X_SEND_REPLY_SIMPLE(client, reply);
}

/** @brief Apply a masked capability update to a namespace. */
static int
ProcXnsSetNamespaceFlags(ClientPtr client)
{
    X_REQUEST_HEAD_AT_LEAST(xXnsSetNamespaceFlagsReq);
    X_REQUEST_FIELD_CARD32(valueMask);
    X_REQUEST_FIELD_CARD32(values);
    X_REQUEST_FIELD_CARD16(nameLen);

    const char *name;
    X_REQUEST_VAR_BEGIN();
    X_REQUEST_VAR_FIELD(name, stuff->nameLen);
    X_REQUEST_VAR_END();

    if (stuff->valueMask & ~XNS_CAPABILITY_ALL)
        return BadValue;

    struct Xnamespace *ns = XnsLookup(name, stuff->nameLen);
    if (!ns)
        return BadName;
    if (XnsAttrs(ns) & XNS_ATTR_IMMUTABLE)
        return BadAccess;

    int rc = XnsSetCaps(ns, stuff->valueMask, stuff->values);
    if (rc != Success)
        return rc;

    xXnsSetNamespaceFlagsReply reply = { .capabilities = XnsCaps(ns) };
    X_REPLY_FIELD_CARD32(capabilities);
    return X_SEND_REPLY_SIMPLE(client, reply);
}

/** @brief Register an auth token in a namespace; reply with its handle. */
static int
ProcXnsAddAuthToken(ClientPtr client)
{
    X_REQUEST_HEAD_AT_LEAST(xXnsAddAuthTokenReq);
    X_REQUEST_FIELD_CARD16(nameLen);
    X_REQUEST_FIELD_CARD16(protoLen);
    X_REQUEST_FIELD_CARD16(dataLen);

    /* three variable-length fields, peeled off in declared order */
    const char *name, *proto, *data;
    X_REQUEST_VAR_BEGIN();
    X_REQUEST_VAR_FIELD(name,  stuff->nameLen);
    X_REQUEST_VAR_FIELD(proto, stuff->protoLen);
    X_REQUEST_VAR_FIELD(data,  stuff->dataLen);
    X_REQUEST_VAR_END();

    if (stuff->protoLen == 0)
        return BadValue;

    struct Xnamespace *ns = XnsLookup(name, stuff->nameLen);
    if (!ns)
        return BadName;
    if (XnsAttrs(ns) & XNS_ATTR_IMMUTABLE)
        return BadAccess;

    CARD32 handle = 0;
    int rc = XnsAddToken(ns, proto, stuff->protoLen, data, stuff->dataLen,
                         &handle);
    if (rc != Success)
        return rc;             /* BadAlloc */

    xXnsAddAuthTokenReply reply = { .tokenHandle = handle };
    X_REPLY_FIELD_CARD32(tokenHandle);
    return X_SEND_REPLY_SIMPLE(client, reply);
}

/** @brief Remove an auth token from a namespace by handle. */
static int
ProcXnsRemoveAuthToken(ClientPtr client)
{
    X_REQUEST_HEAD_AT_LEAST(xXnsRemoveAuthTokenReq);
    X_REQUEST_FIELD_CARD32(tokenHandle);
    X_REQUEST_FIELD_CARD16(nameLen);

    const char *name;
    X_REQUEST_VAR_BEGIN();
    X_REQUEST_VAR_FIELD(name, stuff->nameLen);
    X_REQUEST_VAR_END();

    struct Xnamespace *ns = XnsLookup(name, stuff->nameLen);
    if (!ns)
        return BadName;
    if (XnsAttrs(ns) & XNS_ATTR_IMMUTABLE)
        return BadAccess;

    int rc = XnsRemoveToken(ns, stuff->tokenHandle);
    if (rc != Success)
        return rc;             /* BadMatch */

    xXnsAckReply reply = { 0 };
    return X_SEND_REPLY_SIMPLE(client, reply);
}

/** @brief List every namespace with its capabilities, attributes and counts. */
static int
ProcXnsListNamespaces(ClientPtr client)
{
    X_REQUEST_HEAD_STRUCT(xXnsListNamespacesReq);

    x_rpcbuf_t buf = { .swapped = client->swapped, .err_clear = TRUE };
    CARD32 count = 0;

    struct Xnamespace *ns;
    xorg_list_for_each_entry(ns, &ns_list, entry) {
        x_rpcbuf_write_CARD32(&buf, XnsCaps(ns));
        x_rpcbuf_write_CARD32(&buf, XnsAttrs(ns));
        x_rpcbuf_write_CARD32(&buf, (CARD32) ns->refcnt);
        x_rpcbuf_write_CARD32(&buf, XnsCountTokens(ns));
        x_rpcbuf_write_CARD16(&buf, (CARD16) strlen(ns->name));
        x_rpcbuf_write_CARD16(&buf, 0 /* pad */);
        x_rpcbuf_write_string_pad(&buf, ns->name);
        count++;
    }

    xXnsListNamespacesReply reply = { .count = count };
    X_REPLY_FIELD_CARD32(count);
    return X_SEND_REPLY_WITH_RPCBUF(client, reply, buf);
}

/** @brief List a namespace's auth tokens (handle + protocol name; no key data). */
static int
ProcXnsListAuthTokens(ClientPtr client)
{
    X_REQUEST_HEAD_AT_LEAST(xXnsListAuthTokensReq);
    X_REQUEST_FIELD_CARD16(nameLen);

    const char *name;
    X_REQUEST_VAR_BEGIN();
    X_REQUEST_VAR_FIELD(name, stuff->nameLen);
    X_REQUEST_VAR_END();

    struct Xnamespace *ns = XnsLookup(name, stuff->nameLen);
    if (!ns)
        return BadName;

    x_rpcbuf_t buf = { .swapped = client->swapped, .err_clear = TRUE };
    CARD32 count = 0;
    struct auth_token *at;
    xorg_list_for_each_entry(at, &ns->auth_tokens, entry) {
        x_rpcbuf_write_CARD32(&buf, at->handle);
        x_rpcbuf_write_CARD16(&buf, (CARD16)(at->authProto ? strlen(at->authProto) : 0));
        x_rpcbuf_write_CARD16(&buf, 0 /* pad */);
        x_rpcbuf_write_string_pad(&buf, at->authProto);
        /* deliberately NOT writing authTokenData - no key exfiltration */
        count++;
    }

    xXnsListAuthTokensReply reply = { .count = count };
    X_REPLY_FIELD_CARD32(count);
    return X_SEND_REPLY_WITH_RPCBUF(client, reply, buf);
}

/** @brief Report which namespace a given client (0 = caller) belongs to. */
static int
ProcXnsGetClientNamespace(ClientPtr client)
{
    X_REQUEST_HEAD_STRUCT(xXnsGetClientNamespaceReq);
    X_REQUEST_FIELD_CARD32(clientResource);

    ClientPtr target = client;
    if (stuff->clientResource != 0) {
        int rc = dixLookupResourceOwner(&target, stuff->clientResource, client,
                                        DixGetAttrAccess);
        if (rc != Success)
            return rc;
    }

    struct XnamespaceClientPriv *priv = XnsClientPriv(target);
    const char *nsname = (priv && priv->ns) ? priv->ns->name : "";

    x_rpcbuf_t buf = { .swapped = client->swapped, .err_clear = TRUE };
    x_rpcbuf_write_string_pad(&buf, nsname);

    xXnsGetClientNamespaceReply reply = {
        .isServer = (priv && priv->isServer) ? TRUE : FALSE,
        .nameLen  = (CARD16) strlen(nsname),
    };
    X_REPLY_FIELD_CARD16(nameLen);
    return X_SEND_REPLY_WITH_RPCBUF(client, reply, buf);
}

/* the protocol's grant bits are the server's capability bits */
_Static_assert(XNS_GRANT_ALL == XP_CAP_ALL, "XNS_GRANT_* out of sync with enum xp_cap");
_Static_assert(XNS_GRANT_DISPLAY_CONFIGURE == XP_CAP_BIT(XP_CAP_DISPLAY_CONFIGURE),
               "XNS_GRANT_* out of sync with enum xp_cap");
_Static_assert(XNS_GRANT_COMPOSITOR_LEGACY == XP_CAP_BIT(XP_CAP_COMPOSITOR_LEGACY),
               "XNS_GRANT_* out of sync with enum xp_cap");
_Static_assert(XNS_GRANT_CLIPBOARD_READ == XP_CAP_BIT(XP_CAP_CLIPBOARD_READ),
               "XNS_GRANT_* out of sync with enum xp_cap");

/* printable ASCII only, so a manager can show it without escaping */
static Bool
XnsValidAppString(const char *s, size_t len)
{
    if (len > XNS_APP_STRING_MAX)
        return FALSE;
    for (size_t i = 0; i < len; i++)
        if (s[i] < 0x20 || s[i] > 0x7e)
            return FALSE;
    return TRUE;
}

/** @brief Bind a passed listening socket to a namespace (since 1.1). */
static int
ProcXnsAddListener(ClientPtr client)
{
    SetReqFds(client, 2);
    X_REQUEST_HEAD_AT_LEAST(xXnsAddListenerReq);
    X_REQUEST_FIELD_CARD16(nameLen);
    X_REQUEST_FIELD_CARD16(appIdLen);
    X_REQUEST_FIELD_CARD16(instanceIdLen);
    X_REQUEST_FIELD_CARD16(engineLen);
    X_REQUEST_FIELD_CARD32(ceiling);

    /* take both fds first, so they are closed on every error path
       (including BadLength below) */
    int listen_fd = ReadFdFromClient(client);
    int close_fd = ReadFdFromClient(client);
    int rc = BadValue;
    CARD32 handle = 0;
    struct XnsAppInfo *app = NULL;

    rc = BadLength;
    const char *name, *appId, *instanceId, *engine;
    {
        X_REQUEST_VAR_BEGIN();
        /* X_REQUEST_VAR_FIELD returns BadLength directly; use our own
           bounds check instead so the fds get closed */
        size_t lens[4] = { stuff->nameLen, stuff->appIdLen,
                           stuff->instanceIdLen, stuff->engineLen };
        const char **ptrs[4] = { &name, &appId, &instanceId, &engine };
        for (int i = 0; i < 4; i++) {
            size_t adv = pad_to_int32(lens[i]);
            if (adv > _xreq_left)
                goto fail;
            *ptrs[i] = _xreq_ptr;
            _xreq_ptr += adv;
            _xreq_left -= adv;
        }
        if (_xreq_left != 0)
            goto fail;
    }

    rc = BadValue;
    if (listen_fd < 0 || close_fd < 0)
        goto fail;          /* both fds are required */
    if (!XnsValidAppString(appId, stuff->appIdLen) ||
        !XnsValidAppString(instanceId, stuff->instanceIdLen) ||
        !XnsValidAppString(engine, stuff->engineLen))
        goto fail;

    struct Xnamespace *ns = XnsLookup(name, stuff->nameLen);
    if (!ns) {
        rc = BadName;
        goto fail;
    }
    /* the root namespace is all-powerful: never hand out a socket into it */
    if (ns == &ns_root || XnsCaps(ns) & XNS_CAPABILITY_ADMIN) {
        rc = BadAccess;
        goto fail;
    }

    app = XnsAppInfoNew(appId, stuff->appIdLen, instanceId, stuff->instanceIdLen,
                        engine, stuff->engineLen);
    if (!app) {
        rc = BadAlloc;
        goto fail;
    }
    /* unknown (future) bits are ignored, so XNS_NO_CEILING means no limit */
    app->ceiling = stuff->ceiling & XP_CAP_ALL;

    rc = XnsAddListener(ns, app, listen_fd, close_fd, &handle);
    XnsAppInfoUnref(app);   /* the listener holds its own reference */
    if (rc != Success)
        goto fail;

    xXnsAddListenerReply reply = { .handle = handle };
    X_REPLY_FIELD_CARD32(handle);
    return X_SEND_REPLY_SIMPLE(client, reply);

fail:
    if (listen_fd >= 0)
        close(listen_fd);
    if (close_fd >= 0)
        close(close_fd);
    return rc;
}

/** @brief Stop a listener added with AddListener (since 1.1). */
static int
ProcXnsRemoveListener(ClientPtr client)
{
    X_REQUEST_HEAD_STRUCT(xXnsRemoveListenerReq);
    X_REQUEST_FIELD_CARD32(handle);

    int rc = XnsRemoveListener(stuff->handle);
    if (rc != Success)
        return rc;

    xXnsAckReply reply = { 0 };
    return X_SEND_REPLY_SIMPLE(client, reply);
}

/** @brief Replace a namespace's XPrism capabilities (since 1.1). */
static int
ProcXnsSetNamespaceGrants(ClientPtr client)
{
    X_REQUEST_HEAD_AT_LEAST(xXnsSetNamespaceGrantsReq);
    X_REQUEST_FIELD_CARD32(grants);
    X_REQUEST_FIELD_CARD16(nameLen);

    const char *name;
    X_REQUEST_VAR_BEGIN();
    X_REQUEST_VAR_FIELD(name, stuff->nameLen);
    X_REQUEST_VAR_END();

    if (stuff->grants & ~XNS_GRANT_ALL)
        return BadValue;

    struct Xnamespace *ns = XnsLookup(name, stuff->nameLen);
    if (!ns)
        return BadName;
    /* root holds everything anyway; anon is for unauthenticated clients */
    if ((XnsAttrs(ns) & XNS_ATTR_IMMUTABLE) || ns->superPower)
        return BadAccess;

    ns->xpcaps = stuff->grants;
    XNS_LOG("namespace \"%s\" grants now 0x%x\n", ns->name, (unsigned) ns->xpcaps);

    xXnsAckReply reply = { 0 };
    return X_SEND_REPLY_SIMPLE(client, reply);
}

/** @brief Namespace, app info and peer identity of a client (since 1.1). */
static int
ProcXnsGetClientInfo(ClientPtr client)
{
    X_REQUEST_HEAD_STRUCT(xXnsGetClientInfoReq);
    X_REQUEST_FIELD_CARD32(clientResource);

    ClientPtr target = client;
    if (stuff->clientResource != 0) {
        int rc = dixLookupResourceOwner(&target, stuff->clientResource, client,
                                        DixGetAttrAccess);
        if (rc != Success)
            return rc;
    }

    struct XnamespaceClientPriv *priv = XnsClientPriv(target);
    const char *nsname = (priv && priv->ns) ? priv->ns->name : "";
    struct XnsAppInfo *app = priv ? priv->app : NULL;
    const char *appId = app ? app->appId : "";
    const char *instanceId = app ? app->instanceId : "";
    const char *engine = app ? app->engine : "";

    CARD8 flags = 0;
    if (priv && priv->socketBound)
        flags |= XNS_CLIENT_INFO_SOCKET_BOUND;

    CARD32 pid = 0, uid = 0;
    const struct peer_identity *id = ClientPeerIdentity(target);
    if (id && (id->fields & PEER_ID_PID)) {
        pid = (CARD32) id->pid;
        flags |= XNS_CLIENT_INFO_PID;
    }
    if (id && (id->fields & PEER_ID_UID)) {
        uid = (CARD32) id->uid;
        flags |= XNS_CLIENT_INFO_UID;
    }

    x_rpcbuf_t buf = { .swapped = client->swapped, .err_clear = TRUE };
    x_rpcbuf_write_string_pad(&buf, nsname);
    x_rpcbuf_write_string_pad(&buf, appId);
    x_rpcbuf_write_string_pad(&buf, instanceId);
    x_rpcbuf_write_string_pad(&buf, engine);

    xXnsGetClientInfoReply reply = {
        .flags         = flags,
        .pid           = pid,
        .uid           = uid,
        .nsNameLen     = (CARD16) strlen(nsname),
        .appIdLen      = (CARD16) strlen(appId),
        .instanceIdLen = (CARD16) strlen(instanceId),
        .engineLen     = (CARD16) strlen(engine),
        .grants        = XnsClientGrants(priv),
        .ceiling       = app ? app->ceiling : XNS_NO_CEILING,
    };
    X_REPLY_FIELD_CARD32(grants);
    X_REPLY_FIELD_CARD32(ceiling);
    X_REPLY_FIELD_CARD32(pid);
    X_REPLY_FIELD_CARD32(uid);
    X_REPLY_FIELD_CARD16(nsNameLen);
    X_REPLY_FIELD_CARD16(appIdLen);
    X_REPLY_FIELD_CARD16(instanceIdLen);
    X_REPLY_FIELD_CARD16(engineLen);
    return X_SEND_REPLY_WITH_RPCBUF(client, reply, buf);
}

/* ------------------------------------------------------------------ *
 *  Dispatch (registered for both proc and sproc - all field swapping
 *  happens inside the handlers via the X_REQUEST_* macros, like DPMS)
 * ------------------------------------------------------------------ */

/**
 * @brief Extension request entry point (used for both swapped and unswapped
 *        clients; each handler does its own field swapping via the
 *        X_REQUEST_* macros).
 * @param client the requesting client
 * @return an X11 status code
 */
static int
ProcXnsDispatch(ClientPtr client)
{
    REQUEST(xReq);

    switch (stuff->data) {
    case X_XnsQueryVersion:        return ProcXnsQueryVersion(client);
    case X_XnsListNamespaces:      return ProcXnsListNamespaces(client);
    case X_XnsCreateNamespace:     return ProcXnsCreateNamespace(client);
    case X_XnsDeleteNamespace:     return ProcXnsDeleteNamespace(client);
    case X_XnsQueryNamespace:      return ProcXnsQueryNamespace(client);
    case X_XnsSetNamespaceFlags:   return ProcXnsSetNamespaceFlags(client);
    case X_XnsAddAuthToken:        return ProcXnsAddAuthToken(client);
    case X_XnsRemoveAuthToken:     return ProcXnsRemoveAuthToken(client);
    case X_XnsListAuthTokens:      return ProcXnsListAuthTokens(client);
    case X_XnsGetClientNamespace:  return ProcXnsGetClientNamespace(client);
    case X_XnsAddListener:         return ProcXnsAddListener(client);
    case X_XnsRemoveListener:      return ProcXnsRemoveListener(client);
    case X_XnsGetClientInfo:       return ProcXnsGetClientInfo(client);
    case X_XnsSetNamespaceGrants:  return ProcXnsSetNamespaceGrants(client);
    default:                       return BadRequest;
    }
}

/**
 * @brief Register the namespace management extension.
 *
 * Called from NamespaceExtensionInit() once a namespace config has loaded.
 * Access is gated to superPower clients by the Xace extension hooks, so the
 * extension is invisible and unreachable to namespaced clients.
 */
void
XnsProtoExtensionInit(void)
{
    AddExtension(XNS_EXTENSION_NAME, 0, 0,
                 ProcXnsDispatch, ProcXnsDispatch,
                 NULL, StandardMinorOpcode);
}
