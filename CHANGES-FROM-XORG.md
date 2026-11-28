# XPrism compared to the X.Org Server

This file lists everything XPrism adds, changes or removes compared to
the X.Org Server. XPrism is a fork of XLibre, which is itself a fork of
the X.Org Server, so the differences come in two layers:

1. [What XLibre changed](#from-xlibre), which XPrism inherits.
2. [What XPrism changed](#xprism) on top of XLibre: new features, fixes
   to inherited code, and removed parts.

XPrism keeps the X11 protocol, the Xorg driver ABI of XLibre and its
configuration files. Applications, window managers, drivers and
`xorg.conf` files that work with XLibre work with XPrism, apart from the
[removed parts](#removed).

Commit hashes refer to this repository.

## Contents

- [From XLibre](#from-xlibre)
- [XPrism](#xprism)
  - [Security](#security)
  - [Built-in compositor](#built-in-compositor)
  - [Colour management and HDR](#colour-management-and-hdr)
  - [Display](#display)
  - [Wayland applications (WX11)](#wayland-applications-wx11)
  - [Xvfb, glamor and Present](#xvfb-glamor-and-present)
  - [Platform abstraction layer](#platform-abstraction-layer)
  - [New protocol extensions](#new-protocol-extensions)
  - [New command-line options](#new-command-line-options)
  - [New driver options](#new-driver-options)
  - [New build options](#new-build-options)
  - [Environment variables](#environment-variables)
  - [New tools](#new-tools)
  - [Removed](#removed)
  - [Fixes to inherited code](#fixes-to-inherited-code)
  - [Platforms, testing and releases](#platforms-testing-and-releases)

## From XLibre

XLibre forked the X.Org Server in June 2025. At the point XPrism forked
it (October 2026), XLibre listed these differences from the X.Org Server:

- everything from the X.Org Server, including its unreleased features;
- TearFree modesetting by default, and optional atomic modesetting;
- support for the NVIDIA drivers 340, 390, 470, 570 and newer, with the
  proprietary driver detected automatically (from 25.0.0.16; XPrism
  loads it only with IgnoreABI, see Display below);
- the Xnamespace extension, which separates X clients into namespaces;
- seat management through seatd as well as systemd-logind;
- Xfbdev, the generic framebuffer X server for Linux (kdrive);
- CI builds for several BSDs, Linux, macOS and Windows.

See [XLibre](https://github.com/X11Libre/xserver) for the details of
these and of XLibre's own fixes and cleanups.

## XPrism

### Security

X11 lets any client read the screen, read keystrokes and send input to
other applications. XPrism keeps these abilities but decides per client
who holds them.

**Namespaces** (built on XLibre's Xnamespace,
[doc/Xnamespace.md](doc/Xnamespace.md)): a client in a namespace has its
own clipboard and cannot see or reach the windows, input or screen
contents of other namespaces. XPrism adds:

- **Launcher-assigned identity.** A sandbox launcher hands the server a
  listening socket bound to a namespace and an application ID (bba3835a2,
  33b8131fc). Clients connecting through it are placed there before their
  first request and cannot choose another identity. The launcher can also
  set a capability ceiling for the socket (10c7c2614).
- **Peer identity at connect time.** Each client's uid, gid and pid are
  read once, when it connects, and kept (91370b938).
- **Runtime management.** The X-NAMESPACE extension (below) creates and
  deletes namespaces, manages their auth tokens, listeners and grants,
  and reports which namespace and application a client belongs to.
- **Clipboard across namespaces.** Grants open the session clipboard to a
  namespace (2a2b5a4cb), including large transfers with INCR (2a719455c).
- **Flatpak.** `classify flatpak` in the configuration gives each Flatpak
  application a transient namespace of its own, recognised by its cgroup
  (e69232014). The cgroup only ever moves a client out of the root
  namespace, never into it.
- Auth cookies are no longer printed at startup (75aaf13d6).

**Capabilities** ([doc/capabilities.md](doc/capabilities.md)): twelve
permissions, each checked where the protocol performs the action:

| Capability | Covers |
|---|---|
| `capture.screen` | Reading the root window |
| `capture.window` | Reading other clients' windows and pixmaps |
| `input.inject` | XTEST |
| `input.monitor` | Raw input events, RECORD, keyboard grabs, `QueryKeymap` without the focus |
| `input.virtual_seat` | Creating input devices (master pointers and keyboards) |
| `window.enumerate` | Listing other clients' windows (`QueryTree`) |
| `window.inspect` | Attributes, geometry and properties of other clients' windows |
| `window.manage` | Reparenting, save sets, redirection of other clients' windows |
| `clipboard.read` | Pasting from the session's selections |
| `clipboard.write` | Owning the session's selections |
| `display.configure` | RandR changes |
| `compositor.legacy` | Taking over from the built-in compositor |

- Granted per namespace, limited per launcher socket, changeable at run
  time (ac8139bd1, 10c7c2614).
- Enforced by default (70c7c82b4). `-capmode audit` only logs what would
  be refused; `-capmode off` disables the checks.
- **Permission prompts.** A permission agent can be asked when a client
  needs a capability it lacks, and can allow or deny it once or for as
  long as the application runs (f70232ed9, d2ed91c68).
- **Scoped automation.** A namespace may inject input into one
  application only, through a seat of its own that the server creates and
  removes (7b7b83657).
- **Keyboard and focus rules.** The keyboard and held modifiers belong to
  the focused client (80adc610d, 41356f78a); which window has the focus
  is hidden from confined clients (262a0c673); the active client may warp
  the pointer and receive raw events (5695120f1); a confined client's
  input seats are removed with it (83e0fc19f).
- **Reading inferiors.** Reading one's own window together with another
  client's window inside it needs `capture.window` (bcdbc1552).
- **Legacy access is logged.** Every client with unrestricted access is
  logged when it connects, and so is a server running without any policy
  (679d80dbc).
- **Extension audit.** Every built-in extension was reviewed for confined
  clients. Unreviewed extensions (driver extensions, new ones) are hidden
  from them unless the namespace allows them with `allow-extension`
  (dbc712fe6).
- **RandR write access.** RandR looks up the objects a request changes
  with write access, so SELinux and other XACE policies see the change as
  a write (4e749a551).

New namespace configuration keywords: `grant`, `allow-extension`,
`classify` and `inject-into`.

### Built-in compositor

[doc/compositor.md](doc/compositor.md). The server composites windows
itself; no external compositor is needed (933935481, enabled by default
in db2711525).

- ARGB windows and `_NET_WM_WINDOW_OPACITY` work out of the box.
- The window manager sets per-window opacity, transforms, rounded
  corners, drop shadows and background blur through XPRISM-SCENE
  (f42759413, 5c44abf02, 70cf63e76); input follows transforms
  (2c5a01f8e).
- Only the changed area is repainted; a restack repaints only the
  overlaps it changes (950adb414).
- Each output is composed at most once per refresh, each at its own rate
  (a55a9854e).
- Fullscreen windows are shown directly, without composition, and can be
  page-flipped by Present (17740450d).
- Pending changes are composed before any client reads the screen, so
  `GetImage` and the core protocol give the same results as without a
  compositor; the X Test Suite passes with the compositor on and off
  (aa6d3d3d8, 476874932).
- External compositors (picom, KWin, Mutter) still work: the built-in
  one hands the screen over and takes it back when they exit.
- Works with Xinerama, each screen composing its own part (0e39040f6).
- `-compositor off` turns it off.

### Colour management and HDR

[doc/color.md](doc/color.md).

- Applications describe their windows' colour space with XPRISM-COLOR
  (2a687be7c): sRGB, Display P3, BT.2020 or custom primaries; sRGB,
  gamma 2.2, linear, PQ or HLG transfer functions; HDR luminance.
- The compositor converts each window for its output, on the GPU with
  glamor (2b46c353d), with gamut and tone mapping.
- Outputs can be sRGB, wide-gamut SDR or HDR (PQ, HLG), one colour space
  per CRTC (d4537eea8, 898cdf54a, e851786a4). Applications are told when
  an output's space changes (bdacc4504).
- With glamor, composition happens in linear light in half float
  (9e134dbef).
- The software cursor, hardware cursors and redrawn root tiles are
  converted too (81e54e0a6, 38e07bbf3).

### Display

- **Atomic modesetting by default** in the modesetting driver
  (aad000dee), with a fix for turning CRTCs and outputs off (67466acc3).
- **HDR monitors** detected from their EDID and driven in BT.2020 with
  PQ or HLG (d4dd4ad1c), and **wide-gamut SDR monitors** shown in their
  own primaries (7278673f7). Both are off by default.
- **Extended dynamic range (EDR)** on laptop panels: while a window
  shows HDR or extended sRGB content, the backlight is raised and SDR
  content dimmed by as much, so highlights are brighter and SDR white is
  unchanged (`Option "EDR"`, off by default).
- **Variable refresh rate** reaches the driver again: an XLibre bug
  dropped `_VARIABLE_REFRESH` whenever Xinerama was compiled in but not
  used (c362bc782).
- **NVIDIA's proprietary X driver is not loaded** unless `IgnoreABI`
  is set (`Option "IgnoreABI"` in ServerFlags, or `-ignoreABI`). Built
  for X.Org's ABI, driver 595 corrupts the server's screen data and
  aborts it at startup (XLibre issue #2355), and the driver replaces
  parts of the dispatcher and RandR, which bypasses namespace checks.
  Without it, modesetting drives the displays and NVIDIA GPUs render
  through Vulkan, CUDA and PRIME.

### Wayland applications (WX11)

[wx11/README.md](wx11/README.md). WX11 is a separate program that runs
Wayland applications as ordinary X windows, managed by any X11 window
manager (a7d77194a and following):

- toplevels and popups, with popups kept on screen;
- keyboard and pointer input;
- one output (`wl_output`, `xdg_output`): the X screen;
- clipboard and primary selection between Wayland and X applications,
  including large transfers;
- transparency (ARGB windows);
- one X connection per Wayland application, so XPrism's capabilities
  apply to each; sandboxed applications (`wp_security_context_v1`) are
  placed in a namespace.

### Xvfb, glamor and Present

- Xvfb gives each screen its own CRTCs, sized to the screen (1c2242b4e),
  and places Xinerama screens side by side (`-origin`, a096fe620).
- Xvfb takes an output colour space per screen or per CRTC
  (`-colorspace`, `-crtccolorspace`), and simulates an EDR backlight
  (`-edr`).
- Explicit sync (`PresentPixmapSynced`) with Xvfb and glamor on a render
  node (906e39e52).
- glamor runs Render's convolution filter on the GPU (1634f6162).

### Platform abstraction layer

[doc/pal.md](doc/pal.md). Operating-system specific code is moved behind
one interface with a backend per OS (3d4890215): peer credentials,
process command lines (3929632df) and sandbox detection are done; session
and device handling have the interface only. A test fails when new
OS-specific `#if`s appear outside the layer.

### New protocol extensions

**X-NAMESPACE 1.2** ([doc/Xnamespace-protocol.md](doc/Xnamespace-protocol.md)),
for clients in a `superpower` namespace:

| Request | Since |
|---|---|
| `QueryVersion`, `ListNamespaces`, `CreateNamespace`, `DeleteNamespace`, `QueryNamespace`, `SetNamespaceFlags` | 1.0 |
| `AddAuthToken`, `RemoveAuthToken`, `ListAuthTokens`, `GetClientNamespace` | 1.0 |
| `AddListener`, `RemoveListener`, `GetClientInfo`, `SetNamespaceGrants` | 1.1 |
| `SetPermissionAgent`, `AnswerPermission`, `SetNamespaceInjectScope`, event `PermissionRequest` | 1.2 |

**XPRISM-SCENE 0.4** ([doc/xprism-scene-protocol.md](doc/xprism-scene-protocol.md)),
for the window manager:

| Request | Since |
|---|---|
| `QueryVersion`, `SetNodeAttributes`, `GetNodeAttributes`, `ClearNodeAttributes` (opacity, transform) | 0.1 |
| `SetNodeDecoration`, `GetNodeDecoration` (rounded corners, shadow) | 0.2 |
| Background blur as a decoration | 0.3 |
| Input follows transforms | 0.4 |

**XPRISM-COLOR 0.3** ([doc/xprism-color-protocol.md](doc/xprism-color-protocol.md)),
for applications:

| Request | Since |
|---|---|
| `QueryVersion`, `SetWindowColorSpace`, `GetWindowColorSpace`, `ClearWindowColorSpace` | 0.1 |
| `GetOutputColorSpace` | 0.2 |
| `SelectOutputInput`, event `OutputColorSpaceNotify` | 0.3 |

### New command-line options

| Option | Server | Meaning |
|---|---|---|
| `-capmode off\|audit\|enforce` | all | Capability checks: none, log only, or enforce (the default) |
| `-compositor on\|off` | all | The built-in compositor |
| `-origin X,Y` | Xvfb | Position of the last `-screen` in the Xinerama layout |
| `-colorspace P[,T[,N]]` | Xvfb | Colour space of the last `-screen`'s output |
| `-crtccolorspace C:P[,T[,N]]` | Xvfb | Colour space of one CRTC |

### New driver options

modesetting driver, with matching output properties to change them at
run time:

| Option | Output property | Meaning |
|---|---|---|
| `HDR` | `HDR` | Drive monitors whose EDID offers HDR in BT.2020 with PQ or HLG |
| `WideGamut` | `WIDE_GAMUT` | Show wide-gamut SDR monitors in their own primaries |

### New build options

| Option | Default | Meaning |
|---|---|---|
| `compositor_default` | `true` | Run the built-in compositor unless `-compositor off` is given |
| `xns_listen` | `auto` | Build `xns-listen` and `xprism-permission-agent` |

### Environment variables

For debugging and measurement; none is needed in normal use.

| Variable | Effect |
|---|---|
| `XPRISM_NO_FRAME_PACING` | Compose as often as clients draw |
| `XPRISM_NO_GPU_COLOR` | Do colour conversion on the CPU even with glamor |
| `XPRISM_NO_LINEAR_COMPOSITION` | Compose in the output's encoding instead of linear light |

### New tools

- [xns-listen](utils/xns-listen/README.md): creates a namespace listener
  socket for a sandbox launcher, for example around bubblewrap (c01e92fb9).
- [xprism-permission-agent](utils/xprism-permission-agent/README.md):
  asks the user when an application needs a permission it lacks
  (d2ed91c68).

### Removed

Parts that serve hardware or systems no longer in use, or that conflict
with the security model:

| Removed | Commit | Why |
|---|---|---|
| SECURITY extension | 3249d4780 | Replaced by namespaces and capabilities |
| Xnest | 3d053de49 | Xephyr does the same with full extension support |
| HAL hotplug backend | 757f88f9d | Unmaintained, replaced by udev around 2010 |
| AGP GART | ea9f2ff72 | Only DRI1 and pre-KMS drivers used it; stubs remain so binary drivers still load |
| APM on Linux | d7161b8b4 | 32-bit x86 only, replaced by ACPI (which stays) |
| DGA extension | 22786338f | Direct framebuffer and input access bypasses the window system |
| XvMC | 98013c987 | Superseded by VA-API and VDPAU |
| vgahw module | 5d5ced4a9 | Only pre-KMS drivers used it |
| int10, x86emu, VBE | be636f638 | Ran the video BIOS for pre-KMS drivers; XPrism uses KMS |
| DRI1 | 4793b1c05 | No kernel driver uses it; superseded by DRI2 and DRI3 |
| XQuartz (macOS), rootless, PseudoramiX | 7115ebee1 | XPrism targets Linux and the BSDs |
| XWin (Windows) | 09ac1dd99 | Same |
| macOS and Cygwin support | 964257264 | Same |

About 132,000 lines are removed in total.

### Fixes to inherited code

Bugs found while building XPrism, in code that comes from XLibre or the
X.Org Server:

| Area | Fix | Commit |
|---|---|---|
| dix | `_VARIABLE_REFRESH` never reached the driver when Xinerama was compiled in but unused, so VRR never turned on | c362bc782 |
| dix | A window being destroyed could keep freed pixmaps | 1d707282a |
| dix | `FakeClientID()` lost the client bits for a zero id | 8defc17e5 |
| dix | The no-owner `SelectionNotify` now names the selection asked for | e97551408 |
| Xi | `XIPassiveGrabDevice` byte-swapped the event mask, which must not be swapped | fd32c53f3, d72530ad3 |
| RandR | Changes were looked up with read access only | 4e749a551 |
| os | Servers starting at the same time could remove each other's sockets | 2691e3fc4 |
| glamor | Depth-15 pixmaps failed on GLES | 149dcb396 |
| glamor | A convolved source was downloaded only in part before falling back | 616689c2c |
| Xephyr | Crashed or lost its host connection when the host had no GLX | b71c2ae4b, 320a2eaca |
| Xvfb | The framebuffer file missed the last drawing (OpenBSD) | 973824db4 |
| modesetting | CRTCs and outputs were not turned off with atomic modesetting | 67466acc3 |
| modesetting | Window keys registered on GPU screens; probed DRM node recorded too late | f3e5059d7, 2ce10df53 |
| xfree86 | GPU hotplug: platform device slots, PCI scan, `CreateScreenResources` check, failed PreInit | e55b78500, d1743654d, 09840282d, d41f9030c, 06ce0c9ac |
| xfree86 | `ServerLayout` options, `OutputClass` `HotplugDriver`, empty `-configdir` | 6044621ec, 4ea216425, 0a4790010, 090cd5e8d |
| man pages | Markup, typos and module path | beacf6dad, 41507867d |
| tests | A crashed unit test was reported as passing | f64e3a224 |

### Platforms, testing and releases

- **Platforms:** Linux, FreeBSD, OpenBSD, NetBSD and DragonFly BSD.
  macOS and Windows are no longer supported.
- **CI:** every push builds and tests on Linux (the full suite, with the
  compositor on and off, including piglit, the X Test Suite and a
  virtual KMS device) and on the four BSDs (unit tests and the Python
  test suite).
- **Tests:** every security check has a test that is shown to fail when
  the check is removed. The Python suite also passes with glamor on AMD
  and NVIDIA GPUs. The display paths were checked on real AMD hardware
  (a desktop monitor and a laptop panel), with an NVIDIA GPU rendering
  through PRIME on the laptop.
- **Versions** are `YY.MM.NN`, the NNth release of the month, with `.PP`
  for patch releases. The first release is 26.11.01.
- **Packaging:** an Arch Linux package, `xprism-xserver`
  (`.github/arch`).
- **Project:** a [Code of Conduct](CODE_OF_CONDUCT.md) and a
  [contributing guide](CONTRIBUTING.md).
