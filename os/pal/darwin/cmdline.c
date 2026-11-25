/* SPDX-License-Identifier: MIT OR X11
 *
 * PAL process backend: command line of a process, macOS (sysctl KERN_PROCARGS2). macOS is not an XPrism target;
 * this keeps the inherited XLibre macOS build working.
 * Moved from DetermineClientCmd() in os/client.c.
 */
#include <dix-config.h>

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/sysctl.h>

#include "os/pal/backend.h"
#include "os.h"

static void
get_argmax_from_kern(void *arg)
{
    int *argmax = arg;
    int mib[2];
    size_t len;

    mib[0] = CTL_KERN;
    mib[1] = KERN_ARGMAX;

    len = sizeof(int);
    if (sysctl(mib, 2, argmax, &len, NULL, 0) == -1) {
        ErrorF("Unable to dynamically determine kern.argmax, using ARG_MAX (%d)\n", ARG_MAX);
        *argmax = ARG_MAX;
    }
}

int
pal_backend_process_cmdline(pid_t pid, char **cmdname, char **cmdargs)
{
    {
        static int argmax;
        if (!argmax)
            get_argmax_from_kern(&argmax);

        int mib[3];
        size_t len = argmax;
        int32_t argc = -1;

        char * const procargs = calloc(1, len);
        if (!procargs) {
            ErrorF("Failed to allocate memory (%lu bytes) for KERN_PROCARGS2 result for pid %d: %s\n", len, pid, strerror(errno));
            return -1;
        }

        mib[0] = CTL_KERN;
        mib[1] = KERN_PROCARGS2;
        mib[2] = pid;

        if (sysctl(mib, 3, procargs, &len, NULL, 0) == -1) {
            ErrorF("Failed to determine KERN_PROCARGS2 for pid %d: %s\n", pid, strerror(errno));
            free(procargs);
            return -1;
        }

        if (len < sizeof(argc) || len > argmax) {
            ErrorF("Erroneous length returned when querying KERN_PROCARGS2 for pid %d: %zu\n", pid, len);
            free(procargs);
            return -1;
        }

        /* Ensure we have a failsafe NUL termination just in case the last entry
         * was not actually NUL terminated.
         */
        procargs[len-1] = '\0';

        /* Setup our iterator */
        char *is = procargs;

        /* The first element in the buffer is argc as a 32bit int. When using
         * the older KERN_PROCARGS, this is omitted, and one needs to guess
         * (usually by checking for an `=` character) when we start seeing
         * envvars instead of arguments.
         */
        argc = *(int32_t *)is;
        is += sizeof(argc);

        /* The very next string is the executable path.  Skip over it since
         * this function wants to return argv[0] and argv[1...n].
         */
        is += strlen(is) + 1;

        /* Skip over extra NUL characters to get to the start of argv[0] */
        for (; (is < &procargs[len]) && !(*is); is++);

        if (! (is < &procargs[len])) {
            ErrorF("Arguments were not returned when querying KERN_PROCARGS2 for pid %d: %zu\n", pid, len);
            free(procargs);
            return -1;
        }

        if (cmdname) {
            *cmdname = strdup(is);
        }

        /* Jump over argv[0] and point to argv[1] */
        is += strlen(is) + 1;

        if (cmdargs && is < &procargs[len]) {
            char *args = is;

            /* Remove the NUL terminators except the last one */
            for (int i = 1; i < argc - 1; i++) {
                /* Advance to the NUL terminator */
                is += strlen(is);

                /* Change the NUL to a space, ensuring we don't accidentally remove the terminal NUL */
                if (is < &procargs[len-1]) {
                    *is = ' ';
                }
            }

            *cmdargs = strdup(args);
        }

        free(procargs);
    }
    return 0;
}
