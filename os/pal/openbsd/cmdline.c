/* SPDX-License-Identifier: MIT OR X11
 *
 * PAL process backend: command line of a process, OpenBSD (kvm_getargv).
 * Moved from DetermineClientCmd() in os/client.c.
 */
#include <dix-config.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/types.h>

#include <kvm.h>

#include "os/pal/backend.h"
#include "os.h"

int
pal_backend_process_cmdline(pid_t pid, char **cmdname, char **cmdargs)
{
    {
        kvm_t *kd;
        char errbuf[_POSIX2_LINE_MAX];
        char **argv;
        struct kinfo_proc *kp;
        size_t len = 0;
        int i, n;

        kd = kvm_open(NULL, NULL, NULL, KVM_NO_FILES, errbuf);
        if (kd == NULL)
            return -1;
        kp = kvm_getprocs(kd, KERN_PROC_PID, pid, sizeof(struct kinfo_proc),
                          &n);
        if (n != 1)
            goto done_kvm;
        argv = kvm_getargv(kd, kp, 0);
        if (argv == NULL)
            goto done_kvm;
        if (cmdname) {
            if (argv[0] == NULL)
                goto done_kvm;
            else
                *cmdname = strdup(argv[0]);
        }
        if (cmdargs) {
            i = 1;
            while (argv[i] != NULL) {
                len += strlen(argv[i]) + 1;
                i++;
            }
            *cmdargs = calloc(1, len);
            if (*cmdargs) {
                i = 1;
                while (argv[i] != NULL) {
                    strlcat(*cmdargs, argv[i], len);
                    strlcat(*cmdargs, " ", len);
                    i++;
                }
            }
        }
 done_kvm:
        kvm_close(kd);
    }
    return 0;
}
