/* SPDX-License-Identifier: MIT OR X11
 *
 * Labeled listeners, see listener_priv.h.
 */
#include <dix-config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <X11/X.h>

#include "include/list.h"
#include "os/listener_priv.h"
#include "os/log_priv.h"
#include "os/osdep.h"
#include "os/ospoll.h"

struct labeled_listener {
    int listen_fd;
    int close_fd;
    void *label;
    labeled_listener_closed_proc on_closed;
};

static Bool
is_listening_unix_stream_socket(int fd)
{
    int val;
    socklen_t len;
    struct sockaddr_storage addr;

    len = sizeof(val);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &val, &len) != 0 || val != SOCK_STREAM)
        return FALSE;

#ifdef SO_ACCEPTCONN
    len = sizeof(val);
    if (getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &val, &len) != 0 || !val)
        return FALSE;
#endif

    len = sizeof(addr);
    if (getsockname(fd, (struct sockaddr *) &addr, &len) != 0 ||
        addr.ss_family != AF_UNIX)
        return FALSE;

    return TRUE;
}

static void
listener_stop(struct labeled_listener *l)
{
    ospoll_remove(server_poll, l->listen_fd);
    close(l->listen_fd);
    if (l->close_fd >= 0) {
        ospoll_remove(server_poll, l->close_fd);
        close(l->close_fd);
    }
    free(l);
}

static void
listener_closed(struct labeled_listener *l)
{
    if (l->on_closed)
        l->on_closed(l, l->label);
    listener_stop(l);
}

static void
listener_close_fd_ready(int fd, int xevents, void *data)
{
    /* the launcher closed its end (hang-up) or wrote to it: either way
     * it wants the socket gone */
    listener_closed(data);
}

static void
listener_accept_ready(int fd, int xevents, void *data)
{
    struct labeled_listener *l = data;

    if (xevents & X_NOTIFY_ERROR) {
        LogMessage(X_WARNING, "labeled listener fd %d failed, closing it\n", fd);
        listener_closed(l);
        return;
    }

    int conn = accept(fd, NULL, NULL);
    if (conn < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR &&
            errno != ECONNABORTED)
            LogMessage(X_WARNING, "labeled listener: accept failed: %s\n",
                       strerror(errno));
        return;
    }
    fcntl(conn, F_SETFD, FD_CLOEXEC);

    /* on failure the client was already told (or the fd closed) by
     * ErrorConnMax(); nothing left to do here */
    if (!AddLabeledClientOnFD(conn, l->label))
        LogMessage(X_WARNING, "labeled listener: could not add client\n");
}

struct labeled_listener *
labeled_listener_add(int listen_fd, int close_fd, void *label,
                     labeled_listener_closed_proc on_closed, int *err)
{
    if (!is_listening_unix_stream_socket(listen_fd)) {
        *err = BadValue;
        return NULL;
    }

    struct labeled_listener *l = calloc(1, sizeof(*l));
    if (!l) {
        *err = BadAlloc;
        return NULL;
    }
    l->listen_fd = listen_fd;
    l->close_fd = close_fd;
    l->label = label;
    l->on_closed = on_closed;

    fcntl(listen_fd, F_SETFD, FD_CLOEXEC);
    fcntl(listen_fd, F_SETFL, fcntl(listen_fd, F_GETFL) | O_NONBLOCK);

    if (!ospoll_add(server_poll, listen_fd, ospoll_trigger_level,
                    listener_accept_ready, l)) {
        free(l);
        *err = BadAlloc;
        return NULL;
    }
    ospoll_listen(server_poll, listen_fd, X_NOTIFY_READ);

    if (close_fd >= 0) {
        fcntl(close_fd, F_SETFD, FD_CLOEXEC);
        if (!ospoll_add(server_poll, close_fd, ospoll_trigger_level,
                        listener_close_fd_ready, l)) {
            ospoll_remove(server_poll, listen_fd);
            free(l);
            *err = BadAlloc;
            return NULL;
        }
        ospoll_listen(server_poll, close_fd, X_NOTIFY_READ);
    }

    return l;
}

void
labeled_listener_remove(struct labeled_listener *l)
{
    if (l)
        listener_stop(l);
}
