/* SPDX-License-Identifier: MIT OR X11
 *
 * Platform abstraction for the identity of a locally connected client.
 *
 * The identity is read from the kernel once, when the client connects,
 * and stored with the connection. Code that needs to know who a client
 * is (access control, auditing, the security policy) reads that stored
 * copy instead of querying the socket again, so the answer can't change
 * over the lifetime of the connection.
 */
#ifndef XSERVER_OS_PEER_IDENTITY_H
#define XSERVER_OS_PEER_IDENTITY_H

#include <sys/types.h>

#define PEER_ID_UID     (1u << 0)   /* uid is valid */
#define PEER_ID_GID     (1u << 1)   /* gid is valid */
#define PEER_ID_PID     (1u << 2)   /* pid is valid */
#define PEER_ID_ZONE    (1u << 3)   /* zoneid is valid (Solaris) */
#define PEER_ID_PIDFD   (1u << 4)   /* pidfd refers to the peer process */

struct peer_identity {
    unsigned int fields;    /* PEER_ID_* bits for the members below */
    uid_t uid;              /* effective uid */
    gid_t gid;              /* effective primary gid */
    pid_t pid;              /* process id at connect time */
    int zoneid;             /* Solaris zone */
    /*
     * Linux: a pidfd for the peer process (SO_PEERPIDFD, Linux >= 6.5).
     * Unlike the pid it can't be recycled: once the process exits, the
     * pidfd keeps referring to the dead process instead of to whatever
     * process gets the same pid next. -1 when unavailable.
     */
    int pidfd;
    int nSuppGids;          /* supplementary groups (Solaris only) */
    gid_t *suppGids;
};

/**
 * Read the identity of the peer of a connected local (Unix domain) socket.
 *
 * On success, @p id is filled in and must later be released with
 * peer_identity_release(). Fields the platform can't provide are left
 * unset in id->fields.
 *
 * @return 0 on success, -1 if the platform has no way to identify the peer
 *         or the call failed (then @p id is left empty, pidfd = -1).
 */
int peer_identity_query(int fd, struct peer_identity *id);

/** Free what peer_identity_query() allocated and close the pidfd. */
void peer_identity_release(struct peer_identity *id);

/** An empty identity: no fields set, pidfd = -1. */
void peer_identity_init(struct peer_identity *id);

struct _Client;
/**
 * The identity recorded when @p client connected, for the security layer.
 * Never NULL for a real client; fields == 0 if nothing is known (remote
 * client, or the platform can't tell). Valid for the client's lifetime.
 */
const struct peer_identity *ClientPeerIdentity(struct _Client *client);

#endif /* XSERVER_OS_PEER_IDENTITY_H */
