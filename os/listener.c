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

/*
 * Where getsockopt can't tell (DragonFly answers ENOPROTOOPT), ask accept:
 * on a listening stream socket without a waiting connection it fails with
 * EAGAIN, on anything else with EINVAL or EOPNOTSUPP. A connection that
 * was waiting is handed back in *pending, to be served, not dropped.
 */
static const char *
probe_listening(int fd, int *pending)
{
    int flags = fcntl(fd, F_GETFL);

    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return strerror(errno);
    int conn = accept(fd, NULL, NULL);
    int err = errno;
    fcntl(fd, F_SETFL, flags);
    if (conn >= 0) {
        *pending = conn;
        return NULL;
    }
    if (err == EAGAIN || err == EWOULDBLOCK || err == ECONNABORTED || err == EINTR)
        return NULL;
    return "not a listening stream socket";
}

/* NULL if fd is a listening Unix stream socket, else what it isn't */
static const char *
not_a_listening_unix_stream_socket(int fd, int *pending)
{
    static char why[96];
    int val;
    socklen_t len;
    struct sockaddr_storage addr;
    Bool probe = FALSE;

    *pending = -1;
    len = sizeof(val);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &val, &len) != 0) {
        if (errno != ENOPROTOOPT)
            goto failed_call;
        probe = TRUE;
    } else if (val != SOCK_STREAM)
        return "not a stream socket";

#ifdef SO_ACCEPTCONN
    len = sizeof(val);
    if (getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &val, &len) != 0) {
        if (errno != ENOPROTOOPT)
            goto failed_call;
        probe = TRUE;
    } else if (!val)
        return "not listening";
#endif

    len = sizeof(addr);
    memset(&addr, 0, sizeof(addr));
    if (getsockname(fd, (struct sockaddr *) &addr, &len) != 0)
        goto failed_call;
    if (addr.ss_family != AF_UNIX)
        return "not a Unix socket";

    return probe ? probe_listening(fd, pending) : NULL;

failed_call:
    snprintf(why, sizeof(why), "getsockopt/getsockname: %s", strerror(errno));
    return why;
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
    int pending;
    const char *why = not_a_listening_unix_stream_socket(listen_fd, &pending);
    if (why) {
        LogMessageVerb(X_WARNING, 0, "labeled listener: fd %d refused: %s\n", listen_fd, why);
        *err = BadValue;
        return NULL;
    }

    struct labeled_listener *l = calloc(1, sizeof(*l));
    if (!l) {
        if (pending >= 0)
            close(pending);
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
        if (pending >= 0)
            close(pending);
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
            if (pending >= 0)
                close(pending);
            *err = BadAlloc;
            return NULL;
        }
        ospoll_listen(server_poll, close_fd, X_NOTIFY_READ);
    }

    /* a client the check accepted is served like any other */
    if (pending >= 0) {
        fcntl(pending, F_SETFD, FD_CLOEXEC);
        if (!AddLabeledClientOnFD(pending, label))
            LogMessage(X_WARNING, "labeled listener: could not add client\n");
    }

    return l;
}

void
labeled_listener_remove(struct labeled_listener *l)
{
    if (l)
        listener_stop(l);
}
