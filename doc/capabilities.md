# XPrism capabilities

The permission model from `idea.txt` section 7. Each capability is checked
where the protocol actually does the thing. A *policy provider* says which
capabilities a client holds; today that is Xnamespace.

Code: `dix/capability.c`, `dix/capability_priv.h`. Tests:
`test/pyxtest/test_capability.py`.

## Modes

`-capmode off|audit|enforce`

| Mode | Missing capability |
|---|---|
| `off` | no checks |
| `audit` (default) | logged, nothing denied |
| `enforce` (experimental) | request fails with `BadAccess`; events are dropped |

Enforcement is a Phase V item (`DECISIONS.md`, AD-4). It exists now so the
checks can be tested.

A missing capability is logged once per client and capability:

```
(WW) capability: would deny window.inspect to client 5 (namespace "sandbox", app "org.example.Editor", pid 4242) for GetWindowAttributes on 0x200001 [audit]
```

When the build has no request-name registry, the request shows as
`request <major>.<minor>`.

## Who holds what

- **No policy provider** (no `-namespace`): every client holds every
  capability. Plain X11 sessions behave as before, even with `enforce`.
- **Xnamespace**: the server's own client and the root namespace (any
  `superpower` namespace) hold all capabilities. Every other namespace,
  including `anon`, holds only what its config grants:

  ```
  namespace editor
    auth MIT-MAGIC-COOKIE-1 ...
    grant window.inspect window.enumerate
  ```

  At runtime, X-NAMESPACE `SetNamespaceGrants` replaces a namespace's
  grants (`xns-listen --create --grant ...`). Changes apply from the next
  request, so revoking is immediate.
- **Listener ceiling**: a client that came in through a listener holds
  its namespace's grants AND the listener's ceiling (`AddListener`,
  `xns-listen --ceiling ...`), whatever the namespace is granted later.

Capabilities are checked on top of Xnamespace's own rules, not instead of
them: a client needs both to pass.

## Capabilities and where they are checked

| Capability | Checked at | Notes |
|---|---|---|
| `capture.screen` | resource hook: read access to the root window (`GetImage`, `ShmGetImage`, `CopyArea`/`CopyPlane` source) | |
| `capture.window` | resource hook: read access to another client's window or pixmap | |
| `input.inject` | extension dispatch: XTEST, except `GetVersion` | |
| `input.monitor` | receive hook: XI2 raw key, button and touch events; extension dispatch: RECORD, except `QueryVersion` | Raw *motion* is not covered: the pointer position is public anyway (`QueryPointer`), and apps like xeyes use it. |
| `input.virtual_seat` | device hook: a client creating a device (`XIChangeHierarchy` AddMaster) | |
| `window.enumerate` | resource hook: list access to another client's or the root window (`QueryTree`) | |
| `window.inspect` | resource hook: attribute access to another client's window (`GetWindowAttributes`, `GetGeometry`); property hook: reading or listing another client's window properties | The root window's attributes and properties are public: every toolkit reads them. |
| `window.manage` | resource hook: manage access to another client's or the root window (`ReparentWindow`, `ChangeSaveSet`, Composite redirect, selecting `SubstructureRedirect`/`ResizeRedirect`) | |
| `clipboard.read` | selection hook: `ConvertSelection` | Only the session's `PRIMARY`, `SECONDARY` and `CLIPBOARD`. |
| `clipboard.write` | selection hook: `SetSelectionOwner` | As above. |
| `display.configure` | extension dispatch: the RandR requests that change the configuration (`SetScreenSize`, `SetCrtcConfig`, gamma, transforms, panning, primary output, modes, output/provider properties, monitors, leases) | Checked at dispatch, so every configuring request is covered in one place. Since commit `4e749a551` RandR also looks up the CRTCs, outputs, providers and root window it changes with write access, so Xnamespace (and SELinux) see the write too. |

| `compositor.legacy` | Composite: manual redirection of a root window's subwindows, which is how an external compositor (picom, KWin-X11, Mutter-X11) takes the screen over from the [scene compositor](compositor.md) | A compositor sees every window, so this capability also grants `capture.screen` and `capture.window`. Grant it only to the compositor you chose. Today only unconfined clients can redirect the root at all (Xnamespace keeps confined ones off it first), so the check first matters when Phase V confines session clients. |

"Another client's" means the resource's XID belongs to a different client
(or to the server). A client never needs a capability for its own windows.

### Clipboard and Xnamespace

Xnamespace gives each namespace private copies of the selections: a
confined client asking for `CLIPBOARD` gets `<editor>CLIPBOARD`. Those
are not checked, so a confined client's own clipboard needs no
capability.

The grants open the session's `PRIMARY`, `SECONDARY` and `CLIPBOARD`
(`Xext/namespace/hook-selection.c`):

| Request | Session selection if the client holds |
|---|---|
| `SetSelectionOwner` | `clipboard.write` |
| `ConvertSelection`, XFixes `SelectSelectionInput` | `clipboard.read` |
| `GetSelectionOwner` | either |

Without the grant the request goes to the namespace's private copy, as
before. Revoking a grant applies from the client's next request.

A transfer also needs the owner to write the data onto the requestor's
window and send it `SelectionNotify`, which Xnamespace blocks across
namespaces. So when the server hands a `SelectionRequest` from another
namespace to a confined owner, it records a one-time token: that owner
may set *the requested property* on *that window* and send it *one*
`SelectionNotify` for that selection. Any other property, any other
event, or a second reply is refused (events are dropped silently, as
`SendEvent` denials always are).

Limits:

- INCR transfers (data too large for one request) from a confined owner
  to another namespace don't work yet: the owner would also need
  `PropertyNotify` events from the requestor's window.
- Tested between the session (root namespace) and confined namespaces.
  Between two confined namespaces the same token applies, but there is
  no test for it yet.

## Known gaps

- `CopyArea` with `IncludeInferiors` from the client's *own* window can
  copy other clients' child windows (G1 in `docs/audit-phase1.md`).
- `input.monitor`: core and XI grabs, `QueryKeymap`.
