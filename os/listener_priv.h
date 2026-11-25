/* SPDX-License-Identifier: MIT OR X11
 *
 * Labeled listeners: extra listening sockets handed to the server at
 * runtime, whose clients carry a label chosen by whoever added the socket.
 *
 * This is how a trusted launcher (sandbox engine, session manager) gives
 * an application its own way into the server: it creates and binds a Unix
 * socket itself, makes it reachable only from inside the sandbox, and
 * passes the listening fd to the server. Every client that connects
 * through that socket is labeled, so the security policy can place it
 * (e.g. into an X namespace) without trusting anything the client sends.
 *
 * The launcher also passes a "close fd": when the launcher closes its end
 * (or dies), the server stops listening on the socket. Clients that are
 * already connected stay connected.
 */
#ifndef XSERVER_OS_LISTENER_PRIV_H
#define XSERVER_OS_LISTENER_PRIV_H

#include "include/dix.h"   /* ClientPtr */

struct labeled_listener;

/**
 * Called when a listener stops by itself (its close fd hung up, or the
 * listening socket failed). The listener is freed right after the call
 * returns; the callback must not call labeled_listener_remove() on it.
 */
typedef void (*labeled_listener_closed_proc)(struct labeled_listener *l,
                                             void *label);

/**
 * Start accepting clients on @p listen_fd.
 *
 * @param listen_fd  a listening SOCK_STREAM Unix domain socket
 * @param close_fd   fd whose hang-up stops the listener (e.g. read end of
 *                   a pipe the launcher keeps open), or -1 for none
 * @param label      opaque pointer attached to every accepted client, see
 *                   ClientListenerLabel(); only valid while the listener
 *                   exists
 * @param on_closed  optional, see labeled_listener_closed_proc
 * @param[out] err   on failure: BadValue if @p listen_fd isn't a listening
 *                   Unix stream socket, BadAlloc otherwise
 *
 * On success the listener owns both fds. On failure the caller still owns
 * them.
 */
struct labeled_listener *labeled_listener_add(int listen_fd, int close_fd,
                                              void *label,
                                              labeled_listener_closed_proc on_closed,
                                              int *err);

/** Stop listening and close both fds. Does not call on_closed. */
void labeled_listener_remove(struct labeled_listener *l);

/**
 * The label of the listener a client connected through, or NULL if it
 * came in through a regular listening socket. Only meaningful during the
 * ClientStateInitial callback: afterwards the listener may already be gone.
 */
void *ClientListenerLabel(ClientPtr client);

/* os/connection.c: set up a client on an fd accepted by a labeled listener */
Bool AddLabeledClientOnFD(int fd, void *label);

#endif /* XSERVER_OS_LISTENER_PRIV_H */
