# Capabilities

Capabilities control which privileged X11 operations a client may perform.
Each one is checked at the point where the protocol performs the
operation. Which capabilities a client holds is decided by a policy
provider; at present the only provider is Xnamespace.

The implementation is in `dix/capability.c` and `dix/capability_priv.h`,
the tests in `test/pyxtest/test_capability.py`.

## Modes

The `-capmode` option selects what happens when a client lacks a
capability:

| Mode | Effect |
|---|---|
| `off` | No checks are made. |
| `audit` | The missing capability is logged and the request proceeds. Useful to find out what an application needs before granting it. |
| `enforce` | The request fails with `BadAccess`, or the event is dropped, unless a permission agent allows it (below). This is the default. |

A missing capability is logged once per client and capability:

```
(WW) capability: denied window.inspect to client 5 (namespace "sandbox", app "org.example.Editor", pid 4242) for GetWindowAttributes on 0x200001
```

If the build has no request-name registry, the request is shown as
`request <major>.<minor>`.

## Permission prompts

In `enforce` mode, a desktop's permission agent can ask the user instead
of refusing: it registers with X-NAMESPACE `SetPermissionAgent`, and
gets a `PermissionRequest` event when a confined client's request lacks a
capability. The client waits; the agent answers allow or deny, once or
for as long as the client runs, and the request runs again with the
answer (see [Xnamespace-protocol.md](Xnamespace-protocol.md), 6.14 to
6.16). Without an agent, or without an answer within 60 seconds, the
request is refused as before. The server log shows each prompt and its
answer.
[utils/xprism-permission-agent](../utils/xprism-permission-agent/README.md)
is such an agent: it shows a dialog (kdialog, zenity, or any command) for
each prompt.

## Scoped automation

An automation client (a test runner, an assistant, remote help) can be
allowed to operate one application without `input.inject`, which would
let it type into anything. Its namespace is given an inject scope, the
namespace of the application it may operate:

```
namespace editor-bot
  auth MIT-MAGIC-COOKIE-1 ...
  inject-into editor
```

or at run time with X-NAMESPACE `SetNamespaceInjectScope`
(`xns-listen --create --inject-into editor`). Its clients then see XTest,
and:

- Their injected input goes through a seat of their own: a master
  pointer and keyboard the server creates on their first XTest request
  ("XPrism automation seat N") and removes when they disconnect. The
  user's pointer and keyboard are not moved or pressed; core XTest
  requests are redirected to the seat, and other devices are refused.
- Their key and button events must reach a window of the scope's
  namespace, or their own: the window under the seat's pointer, its
  keyboard focus, or the window holding a grab. Otherwise the request
  fails with `BadAccess`. Moving the pointer is not limited.
- The user goes first: while the user's own input is active (an event
  from any other device within the last 500 ms, including on-screen
  keyboards through the core XTest devices), their key and button
  events wait.
- `GrabControl` (ignoring server grabs) is refused. A launcher ceiling
  without `input.inject` takes the scope away too.

In audit mode nothing is refused, so XTest works as for unconfined
clients.

## Which clients hold which capabilities

Without a policy provider (no `-namespace` option), every client holds
every capability, and a plain X11 session behaves as it always has, even
with `-capmode enforce`.

With Xnamespace, the server's own client and clients of any namespace
marked `superpower` (including the root namespace) hold all
capabilities. Clients of other namespaces, including `anon`, hold only
what their namespace is granted in the configuration file:

```
namespace editor
  auth MIT-MAGIC-COOKIE-1 ...
  grant window.inspect window.enumerate
```

A namespace's grants can be replaced at runtime with the X-NAMESPACE
`SetNamespaceGrants` request (`xns-listen --create --grant ...` uses it).
Changes take effect with the client's next request, so a revoked grant
stops working immediately.

A client that connected through a launcher socket is further limited by
that socket's ceiling (X-NAMESPACE `AddListener`, `xns-listen --ceiling`).
It holds only the capabilities that are both granted to its namespace and
allowed by the ceiling, regardless of what the namespace is granted later.

Capability checks are made in addition to Xnamespace's own isolation
rules. A request has to pass both.

## Capabilities

| Capability | Checked at | Notes |
|---|---|---|
| `capture.screen` | Read access to the root window (`GetImage`, `ShmGetImage`, the source of `CopyArea` and `CopyPlane`) | |
| `capture.window` | Read access to another client's window or pixmap; reading one's own window with its inferiors (`GetImage`, `ShmGetImage`, `CopyArea`/`CopyPlane` with `IncludeInferiors`) while another client's viewable window is inside it | An embedded window (a tray icon in a panel) belongs to its own client. |
| `input.inject` | XTEST requests other than `GetVersion` | Without it, a namespace with an inject scope may still automate one application (see Scoped automation). |
| `input.monitor` | Delivery of XI2 raw key, button and touch events; RECORD requests other than `QueryVersion`; keyboard grabs and `QueryKeymap` without the focus (below) | Raw motion events are not covered. The pointer position is available to every client through `QueryPointer`. |
| `input.virtual_seat` | Creating an input device (`XIChangeHierarchy` with `AddMaster`) | |
| `window.enumerate` | Listing the children of another client's window or of the root window (`QueryTree`) | |
| `window.inspect` | Reading the attributes or geometry of another client's window; reading or listing its properties | The attributes and properties of the root window are readable by everyone, since all toolkits depend on them. |
| `window.manage` | Manage access to another client's window or the root window: `ReparentWindow`, `ChangeSaveSet`, Composite redirection, selecting `SubstructureRedirect` or `ResizeRedirect` | |
| `clipboard.read` | `ConvertSelection` on the session's `PRIMARY`, `SECONDARY` and `CLIPBOARD` | See below. |
| `clipboard.write` | `SetSelectionOwner` on the same selections | See below. |
| `display.configure` | RandR requests that change the configuration: `SetScreenSize`, `SetCrtcConfig`, gamma, transforms, panning, primary output, modes, output and provider properties, monitors, leases | Checked when the request is dispatched. RandR also looks up the objects it modifies with write access, so other XACE policies such as SELinux see the change as a write. |
| `compositor.legacy` | Manual Composite redirection of a root window's children, which is how an external compositor (picom, KWin, Mutter) takes over from the [built-in compositor](compositor.md) | Also grants `capture.screen` and `capture.window`, because a compositor sees every window. At present only unconfined clients are able to redirect a root window, so this check has no effect until session clients are confined (Phase V). |

"Another client's" resource is one whose XID belongs to a different
client or to the server. A client never needs a capability to access its
own windows.

## Clipboard access across namespaces

Xnamespace gives every namespace private copies of the selections. A
confined client that asks for `CLIPBOARD` actually uses
`<namespace>CLIPBOARD`, and no capability is needed for that.

The clipboard capabilities give access to the session's `PRIMARY`,
`SECONDARY` and `CLIPBOARD` instead (`Xext/namespace/hook-selection.c`):

| Request | Uses the session selection if the client holds |
|---|---|
| `SetSelectionOwner` | `clipboard.write` |
| `ConvertSelection`, XFixes `SelectSelectionInput` | `clipboard.read` |
| `GetSelectionOwner` | either |

Without the capability, the request applies to the namespace's private
copy.

To complete a transfer, the selection owner has to write the data to a
property on the requestor's window and send it a `SelectionNotify`
event, both of which Xnamespace normally blocks between namespaces. When
the server delivers a `SelectionRequest` from another namespace to a
confined owner, it therefore records a single-use permission: the owner
may set the requested property on that window and send it one
`SelectionNotify` for that selection. Any other property or event, and
any second reply, is refused. Refused events are dropped without an
error, as is usual for `SendEvent`.

Limitations:

- Incremental (INCR) transfers from a confined owner to another
  namespace do not work yet, because the owner would also need
  `PropertyNotify` events from the requestor's window.
- Transfers between two confined namespaces use the same mechanism but
  are not covered by tests yet.

## The keyboard belongs to the focused client

A client has the focus when the keyboard's focus window, or with
`PointerRoot` the window under the pointer, is one of its windows or
inside one. That client may grab the keyboard (core `GrabKeyboard`, XI
`GrabDevice`: menus) and read which keys are down (`QueryKeymap`). Any
other client needs `input.monitor` for these, since it would see keys
typed into other applications; refused, `QueryKeymap` answers with no
keys down. Passive key grabs (`GrabKey`, XI passive grabs) on a client's
own windows fire only while the focus is in them and need nothing; on
any other window, such as the root (global hot keys), they need
`input.monitor`. Pointer grabs are not limited.

Xnamespace lets confined clients make these requests and leaves the
decision to these checks. A confined client may also move the focus
among its windows (`SetInputFocus`, XI `SetFocus`) while it has it, but
not take it from another client: that is the window manager's. The
request is then ignored without an error.

## Known gaps

- XKB `StateNotify` events tell any client which modifiers are held,
  also while another client has the focus. Toolkits rely on them for the
  keyboard layout indicator.
