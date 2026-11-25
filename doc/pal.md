# XPrism Platform Abstraction Layer (PAL)

Everything XPrism needs from the operating system that differs between
Linux, FreeBSD, OpenBSD, NetBSD, DragonFly BSD and SchOS goes through one
interface: `os/pal/pal.h`. This document records why it looks the way it
does, what OS-specific code exists today, and which backend each platform
gets.

## Rules

1. **Backends are picked at build time** (`os/pal/meson.build`): one
   directory per OS (`os/pal/linux/`, ...), shared fallbacks in
   `os/pal/generic/` and `os/pal/stub/`. No runtime switching, no plugin
   loading.
2. **No OS `#if` outside `os/pal`** (and, until they are replaced, the
   legacy per-OS directories `hw/xfree86/os-support/` and `hw/kdrive/os/`).
   `test/check-os-ifdefs.sh` enforces this: it keeps a per-file count of
   the OS `#if`s that exist today (`test/os-ifdefs.baseline`) and fails if
   a file gains one. The baseline only shrinks.
3. **Feature checks over OS checks** where a feature check works (meson
   `has_function`/`has_header` → `HAVE_*`). The PAL is for behaviour that
   really differs by OS, not for portability shims.
4. **The PAL sits at server level**, not in a DDX. XLibre's
   `hw/xfree86/os-support` serves only the Xorg DDX and kdrive has its
   own; XPrism's display core (AD-1, Phase II) is neither.
5. Modules that have no backend yet still **compile and return
   `-ENOTSUP`**, so callers can be written against the interface now.

## Modules

| Module | Purpose | Status |
|---|---|---|
| identity | Who is on the other end of a socket: uid, gid, pid, Linux pidfd, Solaris zone | Done (`os/pal/identity.c`), all platforms |
| process | What a peer process is (command line, sandbox hint), for logs and unlabeled-client classification | Done; sandbox hints on Linux only |
| platform | Which OS and input backend this build targets | Done |
| session | Open DRM/input devices without root, VT/session switching | Interface; stub backend |
| devices | Enumerate GPUs and input devices, hotplug | Interface; stub backend |
| restrict | Let the server drop privileges it no longer needs | Interface; no-op backend |

## Backends per platform

The plan. Implemented today: the identity column, and the Linux process
hint. Everything else is what the Phase II backends will use.

| | identity | process hint | session | devices | input | restrict |
|---|---|---|---|---|---|---|
| **Linux** | `SO_PEERCRED` + `SO_PEERPIDFD` | cgroup (Flatpak, Snap scopes) | libseat; logind (D-Bus) | udev | libinput | Landlock |
| **FreeBSD** | `LOCAL_PEERCRED` (pid on 13+) | jail (to do) | libseat (seatd) | devd + sysctl | libinput (evdev) | Capsicum (limited) |
| **DragonFly** | `LOCAL_PEERCRED` | — | libseat (seatd) | devd | libinput (evdev) | — |
| **OpenBSD** | `SO_PEERCRED` | — | direct, or libseat if available (to check) | hotplug(4) + `/dev/drm*` | wscons | pledge + unveil |
| **NetBSD** | `LOCAL_PEEREID` | — | libseat (seatd) or direct | drvctl + hotplug | wscons | — |
| **SchOS** | defined by the SchOS backend | | | | | |

Notes on the choices:

- **libseat as the default session backend.** It covers seatd and logind
  with one API, and runs on Linux and most BSDs. XLibre already has a
  libseat integration (`hw/xfree86/os-support/shared/seatd-libseat.c`), but
  only for the Xorg DDX, and in the default `-Dseatd_libseat=auto` mode the
  build only enables it together with udev, so on the BSDs it has to be
  forced on. The PAL session
  module is where it moves.
- **GPU enumeration** uses libdrm's `drmGetDevices2()` on every platform;
  the devices backend adds seat assignment, the boot display flag and
  hotplug, which libdrm doesn't do.
- **Input**: libinput where the kernel offers evdev (Linux, FreeBSD,
  DragonFly); wscons on OpenBSD and NetBSD. The input stack itself is
  Phase V work; the PAL only names the backend for now.
- **Process hints are never trusted.** They are read by pid, can be faked
  by a process that controls its own cgroup or command line, and exist for
  logs and for the AD-2 fallback classification of unlabeled clients.
  Granting anything goes through labeled sockets (X-NAMESPACE listeners).
  Where a pidfd exists, `pal_process_describe()` checks afterwards that the
  process is still alive, so the data can't belong to a different process
  that reused the pid.

## Inventory: OS-specific code today

Outside `os/pal` and the legacy per-OS directories, 33 files have OS
`#if`s (`test/os-ifdefs.baseline`). By area:

| Area | Files | Moves to |
|---|---|---|
| Peer credentials | `os/access.c` | identity (mostly done: `GetLocalClientCreds()` reads the stored identity) |
| Process command line | `os/client.c` (`/proc`, `kvm`, `sysctl`, Solaris `psinfo`) | process |
| Console, VT, power management, bell | `hw/xfree86/os-support/*`, `hw/xfree86/common/xf86Init.c` | session |
| Device and bus discovery | `hw/xfree86/common/xf86AutoConfig.c`, `xf86pciBus.c`, `xf86platformBus.c`, `xf86Configure.c`, `config/udev.c`, `config/wscons.c` | devices |
| Input devices | `hw/kdrive/os/linux/*`, `config/wscons.c` | input backend (Phase V) |
| Privilege handling | `hw/xfree86/xorg-wrapper.c`, `os/utils.c` | session / restrict |
| Sockets and transport | `os/Xtranssock.c`, `os/Xtransutil.c`, `os/connection.c` | stays (portability shims, see rule 3) |
| Misc portability | `include/compiler.h`, `os/xsha1.c`, `os/backtrace.c`, `os/inputthread.c`, `Xext/shm/shm.c` | stays or becomes `HAVE_*` checks |
| macOS / Cygwin leftovers | `include/window.h`, `include/resource.h`, `include/dixfont.h`, ... | delete (unsupported platforms) |

The Xorg DDX keeps `hw/xfree86/os-support` for as long as it exists. The
new display core (Phase II) is written against the PAL only.
