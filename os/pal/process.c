/* SPDX-License-Identifier: MIT OR X11
 *
 * PAL process module, platform independent part. See pal.h.
 */
#include <dix-config.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "os/client_priv.h"
#include "os/pal/backend.h"
#include "os/pal/pal.h"

int
pal_process_describe(const struct peer_identity *id, struct pal_process_info *info)
{
    memset(info, 0, sizeof(*info));
    if (!id || !(id->fields & PEER_ID_PID))
        return -ESRCH;

    const char *name = NULL, *args = NULL;
    DetermineClientCmd(id->pid, &name, &args);  /* platform specific, os/client.c */
    info->cmd_name = (char *) name;
    info->cmd_args = (char *) args;

    char *record = pal_backend_process_grouping(id->pid);
    if (record) {
        pal_process_parse_grouping(record, &info->sandbox_engine, &info->sandbox_app_id);
        free(record);
    }

    /* everything above was read by pid; if the pidfd says the process
       is still alive, the pid can't have been reused in between */
    if ((id->fields & PEER_ID_PIDFD) && pal_backend_pidfd_alive(id->pidfd))
        info->pid_confirmed = true;

    return 0;
}

void
pal_process_release(struct pal_process_info *info)
{
    free(info->cmd_name);
    free(info->cmd_args);
    free(info->sandbox_engine);
    free(info->sandbox_app_id);
    memset(info, 0, sizeof(*info));
}
