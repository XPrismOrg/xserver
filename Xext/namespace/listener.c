/* SPDX-License-Identifier: MIT OR X11
 *
 * Xnamespace: listening sockets bound to a namespace (protocol 1.1).
 *
 * A trusted launcher passes the server a listening Unix socket for a
 * namespace (X_XnsAddListener). Clients accepted on it are placed in that
 * namespace when they connect, whatever authorization data they send.
 * Unlike an auth cookie, which can be copied out of a sandbox and reused
 * from anywhere, the socket can be made reachable only from inside the
 * sandbox.
 */
#include <dix-config.h>

#include <stdlib.h>
#include <string.h>

#include "include/list.h"
#include "dix/capability_priv.h"
#include "os/listener_priv.h"

#include "namespace.h"

struct xns_listener {
    struct xorg_list entry;
    struct Xnamespace *ns;
    CARD32 handle;
    struct labeled_listener *l;
    struct XnsAppInfo *app;
};

static struct xorg_list xns_listeners = { &xns_listeners, &xns_listeners };
static CARD32 xns_listener_seq;

static char *
dup_n(const char *s, size_t n)
{
    char *r = malloc(n + 1);
    if (r) {
        if (n)
            memcpy(r, s, n);
        r[n] = '\0';
    }
    return r;
}

struct XnsAppInfo *
XnsAppInfoNew(const char *appId, size_t appIdLen,
              const char *instanceId, size_t instanceIdLen,
              const char *engine, size_t engineLen)
{
    struct XnsAppInfo *app = calloc(1, sizeof(*app));
    if (!app)
        return NULL;
    app->refcnt = 1;
    app->ceiling = XP_CAP_ALL;
    app->appId = dup_n(appId, appIdLen);
    app->instanceId = dup_n(instanceId, instanceIdLen);
    app->engine = dup_n(engine, engineLen);
    if (!app->appId || !app->instanceId || !app->engine) {
        XnsAppInfoUnref(app);
        return NULL;
    }
    return app;
}

struct XnsAppInfo *
XnsAppInfoRef(struct XnsAppInfo *app)
{
    if (app)
        app->refcnt++;
    return app;
}

void
XnsAppInfoUnref(struct XnsAppInfo *app)
{
    if (!app || --app->refcnt > 0)
        return;
    free(app->appId);
    free(app->instanceId);
    free(app->engine);
    free(app);
}

static void
xns_listener_free(struct xns_listener *xl)
{
    xorg_list_del(&xl->entry);
    XnsAppInfoUnref(xl->app);
    free(xl);
}

/* the launcher closed the close fd: the os layer frees the listener itself */
static void
xns_listener_closed(struct labeled_listener *l, void *label)
{
    struct xns_listener *xl = label;

    XNS_LOG("listener %u of namespace \"%s\" closed by its owner\n",
            (unsigned) xl->handle, xl->ns->name);
    xns_listener_free(xl);
}

int
XnsAddListener(struct Xnamespace *ns, struct XnsAppInfo *app,
               int listen_fd, int close_fd, CARD32 *handleOut)
{
    struct xns_listener *xl = calloc(1, sizeof(*xl));
    if (!xl)
        return BadAlloc;

    int err = Success;
    xl->ns = ns;
    xl->app = XnsAppInfoRef(app);
    xl->l = labeled_listener_add(listen_fd, close_fd, xl, xns_listener_closed, &err);
    if (!xl->l) {
        XnsAppInfoUnref(xl->app);
        free(xl);
        return err;
    }

    do {
        xl->handle = ++xns_listener_seq;
    } while (xl->handle == 0);      /* 0 is never a valid handle */
    xorg_list_append(&xl->entry, &xns_listeners);

    *handleOut = xl->handle;
    XNS_LOG("namespace \"%s\" now listens on fd %d (handle %u, app \"%s\")\n",
            ns->name, listen_fd, (unsigned) xl->handle,
            app ? app->appId : "");
    return Success;
}

int
XnsRemoveListener(CARD32 handle)
{
    struct xns_listener *xl;

    xorg_list_for_each_entry(xl, &xns_listeners, entry) {
        if (xl->handle == handle) {
            labeled_listener_remove(xl->l);
            xns_listener_free(xl);
            return Success;
        }
    }
    return BadValue;
}

void
XnsRemoveListenersOf(struct Xnamespace *ns)
{
    struct xns_listener *xl, *tmp;

    xorg_list_for_each_entry_safe(xl, tmp, &xns_listeners, entry) {
        if (xl->ns == ns) {
            labeled_listener_remove(xl->l);
            xns_listener_free(xl);
        }
    }
}

struct Xnamespace *
XnsListenerNamespace(void *label, struct XnsAppInfo **app)
{
    struct xns_listener *xl;

    if (!label)
        return NULL;

    /* only trust labels that are listeners of ours */
    xorg_list_for_each_entry(xl, &xns_listeners, entry) {
        if (xl == label) {
            if (app)
                *app = XnsAppInfoRef(xl->app);
            return xl->ns;
        }
    }
    return NULL;
}
