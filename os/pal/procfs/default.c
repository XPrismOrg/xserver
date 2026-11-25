/* SPDX-License-Identifier: MIT OR X11
 *
 * PAL process backend: platforms whose command lines come from procfs only.
 */
#include <dix-config.h>

#include "os/pal/backend.h"

int
pal_backend_process_cmdline(pid_t pid, char **cmdname, char **cmdargs)
{
    return pal_backend_procfs_cmdline(pid, cmdname, cmdargs);
}
