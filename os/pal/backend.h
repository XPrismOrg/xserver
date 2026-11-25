/* SPDX-License-Identifier: MIT OR X11
 *
 * What each PAL backend directory (os/pal/<os>/) provides to the common
 * PAL code. Not for use outside os/pal.
 */
#ifndef XSERVER_OS_PAL_BACKEND_H
#define XSERVER_OS_PAL_BACKEND_H

#include <stdbool.h>
#include <sys/types.h>

/* process grouping record of @p pid (Linux: /proc/<pid>/cgroup), malloc'ed,
   or NULL if the platform has none or it can't be read */
char *pal_backend_process_grouping(pid_t pid);

/* is the process behind @p pidfd still alive? false if unknown */
bool pal_backend_pidfd_alive(int pidfd);

#endif /* XSERVER_OS_PAL_BACKEND_H */
