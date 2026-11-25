/* SPDX-License-Identifier: MIT OR X11
 *
 * PAL process backend: command line of a process, from /proc/<pid>/cmdline (Linux, NetBSD, GNU and others).
 * Moved from DetermineClientCmd() in os/client.c.
 */
#include <dix-config.h>

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "os/pal/backend.h"
#include "os.h"

int
pal_backend_procfs_cmdline(pid_t pid, char **cmdname, char **cmdargs)
{
    char path[PATH_MAX + 1];
    int totsize;
    int fd;

    /* Check if /proc/pid/cmdline exists. It's not supported on all
     * operating systems. */
    if (snprintf(path, sizeof(path), "/proc/%d/cmdline", pid) < 0)
        return -1;
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    /* Read the contents of /proc/pid/cmdline. It should contain the
     * process name and arguments. */
    totsize = read(fd, path, sizeof(path));
    close(fd);
    if (totsize <= 0)
        return -1;
    path[totsize - 1] = '\0';

    /* Construct the process name without arguments. */
    if (cmdname) {
        *cmdname = strdup(path);
    }

    /* Construct the arguments for client process. */
    if (cmdargs) {
        int cmdsize = strlen(path) + 1;
        int argsize = totsize - cmdsize;
        char *args = NULL;

        if (argsize > 0)
            args = calloc(1, argsize);
        if (args) {
            int i = 0;

            for (i = 0; i < (argsize - 1); ++i) {
                const char c = path[cmdsize + i];

                args[i] = (c == '\0') ? ' ' : c;
            }
            args[argsize - 1] = '\0';
            *cmdargs = args;
        }
    }
    return 0;
}
