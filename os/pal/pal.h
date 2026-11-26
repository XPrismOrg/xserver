/* SPDX-License-Identifier: MIT OR X11
 *
 * XPrism Platform Abstraction Layer (PAL).
 *
 * Everything the server needs from the operating system that differs
 * between Linux and the BSDs goes through this interface (SchOS uses the
 * Linux ABI and therefore the Linux backends). Backends
 * live in os/pal/<os>/ and are chosen at build time (os/pal/meson.build);
 * there is no runtime switching and no OS #ifdef outside os/pal (checked
 * by test/check-os-ifdefs.sh). See doc/pal.md for the design and the
 * per-OS backend table.
 *
 * Status of each module:
 *   identity   implemented   os/pal/identity.h (peer_identity_*)
 *   process    implemented   pal_process_*        (sandbox hints: Linux)
 *   platform   implemented   pal_platform_*
 *   session    interface     pal_session_*        (stub backend)
 *   devices    interface     pal_device_*         (stub backend)
 *   restrict   interface     pal_restrict_self()  (no-op backend)
 *
 * "interface" modules compile and return "not supported" until their
 * Phase II backends land, so callers can be written against them now.
 *
 * Conventions: functions returning int return 0 or a negative errno
 * value. Strings returned in structs are owned by the struct and freed by
 * its release function. Nothing here may block for long: the server is
 * single-threaded apart from the input thread.
 */
#ifndef XSERVER_OS_PAL_H
#define XSERVER_OS_PAL_H

#include <stdbool.h>
#include <sys/types.h>

#include "os/pal/identity.h"

/* ------------------------------------------------------------------ *
 *  platform: what are we running on
 * ------------------------------------------------------------------ */

/** "linux", "freebsd", "openbsd", "netbsd", "dragonfly", "sunos",
 *  "gnu" or "unknown": the backend this server was built with */
const char *pal_platform_name(void);

/** input backend this platform uses: "libinput" (Linux, FreeBSD,
 *  DragonFly), "wscons" (OpenBSD, NetBSD) or "none" */
const char *pal_platform_input_backend(void);

/* ------------------------------------------------------------------ *
 *  identity: who is on the other end of a socket
 *  -> os/pal/identity.h: peer_identity_query(), peer_identity_release()
 * ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ *
 *  process: what a peer process is, best effort, for logs and for
 *  classifying unlabeled clients (AD-2, "peer credentials" fallback).
 *  Never a basis for granting anything: all of it can be spoofed or is
 *  racy, which is why the security model uses labeled sockets instead.
 * ------------------------------------------------------------------ */

struct pal_process_info {
    char *cmd_name;         /* argv[0] or executable name, or NULL */
    char *cmd_args;         /* remaining arguments, space separated, or NULL */
    /* What the OS's process grouping says about a sandbox, e.g. Linux
       cgroups ("flatpak", "org.example.Editor"), FreeBSD jails ("jail",
       jail name). NULL if none or unknown. Unverified hints. */
    char *sandbox_engine;
    char *sandbox_app_id;
    /* true if, after reading the above, the identity's pidfd showed the
       process still alive, so the data belongs to it and not to a
       process that reused its pid. Only possible with PEER_ID_PIDFD. */
    bool pid_confirmed;
};

/**
 * Describe the process behind @p id (needs PEER_ID_PID).
 * @return 0 on success (fields may still be NULL), -ESRCH if there is no
 *         pid, -ENOMEM.
 */
int pal_process_describe(const struct peer_identity *id,
                         struct pal_process_info *info);

void pal_process_release(struct pal_process_info *info);

/**
 * Command line of process @p pid, for logs (X-Resource, Xnamespace).
 * Sets *cmdname to argv[0] and *cmdargs to the other arguments joined by
 * spaces, malloc'ed, or NULL if unknown. Either pointer may be NULL.
 */
void pal_process_cmdline(pid_t pid, char **cmdname, char **cmdargs);

/**
 * Backend hook, exposed for unit tests: extract a sandbox hint from the
 * process grouping record of a process (Linux: /proc/<pid>/cgroup
 * contents). Sets *engine / *app_id to malloc'ed strings, or leaves them
 * NULL. Backends without such a record ignore @p record.
 */
void pal_process_parse_grouping(const char *record, char **engine, char **app_id);

/* ------------------------------------------------------------------ *
 *  session: access to the seat without being root
 *
 *  Opens DRM and input device nodes through the session manager and
 *  tells the server when the session is switched away and back (VT
 *  switch). Backends: libseat (seatd or logind; Linux, FreeBSD,
 *  DragonFly, NetBSD), systemd-logind over D-Bus (Linux), direct (the
 *  server opens devices itself; root or a suitably privileged user;
 *  OpenBSD), stub.
 * ------------------------------------------------------------------ */

struct pal_session;

struct pal_session_callbacks {
    /* the session became active: devices may be used again */
    void (*activate)(void *data);
    /* the session is being switched away: stop using devices (drop DRM
       master, stop reading input) before returning */
    void (*deactivate)(void *data);
};

/**
 * Take the seat. Dispatching happens through the server's poll loop, so
 * callbacks run from the main loop.
 * @return 0, or -ENOTSUP if no backend is available
 */
int pal_session_open(const struct pal_session_callbacks *cb, void *data,
                     struct pal_session **out);

void pal_session_close(struct pal_session *s);

/** name of the seat, e.g. "seat0" */
const char *pal_session_seat(struct pal_session *s);

/**
 * Open a device node (DRM card, input event device) for the session.
 * @return an fd (>= 0) or a negative errno; *device_id identifies it for
 *         pal_session_close_device()
 */
int pal_session_open_device(struct pal_session *s, const char *path, int *device_id);

void pal_session_close_device(struct pal_session *s, int device_id);

/** switch to virtual terminal / session @p vt (1-based) */
int pal_session_switch(struct pal_session *s, int vt);

/* ------------------------------------------------------------------ *
 *  devices: find GPUs and input devices, and follow hotplug
 *
 *  Backends: udev (Linux), devd + sysctl (FreeBSD, DragonFly),
 *  hotplug(4) + drm/wscons enumeration (OpenBSD, NetBSD), stub.
 *  GPU enumeration itself can use libdrm's drmGetDevices2() everywhere;
 *  the backend adds seat assignment, boot-VGA detection and hotplug.
 * ------------------------------------------------------------------ */

enum pal_device_class {
    PAL_DEVICE_GPU,
    PAL_DEVICE_INPUT,
};

struct pal_device {
    enum pal_device_class cls;
    const char *devnode;    /* "/dev/dri/card0", "/dev/input/event3", ... */
    const char *seat;       /* "seat0" */
    const char *name;       /* human-readable, may be NULL */
    dev_t devnum;
    bool boot_display;      /* GPU: the one the firmware used */
};

struct pal_device_monitor;

typedef void (*pal_device_proc)(const struct pal_device *dev, void *data);

/** call @p found for every present device of class @p cls */
int pal_device_enumerate(enum pal_device_class cls, pal_device_proc found, void *data);

/**
 * Report devices added and removed from now on (from the main loop).
 * @return 0, or -ENOTSUP if the platform has no hotplug notification
 */
int pal_device_monitor_start(pal_device_proc added, pal_device_proc removed,
                             void *data, struct pal_device_monitor **out);

void pal_device_monitor_stop(struct pal_device_monitor *m);

/* ------------------------------------------------------------------ *
 *  restrict: let the server give up what it no longer needs
 *
 *  Backends: pledge/unveil (OpenBSD), Capsicum (FreeBSD; limited, as the
 *  server keeps opening devices), Landlock (Linux), no-op.
 * ------------------------------------------------------------------ */

enum pal_restrict_stage {
    /* initialization done: listening sockets, config and devices are
       open; from now on only accept, hotplugged devices (through the
       session) and the paths the server still needs */
    PAL_RESTRICT_RUNNING,
};

/** @return 0 (also when the backend does nothing), or a negative errno */
int pal_restrict_self(enum pal_restrict_stage stage);

#endif /* XSERVER_OS_PAL_H */
