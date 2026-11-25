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

/* command line of @p pid: *cmdname = argv[0], *cmdargs = the remaining
   arguments joined by spaces (either may be NULL to skip it; outputs are
   malloc'ed or left untouched). 0 on success, -1 if unknown */
int pal_backend_process_cmdline(pid_t pid, char **cmdname, char **cmdargs);

/* the /proc/<pid>/cmdline reader (os/pal/procfs), for backends that try
   it first */
int pal_backend_procfs_cmdline(pid_t pid, char **cmdname, char **cmdargs);

/* is the process behind @p pidfd still alive? false if unknown */
bool pal_backend_pidfd_alive(int pidfd);

#endif /* XSERVER_OS_PAL_BACKEND_H */
