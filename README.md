<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="doc/logo/logo-dark.svg">
    <img src="doc/logo/logo-light.svg" alt="XPrism" width="420">
  </picture>
</p>

<p align="center">
  <a href="https://github.com/XPrismOrg/xserver/actions/workflows/xprism.yml"><img src="https://github.com/XPrismOrg/xserver/actions/workflows/xprism.yml/badge.svg" alt="CI"></a>
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

XPrism runs as a regular X server with everything XLibre provides, and
adds the features below. It is young: the protocols it adds may still
change, and the hardware paths have been tried on few machines (see
[Status](#status)).

The current version is **26.11.01**. Versions are `YY.MM.NN`, the NNth
release of that month, with `.PP` added for patch releases (`26.11.01.01`).

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
limited per launcher socket, and can be changed at runtime. They are
enforced by default; `-capmode audit` only logs what would be denied, and
a permission agent can ask the user. See [doc/capabilities.md](doc/capabilities.md).

### Server-side compositing

By default the server composites windows itself (`-compositor off`
disables it). ARGB windows and window opacity work without an external compositor, and
fullscreen windows bypass composition. The window manager can set
per-window opacity, transforms, rounded corners, shadows and background
blur through the XPRISM-SCENE extension.
External compositors such as picom still work. See
[doc/compositor.md](doc/compositor.md) and
[doc/xprism-scene-protocol.md](doc/xprism-scene-protocol.md).

### Colour management and HDR

Windows can declare their colour space
([XPRISM-COLOR](doc/xprism-color-protocol.md)); the compositor converts
them on the GPU and composes in linear light, for sRGB, wide-gamut and
HDR (PQ, HLG) monitors, one space per monitor. See [doc/color.md](doc/color.md).

### Modern display handling

Atomic modesetting is on by default, each output is composed at most
once per refresh, and variable refresh rate (FreeSync) works.

### Wayland applications

[WX11](wx11/README.md) runs Wayland applications as ordinary X windows,
managed by any X11 window manager: toplevels, popups, keyboard and
pointer, clipboard and primary selection, transparency. Each Wayland
application gets an X connection of its own, so XPrism's permissions
apply to it like to any X client, and sandboxed ones (via
`wp_security_context_v1`) land in a namespace.

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
| NetBSD, DragonFly BSD | Built and unit-tested in CI |

## Building

XPrism uses Meson and has the same dependencies as XLibre. The
[XLibre build instructions](https://github.com/X11Libre/xserver/wiki/Building-XLibre)
list the packages for common distributions.

```sh
git clone https://github.com/XPrismOrg/xserver.git
cd xserver
meson setup build -Dxvfb=true
ninja -C build
(cd wx11 && cargo build --release)   # WX11, optional
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
./build/hw/vfb/Xvfb :5 -namespace ns.conf
```

Clients that authenticate with the second cookie run in `sandbox`. They can
paste from the session clipboard but cannot access anything else, and the
server log lists each capability they were refused. Add `-capmode audit`
to let them through and only log what they would need, or run
[xprism-permission-agent](utils/xprism-permission-agent/README.md) to be
asked. For running real
applications in a sandbox, see the bubblewrap example in
[utils/xns-listen](utils/xns-listen/README.md).

## Status

The first release is **26.11.01**. The eight development phases are
complete for it; what they leave for later releases is listed with each.

| Phase | Done in 26.11.01 | Later |
|---|---|---|
| I. Foundation | Codebase audit, security hooks, launcher-assigned identity, capabilities, platform abstraction layer, CI, large clipboard transfers (INCR) between namespaces, Flatpak applications confined by their cgroup | |
| II. Display | Atomic modesetting by default (mode changes, DPMS, screen saver, rotation and gamma tested in CI), per-output scheduling, variable refresh rate (tested on FreeSync) | Testing hotplug and multi-GPU on hardware |
| III. Compositor | Server-side compositor controlled by the window manager ([XPRISM-SCENE](doc/xprism-scene-protocol.md)), explicit sync, direct scanout with page flips; tested on hardware | Direct scanout per monitor |
| IV. Colour | Per-window colour spaces, sRGB, wide-gamut and HDR (PQ, HLG) outputs, HDR and wide gamut driven from the EDID, colour-converted cursors | fp16 scanout; trying an HDR monitor |
| V. Permissions | Capabilities enforced by default, permission prompts ([xprism-permission-agent](utils/xprism-permission-agent/README.md)), application-scoped keyboard, focus and input, virtual seats, extension audit ([doc/capabilities.md](doc/capabilities.md)) | |
| VI. Wayland | [WX11](wx11/README.md): toplevels, popups kept on screen, input, clipboard and primary selection, transparency, one X connection per application, sandboxes through security contexts | Zero-copy dma-buf and Present, touch, subsurfaces, drag and drop |
| VII. Portability | Linux, FreeBSD, OpenBSD, NetBSD and DragonFly BSD built and unit-tested in CI | The test suite on the BSDs, hardware validation on more GPUs |
| VIII. Release | Versioning, Code of Conduct, contributing guide, Arch packaging (`.github/arch`), full test suite green | |

## Documentation

- [doc/capabilities.md](doc/capabilities.md): the capability model
- [doc/Xnamespace.md](doc/Xnamespace.md): namespaces and their configuration
- [doc/Xnamespace-protocol.md](doc/Xnamespace-protocol.md): the X-NAMESPACE extension
- [doc/compositor.md](doc/compositor.md): the server-side compositor
- [doc/color.md](doc/color.md): colour management, wide gamut and HDR (design)
- [doc/xprism-color-protocol.md](doc/xprism-color-protocol.md): the XPRISM-COLOR extension
- [doc/xprism-scene-protocol.md](doc/xprism-scene-protocol.md): the XPRISM-SCENE extension
- [doc/pal.md](doc/pal.md): the platform abstraction layer
- [wx11/README.md](wx11/README.md): WX11, Wayland applications on XPrism
- [CODE_OVERVIEW.md](CODE_OVERVIEW.md): layout of the source tree

## Contributing

Contributions are welcome. [CONTRIBUTING.md](CONTRIBUTING.md) explains how
to build, test and submit changes, and everyone taking part follows the
[Code of Conduct](CODE_OF_CONDUCT.md). In short:

- Every security check comes with a test, and the test must be shown to
  fail when the check is removed.
- No new OS-specific `#ifdef`s outside the platform abstraction layer. The
  `os-ifdefs` test enforces this.
- Changes to code inherited from XLibre are kept small and self-contained,
  so that merging from upstream stays practical and fixes can be offered
  back.

Questions go to [Discussions](https://github.com/orgs/XPrismOrg/discussions),
security issues to the [private reporting
form](https://github.com/XPrismOrg/xserver/security/advisories/new).

## License

XPrism is distributed under the MIT/X11 license, as are the X.Org Server
and XLibre. XPrism's own work is Copyright © 2026 Nexoniarz and XPrismOrg. See
[COPYING](COPYING) for the full terms and copyright notices.

X Window System is a trademark of The Open Group.
