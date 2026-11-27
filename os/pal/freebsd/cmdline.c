/* SPDX-License-Identifier: MIT OR X11
 *
 * PAL process backend: command line of a process, FreeBSD and DragonFly (sysctl KERN_PROC_ARGS).
 * Moved from DetermineClientCmd() in os/client.c.
 */
#include <dix-config.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/sysctl.h>

#include "os/pal/backend.h"
#include "os.h"

#include "dix.h"   /* ARRAY_SIZE */

int
pal_backend_process_cmdline(pid_t pid, char **cmdname, char **cmdargs)
{
    {
        int mib[] = {
            CTL_KERN,
            KERN_PROC,
            KERN_PROC_ARGS,
            pid,
        };

        /* Determine exact size instead of relying on kern.argmax */
        size_t len;
        if (sysctl(mib, ARRAY_SIZE(mib), NULL, &len, NULL, 0) != 0) {
            ErrorF("Failed to query KERN_PROC_ARGS length for PID %d: %s\n", pid, strerror(errno));
            return -1;
        }

        /* Read KERN_PROC_ARGS contents. Similar to /proc/pid/cmdline
         * the process name and each argument are separated by NUL byte.
         * DragonFly's size query can report less than the read then
         * needs (ENOMEM): grow the buffer and retry, up to a limit. */
        char *procargs = NULL;
        size_t size = len < 256 ? 256 : len;
        for (;;) {
            char *const grown = realloc(procargs, size);
            if (!grown) {
                ErrorF("Failed to allocate memory (%zu bytes) for KERN_PROC_ARGS result for pid %d: %s\n", size, pid, strerror(errno));
                free(procargs);
                return -1;
            }
            procargs = grown;
            memset(procargs, 0, size);
            len = size;
            if (sysctl(mib, ARRAY_SIZE(mib), procargs, &len, NULL, 0) == 0)
                break;
            if (errno != ENOMEM || size >= (1 << 20)) {
                ErrorF("Failed to get KERN_PROC_ARGS for PID %d: %s\n", pid, strerror(errno));
                free(procargs);
                return -1;
            }
            size *= 2;
        }
        if (len == 0 || procargs[len - 1] != '\0') {
            /* nothing, or cut short: not a usable command line */
            free(procargs);
            return -1;
        }

        /* Construct the process name without arguments. */
        if (cmdname) {
            *cmdname = strdup(procargs);
        }

        /* Construct the arguments for client process. */
        if (cmdargs) {
            size_t cmdsize = strlen(procargs) + 1;
            size_t argsize = len - cmdsize;
            char *args = NULL;

            if (argsize > 0)
                args = procargs + cmdsize;
            if (args) {
                /* Replace NUL with space except terminating NUL */
                for (size_t i = 0; i < (argsize - 1); i++) {
                    if (args[i] == '\0')
                        args[i] = ' ';
                }
                *cmdargs = strdup(args);
            }
        }
        free(procargs);
    }
    return 0;
}
