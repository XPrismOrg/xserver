/* SPDX-License-Identifier: MIT OR X11
 *
 * PAL process backend: Linux.
 *
 * The sandbox hint comes from the process's cgroup. Flatpak and Snap run
 * each app in a systemd scope named after it:
 *   app-flatpak-<app id>-<n>.scope
 *   snap.<snap name>.<app>-<uuid>.scope  (also .<uuid>.scope)
 * systemd escapes some characters in unit names as \xNN (e.g. '-' in an
 * app id), which is undone here.
 */
#include <dix-config.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

#include "os/pal/backend.h"
#include "os/pal/pal.h"

char *
pal_backend_process_grouping(pid_t pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%ld/cgroup", (long) pid);

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return NULL;

    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return NULL;
    buf[n] = '\0';
    return strdup(buf);
}

bool
pal_backend_pidfd_alive(int pidfd)
{
#ifdef SYS_pidfd_send_signal
    return pidfd >= 0 && syscall(SYS_pidfd_send_signal, pidfd, 0, NULL, 0) == 0;
#else
    return false;
#endif
}

static int
hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* undo systemd's \xNN unit name escaping; NULL on malformed input */
static char *
unescape_unit(const char *s, size_t len)
{
    char *out = malloc(len + 1), *o = out;
    if (!out)
        return NULL;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\\' && i + 3 < len && s[i + 1] == 'x' &&
            hexval(s[i + 2]) >= 0 && hexval(s[i + 3]) >= 0) {
            *o++ = (char) (hexval(s[i + 2]) * 16 + hexval(s[i + 3]));
            i += 3;
        } else {
            *o++ = s[i];
        }
    }
    *o = '\0';
    return out;
}

/* does s[0..len) end in "-<digits>.scope"? returns the length without it */
static size_t
strip_instance_suffix(const char *s, size_t len)
{
    static const char sfx[] = ".scope";
    size_t sl = sizeof(sfx) - 1;
    if (len <= sl || strncmp(s + len - sl, sfx, sl) != 0)
        return 0;
    size_t end = len - sl, i = end;
    while (i > 0 && s[i - 1] >= '0' && s[i - 1] <= '9')
        i--;
    if (i == end || i == 0 || s[i - 1] != '-')
        return 0;
    return i - 1;
}

static void
parse_unit(const char *unit, size_t len, char **engine, char **app_id)
{
    static const char flatpak[] = "app-flatpak-";
    static const char snap[] = "snap.";

    if (len > sizeof(flatpak) - 1 && strncmp(unit, flatpak, sizeof(flatpak) - 1) == 0) {
        const char *id = unit + sizeof(flatpak) - 1;
        size_t idlen = strip_instance_suffix(id, len - (sizeof(flatpak) - 1));
        if (idlen == 0)
            return;
        *app_id = unescape_unit(id, idlen);
        *engine = *app_id ? strdup("flatpak") : NULL;
        return;
    }

    if (len > sizeof(snap) - 1 && strncmp(unit, snap, sizeof(snap) - 1) == 0) {
        const char *name = unit + sizeof(snap) - 1;
        const char *dot = memchr(name, '.', len - (sizeof(snap) - 1));
        if (!dot || dot == name)
            return;
        *app_id = unescape_unit(name, dot - name);
        *engine = *app_id ? strdup("snap") : NULL;
    }
}

void
pal_process_parse_grouping(const char *record, char **engine, char **app_id)
{
    *engine = NULL;
    *app_id = NULL;
    if (!record)
        return;

    /* lines "hierarchy-id:controllers:path"; on cgroup v2 there is one,
       "0::/path". Look at every line's path, last component. */
    const char *line = record;
    while (*line && !*engine) {
        const char *nl = strchr(line, '\n');
        size_t linelen = nl ? (size_t) (nl - line) : strlen(line);

        const char *path = NULL;
        const char *c1 = memchr(line, ':', linelen);
        if (c1) {
            const char *c2 = memchr(c1 + 1, ':', linelen - (c1 + 1 - line));
            if (c2)
                path = c2 + 1;
        }
        if (path) {
            size_t plen = linelen - (path - line);
            /* walk the components: the scope may be followed by a
               sub-cgroup (e.g. Snap's or a manager's own) */
            const char *p = path;
            while (p < path + plen && !*engine) {
                const char *slash = memchr(p, '/', path + plen - p);
                size_t clen = slash ? (size_t) (slash - p) : (size_t) (path + plen - p);
                if (clen)
                    parse_unit(p, clen, engine, app_id);
                if (!*engine && *app_id) {   /* engine alloc failed */
                    free(*app_id);
                    *app_id = NULL;
                }
                p += clen + 1;
            }
        }
        line = nl ? nl + 1 : line + linelen;
    }
}
