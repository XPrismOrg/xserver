X-NAMESPACE protocol extension, version 1.2
===========================================

This document specifies an X11 protocol extension for the Xnamespace
feature. Its purpose is to let an authorized management client
**dynamically manage namespaces at runtime** — the settings that today can
only be provisioned statically from the `namespace.conf` file at server
startup (see `doc/Xnamespace.md` and `Xext/namespace/config.c`).

The expected user is a container/session manager: it creates a namespace
when a container starts and deletes it when the container finishes. Traffic
is very low, so requests are kept simple and synchronous (round-tripped)
rather than optimised for throughput — many X11 packets are needlessly
complex and this extension deliberately avoids that.

The wire protocol mirrors the existing in-memory model (`struct Xnamespace`
in `Xext/namespace/namespace.h`) so the extension stays a thin, auditable
layer over data the server already maintains.

Status: implemented in XPrism (`Xext/namespace/proto.c`), version 1.2.
Additions keep compatibility and raise the minor version (section 5).


1. Goals and non-goals
----------------------

Goals:

 * Enumerate namespaces and their settings.
 * Create and delete namespaces at runtime.
 * Query and modify per-namespace capability flags
   (`mouse-motion`, `shape`, `transparency`, `xinput`, `xkeyboard`,
   `superpower`).
 * Add, list and remove authentication tokens that map clients into a
   namespace (the `auth` lines in the config file today).
 * Auto-remove a namespace when its last client exits (transient
   namespaces for transient containers).

Not provided, by design:

 * **No persistence.** Xservers never persist state; runtime changes live
   only in memory. The config file remains the boot-time seed and is never
   rewritten. There is no `SaveConfig`/`ReloadConfig`.
 * **No client reassignment.** A client's namespace is fixed at connect
   time by its auth token. There is no `AssignClient` request. A namespace
   is emptied by terminating its clients, not by moving them.
 * **No namespace hierarchy yet.** A name delimiter is reserved for future
   nesting (section 3), but inheritance semantics are undefined and using
   the delimiter now is an error.
 * **No events yet.** With a single synchronous manager there is no
   multi-manager consistency problem to solve. A `NamespaceNotify` event
   may be added later if a use case appears.


2. Security model (the load-bearing part)
-----------------------------------------

The entire point of namespaces is isolation, and a management protocol is a
direct path to subverting it. So the access rule is simple and absolute:

> **The whole extension is reachable only by clients in a `superPower`
> namespace** (today only the built-in `root`, plus any namespace
> explicitly granted `superpower`). To every other client the extension
> does not exist.

This is enforced exactly where the existing code already enforces the same
boundary for `SECURITY`, `RECORD` and `XTEST` — the two Xace extension
hooks, both of which already gate on `subj->ns->superPower`:

 * `hook-ext-access.c` (fires on `QueryExtension`): non-superPower clients
   get `BadAccess`, so the extension is **invisible** — it reports as not
   present. A sandboxed client cannot even discover it exists.
 * `hook-ext-dispatch.c` (fires on every request to the extension):
   non-superPower clients get `BadAccess`, so even a client that hard-codes
   the major opcode cannot invoke anything.

Adding `Xnamespace-mgmt` to the blacklist in both hooks is the entire
access-control implementation. Consequences:

 * There is **no per-request privilege table**. Every request handler may
   assume its caller is superPower. One choke point to audit, not twelve.
 * `QueryVersion` is reachable only by superPower clients too. That is
   fine: a client that cannot see the extension has nothing to negotiate,
   and "not present" is the honest answer for it.
 * No client can grant itself capabilities it lacks, because a
   non-superPower client cannot reach any request, mutating or not.

The built-in namespaces `root` and `anon` are **immutable** over the
protocol: their name, capabilities, attributes and token set are fixed at
boot. Any mutating request targeting them (delete, set-flags, add/remove
token) returns `BadAccess`. They are configured only via the config file.


3. Naming and identity
-----------------------

Namespaces are identified **by name** — the same unique key the code
already uses (`XnsFindByName()`, `select_ns()`). Names are the only
namespace identifier on the wire; there are no protocol-level numeric IDs.
The server is free to keep an internal id or resource for its own
bookkeeping, but that is an implementation detail and never appears in a
request, reply or error. Given the low, synchronous traffic, string lookup
costs nothing and a single identifier is simpler to reason about.

Name rules (violations return `BadName`):

 * Must be non-empty.
 * Must be unique (duplicate on `CreateNamespace` → `BadName`).
 * Must not contain the reserved nesting delimiter `'/'`. The delimiter is
   reserved now so that a future hierarchy (`parent/child`) can be added
   without a grammar break; using it today is rejected.
 * Should be restricted to printable, non-whitespace characters so that any
   protocol-created name remains expressible in `namespace.conf` (the
   config parser splits on whitespace and treats `#` as a comment).
   Implementations reject other characters with `BadName`.

`BadName` (core error 15) is reused as the single name-related error:
malformed, reserved-delimiter, duplicate-on-create, or no-such-namespace.

Auth tokens, which have no natural human name, are referenced within a
namespace by a small server-assigned opaque handle (`CARD32`) returned at
creation. This is a per-namespace bookkeeping handle, not a global resource.


4. Capabilities and attributes
-------------------------------

Two `CARD32` words describe a namespace.

**Capabilities** — access permissions (the `allow*` / `superPower` fields):

 | Bit     | Name                           |
 | ------- | ------------------------------ |
 | 1 << 0  | `XNS_CAPABILITY_MOUSE_MOTION`  |
 | 1 << 1  | `XNS_CAPABILITY_SHAPE`         |
 | 1 << 2  | `XNS_CAPABILITY_TRANSPARENCY`  |
 | 1 << 3  | `XNS_CAPABILITY_INPUT`         |
 | 1 << 4  | `XNS_CAPABILITY_KEYBOARD`      |
 | 1 << 5  | `XNS_CAPABILITY_ADMIN`         |

**Attributes** — lifecycle/status (new fields on `struct Xnamespace`):

 | Bit     | Name                  | meaning                                 |
 | ------- | --------------------- | --------------------------------------- |
 | 1 << 0  | `XNS_ATTR_IMMUTABLE`  | read-only; set by server for root/anon  |
 | 1 << 1  | `XNS_ATTR_TRANSIENT`  | destroy namespace when last client exits|

Unused bits in either word are reserved, must be 0, and yield `BadValue`.
New capabilities/attributes are added by allocating the next bit and
bumping the minor version.


5. Versioning
-------------

`QueryVersion` negotiates (major, minor). This document is major 1, minor 2.
The client sends the highest version it supports; the server replies with
the highest it supports that is not greater than the client's.


6. Requests
-----------

Every request begins with the standard 4-byte extension header (major
opcode, 1-byte minor opcode, 16-bit length in 4-byte units). All requests
are 4-byte aligned. A namespace name is encoded as a single length-prefixed
STRING8 (`CARD16` length + bytes + zero padding to a 4-byte boundary) — one
uniform encoding, easy to parse.

All mutating requests carry a reply so the manager round-trips and learns
success or failure synchronously, instead of relying on asynchronous error
delivery. Replies are deliberately minimal.

Minor opcodes:

 | Opcode | Request              | Reply |
 | ------ | -------------------- | ----- |
 |   0    | `QueryVersion`       | yes   |
 |   1    | `ListNamespaces`     | yes   |
 |   2    | `CreateNamespace`    | yes (ack) |
 |   3    | `DeleteNamespace`    | yes (ack) |
 |   4    | `QueryNamespace`     | yes   |
 |   5    | `SetNamespaceFlags`  | yes (ack) |
 |   6    | `AddAuthToken`       | yes   |
 |   7    | `RemoveAuthToken`    | yes (ack) |
 |   8    | `ListAuthTokens`     | yes   |
 |   9    | `GetClientNamespace` | yes   |
 |  10    | `AddListener` (1.1)  | yes   |
 |  11    | `RemoveListener` (1.1) | yes (ack) |
 |  12    | `GetClientInfo` (1.1) | yes  |
 |  13    | `SetNamespaceGrants` (1.1) | yes (ack) |
 |  14    | `SetPermissionAgent` (1.2) | no |
 |  15    | `AnswerPermission` (1.2) | no |
 |  16    | `SetNamespaceInjectScope` (1.2) | yes (ack) |

Event 0 (since 1.2) is `PermissionRequest`, section 6.16.

### 6.0 QueryVersion

```
  QueryVersion
    client-major-version: CARD16
    client-minor-version: CARD16
  =>
    major-version: CARD16
    minor-version: CARD16
```

### 6.1 ListNamespaces

```
  ListNamespaces
  =>
    namespaces: LISTofNAMESPACEINFO
```

where each NAMESPACEINFO is a fixed header followed by the name:

```
    capabilities: CARD32       (section 4)
    attributes:   CARD32       (section 4)
    refcnt:       CARD32        (clients currently assigned)
    num-tokens:   CARD32
    name:         STRING8       (length-prefixed, padded)
```

### 6.2 CreateNamespace

```
  CreateNamespace
    capabilities: CARD32       (initial caps)
    attributes:   CARD32       (XNS_ATTR_TRANSIENT honored; XNS_ATTR_IMMUTABLE
                                rejected with BadValue if set)
    name:         STRING8
  =>
    (empty ack)
```

Errors: `BadName` (empty / illegal char / reserved `'/'` / duplicate),
`BadValue` (reserved capability or attribute bit set), `BadAlloc`.

### 6.3 DeleteNamespace

```
  DeleteNamespace
    onclients: CARD8           (0 = fail if any client present,
                                1 = terminate all clients, then delete)
    pad:       3 bytes
    name:      STRING8
  =>
    (empty ack)
```

 * Deleting a built-in namespace → `BadAccess`.
 * `onclients == 0` and the namespace still has clients → `BadAccess`
   (busy). The manager must terminate them first or pass `onclients == 1`.
 * `onclients == 1`: the server disconnects every client in the namespace
   (as if each had closed its connection), then frees the namespace and
   removes its auth tokens (calling `RemoveAuthorization()` for each).
 * Any other `onclients` value → `BadValue`.

### 6.4 QueryNamespace

```
  QueryNamespace
    name: STRING8
  =>
    capabilities: CARD32
    attributes:   CARD32
    refcnt:       CARD32
    num-tokens:   CARD32
```

`BadName` if no such namespace.

### 6.5 SetNamespaceFlags

Atomically updates the subset of capability bits selected by `value-mask`:

```
  SetNamespaceFlags
    value-mask: CARD32         (which capability bits to apply)
    values:     CARD32         (new values for the masked bits)
    name:       STRING8
  =>
    capabilities: CARD32       (resulting capabilities)
```

`new_caps = (old_caps & ~value-mask) | (values & value-mask)`. Targeting a
built-in namespace → `BadAccess`. A reserved bit in `value-mask` →
`BadValue`. `BadName` if no such namespace. (Attributes are set only at
creation in v0.1; there is no runtime attribute change.)

### 6.6 AddAuthToken

Runtime equivalent of an `auth` config line; internally calls
`AddAuthorization()` exactly as `config.c` does and records the token.

```
  AddAuthToken
    name:        STRING8        (namespace name)
    auth-proto:  STRING8        (e.g. "MIT-MAGIC-COOKIE-1")
    auth-data:   STRING8        (raw key bytes, length-prefixed — NOT hex)
  =>
    token-handle: CARD32        (per-namespace handle for removal)
```

The key is carried as raw bytes; the hex encoding in the config file
(`hex2bin()` in `config.c`) is a text-file affordance with no place in a
binary protocol. Targeting a built-in namespace → `BadAccess`. `BadName`
if no such namespace; `BadValue` if `auth-proto` is empty; `BadAlloc` on
failure.

(Three length-prefixed STRING8s in a fixed order — the request stays
trivially parseable despite three variable-length fields.)

### 6.7 RemoveAuthToken

```
  RemoveAuthToken
    token-handle: CARD32
    name:         STRING8
  =>
    (empty ack)
```

Calls `RemoveAuthorization()` and unlinks the token. Removing a token does
not disconnect clients already authenticated with it (standard X
authorization semantics); it only prevents future connections from using
it. `BadMatch` if the handle does not belong to the named namespace;
`BadAccess` for a built-in namespace; `BadName` if no such namespace.

### 6.8 ListAuthTokens

```
  ListAuthTokens
    name: STRING8
  =>
    tokens: LISTofAUTHTOKENINFO
```

where AUTHTOKENINFO is:

```
    token-handle: CARD32
    proto:        STRING8       (length-prefixed, padded)
```

Secret key material is **never** returned — only the protocol name and the
handle. A compromised manager cannot exfiltrate cookies it did not create.

### 6.9 GetClientNamespace

Read-only introspection: which namespace a given client belongs to. Useful
for auditing a namespace's members before a `DeleteNamespace onclients=1`.

```
  GetClientNamespace
    client-resource: CARD32     (a resource base; 0 = the calling client)
  =>
    is-server: BOOL
    pad:       3 bytes
    name:      STRING8          (the client's namespace name)
```

This is the protocol view of `XnsClientPriv(client)->ns`.

### 6.10 AddListener (since 1.1)

Bind a listening socket to a namespace. This is how a launcher (a sandbox
engine, a session manager) labels an application reliably: it creates and
binds a Unix socket, makes it reachable only from inside the sandbox, and
hands the listening fd to the server. Every client accepted on that socket
is placed in the namespace. Unlike an auth cookie, which can be copied out
of a sandbox and used from anywhere, a socket path can be kept inside it.

```
  AddListener
    name:        STRING8
    app-id:      STRING8        (may be empty)
    instance-id: STRING8        (may be empty)
    engine:      STRING8        (may be empty)
    ceiling:     CARD32         (XPrism capabilities, see 6.13;
                                 0xffffffff = no limit)
    fds:  2 (passed with the request, SCM_RIGHTS)
          1. listening SOCK_STREAM Unix domain socket
          2. close fd, e.g. the read end of a pipe
  =>
    handle: CARD32
```

 * app-id, instance-id and engine describe the app behind the socket
   (e.g. `org.example.Editor`, a Flatpak instance id, `flatpak`). Each is
   at most 255 bytes of printable ASCII. Every client from the socket
   carries them (see GetClientInfo), even after the listener is gone.
   They are the launcher's claim; the server can't verify them.
 * The ceiling is the most a client from the socket may ever hold: it
   holds its namespace's grants AND the ceiling, even after the namespace
   is granted more. This is the capability ceiling: a launcher can add
   a socket to a shared namespace without handing out everything that
   namespace may later be granted.
 * Clients on the socket need no other authorization, and the
   authorization data they send is ignored: a client that sends another
   namespace's cookie still lands in this namespace.
 * When the close fd hangs up (the launcher closed the other end or
   exited) or becomes readable, the server stops listening on the socket.
 * Already connected clients stay connected when a listener goes away.
 * Deleting the namespace removes its listeners. A transient namespace is
   still removed when its last client leaves, listeners included.
 * Errors: `BadName` (no such namespace), `BadAccess` (the root namespace,
   or any namespace with the admin capability: no socket may lead into
   one), `BadValue` (missing fds, the first is not a listening Unix
   stream socket, or an app string is too long or not printable ASCII),
   `BadLength`. The passed fds are closed on every error.

`utils/xns-listen` is a ready-made launcher-side client for this request;
see its README for a bubblewrap example.

### 6.11 RemoveListener (since 1.1)

```
  RemoveListener
    handle: CARD32
  =>
    (empty)
```

Stops listening and closes both fds. `BadValue` if no listener has that
handle.

### 6.12 GetClientInfo (since 1.1)

Everything the server knows about a client: its namespace, the app info
of the listener it came through, and the identity the OS reported when it
connected.

```
  GetClientInfo
    client-resource: CARD32     (a resource of the client, or since 1.2
                                 its resource base; 0 = caller)
  =>
    flags:       CARD8          (1 = came through a listener,
                                 2 = pid valid, 4 = uid valid)
    pid:         CARD32         (in the server's pid namespace)
    uid:         CARD32
    grants:      CARD32         (XPrism capabilities it holds now)
    ceiling:     CARD32         (its listener's, or 0xffffffff)
    namespace:   STRING8
    app-id:      STRING8
    instance-id: STRING8
    engine:      STRING8
```


### 6.13 SetNamespaceGrants (since 1.1)

Replace a namespace's XPrism capabilities (`doc/capabilities.md`). They
apply from its clients' next request on, so a grant can be revoked at any
time.

```
  SetNamespaceGrants
    name:   STRING8
    grants: CARD32
  =>
    (empty)
```

Bits, in this order from bit 0: `capture.screen`, `capture.window`,
`input.inject`, `input.monitor`, `input.virtual_seat`,
`window.enumerate`, `window.inspect`, `window.manage`, `clipboard.read`,
`clipboard.write`, `display.configure`, `compositor.legacy` (`XNS_GRANT_*` in
`namespaceproto.h`). `BadValue` for other bits, `BadAccess` for the
built-in namespaces (`root` holds everything; `anon` is for
unauthenticated clients and is never granted anything), `BadName`.

`QueryNamespace` reports a namespace's grants in the former `pad1` field
(all bits for a superpower namespace).

### 6.14 SetPermissionAgent (since 1.2)

The caller becomes (`enable` 1) or stops being (0) the permission agent.
When a confined client's request is refused for a capability it lacks
(capability mode `enforce`), the agent is asked instead: it gets a
`PermissionRequest` event, and the client waits for the answer.

```
  SetPermissionAgent
    enable: CARD8
  =>
    (no reply)
```

One agent at a time: `BadAccess` if another client is the agent.
`BadValue` for `enable` other than 0 or 1. When the agent stops being one
or disconnects, every open prompt is refused.

### 6.15 AnswerPermission (since 1.2)

```
  AnswerPermission
    id:     CARD32
    answer: CARD8   0 deny once, 1 allow once,
                    2 allow while the client runs, 3 deny and do not ask again
  =>
    (no reply)
```

Only the agent may answer (`BadAccess`). `BadValue` for other answers. An
id that is no longer open (answered, timed out, its client gone) is
ignored.

### 6.16 PermissionRequest event (since 1.2)

Sent to the agent; event base + 0.

```
  PermissionRequest
    capability:     CARD8    bit number, as in SetNamespaceGrants
    id:             CARD32   for AnswerPermission
    clientResource: CARD32   the client's resource base; GetClientInfo takes it
    target:         CARD32   the resource the request is about, or 0
    majorOpcode:    CARD8    the request
    minorOpcode:    CARD16
    timestamp:      TIMESTAMP
```

The client's request, and the ones it sends after it, wait until the
answer comes. The request then runs again, and the answer decides the
capability check: allowed, it proceeds; refused, it fails with
`BadAccess`. Without an answer in 60 seconds, the request is refused.
"Allow while the client runs" and "deny and do not ask again" hold for
that one client, not its namespace, until it disconnects.

Only capabilities whose checks come before a request changes anything are
asked for: `capture.screen`, `capture.window`, `input.inject`,
`input.monitor`, `window.enumerate`, `window.inspect`,
`display.configure`. The others are refused as before. Unconfined
clients, audit mode, and the agent itself are never asked about.

A request that Xnamespace's own isolation also refuses (a window of
another namespace) fails after an "allow" too.

### 6.17 SetNamespaceInjectScope (since 1.2)

Scoped automation (`doc/capabilities.md`): clients of namespace `name`
may use XTest, through a seat of their own, into the windows of namespace
`target`. An empty `target` takes that away. The config file says the
same with `inject-into TARGET`.

```
  SetNamespaceInjectScope
    name:   STRING8
    target: STRING8
  =>
    (empty)
```

`BadName` for an unknown `name`, `BadAccess` for the built-in and
superpower namespaces. `target` need not exist yet.

7. Errors
---------

The extension defines no new error codes; it reuses core errors:

 * `BadAccess` — operation on an immutable built-in namespace; delete of a
   busy namespace without `onclients=1`. (Also the result of a
   non-superPower client reaching the extension, though the Xace hooks make
   that unreachable in practice — defence in depth.)
 * `BadName`   — namespace name empty, illegal, reserved-delimiter,
   duplicate on create, or not found.
 * `BadValue`  — reserved capability/attribute bit set; bad `onclients`
   value; empty auth protocol; unknown listener handle; bad
   listener fds.
 * `BadMatch`  — token handle does not belong to the named namespace.
 * `BadAlloc`  — out of memory.


8. Implementation
-----------------

The extension is registered by `NamespaceExtensionInit()`
(`Xext/namespace/namespace.c`) once the namespace configuration has
loaded; without namespaces there is nothing to manage. The requests are
in `Xext/namespace/proto.c`, the wire format in
`Xext/namespace/namespaceproto.h`.

 | Protocol concept       | Code                                            |
 | ---------------------- | ----------------------------------------------- |
 | namespace record       | `struct Xnamespace` (`namespace.h`)             |
 | capability bits        | `allow*` / `superPower` fields                  |
 | grants                 | `xpcaps` (`dix/capability_priv.h`)              |
 | find by name           | `XnsFindByName()` (`config.c`)                  |
 | name check             | `validateName()` (`config.c`)                   |
 | listeners              | `Xext/namespace/listener.c`, `os/listener.c`    |
 | client → namespace     | `XnsClientPriv()->ns`                           |

The config file and the protocol share one set of setters, so a namespace
created either way behaves the same. Only management clients (those in a
`superpower` namespace) see the extension: `hook-ext-access.c` hides it
from everyone else and `hook-ext-dispatch.c` refuses their requests with
`BadAccess`. Everything runs in the single-threaded dispatch loop, so no
locking is needed.

Names are printable ASCII without whitespace, `#` (which starts a comment
in the config file) or `/` (kept for nesting), at most 255 bytes, so a
name created through the protocol can also be written in the config
file. A `DeleteNamespace` that races with a transient namespace removing
itself gets `BadName`; the namespace is gone either way.
