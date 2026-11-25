/* SPDX-License-Identifier: MIT OR X11
 *
 * Platform abstraction for the identity of a locally connected client.
 * See peer_identity.h.
 *
 * Per platform:
 *   Solaris/illumos  getpeerucred()      uid, gid, pid, zone, groups
 *   Linux            SO_PEERCRED         uid, gid, pid
 *                    SO_PEERPIDFD        pidfd (Linux >= 6.5)
 *   OpenBSD          SO_PEERCRED         uid, gid, pid
 *   FreeBSD, macOS,  LOCAL_PEERCRED      uid, gid (+ pid where xucred has
 *   DragonFly                            cr_pid, e.g. FreeBSD >= 13)
 *   NetBSD           LOCAL_PEEREID       uid, gid, pid
 *   others           getpeereid()        uid, gid (+ LOCAL_PEERPID on macOS)
 *
 * The code used to live in GetLocalClientCreds() (os/access.c), which
 * queried the socket every time it was called.
 */
#include <dix-config.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

#ifndef WIN32
#include <sys/socket.h>
#include <sys/un.h>
#endif

#ifdef HAVE_GETPEERUCRED
#include <ucred.h>
#endif

#ifdef HAVE_SYS_UCRED_H
#include <sys/ucred.h>
#endif

#include "os/peer_identity.h"

#if defined(__linux__) && !defined(SO_PEERPIDFD)
#define SO_PEERPIDFD 77     /* Linux 6.5; older libc headers lack it */
#endif

void
peer_identity_init(struct peer_identity *id)
{
    memset(id, 0, sizeof(*id));
    id->pidfd = -1;
}

void
peer_identity_release(struct peer_identity *id)
{
    if (id->pidfd >= 0)
        close(id->pidfd);
    free(id->suppGids);
    peer_identity_init(id);
}

#if defined(__linux__)
static void
query_pidfd(int fd, struct peer_identity *id)
{
    int pidfd = -1;
    socklen_t len = sizeof(pidfd);

    /* ENOPROTOOPT on kernels older than 6.5: just no pidfd then */
    if (getsockopt(fd, SOL_SOCKET, SO_PEERPIDFD, &pidfd, &len) == 0 &&
        pidfd >= 0) {
        id->pidfd = pidfd;
        id->fields |= PEER_ID_PIDFD;
    }
}
#endif

int
peer_identity_query(int fd, struct peer_identity *id)
{
    peer_identity_init(id);

#if defined(HAVE_GETPEERUCRED)
    ucred_t *peercred = NULL;
    const gid_t *gids;

    if (getpeerucred(fd, &peercred) < 0)
        return -1;
    id->uid = ucred_geteuid(peercred);
    if (id->uid != (uid_t) -1)
        id->fields |= PEER_ID_UID;
    id->gid = ucred_getegid(peercred);
    if (id->gid != (gid_t) -1)
        id->fields |= PEER_ID_GID;
    id->pid = ucred_getpid(peercred);
    if (id->pid != -1)
        id->fields |= PEER_ID_PID;
#ifdef HAVE_GETZONEID
    id->zoneid = ucred_getzoneid(peercred);
    if (id->zoneid != -1)
        id->fields |= PEER_ID_ZONE;
#endif
    int n = ucred_getgroups(peercred, &gids);
    if (n > 0) {
        id->suppGids = calloc(n, sizeof(gid_t));
        if (id->suppGids) {
            memcpy(id->suppGids, gids, n * sizeof(gid_t));
            id->nSuppGids = n;
        }
    }
    ucred_free(peercred);
    return 0;

#elif defined(SO_PEERCRED)
#ifdef __OpenBSD__
    struct sockpeercred peercred;
#else
    struct ucred peercred;
#endif
    socklen_t len = sizeof(peercred);

    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peercred, &len) == -1)
        return -1;
    id->uid = peercred.uid;
    id->gid = peercred.gid;
    id->pid = peercred.pid;
    id->fields = PEER_ID_UID | PEER_ID_GID | PEER_ID_PID;
#if defined(__linux__)
    query_pidfd(fd, id);
#endif
    return 0;

#elif defined(LOCAL_PEERCRED) && defined(HAVE_XUCRED_CR_PID)
    struct xucred peercred;
    socklen_t len = sizeof(peercred);

    if (getsockopt(fd, SOL_LOCAL, LOCAL_PEERCRED, &peercred, &len) != 0 ||
        peercred.cr_version != XUCRED_VERSION)
        return -1;
    id->uid = peercred.cr_uid;
    id->gid = peercred.cr_gid;
    id->pid = peercred.cr_pid;
    id->fields = PEER_ID_UID | PEER_ID_GID | PEER_ID_PID;
    return 0;

#elif defined(LOCAL_PEEREID)
    /* NetBSD */
    struct unpcbid peereid;
    socklen_t len = sizeof(peereid);

    if (getsockopt(fd, 0, LOCAL_PEEREID, &peereid, &len) != 0)
        return -1;
    id->uid = peereid.unp_euid;
    id->gid = peereid.unp_egid;
    id->pid = peereid.unp_pid;
    id->fields = PEER_ID_UID | PEER_ID_GID | PEER_ID_PID;
    return 0;

#elif defined(HAVE_GETPEEREID)
    uid_t uid;
    gid_t gid;

    if (getpeereid(fd, &uid, &gid) == -1)
        return -1;
    id->uid = uid;
    id->gid = gid;
    id->fields = PEER_ID_UID | PEER_ID_GID;
#if defined(LOCAL_PEERPID)
    pid_t pid;
    socklen_t len = sizeof(pid);

    if (getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &pid, &len) == 0) {
        id->pid = pid;
        id->fields |= PEER_ID_PID;
    }
#endif
    return 0;

#else
    /* No way to learn the peer's credentials on this platform */
    (void) fd;
    return -1;
#endif
}
