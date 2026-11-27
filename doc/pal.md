# Platform abstraction layer

Code that has to behave differently on Linux, FreeBSD, OpenBSD, NetBSD
and DragonFly BSD goes through one interface, `os/pal/pal.h`. This
document describes how that interface is organized, which backend each
platform uses, and what OS-specific code still exists outside it.

SchOS implements the Linux ABI and system calls, so it uses the Linux
backends and needs no code of its own. Anything that works on Linux is
expected to work on SchOS.

## Rules

1. Backends are selected at build time in `os/pal/meson.build`, with one
   directory per OS (`os/pal/linux/`, `os/pal/freebsd/`, ...) and shared
   code in `os/pal/procfs/`, `os/pal/generic/` and `os/pal/stub/`. There
   is no runtime selection and no plugin loading.
2. OS-specific `#if`s are not allowed outside `os/pal`, apart from the
   legacy per-OS directories `hw/xfree86/os-support/` and `hw/kdrive/os/`.
   `test/check-os-ifdefs.sh` keeps a per-file count of the existing ones
   in `test/os-ifdefs.baseline` and fails when a file gains one. The
   baseline is only ever lowered.
3. A feature check (Meson `has_function`, `has_header`, giving `HAVE_*`)
   is preferred over an OS check wherever it is enough. The abstraction
   layer is for behaviour that genuinely differs between systems.
4. The layer belongs to the server as a whole, not to one DDX. XLibre's
   `hw/xfree86/os-support` serves only the Xorg DDX, and kdrive has its
   own copy; the new display code planned for Phase II uses neither.
5. Modules without a backend still build and return `-ENOTSUP`, so that
   callers can already be written against the interface.

## Modules

| Module | Purpose | Status |
|---|---|---|
| identity | Credentials of the peer of a socket: uid, gid, pid, Linux pidfd, Solaris zone | Implemented for all platforms (`os/pal/identity.c`) |
| process | Information about a peer process (command line, sandbox hint), used for logging and for classifying unlabeled clients | Command lines on all platforms (`os/pal/<os>/cmdline.c`); sandbox hints on Linux |
| platform | The OS and input backend the build targets | Implemented |
| session | Opening DRM and input devices without root, VT and session switching | Interface only, stub backend |
| devices | Enumerating GPUs and input devices, hotplug | Interface only, stub backend |
| restrict | Dropping privileges the server no longer needs | Interface only, no-op backend |

## Backends

The identity column and the Linux process hint are implemented. The rest
of the table is the plan for Phase II.

| | Identity | Process hint | Session | Devices | Input | Restrict |
|---|---|---|---|---|---|---|
| Linux, SchOS | `SO_PEERCRED`, `SO_PEERPIDFD` | cgroup (Flatpak and Snap scopes) | libseat, logind | udev | libinput | Landlock |
| FreeBSD | `LOCAL_PEERCRED` (pid from 13.0) | jail (planned) | libseat (seatd) | devd, sysctl | libinput (evdev) | Capsicum, limited |
| DragonFly | `LOCAL_PEERCRED` | none | libseat (seatd) | devd | libinput (evdev) | none |
| OpenBSD | `SO_PEERCRED` | none | direct, or libseat if available | hotplug(4), `/dev/drm*` | wscons | pledge, unveil |
| NetBSD | `LOCAL_PEEREID` | none | libseat (seatd) or direct | drvctl, hotplug | wscons | none |

Notes:

- libseat is the default session backend because it covers both seatd
  and logind with one API and is available on Linux and most BSDs. XLibre
  already integrates it in `hw/xfree86/os-support/shared/seatd-libseat.c`,
  but only for the Xorg DDX, and with the default `-Dseatd_libseat=auto`
  it is only enabled together with udev, so the BSDs have to force it on.
  That code moves into the session module.
- GPUs are enumerated with libdrm's `drmGetDevices2()` everywhere. The
  devices module adds what libdrm does not provide: seat assignment, the
  boot display flag and hotplug notification.
- Input uses libinput where the kernel provides evdev (Linux, FreeBSD,
  DragonFly) and wscons on OpenBSD and NetBSD. The input stack itself is
  Phase V work; for now the layer only reports which backend applies.
- Process hints are never used to grant anything. They are looked up by
  pid and can be forged by a process that controls its own cgroup or
  command line, so they serve logging and the fallback classification of
  unlabeled clients only (`classify` in the namespace configuration,
  which only ever moves a client out of the root namespace). Permissions are granted through labeled
  sockets (X-NAMESPACE listeners). Where a pidfd is available,
  `pal_process_describe()` checks afterwards that the process still
  exists, so the result cannot describe an unrelated process that reused
  the pid.

## Remaining OS-specific code

Outside `os/pal` and the legacy per-OS directories, 22 files still
contain OS `#if`s (see `test/os-ifdefs.baseline`):

| Area | Files | Destination |
|---|---|---|
| Peer credentials | `os/access.c` | identity; `GetLocalClientCreds()` already reads the stored identity |
| Console, VT, power management, bell | `hw/xfree86/os-support/*`, `hw/xfree86/common/xf86Init.c` | session |
| Device and bus discovery | `hw/xfree86/common/xf86AutoConfig.c`, `xf86pciBus.c`, `xf86platformBus.c`, `xf86Configure.c`, `config/udev.c`, `config/wscons.c` | devices |
| Input devices | `hw/kdrive/os/linux/*`, `config/wscons.c` | input backend (Phase V) |
| Privilege handling | `hw/xfree86/xorg-wrapper.c`, `os/utils.c` | session, restrict |
| Sockets and transport | `os/Xtranssock.c`, `os/Xtransutil.c`, `os/connection.c` | stays; portability code (rule 3) |
| Other portability code | `include/compiler.h`, `os/xsha1.c`, `os/backtrace.c`, `os/inputthread.c`, `Xext/shm/shm.c` | stays, or becomes `HAVE_*` checks |

Process command lines (`os/client.c`) and the macOS and Cygwin code have
already been moved or removed.

The Xorg DDX keeps using `hw/xfree86/os-support` for as long as it
exists. New display code is written against this layer only.
