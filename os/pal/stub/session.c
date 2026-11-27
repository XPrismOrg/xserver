/* SPDX-License-Identifier: MIT OR X11
 *
 * PAL session/devices/restrict: stub backend, until the real
 * backends (libseat, logind, udev, devd, hotplug(4), pledge, Landlock)
 * land. Everything reports "not supported"; restrict does nothing.
 */
#include <dix-config.h>

#include <errno.h>
#include <stddef.h>

#include "os/pal/pal.h"

int
pal_session_open(const struct pal_session_callbacks *cb, void *data,
                 struct pal_session **out)
{
    (void) cb;
    (void) data;
    *out = NULL;
    return -ENOTSUP;
}

void
pal_session_close(struct pal_session *s)
{
    (void) s;
}

const char *
pal_session_seat(struct pal_session *s)
{
    (void) s;
    return "seat0";
}

int
pal_session_open_device(struct pal_session *s, const char *path, int *device_id)
{
    (void) s;
    (void) path;
    *device_id = -1;
    return -ENOTSUP;
}

void
pal_session_close_device(struct pal_session *s, int device_id)
{
    (void) s;
    (void) device_id;
}

int
pal_session_switch(struct pal_session *s, int vt)
{
    (void) s;
    (void) vt;
    return -ENOTSUP;
}

int
pal_device_enumerate(enum pal_device_class cls, pal_device_proc found, void *data)
{
    (void) cls;
    (void) found;
    (void) data;
    return -ENOTSUP;
}

int
pal_device_monitor_start(pal_device_proc added, pal_device_proc removed,
                         void *data, struct pal_device_monitor **out)
{
    (void) added;
    (void) removed;
    (void) data;
    *out = NULL;
    return -ENOTSUP;
}

void
pal_device_monitor_stop(struct pal_device_monitor *m)
{
    (void) m;
}

int
pal_restrict_self(enum pal_restrict_stage stage)
{
    (void) stage;
    return 0;
}
