<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="doc/logo/logo-dark.svg">
    <img src="doc/logo/logo-light.svg" alt="XPrism — a modern display server for Unix" width="560">
  </picture>
</p>

<p align="center">
  <em>Compatibility without stagnation. Security without sacrificing control.</em>
</p>

<p align="center">
  <a href="https://github.com/Nexoniarz/xprism/actions/workflows/xprism.yml"><img src="https://github.com/Nexoniarz/xprism/actions/workflows/xprism.yml/badge.svg" alt="CI"></a>
  <img src="https://img.shields.io/badge/status-early%20development-orange" alt="Status: early development">
  <img src="https://img.shields.io/badge/platforms-Linux%20%7C%20FreeBSD%20%7C%20OpenBSD-blue" alt="Platforms">
  <img src="https://img.shields.io/badge/license-MIT%2FX11-green" alt="License: MIT/X11">
</p>

---

XPrism is a display server for Linux and the BSDs that keeps X11 — every
application, window manager and workflow that depends on it — and fixes
what X11 never had: **a way to decide what each application is allowed
to do.**

On a classic X server, any program you run can read every keystroke,
capture every window and inject input into other applications. Wayland
solved that by removing those abilities from everyone, which broke
screen recorders, automation tools, remote desktops and much of what made
X11 useful. XPrism takes a different path: the abilities stay, but the
**display server** enforces who holds them, and **you** decide.

XPrism is a fork of [XLibre](https://github.com/X11Libre/xserver), itself
the continuation of the X.Org Server.

> [!IMPORTANT]
> XPrism is in **early development** (Phase I of the [roadmap](#roadmap)).
> Today it runs as a normal X server with XLibre's features plus the
> security foundation described below. It is not yet ready for daily use,
> and the protocols added here may still change.

## Contents

- [Principles](#principles)
- [What works today](#what-works-today)
- [How the security model works](#how-the-security-model-works)
- [Roadmap](#roadmap)
- [Platforms](#platforms)
- [Building](#building)
- [Trying it out](#trying-it-out)
- [Documentation](#documentation)
- [Development](#development)
- [License and credits](#license-and-credits)

## Principles

- **User sovereignty.** You decide what applications may do on your
  computer — not the application, and not the desktop.
- **Security by design.** Restrictions are enforced by the display server
  itself, where every request passes, not by convention.
- **Backward compatibility.** Existing X11 applications keep working
  wherever reasonably possible.
- **Modernization over stagnation.** Old implementation choices don't
  block better architecture; obsolete code is removed, not preserved.
- **Platform independence.** Linux is one target among several, never the
  only one.

## What works today

| Area | What you get | Docs |
|---|---|---|
| **App isolation** | Applications run in *namespaces* (from XLibre's Xnamespace): separate clipboards, no access to other namespaces' windows, input or screen contents. | [`doc/Xnamespace.md`](doc/Xnamespace.md) |
| **Launcher-assigned identity** | A sandbox launcher hands the server a socket bound to a namespace and an app identity. Whatever connects through it lands there — the app can't choose or fake its own identity. | [`doc/Xnamespace-protocol.md`](doc/Xnamespace-protocol.md), [`xns-listen`](utils/xns-listen/README.md) |
| **Capabilities** | Twelve fine-grained permissions (`capture.screen`, `input.inject`, `clipboard.read`, …), checked where the protocol actually does the thing. Granted per namespace, capped per launcher socket, changeable at runtime. | [`doc/capabilities.md`](doc/capabilities.md) |
| **Audit mode** | By default nothing is denied: every request that *would* need a missing capability is logged, so policies can be tested against real applications first. | [`doc/capabilities.md`](doc/capabilities.md#modes) |
| **Clipboard grants** | A confined app granted `clipboard.read` / `clipboard.write` can paste from and copy to the desktop's clipboard — and nothing more. | [`doc/capabilities.md`](doc/capabilities.md#clipboard-and-xnamespace) |
| **Scene compositor** *(experimental)* | `-compositor on`: the server composites every window itself. Translucent windows and window opacity work without picom; the window manager sets per-window opacity and transforms through the XPRISM-SCENE protocol; external compositors still work and take over when they start. | [`doc/compositor.md`](doc/compositor.md) |
| **Platform Abstraction Layer** | All OS-specific behaviour goes through one interface with per-OS backends; a test keeps new OS `#ifdef`s from spreading. | [`doc/pal.md`](doc/pal.md) |
| **Everything from XLibre** | TearFree and atomic modesetting, NVIDIA driver support, seatd/logind, Xvfb, Xephyr, Xfbdev. | [XLibre](https://github.com/X11Libre/xserver) |

## How the security model works

```
 launcher (Flatpak, bubblewrap, …)                 XPrism
 ─────────────────────────────────                 ──────────────────────────────
 creates a socket, hands it to the server  ──▶     socket = namespace "editor"
 with an app id and a capability ceiling           + app "org.example.Editor"
                                                   + ceiling: clipboard.*
 starts the app with only that socket      ──▶     every client on it is in "editor"
                                                   every request is checked against
                                                   namespace grants ∩ ceiling
```

1. **Identity comes from the launcher, never from the app.** Clients that
   connect through a launcher's socket are placed in its namespace by the
   server, before they send a single request.
2. **Isolation is the default.** A confined app sees its own windows and
   its own clipboard, nothing else.
3. **Capabilities open specific doors.** A namespace holds only what its
   configuration (or a privileged manager at runtime) grants; the
   launcher's ceiling limits that further, for good.
4. **Unconfined clients keep full X11.** Without namespaces configured,
   XPrism behaves like any X server — nothing breaks.

Enforcement of the capability checks (`-capmode enforce`) exists for
testing and becomes the default in Phase V, together with a permission
prompt protocol.

## Roadmap

| Phase | Focus | Status |
|---|---|---|
| **I. Foundation** | Codebase audit, security hooks, launcher-assigned identity, capability model in audit mode, Platform Abstraction Layer, CI on Linux, FreeBSD and OpenBSD | **In progress** — nearly complete |
| **II. Display & scheduling** | Atomic KMS, per-output scheduling and vblank, hotplug, VRR, multi-GPU | Planned |
| **III. Rendering & compositing** | Server-side GPU compositor with the window manager in control, explicit sync, direct scanout | **Started** — scene compositor (experimental) |
| **IV. Color & HDR** | Per-output color management, wide gamut, HDR | Planned |
| **V. Security enforcement & input** | Enforcement on by default, permission prompts, virtual seats, app-scoped input | Planned |
| **VI. WX11** | Run Wayland applications as X11 windows, built on Smithay | Planned |
| **VII. Platform completion** | Finish the BSD backends and hardware validation; SchOS backend | Planned |
| **VIII. NXDE** | A desktop environment built on the NXWM window manager | Planned |
| **IX. Stabilization** | Performance, security audit, packaging, first release | Planned |

## Platforms

| Platform | Status |
|---|---|
| Linux | Builds and passes the full test suite in CI |
| FreeBSD | Builds and passes the unit tests in CI |
| OpenBSD | Builds in CI |
| NetBSD, DragonFly BSD | Planned backends (inherited XLibre build support) |
| SchOS | Planned backend |

Platform differences — session and device access, hotplug, sandbox
detection, privilege dropping — live behind the
[Platform Abstraction Layer](doc/pal.md).

## Building

XPrism builds with [Meson](https://mesonbuild.com) and needs the same
dependencies as XLibre (see
[Building XLibre](https://github.com/X11Libre/xserver/wiki/Building-XLibre)
for per-distribution package lists).

```sh
git clone https://github.com/Nexoniarz/xprism.git
cd xprism
meson setup build -Dxvfb=true
ninja -C build
```

To run the tests (unit tests, the Python protocol tests in
[`test/pyxtest`](test/pyxtest) and the source checks):

```sh
meson test -C build
```

Useful options: `-Dxorg=true` for the hardware server, `-Dxephyr=true`
for a nested server, `-Dnamespace=true` (default) for app isolation,
`-Dxns_listen=enabled` for the launcher helper. All options are in
[`meson_options.txt`](meson_options.txt).

## Trying it out

The safest way to see the security model in action is a virtual server.
Write a namespace configuration (a namespace with a cookie for a
"sandbox"):

```sh
cat > ns.conf <<'EOF'
auth MIT-MAGIC-COOKIE-1 11111111111111111111111111111111
namespace sandbox
  auth MIT-MAGIC-COOKIE-1 22222222222222222222222222222222
  grant clipboard.read
EOF

./build/hw/vfb/Xvfb :5 -namespace ns.conf -capmode audit &
```

Clients authorized with the second cookie run inside `sandbox`: they can
paste from the session clipboard but can't see or touch anything else, and
the server log shows every capability they would be missing.

For real sandboxes, [`xns-listen`](utils/xns-listen/README.md) gives an
application its own socket into a namespace — the README there has a
complete bubblewrap example.

## Documentation

| Document | Contents |
|---|---|
| [`doc/capabilities.md`](doc/capabilities.md) | The capability model: every capability, where it is checked, grants, ceilings, modes |
| [`doc/Xnamespace.md`](doc/Xnamespace.md) | Namespaces and their configuration |
| [`doc/Xnamespace-protocol.md`](doc/Xnamespace-protocol.md) | The X-NAMESPACE protocol, including XPrism's listener and grant requests |
| [`utils/xns-listen/README.md`](utils/xns-listen/README.md) | The launcher helper |
| [`doc/compositor.md`](doc/compositor.md) | The server-side scene compositor and legacy compositor mode |
| [`doc/xprism-scene-protocol.md`](doc/xprism-scene-protocol.md) | XPRISM-SCENE: how the window manager describes the scene |
| [`doc/pal.md`](doc/pal.md) | The Platform Abstraction Layer and per-OS backends |
| [`CODE_OVERVIEW.md`](CODE_OVERVIEW.md) | A map of the server's source tree |

## Development

Development happens on the `main` branch. A few rules the codebase holds
itself to:

- **Every security check has a test that fails without it.** New checks
  are verified by deliberately breaking them and watching the test fail.
- **No new OS-specific `#ifdef`s outside the PAL.** The `os-ifdefs` test
  enforces it.
- **Upstream merges stay possible.** XPrism changes live in new files
  where it can, and upstream code is changed only where needed.

Fixes to code inherited from XLibre are written so they can be offered
back upstream.

## License and credits

XPrism is distributed under the MIT/X11 license, like the X.Org and
XLibre servers it is built on — see [`COPYING`](COPYING) for the full
terms and copyright notices.

XPrism stands on decades of work by the X.Org community and on the
XLibre project, which keeps the X server alive and moving. X Window
System is a trademark of The Open Group.
