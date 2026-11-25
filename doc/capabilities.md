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

  Namespaces created at runtime (X-NAMESPACE `CreateNamespace`, e.g. by
  `xns-listen --create`) hold no capabilities yet; the protocol has no way
  to grant them.

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
| `display.configure` | extension dispatch: the RandR requests that change the configuration (`SetScreenSize`, `SetCrtcConfig`, gamma, transforms, panning, primary output, modes, output/provider properties, monitors, leases) | Not via the resource hook: RandR looks up outputs and CRTCs with *read* access even in most requests that change them, and `SetScreenSize` looks up none. |

"Another client's" means the resource's XID belongs to a different client
(or to the server). A client never needs a capability for its own windows.

### Clipboard and Xnamespace

Xnamespace gives each namespace private copies of the selections: a
confined client asking for `CLIPBOARD` gets `<editor>CLIPBOARD`. Those
are not checked, so a confined client's own clipboard needs no
capability, and it cannot reach the session clipboard at all. Letting a
`clipboard.read`/`clipboard.write` grant open the session clipboard to a
confined client is still to do.

## Known gaps

- `CopyArea` with `IncludeInferiors` from the client's *own* window can
  copy other clients' child windows (G1 in `docs/audit-phase1.md`).
- `input.monitor`: core and XI grabs, `QueryKeymap`.
- No capability ceiling per listener yet (AD-2): the namespace's grants
  apply to every client in it.
- No way to grant capabilities at runtime (protocol).
