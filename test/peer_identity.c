/* SPDX-License-Identifier: MIT
 *
 * Tests for os/pal/identity.c: the identity of the process on the other
 * end of a local socket.
 */

/* Test relies on assert() */
#undef NDEBUG

#include <dix-config.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif

#include "os/pal/identity.h"
#include "tests-common.h"

/*
 * Fork a child that connects to a fresh listening socket and then waits
 * until told to exit. Returns the accepted server-side fd; *child_pid and
 * *release_fd (write a byte to let the child exit) are set for the caller.
 */
static int
connect_from_child(pid_t *child_pid, int *release_fd)
{
    char dir[] = "/tmp/peer-identity-XXXXXX";
    assert(mkdtemp(dir) != NULL);

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/sock", dir);

    int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(listener >= 0);
    assert(bind(listener, (struct sockaddr *) &addr, sizeof(addr)) == 0);
    assert(listen(listener, 1) == 0);

    int release[2];
    assert(pipe(release) == 0);

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(release[1]);
        int s = socket(AF_UNIX, SOCK_STREAM, 0);
        if (s < 0 || connect(s, (struct sockaddr *) &addr, sizeof(addr)) != 0)
            _exit(1);
        char c;
        (void) !read(release[0], &c, 1);
        _exit(0);
    }
    close(release[0]);

    int conn = accept(listener, NULL, NULL);
    assert(conn >= 0);
    close(listener);
    unlink(addr.sun_path);
    rmdir(dir);

    *child_pid = pid;
    *release_fd = release[1];
    return conn;
}

static void
finish_child(pid_t pid, int release_fd)
{
    int status;

    assert(write(release_fd, "x", 1) == 1);
    close(release_fd);
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void
peer_identity_init_is_empty(void)
{
    struct peer_identity id;

    peer_identity_init(&id);
    assert(id.fields == 0);
    assert(id.pidfd == -1);
    assert(id.suppGids == NULL);
    peer_identity_release(&id);   /* must be harmless on an empty identity */
}

static void
peer_identity_identifies_connecting_process(void)
{
    pid_t child;
    int release_fd;
    int conn = connect_from_child(&child, &release_fd);

    struct peer_identity id;
    assert(peer_identity_query(conn, &id) == 0);

    assert(id.fields & PEER_ID_UID);
    assert(id.uid == geteuid());
    assert(id.fields & PEER_ID_GID);
    assert(id.gid == getegid());

#if defined(__linux__) || defined(__OpenBSD__) || defined(__NetBSD__) || \
    defined(HAVE_XUCRED_CR_PID) || defined(HAVE_GETPEERUCRED)
    /* these platforms report the pid; it must be the child's, not ours */
    assert(id.fields & PEER_ID_PID);
    assert(id.pid == child);
#endif

    peer_identity_release(&id);
    assert(id.fields == 0 && id.pidfd == -1);

    finish_child(child, release_fd);
    close(conn);
}

#ifdef __linux__
static int
pidfd_send_signal_(int pidfd, int sig)
{
#ifdef SYS_pidfd_send_signal
    return syscall(SYS_pidfd_send_signal, pidfd, sig, NULL, 0);
#else
    errno = ENOSYS;
    return -1;
#endif
}

/*
 * The pidfd must keep referring to the process that connected, even after
 * it exits: then it reports the process as gone (ESRCH) instead of
 * silently pointing at whatever process gets the same pid next.
 */
static void
peer_identity_pidfd_tracks_exited_peer(void)
{
    pid_t child;
    int release_fd;
    int conn = connect_from_child(&child, &release_fd);

    struct peer_identity id;
    assert(peer_identity_query(conn, &id) == 0);

    if (!(id.fields & PEER_ID_PIDFD)) {
        /* kernel older than 6.5: no SO_PEERPIDFD, nothing to check */
        assert(id.pidfd == -1);
        printf("  (no SO_PEERPIDFD on this kernel, skipping pidfd checks)\n");
        peer_identity_release(&id);
        finish_child(child, release_fd);
        close(conn);
        return;
    }

    assert(id.pidfd >= 0);
    assert(fcntl(id.pidfd, F_GETFD) != -1);
    /* signal 0 only checks that the process is alive */
    assert(pidfd_send_signal_(id.pidfd, 0) == 0);

    finish_child(child, release_fd);
    close(conn);

    assert(pidfd_send_signal_(id.pidfd, 0) == -1);
    assert(errno == ESRCH);

    int pidfd = id.pidfd;
    peer_identity_release(&id);
    /* release must close the pidfd */
    assert(fcntl(pidfd, F_GETFD) == -1 && errno == EBADF);
}
#endif

static void
peer_identity_rejects_non_socket(void)
{
    int fds[2];
    struct peer_identity id;

    assert(pipe(fds) == 0);
    assert(peer_identity_query(fds[0], &id) == -1);
    assert(id.fields == 0 && id.pidfd == -1);
    close(fds[0]);
    close(fds[1]);
}

const testfunc_t*
peer_identity_test(void)
{
    static const testfunc_t testfuncs[] = {
        peer_identity_init_is_empty,
        peer_identity_identifies_connecting_process,
#ifdef __linux__
        peer_identity_pidfd_tracks_exited_peer,
#endif
        peer_identity_rejects_non_socket,
        NULL,
    };

    return testfuncs;
}
