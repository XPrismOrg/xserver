/* SPDX-License-Identifier: MIT
 *
 * Tests for the XPrism Platform Abstraction Layer (os/pal).
 */

/* Test relies on assert() */
#undef NDEBUG

#include <dix-config.h>

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include "os/pal/pal.h"
#include "tests-common.h"

static void
pal_platform_is_known(void)
{
    const char *name = pal_platform_name();
    assert(name && strcmp(name, "unknown") != 0);
#ifdef __linux__
    assert(strcmp(name, "linux") == 0);
    assert(strcmp(pal_platform_input_backend(), "libinput") == 0);
#endif
}

static void
check_grouping(const char *record, const char *engine, const char *app_id)
{
    char *e = (char *) 0x1, *a = (char *) 0x1;

    pal_process_parse_grouping(record, &e, &a);
    if (!engine) {
        assert(e == NULL && a == NULL);
        return;
    }
    if (!e || strcmp(e, engine) != 0 || !a || strcmp(a, app_id) != 0) {
        fprintf(stderr, "record %s: got %s/%s, want %s/%s\n", record,
                e ? e : "(null)", a ? a : "(null)", engine, app_id);
        assert(0);
    }
    free(e);
    free(a);
}

static void
pal_grouping_hints(void)
{
#ifdef __linux__
    /* cgroup v2, Flatpak */
    check_grouping("0::/user.slice/user-1000.slice/user@1000.service/app.slice/"
                   "app-flatpak-org.mozilla.firefox-12345.scope\n",
                   "flatpak", "org.mozilla.firefox");
    /* systemd escapes '-' in the app id */
    check_grouping("0::/user.slice/user-1000.slice/user@1000.service/app.slice/"
                   "app-flatpak-com.example.my\\x2dapp-7.scope\n",
                   "flatpak", "com.example.my-app");
    /* Snap, with and without a sub-cgroup below the scope */
    check_grouping("0::/user.slice/user-1000.slice/user@1000.service/app.slice/"
                   "snap.firefox.firefox-0b0b5d1e-7f0a-4a1e-9d0c-1f2e3d4c5b6a.scope\n",
                   "snap", "firefox");
    check_grouping("0::/user.slice/snap.spotify.spotify.1a2b3c.scope/sub\n",
                   "snap", "spotify");
    /* cgroup v1: several lines, the hint on a later one */
    check_grouping("12:pids:/user.slice/user-1000.slice\n"
                   "1:name=systemd:/user.slice/user-1000.slice/user@1000.service/"
                   "app-flatpak-org.gnome.Maps-99.scope\n",
                   "flatpak", "org.gnome.Maps");

    /* not a sandbox: plain session, a desktop launcher's scope */
    check_grouping("0::/user.slice/user-1000.slice/session-2.scope\n", NULL, NULL);
    check_grouping("0::/user.slice/user-1000.slice/user@1000.service/app.slice/"
                   "app-gnome-org.gnome.Terminal-4242.scope\n", NULL, NULL);
    /* malformed */
    check_grouping("0::/app.slice/app-flatpak-org.example.Foo.scope\n", NULL, NULL);
    check_grouping("0::/app.slice/app-flatpak--1.scope\n", NULL, NULL);
    check_grouping("garbage", NULL, NULL);
    check_grouping("", NULL, NULL);
    check_grouping(NULL, NULL, NULL);
#else
    /* other platforms have no grouping hint source yet */
    check_grouping("0::/app-flatpak-org.example.Foo-1.scope\n", NULL, NULL);
#endif
}

static void
pal_process_describes_peer(void)
{
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    struct peer_identity id;
    if (peer_identity_query(sv[0], &id) != 0 || !(id.fields & PEER_ID_PID)) {
        printf("  (no peer pid on this platform, skipping)\n");
        close(sv[0]);
        close(sv[1]);
        return;
    }
    assert(id.pid == getpid());

    struct pal_process_info info;
    assert(pal_process_describe(&id, &info) == 0);
    /* the peer is this test program */
    assert(info.cmd_name && strstr(info.cmd_name, "tests"));
    /* tests don't run inside Flatpak or Snap */
    assert(info.sandbox_engine == NULL && info.sandbox_app_id == NULL);
    if (id.fields & PEER_ID_PIDFD)
        assert(info.pid_confirmed);

    pal_process_release(&info);
    assert(info.cmd_name == NULL && !info.pid_confirmed);

    /* no pid: nothing to describe */
    struct peer_identity empty;
    peer_identity_init(&empty);
    assert(pal_process_describe(&empty, &info) == -ESRCH);

    peer_identity_release(&id);
    close(sv[0]);
    close(sv[1]);
}

static void
pal_cmdline_of_processes(void)
{
    char *name = (char *) 0x1, *args = (char *) 0x1;

    /* this test program, started as ".../tests" */
    pal_process_cmdline(getpid(), &name, &args);
    assert(name && strstr(name, "tests"));
    free(name);
    free(args);

    /* only one of them */
    name = (char *) 0x1;
    pal_process_cmdline(getpid(), &name, NULL);
    assert(name && strstr(name, "tests"));
    free(name);

    /* no such process, no pid */
    name = args = (char *) 0x1;
    pal_process_cmdline(0x7ffffff0, &name, &args);
    assert(name == NULL && args == NULL);
    name = args = (char *) 0x1;
    pal_process_cmdline(0, &name, &args);
    assert(name == NULL && args == NULL);

    /* a child with known arguments */
    int pipefd[2];
    assert(pipe(pipefd) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(pipefd[1]);
        dup2(pipefd[0], 0);    /* "read x" blocks on the pipe */
        execl("/bin/sh", "/bin/sh", "-c", "read x", "xprism-arg", (char *) NULL);
        _exit(127);
    }
    close(pipefd[0]);
    /* wait until it has exec'ed: the name changes from "tests" */
    for (int i = 0; i < 200; i++) {
        pal_process_cmdline(child, &name, &args);
        if (name && !strstr(name, "tests"))
            break;
        free(name);
        free(args);
        name = args = NULL;
        usleep(10000);
    }
    assert(name && strcmp(name, "/bin/sh") == 0);
    assert(args && strcmp(args, "-c read x xprism-arg") == 0);
    free(name);
    free(args);
    close(pipefd[1]);       /* "read x" sees EOF and exits */
    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
}

static void
pal_stub_modules_report_unsupported(void)
{
    /* until the real backends are written */
    struct pal_session *s = (struct pal_session *) 0x1;
    assert(pal_session_open(NULL, NULL, &s) == -ENOTSUP && s == NULL);
    struct pal_device_monitor *m = (struct pal_device_monitor *) 0x1;
    assert(pal_device_monitor_start(NULL, NULL, NULL, &m) == -ENOTSUP && m == NULL);
    assert(pal_device_enumerate(PAL_DEVICE_GPU, NULL, NULL) == -ENOTSUP);
    assert(pal_restrict_self(PAL_RESTRICT_RUNNING) == 0);
}

const testfunc_t*
pal_test(void)
{
    static const testfunc_t testfuncs[] = {
        pal_platform_is_known,
        pal_grouping_hints,
        pal_process_describes_peer,
        pal_cmdline_of_processes,
        pal_stub_modules_report_unsupported,
        NULL,
    };

    return testfuncs;
}
