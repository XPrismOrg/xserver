/* SPDX-License-Identifier: MIT OR X11
 *
 * xprism-permission-agent: ask the user when a confined application
 * needs a capability it was not granted.
 *
 * Registers with X-NAMESPACE SetPermissionAgent. For each
 * PermissionRequest, it runs the ask command (through /bin/sh) with the
 * application's details in the environment, and answers with the
 * command's exit status:
 *
 *   0  allow once          2  allow while the application runs
 *   1  deny once           3  deny, and do not ask again
 *
 * Anything else (a crash, a closed dialog) denies once. Prompts are
 * asked in parallel; the server refuses one after 60 seconds without an
 * answer.
 *
 * Needs X-NAMESPACE, i.e. the root namespace's authorization (the
 * session's $XAUTHORITY), and the server in capability mode enforce.
 */
#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>

#include <xcb/xcb.h>
#include <xcb/xcbext.h>

/* from Xext/namespace/namespaceproto.h, as in xns-listen */
#define XNS_EXTENSION_NAME          "X-NAMESPACE"
#define X_XnsQueryVersion           0
#define X_XnsGetClientInfo          12
#define X_XnsSetPermissionAgent     14
#define X_XnsAnswerPermission       15
#define XNS_CLIENT_INFO_PID         (1u << 1)
#define XNS_PERMISSION_DENY_ONCE    0
#define XNS_PERMISSION_ALLOW_ONCE   1
#define XNS_PERMISSION_ALLOW_ALWAYS 2
#define XNS_PERMISSION_DENY_ALWAYS  3

static const struct {
    const char *name;
    const char *what;       /* "<app> wants to ..." */
} capabilities[] = {
    { "capture.screen",     "take pictures of the whole screen" },
    { "capture.window",     "read what another application's window shows" },
    { "input.inject",       "type and click on your behalf" },
    { "input.monitor",      "see what you type and click in other applications" },
    { "input.virtual_seat", "add input devices" },
    { "window.enumerate",   "list the windows of other applications" },
    { "window.inspect",     "read details of other applications' windows" },
    { "window.manage",      "move, resize and manage other applications' windows" },
    { "clipboard.read",     "read the clipboard" },
    { "clipboard.write",    "change the clipboard" },
    { "display.configure",  "change the display settings" },
    { "compositor.legacy",  "take over drawing the screen" },
};
#define NUM_CAPS (sizeof(capabilities) / sizeof(capabilities[0]))

static const char *progname = "xprism-permission-agent";
static xcb_connection_t *conn;
static uint8_t xns_opcode, xns_event;
static int verbose;

static void
usage(FILE *f)
{
    fprintf(f,
"Usage: %s [--verbose] --ask COMMAND\n"
"\n"
"Asks the user, through COMMAND, whenever a confined X client needs a\n"
"capability it was not granted. COMMAND runs through /bin/sh for each\n"
"request, with these variables set:\n"
"\n"
"  XPRISM_PROMPT       a sentence to show, e.g. \"org.example.Editor wants\n"
"                      to take pictures of the whole screen.\"\n"
"  XPRISM_CAPABILITY   capture.screen, input.inject, ...\n"
"  XPRISM_APP_ID       the application's id, if its launcher gave one\n"
"  XPRISM_NAMESPACE    the X namespace it runs in\n"
"  XPRISM_PID          its process id, if known\n"
"  XPRISM_COMMAND      its command name, if known\n"
"\n"
"and answers with its exit status: 0 allow once, 1 deny, 2 allow while\n"
"the application runs, 3 deny and do not ask again. Anything else denies.\n"
"\n"
"With KDE, for example:\n"
"  %s --ask 'kdialog --title Permission --yesnocancel \"$XPRISM_PROMPT\" \\\n"
"      --yes-label \"Allow\" --no-label \"Allow once\" --cancel-label \"Deny\";\n"
"      case $? in 0) exit 2;; 1) exit 0;; *) exit 1;; esac'\n"
"\n"
"Prints \"ready\" on stdout once registered. Runs until the X connection\n"
"closes or it gets SIGINT/SIGTERM.\n",
            progname, progname);
}

static void *
xns_request(uint8_t minor, const void *body, size_t body_len, int isvoid, int *xerr)
{
    size_t len = 4 + body_len;
    uint8_t req[64] = { 0 };
    if (len > sizeof(req)) {
        *xerr = -1;
        return NULL;
    }
    req[0] = xns_opcode;
    req[1] = minor;
    uint16_t words = (uint16_t) (len / 4);
    memcpy(req + 2, &words, 2);
    memcpy(req + 4, body, body_len);

    struct iovec iov[3] = { { 0 }, { 0 }, { .iov_base = req, .iov_len = len } };
    xcb_protocol_request_t xreq = { .count = 1, .opcode = minor, .isvoid = isvoid };
    unsigned seq = xcb_send_request(conn, XCB_REQUEST_CHECKED | XCB_REQUEST_RAW,
                                    &iov[2], &xreq);
    *xerr = 0;
    if (isvoid) {
        xcb_generic_error_t *err = xcb_request_check(conn, (xcb_void_cookie_t) { seq });
        if (err) {
            *xerr = err->error_code;
            free(err);
        }
        else if (xcb_connection_has_error(conn))
            *xerr = -1;
        return NULL;
    }
    xcb_generic_error_t *err = NULL;
    void *reply = xcb_wait_for_reply(conn, seq, &err);
    if (err) {
        *xerr = err->error_code;
        free(err);
        free(reply);
        return NULL;
    }
    if (!reply)
        *xerr = -1;
    return reply;
}

static void
answer(uint32_t id, uint8_t ans)
{
    uint8_t body[8] = { 0 };
    int xerr;
    memcpy(body, &id, 4);
    body[4] = ans;
    xns_request(X_XnsAnswerPermission, body, sizeof(body), 1, &xerr);
    if (xerr)
        fprintf(stderr, "%s: answering prompt %u failed (X error %d)\n", progname, id, xerr);
    else if (verbose)
        fprintf(stderr, "%s: prompt %u answered %u\n", progname, id, ans);
}

/* the strings of a GetClientInfo reply */
struct client_info {
    char ns[256], app[256];
    uint32_t pid;
    int has_pid;
};

static void
copy_str(char *dst, const uint8_t **p, uint16_t n, const uint8_t *end)
{
    size_t len = n < 255 ? n : 255;
    if (*p + len > end)
        len = 0;
    memcpy(dst, *p, len);
    dst[len] = '\0';
    *p += (n + 3u) & ~3u;
}

static void
client_info(uint32_t resource, struct client_info *ci)
{
    memset(ci, 0, sizeof(*ci));
    int xerr;
    uint8_t *r = xns_request(X_XnsGetClientInfo, &resource, 4, 0, &xerr);
    if (!r)
        return;
    uint32_t length;
    uint16_t lens[4];
    memcpy(&length, r + 4, 4);
    memcpy(&ci->pid, r + 8, 4);
    memcpy(lens, r + 16, 8);
    ci->has_pid = (r[1] & XNS_CLIENT_INFO_PID) && ci->pid;
    const uint8_t *p = r + 32, *end = r + 32 + 4 * (size_t) length;
    copy_str(ci->ns, &p, lens[0], end);
    copy_str(ci->app, &p, lens[1], end);
    free(r);
}

/* prompts being asked: child pid -> prompt id */
#define MAX_ASKING 64
static struct { pid_t pid; uint32_t id; } asking[MAX_ASKING];

static void
ask(const char *command, uint32_t id, unsigned cap, uint32_t resource)
{
    struct client_info ci;
    client_info(resource, &ci);

    char comm[64] = "";
    if (ci.has_pid) {
        char path[64];
        snprintf(path, sizeof(path), "/proc/%u/comm", ci.pid);
        FILE *f = fopen(path, "r");
        if (f) {
            if (fgets(comm, sizeof(comm), f))
                comm[strcspn(comm, "\n")] = '\0';
            fclose(f);
        }
    }

    const char *capname = cap < NUM_CAPS ? capabilities[cap].name : "unknown";
    const char *who = ci.app[0] ? ci.app : comm[0] ? comm : "An application";
    char prompt[768];
    if (cap < NUM_CAPS)
        snprintf(prompt, sizeof(prompt), "%s%s%s wants to %s.", who,
                 ci.ns[0] ? " in " : "", ci.ns[0] ? ci.ns : "", capabilities[cap].what);
    else
        snprintf(prompt, sizeof(prompt), "%s wants capability %u.", who, cap);

    int slot = -1;
    for (int i = 0; i < MAX_ASKING; i++)
        if (!asking[i].pid) {
            slot = i;
            break;
        }
    if (slot < 0) {
        fprintf(stderr, "%s: too many open prompts, denying %u\n", progname, id);
        answer(id, XNS_PERMISSION_DENY_ONCE);
        return;
    }
    if (verbose)
        fprintf(stderr, "%s: prompt %u: %s\n", progname, id, prompt);

    pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "%s: fork: %s\n", progname, strerror(errno));
        answer(id, XNS_PERMISSION_DENY_ONCE);
        return;
    }
    if (child == 0) {
        char pidstr[16] = "";
        if (ci.has_pid)
            snprintf(pidstr, sizeof(pidstr), "%u", ci.pid);
        setenv("XPRISM_PROMPT", prompt, 1);
        setenv("XPRISM_CAPABILITY", capname, 1);
        setenv("XPRISM_APP_ID", ci.app, 1);
        setenv("XPRISM_NAMESPACE", ci.ns, 1);
        setenv("XPRISM_PID", pidstr, 1);
        setenv("XPRISM_COMMAND", comm, 1);
        close(xcb_get_file_descriptor(conn));
        execl("/bin/sh", "sh", "-c", command, (char *) NULL);
        _exit(127);
    }
    asking[slot].pid = child;
    asking[slot].id = id;
}

static void
reap(void)
{
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < MAX_ASKING; i++) {
            if (asking[i].pid != pid)
                continue;
            /* exit status -> answer: 0 allows, as shells expect */
            static const uint8_t answers[] = {
                XNS_PERMISSION_ALLOW_ONCE, XNS_PERMISSION_DENY_ONCE,
                XNS_PERMISSION_ALLOW_ALWAYS, XNS_PERMISSION_DENY_ALWAYS,
            };
            int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            uint8_t ans = (code >= 0 && code < 4) ? answers[code] : XNS_PERMISSION_DENY_ONCE;
            answer(asking[i].id, ans);
            asking[i].pid = 0;
        }
    }
    xcb_flush(conn);
}

static volatile sig_atomic_t stop;

static void
on_signal(int sig)
{
    (void) sig;
    stop = 1;
}

int
main(int argc, char **argv)
{
    static const struct option options[] = {
        { "ask",     required_argument, NULL, 'a' },
        { "verbose", no_argument,       NULL, 'v' },
        { "help",    no_argument,       NULL, 'h' },
        { 0 }
    };
    const char *command = NULL;
    int c;
    while ((c = getopt_long(argc, argv, "a:vh", options, NULL)) != -1) {
        switch (c) {
        case 'a': command = optarg; break;
        case 'v': verbose = 1; break;
        case 'h': usage(stdout); return 0;
        default:  usage(stderr); return 1;
        }
    }
    if (!command || optind != argc) {
        usage(stderr);
        return 1;
    }

    conn = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(conn)) {
        fprintf(stderr, "%s: cannot connect to the X server (is $DISPLAY set?)\n", progname);
        return 1;
    }
    const xcb_query_extension_reply_t *ext =
        xcb_get_extension_data(conn, &(xcb_extension_t) { XNS_EXTENSION_NAME, 0 });
    if (!ext || !ext->present) {
        fprintf(stderr, "%s: the X server offers no X-NAMESPACE to this client: it needs "
                "the session's own authorization ($XAUTHORITY) and a server started "
                "with -namespace\n", progname);
        return 1;
    }
    xns_opcode = ext->major_opcode;
    xns_event = ext->first_event;

    int xerr;
    uint16_t ver[2] = { 1, 2 };
    uint8_t *r = xns_request(X_XnsQueryVersion, ver, sizeof(ver), 0, &xerr);
    uint16_t major = 0, minor = 0;
    if (r) {
        memcpy(&major, r + 8, 2);
        memcpy(&minor, r + 10, 2);
        free(r);
    }
    if (major != 1 || minor < 2) {
        fprintf(stderr, "%s: the X server's X-NAMESPACE is %u.%u; permission prompts need 1.2\n",
                progname, major, minor);
        return 1;
    }

    uint8_t enable[4] = { 1, 0, 0, 0 };
    xns_request(X_XnsSetPermissionAgent, enable, sizeof(enable), 1, &xerr);
    if (xerr == 10) {
        fprintf(stderr, "%s: another permission agent is running\n", progname);
        return 1;
    }
    if (xerr) {
        fprintf(stderr, "%s: SetPermissionAgent failed (X error %d)\n", progname, xerr);
        return 1;
    }

    struct sigaction sa = { .sa_handler = on_signal };
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    printf("ready\n");
    fflush(stdout);

    struct pollfd pfd = { .fd = xcb_get_file_descriptor(conn), .events = POLLIN };
    while (!stop) {
        xcb_generic_event_t *ev;
        while ((ev = xcb_poll_for_event(conn))) {
            if ((ev->response_type & 0x7f) == xns_event) {
                const uint8_t *e = (const uint8_t *) ev;
                uint32_t id, resource;
                memcpy(&id, e + 4, 4);
                memcpy(&resource, e + 8, 4);
                ask(command, id, e[1], resource);
            }
            free(ev);
        }
        if (xcb_connection_has_error(conn))
            break;
        reap();
        poll(&pfd, 1, 100);
    }

    /* open prompts are refused by the server as we go */
    xcb_disconnect(conn);
    for (int i = 0; i < MAX_ASKING; i++)
        if (asking[i].pid)
            kill(asking[i].pid, SIGTERM);
    return 0;
}
