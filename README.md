<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="doc/logo/logo-dark.svg">
    <img src="doc/logo/logo-light.svg" alt="XPrism" width="420">
  </picture>
</p>

<p align="center">
  <a href="https://github.com/Nexoniarz/xprism/actions/workflows/xprism.yml"><img src="https://github.com/Nexoniarz/xprism/actions/workflows/xprism.yml/badge.svg" alt="CI"></a>
  <img src="https://img.shields.io/badge/license-MIT%2FX11-blue" alt="License: MIT/X11">
</p>

XPrism is an X11 display server for Linux and the BSDs. It is a fork of
[XLibre](https://github.com/X11Libre/xserver), which continues the X.Org
Server.

The goal is to keep the X11 protocol and the software built on it, and to
add what X11 has always lacked: control over what each application is
allowed to do. A client on a classic X server can read the screen, log
keystrokes and send input to other applications. XPrism keeps those
abilities available, but the server decides which clients hold them, based
on a policy set by the user or the sandbox that launched the application.

The project is in early development. It runs as a regular X server with
everything XLibre provides. The features below are usable for testing, but
the protocols added by XPrism may still change.

## Features

### Application isolation

Clients can be placed in namespaces (based on XLibre's Xnamespace). A
client in a namespace has its own clipboard and cannot see or reach the
windows, input or screen contents of other namespaces. See
[doc/Xnamespace.md](doc/Xnamespace.md).

### Launcher-assigned identity

A sandbox launcher can hand the server a socket that is bound to a
namespace and an application ID. Clients connecting through it are
placed there before their first request and cannot choose a different
identity. See [doc/Xnamespace-protocol.md](doc/Xnamespace-protocol.md)
and [utils/xns-listen](utils/xns-listen/README.md).

### Capabilities

XPrism defines twelve permissions, such as `capture.screen`,
`input.inject` and `clipboard.read`, each checked at the point where the
protocol performs the action. They are granted per namespace, can be
limited per launcher socket, and can be changed at runtime. By default the server only logs
what would be denied. See [doc/capabilities.md](doc/capabilities.md).

### Server-side compositing (experimental)

With `-compositor on`, the server composites windows itself. ARGB
windows and window opacity work without an external compositor, and
fullscreen windows bypass composition. The window manager can set
per-window opacity, transforms, rounded corners, shadows and background
blur through the XPRISM-SCENE extension.
External compositors such as picom still work. See
[doc/compositor.md](doc/compositor.md) and
[doc/xprism-scene-protocol.md](doc/xprism-scene-protocol.md).

### Platform abstraction layer

Operating-system specific code (process information, sandbox detection,
and later session and device handling) is kept behind one interface with
a backend per OS. See [doc/pal.md](doc/pal.md).

## Supported platforms

| Platform | Status |
|---|---|
| Linux | Built and tested in CI |
| FreeBSD | Built and unit-tested in CI |
| OpenBSD | Built in CI |
| NetBSD, DragonFly BSD | Build support inherited from XLibre, untested |

## Building

XPrism uses Meson and has the same dependencies as XLibre. The
[XLibre build instructions](https://github.com/X11Libre/xserver/wiki/Building-XLibre)
list the packages for common distributions.

```sh
git clone https://github.com/Nexoniarz/xprism.git
cd xprism
meson setup build -Dxvfb=true
ninja -C build
```

Run the test suite with:

```sh
meson test -C build
```

Commonly used options are `-Dxorg=true` (the hardware server),
`-Dxephyr=true` (a nested server) and `-Dxns_listen=enabled` (the launcher
helper). See [meson_options.txt](meson_options.txt) for the full list.

## Trying it out

Namespaces are configured in a file. This one defines a namespace called
`sandbox` whose clients may read the session clipboard:

```
auth MIT-MAGIC-COOKIE-1 11111111111111111111111111111111
namespace sandbox
  auth MIT-MAGIC-COOKIE-1 22222222222222222222222222222222
  grant clipboard.read
```

Start a virtual server with it:

```sh
./build/hw/vfb/Xvfb :5 -namespace ns.conf -capmode audit
```

Clients that authenticate with the second cookie run in `sandbox`. They can
paste from the session clipboard but cannot access anything else, and the
server log lists each capability they would need. For running real
applications in a sandbox, see the bubblewrap example in
[utils/xns-listen](utils/xns-listen/README.md).

## Roadmap

| Phase | Scope | Status |
|---|---|---|
| I | Codebase audit, security hooks, launcher-assigned identity, capabilities in audit mode, platform abstraction layer, CI on Linux, FreeBSD and OpenBSD | Nearly complete |
| II | Atomic modesetting, per-output scheduling, hotplug, variable refresh rate, multi-GPU | Planned |
| III | Server-side compositor controlled by the window manager, explicit sync, direct scanout | Nearly complete: needs testing on real hardware; direct scanout per monitor waits for Phase II |
| IV | Per-output color management, wide gamut and HDR | Planned |
| V | Capability enforcement by default, permission prompts, virtual seats, application-scoped input | Planned |
| VI | Running Wayland applications as X11 windows (WX11) | Planned |
| VII | Completing the BSD backends, hardware validation | Planned |
| VIII | Stabilization, security review, packaging, first release | Planned |

## Documentation

- [doc/capabilities.md](doc/capabilities.md): the capability model
- [doc/Xnamespace.md](doc/Xnamespace.md): namespaces and their configuration
- [doc/Xnamespace-protocol.md](doc/Xnamespace-protocol.md): the X-NAMESPACE extension
- [doc/compositor.md](doc/compositor.md): the server-side compositor
- [doc/xprism-scene-protocol.md](doc/xprism-scene-protocol.md): the XPRISM-SCENE extension
- [doc/pal.md](doc/pal.md): the platform abstraction layer
- [CODE_OVERVIEW.md](CODE_OVERVIEW.md): layout of the source tree

## Contributing

Development happens on the `main` branch. Changes are expected to follow a
few rules:

- Every security check comes with a test, and the test must be shown to
  fail when the check is removed.
- No new OS-specific `#ifdef`s outside the platform abstraction layer. The
  `os-ifdefs` test enforces this.
- Changes to code inherited from XLibre are kept small and self-contained,
  so that merging from upstream stays practical and fixes can be offered
  back.

## License

XPrism is distributed under the MIT/X11 license, as are the X.Org Server
and XLibre. See [COPYING](COPYING) for the full terms and copyright
notices.

X Window System is a trademark of The Open Group.
