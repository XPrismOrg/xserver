/* SPDX-License-Identifier: MIT OR X11
 *
 * PAL process backend: command line of a process, Solaris/illumos (/proc/<pid>/cmdline, else /proc/<pid>/psinfo).
 * Moved from DetermineClientCmd() in os/client.c.
 */
#include <dix-config.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <procfs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "os/pal/backend.h"
#include "os.h"

int
pal_backend_process_cmdline(pid_t pid, char **cmdname, char **cmdargs)
{
    char path[PATH_MAX + 1];
    int totsize;
    int fd;

    if (pal_backend_procfs_cmdline(pid, cmdname, cmdargs) == 0)
        return 0;

    /* Solaris prior to 11.3.5 does not support /proc/pid/cmdline, but
     * makes information similar to what ps shows available in a binary
     * structure in the /proc/pid/psinfo file. */
    if (snprintf(path, sizeof(path), "/proc/%d/psinfo", pid) < 0)
        return -1;
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        ErrorF("Failed to open %s: %s\n", path, strerror(errno));
        return -1;
    }
    else {
        psinfo_t psinfo = { 0 };
        char *sp;

        totsize = read(fd, &psinfo, sizeof(psinfo_t));
        close(fd);
        if (totsize <= 0)
            return -1;

        /* pr_psargs is the first PRARGSZ (80) characters of the command
         * line string - assume up to the first space is the command name,
         * since it's not delimited.   While there is also pr_fname, that's
         * more limited, giving only the first 16 chars of the basename of
         * the file that was exec'ed, thus cutting off many long gnome
         * command names, or returning "isapython2.6" for all python scripts.
         */
        psinfo.pr_psargs[PRARGSZ - 1] = '\0';
        sp = strchr(psinfo.pr_psargs, ' ');
        if (sp)
            *sp++ = '\0';

        if (cmdname)
            *cmdname = strdup(psinfo.pr_psargs);

        if (cmdargs && sp)
            *cmdargs = strdup(sp);
    }
    return 0;
}
