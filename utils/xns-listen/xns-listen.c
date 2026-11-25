/* SPDX-License-Identifier: MIT OR X11
 *
 * xns-listen: give an application its own socket into an X namespace.
 *
 * For sandbox launchers (Flatpak, bubblewrap, firejail, session managers).
 * Creates a Unix socket, hands it to the X server with X-NAMESPACE
 * AddListener, and runs a command (typically the sandbox launcher, which
 * makes the socket visible inside the sandbox as its X display). Every
 * client that connects through the socket is placed in the namespace,
 * whatever authorization it presents.
 *
 * When the command exits (or, without a command, on SIGINT/SIGTERM/SIGHUP)
 * the socket stops accepting clients and is removed; with --create, the
 * namespace is deleted as well, disconnecting anything still in it.
 *
 * xns-listen itself must be able to use X-NAMESPACE, i.e. connect with
 * the root namespace's authorization (the usual $XAUTHORITY of the
 * session). The sandboxed app must of course not get that.
 */
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/wait.h>

#include <xcb/xcb.h>
#include <xcb/xcbext.h>

/* from Xext/namespace/namespaceproto.h; duplicated so this tool builds
   without the server tree's include paths */
#define XNS_EXTENSION_NAME          "X-NAMESPACE"
#define X_XnsQueryVersion           0
#define X_XnsCreateNamespace        2
#define X_XnsDeleteNamespace        3
#define X_XnsAddListener            10
#define X_XnsSetNamespaceGrants     13
#define XNS_NO_CEILING              0xffffffffu
#define XNS_DELETE_KILL_CLIENTS     1
#define XNS_APP_STRING_MAX          255

static const struct {
    const char *name;
    uint32_t bit;
} capabilities[] = {
    { "mouse-motion", 1u << 0 },
    { "shape",        1u << 1 },
    { "transparency", 1u << 2 },
    { "xinput",       1u << 3 },
    { "xkeyboard",    1u << 4 },
    /* deliberately no "admin": a socket must never lead into one */
};

/* XPrism capabilities, in protocol bit order (XNS_GRANT_*) */
static const char *const grant_names[] = {
    "capture.screen", "capture.window", "input.inject", "input.monitor",
    "input.virtual_seat", "window.enumerate", "window.inspect",
    "window.manage", "clipboard.read", "clipboard.write", "display.configure",
};

static const char *progname = "xns-listen";

static void
usage(FILE *f)
{
    fprintf(f,
"Usage: %s [options] SOCKET [-- COMMAND [ARG...]]\n"
"\n"
"Create the Unix socket SOCKET and bind it to an X namespace: every X\n"
"client that connects through it is placed in that namespace. Runs\n"
"COMMAND (with XNS_SOCKET set to SOCKET) and cleans up when it exits;\n"
"without COMMAND, runs until SIGINT/SIGTERM/SIGHUP.\n"
"\n"
"  -d, --display DISPLAY     X server to use (default: $DISPLAY)\n"
"  -n, --namespace NAME      namespace for clients on SOCKET (required)\n"
"  -c, --create              create the namespace, delete it on exit\n"
"  -a, --allow CAP[,CAP...]  with --create: capabilities to grant\n"
"                            (mouse-motion, shape, transparency, xinput,\n"
"                            xkeyboard); default: none\n"
"  -g, --grant CAP[,CAP...]  with --create: XPrism capabilities for the\n"
"                            namespace, e.g. window.inspect,clipboard.read\n"
"      --ceiling CAP[,CAP...]  most capabilities clients on SOCKET may ever\n"
"                            hold, whatever the namespace is granted;\n"
"                            \"none\" for none; default: no limit\n"
"      --app-id ID           app id, e.g. org.example.Editor\n"
"      --instance-id ID      instance id, e.g. a Flatpak instance\n"
"      --engine NAME         sandbox engine, e.g. flatpak, bubblewrap\n"
"  -h, --help                show this help\n"
"\n"
"Prints \"ready\" on stdout once SOCKET accepts clients.\n"
"Exit status: COMMAND's, or 0 when stopped by a signal, or 1 on error.\n",
            progname);
}

/* ------------------------------------------------------------------ */

static xcb_connection_t *conn;
static uint8_t xns_opcode;

static size_t
pad4(size_t n)
{
    return (n + 3) & ~(size_t) 3;
}

/*
 * Send one X-NAMESPACE request (header + body built by the caller, body
 * already padded) with optional fds, and wait for its reply. Returns the
 * reply (free() it) or NULL; on X error *xerr is set to the error code.
 * xcb closes the fds once sent.
 */
static void *
xns_request(uint8_t minor, const void *body, size_t body_len,
            int *fds, unsigned nfds, int *xerr)
{
    size_t len = 4 + body_len;
    uint8_t *req = calloc(1, len);
    if (!req) {
        *xerr = -1;
        return NULL;
    }
    req[0] = xns_opcode;
    req[1] = minor;
    uint16_t words = (uint16_t) (len / 4);
    memcpy(req + 2, &words, 2);         /* native byte order, like xcb */
    memcpy(req + 4, body, body_len);

    struct iovec iov[3] = { { 0 }, { 0 }, { .iov_base = req, .iov_len = len } };
    xcb_protocol_request_t xreq = {
        .count = 1, .ext = NULL, .opcode = minor, .isvoid = 0,
    };
    unsigned seq = xcb_send_request_with_fds(conn, XCB_REQUEST_CHECKED | XCB_REQUEST_RAW,
                                             &iov[2], &xreq, nfds, fds);
    free(req);

    xcb_generic_error_t *err = NULL;
    void *reply = xcb_wait_for_reply(conn, seq, &err);
    *xerr = 0;
    if (err) {
        *xerr = err->error_code;
        free(err);
        free(reply);
        return NULL;
    }
    if (!reply)
        *xerr = -1;     /* connection lost */
    return reply;
}

/* append a padded string to buf at *off */
static void
put_str(uint8_t *buf, size_t *off, const char *s, size_t n)
{
    memcpy(buf + *off, s, n);
    *off += pad4(n);
}

static const char *
xerr_name(int e)
{
    switch (e) {
    case 2:  return "BadValue";
    case 10: return "BadAccess";
    case 11: return "BadAlloc";
    case 15: return "BadName";
    case 16: return "BadLength";
    case -1: return "connection lost";
    default: return "X error";
    }
}

static int
xns_init(void)
{
    const xcb_query_extension_reply_t *ext =
        xcb_get_extension_data(conn, &(xcb_extension_t) { XNS_EXTENSION_NAME, 0 });
    if (!ext || !ext->present) {
        fprintf(stderr, "%s: the X server does not offer %s (not started with "
                "-namespace, or this connection is not in the root namespace)\n",
                progname, XNS_EXTENSION_NAME);
        return -1;
    }
    xns_opcode = ext->major_opcode;

    uint16_t ver[2] = { 1, 1 };
    int xerr;
    uint8_t *r = xns_request(X_XnsQueryVersion, ver, sizeof(ver), NULL, 0, &xerr);
    if (!r) {
        fprintf(stderr, "%s: QueryVersion failed: %s\n", progname, xerr_name(xerr));
        return -1;
    }
    uint16_t major, minor;
    memcpy(&major, r + 8, 2);
    memcpy(&minor, r + 10, 2);
    free(r);
    if (major < 1 || (major == 1 && minor < 1)) {
        fprintf(stderr, "%s: X server speaks %s %u.%u, need 1.1\n",
                progname, XNS_EXTENSION_NAME, major, minor);
        return -1;
    }
    return 0;
}

static int
xns_create(const char *name, uint32_t caps)
{
    size_t n = strlen(name);
    uint8_t body[12 + 256] = { 0 };
    uint32_t attrs = 0;
    uint16_t nlen = (uint16_t) n;
    memcpy(body, &caps, 4);
    memcpy(body + 4, &attrs, 4);
    memcpy(body + 8, &nlen, 2);
    size_t off = 12;
    put_str(body, &off, name, n);

    int xerr;
    void *r = xns_request(X_XnsCreateNamespace, body, off, NULL, 0, &xerr);
    if (!r) {
        fprintf(stderr, "%s: cannot create namespace \"%s\": %s%s\n", progname, name,
                xerr_name(xerr), xerr == 15 ? " (exists already, or invalid name)" : "");
        return -1;
    }
    free(r);
    return 0;
}

static void
xns_delete(const char *name)
{
    size_t n = strlen(name);
    uint8_t body[4 + 256] = { 0 };
    uint16_t nlen = (uint16_t) n;
    body[0] = XNS_DELETE_KILL_CLIENTS;
    memcpy(body + 2, &nlen, 2);
    size_t off = 4;
    put_str(body, &off, name, n);

    int xerr;
    void *r = xns_request(X_XnsDeleteNamespace, body, off, NULL, 0, &xerr);
    if (!r)
        fprintf(stderr, "%s: cannot delete namespace \"%s\": %s\n", progname, name,
                xerr_name(xerr));
    free(r);
}

static int
xns_set_grants(const char *name, uint32_t grants)
{
    size_t n = strlen(name);
    uint8_t body[8 + 256] = { 0 };
    uint16_t nlen = (uint16_t) n;
    memcpy(body, &grants, 4);
    memcpy(body + 4, &nlen, 2);
    size_t off = 8;
    put_str(body, &off, name, n);

    int xerr;
    void *r = xns_request(X_XnsSetNamespaceGrants, body, off, NULL, 0, &xerr);
    if (!r) {
        fprintf(stderr, "%s: cannot grant capabilities to \"%s\": %s\n",
                progname, name, xerr_name(xerr));
        return -1;
    }
    free(r);
    return 0;
}

static int
xns_add_listener(const char *name, const char *app_id, const char *instance_id,
                 const char *engine, uint32_t ceiling, int listen_fd, int close_fd)
{
    const char *strs[4] = { name, app_id, instance_id, engine };
    size_t lens[4], total = 12;
    for (int i = 0; i < 4; i++) {
        lens[i] = strlen(strs[i]);
        total += pad4(lens[i]);
    }

    uint8_t *body = calloc(1, total);
    if (!body)
        return -1;
    for (int i = 0; i < 4; i++) {
        uint16_t l = (uint16_t) lens[i];
        memcpy(body + 2 * i, &l, 2);
    }
    memcpy(body + 8, &ceiling, 4);
    size_t off = 12;
    for (int i = 0; i < 4; i++)
        put_str(body, &off, strs[i], lens[i]);

    int fds[2] = { listen_fd, close_fd };
    int xerr;
    void *r = xns_request(X_XnsAddListener, body, off, fds, 2, &xerr);
    free(body);
    if (!r) {
        const char *why = "";
        if (xerr == 15)
            why = " (no such namespace; use --create?)";
        else if (xerr == 10)
            why = " (refused: root or admin namespace)";
        fprintf(stderr, "%s: AddListener for \"%s\" failed: %s%s\n",
                progname, name, xerr_name(xerr), why);
        return -1;
    }
    free(r);
    return 0;
}

/* ------------------------------------------------------------------ */

static int
parse_caps(const char *list, uint32_t *caps)
{
    char *copy = strdup(list), *save = NULL;
    if (!copy)
        return -1;
    for (char *tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        size_t i;
        for (i = 0; i < sizeof(capabilities) / sizeof(capabilities[0]); i++) {
            if (strcmp(tok, capabilities[i].name) == 0) {
                *caps |= capabilities[i].bit;
                break;
            }
        }
        if (i == sizeof(capabilities) / sizeof(capabilities[0])) {
            fprintf(stderr, "%s: unknown capability \"%s\"\n", progname, tok);
            free(copy);
            return -1;
        }
    }
    free(copy);
    return 0;
}

static int
parse_grants(const char *list, uint32_t *grants)
{
    *grants = 0;
    if (strcmp(list, "none") == 0)
        return 0;
    char *copy = strdup(list), *save = NULL;
    if (!copy)
        return -1;
    for (char *tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        size_t i, n = sizeof(grant_names) / sizeof(grant_names[0]);
        for (i = 0; i < n; i++) {
            if (strcmp(tok, grant_names[i]) == 0) {
                *grants |= 1u << i;
                break;
            }
        }
        if (i == n) {
            fprintf(stderr, "%s: unknown XPrism capability \"%s\"\n", progname, tok);
            free(copy);
            return -1;
        }
    }
    free(copy);
    return 0;
}

static int
check_app_string(const char *what, const char *s)
{
    size_t n = strlen(s);
    if (n > XNS_APP_STRING_MAX) {
        fprintf(stderr, "%s: %s longer than %d bytes\n", progname, what, XNS_APP_STRING_MAX);
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        if (s[i] < 0x20 || s[i] > 0x7e) {
            fprintf(stderr, "%s: %s must be printable ASCII\n", progname, what);
            return -1;
        }
    }
    return 0;
}

static int
make_listening_socket(const char *path)
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    if (snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path) >=
        (int) sizeof(addr.sun_path)) {
        fprintf(stderr, "%s: socket path too long\n", progname);
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    /* only our user may connect; the sandbox gets in via a bind mount */
    mode_t old = umask(077);
    int rc = bind(fd, (struct sockaddr *) &addr, sizeof(addr));
    umask(old);
    if (rc != 0) {
        fprintf(stderr, "%s: cannot create %s: %s\n", progname, path, strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, 16) != 0) {
        perror("listen");
        close(fd);
        unlink(path);
        return -1;
    }
    return fd;
}

int
main(int argc, char **argv)
{
    const char *display = NULL, *ns = NULL;
    const char *app_id = "", *instance_id = "", *engine = "";
    int create = 0;
    uint32_t caps = 0, grants = 0, ceiling = XNS_NO_CEILING;
    const char *allow = NULL, *grant = NULL, *ceiling_arg = NULL;

    enum { OPT_APP_ID = 256, OPT_INSTANCE_ID, OPT_ENGINE, OPT_CEILING };
    static const struct option opts[] = {
        { "display",     required_argument, NULL, 'd' },
        { "namespace",   required_argument, NULL, 'n' },
        { "create",      no_argument,       NULL, 'c' },
        { "allow",       required_argument, NULL, 'a' },
        { "grant",       required_argument, NULL, 'g' },
        { "ceiling",     required_argument, NULL, OPT_CEILING },
        { "app-id",      required_argument, NULL, OPT_APP_ID },
        { "instance-id", required_argument, NULL, OPT_INSTANCE_ID },
        { "engine",      required_argument, NULL, OPT_ENGINE },
        { "help",        no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };

    int opt;
    /* "+": stop at the first non-option, so COMMAND's options stay its own */
    while ((opt = getopt_long(argc, argv, "+d:n:ca:g:h", opts, NULL)) != -1) {
        switch (opt) {
        case 'd': display = optarg; break;
        case 'n': ns = optarg; break;
        case 'c': create = 1; break;
        case 'a': allow = optarg; break;
        case 'g': grant = optarg; break;
        case OPT_CEILING: ceiling_arg = optarg; break;
        case OPT_APP_ID: app_id = optarg; break;
        case OPT_INSTANCE_ID: instance_id = optarg; break;
        case OPT_ENGINE: engine = optarg; break;
        case 'h': usage(stdout); return 0;
        default: usage(stderr); return 1;
        }
    }
    if (!ns || optind >= argc) {
        usage(stderr);
        return 1;
    }
    const char *sock_path = argv[optind++];
    if (optind < argc && strcmp(argv[optind], "--") == 0)
        optind++;
    char **command = optind < argc ? &argv[optind] : NULL;

    if (strlen(ns) == 0 || strlen(ns) > 255) {
        fprintf(stderr, "%s: bad namespace name\n", progname);
        return 1;
    }
    if (allow && !create) {
        fprintf(stderr, "%s: --allow needs --create\n", progname);
        return 1;
    }
    if (allow && parse_caps(allow, &caps) != 0)
        return 1;
    if (grant && !create) {
        fprintf(stderr, "%s: --grant needs --create (use --ceiling to limit "
                "clients of an existing namespace)\n", progname);
        return 1;
    }
    if (grant && parse_grants(grant, &grants) != 0)
        return 1;
    if (ceiling_arg && parse_grants(ceiling_arg, &ceiling) != 0)
        return 1;
    if (check_app_string("--app-id", app_id) || check_app_string("--instance-id", instance_id) ||
        check_app_string("--engine", engine))
        return 1;

    /* signals are handled synchronously in the wait loop below */
    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    sigaddset(&sigs, SIGHUP);
    sigaddset(&sigs, SIGCHLD);
    sigprocmask(SIG_BLOCK, &sigs, NULL);

    conn = xcb_connect(display, NULL);
    if (xcb_connection_has_error(conn)) {
        fprintf(stderr, "%s: cannot connect to X server %s\n", progname,
                display ? display : getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)");
        return 1;
    }
    if (xns_init() != 0)
        return 1;

    int status = 1, created = 0, listen_fd = -1, have_socket = 0;
    int pipefd[2] = { -1, -1 };

    if (create) {
        if (xns_create(ns, caps) != 0)
            goto out;
        created = 1;
        if (grants && xns_set_grants(ns, grants) != 0)
            goto out;
    }

    listen_fd = make_listening_socket(sock_path);
    if (listen_fd < 0)
        goto out;
    have_socket = 1;

    /* the server stops listening when the write end closes: that is, when
       we exit, however we exit. O_CLOEXEC keeps COMMAND from holding it. */
    if (pipe(pipefd) != 0) {
        perror("pipe");
        close(listen_fd);
        goto out;
    }
    fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
    /* xcb closes both fds once they are sent */
    if (xns_add_listener(ns, app_id, instance_id, engine, ceiling, listen_fd, pipefd[0]) != 0)
        goto out;
    pipefd[0] = -1;

    printf("ready\n");
    fflush(stdout);

    pid_t child = -1;
    if (command) {
        child = fork();
        if (child < 0) {
            perror("fork");
            goto out;
        }
        if (child == 0) {
            sigprocmask(SIG_UNBLOCK, &sigs, NULL);
            setenv("XNS_SOCKET", sock_path, 1);
            execvp(command[0], command);
            fprintf(stderr, "%s: cannot run %s: %s\n", progname, command[0], strerror(errno));
            _exit(127);
        }
    }

    status = 0;
    for (;;) {
        /* sigwait(), not sigwaitinfo(): OpenBSD only has the former */
        int sig;
        int err = sigwait(&sigs, &sig);
        if (err != 0) {
            if (err == EINTR)
                continue;
            fprintf(stderr, "%s: sigwait: %s\n", progname, strerror(err));
            status = 1;
            break;
        }
        if (sig == SIGCHLD) {
            int ws;
            if (child > 0 && waitpid(child, &ws, WNOHANG) == child) {
                status = WIFEXITED(ws) ? WEXITSTATUS(ws) : 128 + WTERMSIG(ws);
                break;
            }
            continue;
        }
        if (child > 0)
            kill(child, sig);   /* let COMMAND decide; we exit when it does */
        else
            break;
    }

out:
    if (pipefd[1] >= 0)
        close(pipefd[1]);       /* server stops accepting on the socket */
    if (pipefd[0] >= 0)
        close(pipefd[0]);
    if (have_socket)
        unlink(sock_path);
    if (created)
        xns_delete(ns);
    xcb_disconnect(conn);
    return status;
}
