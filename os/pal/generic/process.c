/* SPDX-License-Identifier: MIT OR X11
 *
 * PAL process backend for platforms without a sandbox hint source yet.
 * (FreeBSD jails, OpenBSD/NetBSD have no app grouping to read.)
 */
#include <dix-config.h>

#include <stddef.h>

#include "os/pal/backend.h"
#include "os/pal/pal.h"

char *
pal_backend_process_grouping(pid_t pid)
{
    (void) pid;
    return NULL;
}

bool
pal_backend_pidfd_alive(int pidfd)
{
    (void) pidfd;
    return false;
}

void
pal_process_parse_grouping(const char *record, char **engine, char **app_id)
{
    (void) record;
    *engine = NULL;
    *app_id = NULL;
}
