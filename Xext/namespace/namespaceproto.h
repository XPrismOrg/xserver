/* SPDX-License-Identifier: MIT OR X11 OR AGPLv3
 *
 * Xnamespace management extension - protocol definitions (DRAFT v1.0)
 *
 * See doc/Xnamespace-protocol.md for the specification.
 *
 * This header is the on-the-wire contract: it would normally live in
 * xorgproto (as Xnamespaceproto.h) and be shared with client bindings
 * (e.g. go-x11proto). It is kept here for now while the design is in flux.
 */
#ifndef _XSERVER_NAMESPACEPROTO_H
#define _XSERVER_NAMESPACEPROTO_H

#include <X11/Xmd.h>
#include <X11/Xproto.h>

#define XNS_EXTENSION_NAME      "X-NAMESPACE"
#define XNS_MAJOR_VERSION       1
#define XNS_MINOR_VERSION       2

/* maximum namespace name length (keeps names config-file expressible) */
#define XNS_NAME_MAX            255

/* request opcodes (minor) */
#define X_XnsQueryVersion       0
#define X_XnsListNamespaces     1
#define X_XnsCreateNamespace    2
#define X_XnsDeleteNamespace    3
#define X_XnsQueryNamespace     4
#define X_XnsSetNamespaceFlags  5
#define X_XnsAddAuthToken       6
#define X_XnsRemoveAuthToken    7
#define X_XnsListAuthTokens     8
#define X_XnsGetClientNamespace 9
#define X_XnsAddListener        10  /* since 1.1 */
#define X_XnsRemoveListener     11  /* since 1.1 */
#define X_XnsGetClientInfo      12  /* since 1.1 */
#define X_XnsSetNamespaceGrants 13  /* since 1.1 */
#define X_XnsSetPermissionAgent 14  /* since 1.2 */
#define X_XnsAnswerPermission   15  /* since 1.2 */
#define X_XnsSetNamespaceInjectScope 16 /* since 1.2 */
#define XnsNumberRequests       17

/* events (since 1.2) */
#define XnsPermissionRequest    0
#define XnsNumberEvents         1

/* XPrism capabilities (grants), see doc/capabilities.md. Bit n is the
   n-th entry of enum xp_cap in dix/capability_priv.h. Since 1.1. */
#define XNS_GRANT_CAPTURE_SCREEN      (1u << 0)
#define XNS_GRANT_CAPTURE_WINDOW      (1u << 1)
#define XNS_GRANT_INPUT_INJECT        (1u << 2)
#define XNS_GRANT_INPUT_MONITOR       (1u << 3)
#define XNS_GRANT_INPUT_VIRTUAL_SEAT  (1u << 4)
#define XNS_GRANT_WINDOW_ENUMERATE    (1u << 5)
#define XNS_GRANT_WINDOW_INSPECT      (1u << 6)
#define XNS_GRANT_WINDOW_MANAGE       (1u << 7)
#define XNS_GRANT_CLIPBOARD_READ      (1u << 8)
#define XNS_GRANT_CLIPBOARD_WRITE     (1u << 9)
#define XNS_GRANT_DISPLAY_CONFIGURE   (1u << 10)
#define XNS_GRANT_COMPOSITOR_LEGACY   (1u << 11)
#define XNS_GRANT_ALL                 0x00000fffu
#define XNS_NO_CEILING                0xffffffffu /* AddListener: no limit */

#define XNS_APP_STRING_MAX      255 /* app id, instance id, engine */

/* capability bits (see struct Xnamespace allow* / superPower) */
#define XNS_CAPABILITY_MOUSE_MOTION     (1u << 0)
#define XNS_CAPABILITY_SHAPE            (1u << 1)
#define XNS_CAPABILITY_TRANSPARENCY     (1u << 2)
#define XNS_CAPABILITY_INPUT            (1u << 3)
#define XNS_CAPABILITY_KEYBOARD         (1u << 4)
#define XNS_CAPABILITY_ADMIN            (1u << 5)
#define XNS_CAPABILITY_ALL              0x0000003fu

/* namespace attribute bits */
#define XNS_ATTR_IMMUTABLE              (1u << 0)   /* read-only (root/anon) */
#define XNS_ATTR_TRANSIENT              (1u << 1)   /* drop when last client exits */
#define XNS_ATTR_ALL                    0x00000003u

/* values for xXnsDeleteNamespaceReq.onClients */
#define XNS_DELETE_FAIL_IF_BUSY         0   /* BadAccess if any client present */
#define XNS_DELETE_KILL_CLIENTS         1   /* terminate clients, then delete */

/* ------------------------------------------------------------------ *
 *  Requests
 *
 *  Every variable-length field carries its byte length in a *Len field
 *  of the fixed struct; the payloads follow in declared order, each
 *  padded to a 4-byte boundary. This matches the xcb/xgb codegen idiom
 *  (length field + char-list) and the X_REQUEST_VAR_* parsing macros.
 * ------------------------------------------------------------------ */

typedef struct {
    CARD8   reqType;            /* extension major opcode */
    CARD8   xnsReqType;         /* X_XnsQueryVersion */
    CARD16  length;
    CARD16  clientMajorVersion;
    CARD16  clientMinorVersion;
} xXnsQueryVersionReq;
#define sz_xXnsQueryVersionReq 8

typedef struct {
    CARD8   reqType;
    CARD8   xnsReqType;         /* X_XnsListNamespaces */
    CARD16  length;
} xXnsListNamespacesReq;
#define sz_xXnsListNamespacesReq 4

typedef struct {
    CARD8   reqType;
    CARD8   xnsReqType;         /* X_XnsCreateNamespace */
    CARD16  length;
    CARD32  capabilities;
    CARD32  attributes;         /* XNS_ATTR_TRANSIENT honored; IMMUTABLE rejected */
    CARD16  nameLen;
    CARD16  pad0;
    /* CARD8 name[nameLen], padded */
} xXnsCreateNamespaceReq;
#define sz_xXnsCreateNamespaceReq 16

typedef struct {
    CARD8   reqType;
    CARD8   xnsReqType;         /* X_XnsDeleteNamespace */
    CARD16  length;
    CARD8   onClients;          /* XNS_DELETE_* */
    CARD8   pad0;
    CARD16  nameLen;
    /* CARD8 name[nameLen], padded */
} xXnsDeleteNamespaceReq;
#define sz_xXnsDeleteNamespaceReq 8

typedef struct {
    CARD8   reqType;
    CARD8   xnsReqType;         /* X_XnsQueryNamespace */
    CARD16  length;
    CARD16  nameLen;
    CARD16  pad0;
    /* CARD8 name[nameLen], padded */
} xXnsQueryNamespaceReq;
#define sz_xXnsQueryNamespaceReq 8

typedef struct {
    CARD8   reqType;
    CARD8   xnsReqType;         /* X_XnsSetNamespaceFlags */
    CARD16  length;
    CARD32  valueMask;          /* which capability bits to apply */
    CARD32  values;             /* new values for masked bits */
    CARD16  nameLen;
    CARD16  pad0;
    /* CARD8 name[nameLen], padded */
} xXnsSetNamespaceFlagsReq;
#define sz_xXnsSetNamespaceFlagsReq 16

typedef struct {
    CARD8   reqType;
    CARD8   xnsReqType;         /* X_XnsAddAuthToken */
    CARD16  length;
    CARD16  nameLen;
    CARD16  protoLen;
    CARD16  dataLen;
    CARD16  pad0;
    /* CARD8 name[nameLen], proto[protoLen], data[dataLen]; each padded */
} xXnsAddAuthTokenReq;
#define sz_xXnsAddAuthTokenReq 12

typedef struct {
    CARD8   reqType;
    CARD8   xnsReqType;         /* X_XnsRemoveAuthToken */
    CARD16  length;
    CARD32  tokenHandle;
    CARD16  nameLen;
    CARD16  pad0;
    /* CARD8 name[nameLen], padded */
} xXnsRemoveAuthTokenReq;
#define sz_xXnsRemoveAuthTokenReq 12

typedef struct {
    CARD8   reqType;
    CARD8   xnsReqType;         /* X_XnsListAuthTokens */
    CARD16  length;
    CARD16  nameLen;
    CARD16  pad0;
    /* CARD8 name[nameLen], padded */
} xXnsListAuthTokensReq;
#define sz_xXnsListAuthTokensReq 8

typedef struct {
    CARD8   reqType;
    CARD8   xnsReqType;         /* X_XnsGetClientNamespace */
    CARD16  length;
    CARD32  clientResource;     /* 0 = the calling client */
} xXnsGetClientNamespaceReq;
#define sz_xXnsGetClientNamespaceReq 8

/* ------------------------------------------------------------------ *
 *  Replies (all 32-byte aligned header; variable tails via rpcbuf)
 * ------------------------------------------------------------------ */

typedef struct {
    BYTE    type;               /* X_Reply */
    CARD8   pad0;
    CARD16  sequenceNumber;
    CARD32  length;
    CARD16  majorVersion;
    CARD16  minorVersion;
    CARD32  pad1;
    CARD32  pad2;
    CARD32  pad3;
    CARD32  pad4;
    CARD32  pad5;
} xXnsQueryVersionReply;
#define sz_xXnsQueryVersionReply 32

/* ListNamespaces reply: header + `count` NAMESPACEINFO records (rpcbuf).
   each record: capabilities, attributes, refcnt, numTokens (CARD32 x4),
   nameLen (CARD16), pad (CARD16), name bytes (padded). */
typedef struct {
    BYTE    type;
    CARD8   pad0;
    CARD16  sequenceNumber;
    CARD32  length;             /* size of trailing records, in 4-byte units */
    CARD32  count;              /* number of namespaces */
    CARD32  pad1;
    CARD32  pad2;
    CARD32  pad3;
    CARD32  pad4;
    CARD32  pad5;
} xXnsListNamespacesReply;
#define sz_xXnsListNamespacesReply 32

typedef struct {
    BYTE    type;
    CARD8   pad0;
    CARD16  sequenceNumber;
    CARD32  length;             /* 0 - empty ack */
    CARD32  pad1;
    CARD32  pad2;
    CARD32  pad3;
    CARD32  pad4;
    CARD32  pad5;
    CARD32  pad6;
} xXnsAckReply;                 /* Create / Delete / RemoveAuthToken */
#define sz_xXnsAckReply 32

typedef struct {
    BYTE    type;
    CARD8   pad0;
    CARD16  sequenceNumber;
    CARD32  length;             /* 0 */
    CARD32  capabilities;
    CARD32  attributes;
    CARD32  refcnt;
    CARD32  numTokens;
    CARD32  grants;             /* XNS_GRANT_*, since 1.1 (was pad) */
    CARD32  pad2;
} xXnsQueryNamespaceReply;
#define sz_xXnsQueryNamespaceReply 32

typedef struct {
    BYTE    type;
    CARD8   pad0;
    CARD16  sequenceNumber;
    CARD32  length;             /* 0 */
    CARD32  capabilities;       /* resulting capabilities */
    CARD32  pad1;
    CARD32  pad2;
    CARD32  pad3;
    CARD32  pad4;
    CARD32  pad5;
} xXnsSetNamespaceFlagsReply;
#define sz_xXnsSetNamespaceFlagsReply 32

typedef struct {
    BYTE    type;
    CARD8   pad0;
    CARD16  sequenceNumber;
    CARD32  length;             /* 0 */
    CARD32  tokenHandle;
    CARD32  pad1;
    CARD32  pad2;
    CARD32  pad3;
    CARD32  pad4;
    CARD32  pad5;
} xXnsAddAuthTokenReply;
#define sz_xXnsAddAuthTokenReply 32

/* ListAuthTokens reply: header + `count` records (rpcbuf).
   each record: tokenHandle (CARD32), protoLen (CARD16), pad (CARD16),
   proto bytes (padded). NO key material. */
typedef struct {
    BYTE    type;
    CARD8   pad0;
    CARD16  sequenceNumber;
    CARD32  length;
    CARD32  count;
    CARD32  pad1;
    CARD32  pad2;
    CARD32  pad3;
    CARD32  pad4;
    CARD32  pad5;
} xXnsListAuthTokensReply;
#define sz_xXnsListAuthTokensReply 32

/* GetClientNamespace reply: header + namespace name (rpcbuf) */
typedef struct {
    BYTE    type;
    CARD8   isServer;
    CARD16  sequenceNumber;
    CARD32  length;             /* size of trailing name, in 4-byte units */
    CARD16  nameLen;
    CARD16  pad0;
    CARD32  pad1;
    CARD32  pad2;
    CARD32  pad3;
    CARD32  pad4;
    CARD32  pad5;
} xXnsGetClientNamespaceReply;
#define sz_xXnsGetClientNamespaceReply 32

/*
 * AddListener (since 1.1): bind a listening socket to a namespace.
 *
 * Two file descriptors are passed with the request (SCM_RIGHTS), in order:
 *   1. a listening SOCK_STREAM Unix domain socket, bound and listening;
 *   2. a "close fd" (e.g. the read end of a pipe). When it hangs up or
 *      becomes readable, the server stops listening on the socket.
 * Every client accepted on the socket is placed in the namespace,
 * regardless of the authorization data it sends, and needs no other
 * authorization. Clients already connected stay connected when the
 * listener goes away. Deleting the namespace removes its listeners.
 *
 * The launcher may describe the app behind the socket: app id, instance id
 * and sandbox engine (each up to XNS_APP_STRING_MAX printable ASCII bytes,
 * may be empty). Every client from the socket carries this description,
 * see GetClientInfo. It is what the launcher claims, nothing more: the
 * server cannot check it.
 *
 * The ceiling caps the capabilities of every client from the socket: a
 * client holds its namespace's grants AND the ceiling, whatever the
 * namespace is granted later.
 */
typedef struct {
    CARD8   reqType;
    CARD8   xnsReqType;         /* X_XnsAddListener */
    CARD16  length;
    CARD16  nameLen;
    CARD16  appIdLen;
    CARD16  instanceIdLen;
    CARD16  engineLen;
    CARD32  ceiling;            /* most XNS_GRANT_* its clients may ever
                                   hold; XNS_NO_CEILING for no limit */
    /* CARD8 name[nameLen], padded
       CARD8 appId[appIdLen], padded
       CARD8 instanceId[instanceIdLen], padded
       CARD8 engine[engineLen], padded */
} xXnsAddListenerReq;
#define sz_xXnsAddListenerReq 16

typedef struct {
    BYTE    type;
    CARD8   pad0;
    CARD16  sequenceNumber;
    CARD32  length;
    CARD32  handle;             /* for RemoveListener */
    CARD32  pad1;
    CARD32  pad2;
    CARD32  pad3;
    CARD32  pad4;
    CARD32  pad5;
} xXnsAddListenerReply;
#define sz_xXnsAddListenerReply 32

/* RemoveListener (since 1.1): stop a listener added with AddListener. */
typedef struct {
    CARD8   reqType;
    CARD8   xnsReqType;         /* X_XnsRemoveListener */
    CARD16  length;
    CARD32  handle;
} xXnsRemoveListenerReq;
#define sz_xXnsRemoveListenerReq 8

/* GetClientInfo (since 1.1): what the server knows about a client. */
#define XNS_CLIENT_INFO_SOCKET_BOUND  (1u << 0) /* came in via AddListener */
#define XNS_CLIENT_INFO_PID           (1u << 1) /* pid is valid */
#define XNS_CLIENT_INFO_UID           (1u << 2) /* uid is valid */

typedef struct {
    CARD8   reqType;
    CARD8   xnsReqType;         /* X_XnsGetClientInfo */
    CARD16  length;
    CARD32  clientResource;     /* a resource of the client; 0 = caller */
} xXnsGetClientInfoReq;
#define sz_xXnsGetClientInfoReq 8

typedef struct {
    BYTE    type;
    CARD8   flags;              /* XNS_CLIENT_INFO_* */
    CARD16  sequenceNumber;
    CARD32  length;
    CARD32  pid;                /* as seen from the server's pid namespace */
    CARD32  uid;
    CARD16  nsNameLen;
    CARD16  appIdLen;
    CARD16  instanceIdLen;
    CARD16  engineLen;
    CARD32  grants;             /* XNS_GRANT_* the client holds now
                                   (namespace grants AND ceiling) */
    CARD32  ceiling;            /* its listener's ceiling, or XNS_NO_CEILING */
    /* CARD8 nsName[nsNameLen], padded, then appId, instanceId, engine,
       each padded */
} xXnsGetClientInfoReply;
#define sz_xXnsGetClientInfoReply 32

/* SetNamespaceGrants (since 1.1): replace a namespace's XPrism
   capabilities. Takes effect for its clients' next request. BadAccess for
   the built-in namespaces. Replies with an empty ack. */
typedef struct {
    CARD8   reqType;
    CARD8   xnsReqType;         /* X_XnsSetNamespaceGrants */
    CARD16  length;
    CARD32  grants;             /* XNS_GRANT_* */
    CARD16  nameLen;
    CARD16  pad0;
    /* CARD8 name[nameLen], padded */
} xXnsSetNamespaceGrantsReq;
#define sz_xXnsSetNamespaceGrantsReq 12

/* SetPermissionAgent (since 1.2): the caller becomes (enable = 1) or
   stops being (0) the permission agent, which is asked when a confined
   client lacks a capability (capability mode enforce). One agent at a
   time: BadAccess if another client is it. No reply. */
typedef struct {
    CARD8   reqType;
    CARD8   xnsReqType;         /* X_XnsSetPermissionAgent */
    CARD16  length;
    CARD8   enable;
    CARD8   pad0;
    CARD16  pad1;
} xXnsSetPermissionAgentReq;
#define sz_xXnsSetPermissionAgentReq 8

/* answers */
#define XNS_PERMISSION_DENY_ONCE        0
#define XNS_PERMISSION_ALLOW_ONCE       1
#define XNS_PERMISSION_ALLOW_ALWAYS     2   /* while the client runs */
#define XNS_PERMISSION_DENY_ALWAYS      3   /* and do not ask again */

/* AnswerPermission (since 1.2): answer a PermissionRequest. Only the
   agent may; BadAccess for others. A prompt that is no longer open (it
   timed out, its client went) is ignored. No reply. */
typedef struct {
    CARD8   reqType;
    CARD8   xnsReqType;         /* X_XnsAnswerPermission */
    CARD16  length;
    CARD32  id;
    CARD8   answer;             /* XNS_PERMISSION_* */
    CARD8   pad0;
    CARD16  pad1;
} xXnsAnswerPermissionReq;
#define sz_xXnsAnswerPermissionReq 12

/* SetNamespaceInjectScope (since 1.2): scoped automation. Clients of
   namespace name may use XTest, through a seat of their own, into the
   windows of namespace target (targetLen 0: no longer). BadAccess for the
   built-in and superpower namespaces, BadName. Replies with an empty
   ack. */
typedef struct {
    CARD8   reqType;
    CARD8   xnsReqType;         /* X_XnsSetNamespaceInjectScope */
    CARD16  length;
    CARD16  nameLen;
    CARD16  targetLen;
    /* CARD8 name[nameLen], padded, then target[targetLen], padded */
} xXnsSetNamespaceInjectScopeReq;
#define sz_xXnsSetNamespaceInjectScopeReq 8

/* PermissionRequest event (since 1.2), to the agent: a client's request
   needs a capability it lacks; the client waits for the answer (at most
   60 seconds, then refused). clientResource is the client's resource
   base, which GetClientInfo takes. */
typedef struct {
    BYTE    type;               /* event base + XnsPermissionRequest */
    CARD8   capability;         /* bit number, as XNS_GRANT_* */
    CARD16  sequenceNumber;
    CARD32  id;
    CARD32  clientResource;
    CARD32  target;             /* the resource the request is about, or 0 */
    CARD8   majorOpcode;        /* the request */
    CARD8   pad0;
    CARD16  minorOpcode;
    CARD32  timestamp;
    CARD32  pad1;
    CARD32  pad2;
} xXnsPermissionRequestEvent;

#endif /* _XSERVER_NAMESPACEPROTO_H */
