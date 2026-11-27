/* SPDX-License-Identifier: MIT OR X11
 *
 * Classifying sandboxed clients that come without a label (Flatpak).
 *
 * Flatpak gives its applications the session's X socket and cookie, so
 * they would land in the root namespace. The process's cgroup tells which
 * sandbox it runs in (os/pal: "flatpak", "org.example.App"). A namespace
 * with "classify flatpak org.example.App" takes that application; one
 * with "classify flatpak" is a template: each application gets a
 * namespace of its own made from it, "<template>.<app id>", which goes
 * away with its last client.
 *
 * The hint is unverified (doc/pal.md), so it only ever moves a client
 * *out of* the root namespace: a process that can forge its cgroup is not
 * sandboxed, and holds the root namespace anyway. Clients placed by a
 * listener socket or by a namespace's own cookie are left alone.
 */
#include <dix-config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dix/dix_priv.h"
#include "os/pal/identity.h"
#include "os/pal/pal.h"

#include "namespace.h"
#include "namespaceproto.h"
#include "hooks.h"

static Bool anyRules(void)
{
    struct Xnamespace *walk;
    xorg_list_for_each_entry(walk, &ns_list, entry)
        if (walk->classifyEngine)
            return TRUE;
    return FALSE;
}

/* the namespace of its own for @p appId, made from @p tmpl */
static struct Xnamespace *perAppNamespace(struct Xnamespace *tmpl, const char *appId)
{
    char name[XNS_NAME_MAX + 1];
    int n = snprintf(name, sizeof(name), "%s.%s", tmpl->name, appId);
    if (n < 0 || (size_t) n >= sizeof(name))
        return tmpl;

    struct Xnamespace *ns = XnsLookup(name, n);
    if (ns) {
        /* ours from an earlier client; a configured namespace of the same
           name isn't, and the template is the safe fallback */
        if (ns->classifiedFrom && strcmp(ns->classifiedFrom, tmpl->name) == 0)
            return ns;
        return tmpl;
    }

    int err;
    ns = XnsCreate(name, n, XnsCaps(tmpl), XNS_ATTR_TRANSIENT, &err);
    if (!ns)
        return tmpl;
    ns->xpcaps = tmpl->xpcaps;
    ns->injectInto = tmpl->injectInto ? strdup(tmpl->injectInto) : NULL;
    ns->allowExtensions = tmpl->allowExtensions ? strdup(tmpl->allowExtensions) : NULL;
    ns->classifiedFrom = strdup(tmpl->name);
    if ((tmpl->injectInto && !ns->injectInto) ||
        (tmpl->allowExtensions && !ns->allowExtensions) || !ns->classifiedFrom) {
        XnsDestroyNamespace(ns);
        return tmpl;
    }
    return ns;
}

static struct Xnamespace *findTarget(const char *engine, const char *appId)
{
    struct Xnamespace *walk, *tmpl = NULL;
    xorg_list_for_each_entry(walk, &ns_list, entry) {
        if (!walk->classifyEngine || walk->superPower || walk == &ns_root ||
            strcmp(walk->classifyEngine, engine) != 0)
            continue;
        if (walk->classifyAppId) {
            if (strcmp(walk->classifyAppId, appId) == 0)
                return walk;        /* this application exactly */
        } else if (!tmpl)
            tmpl = walk;
    }
    return tmpl ? perAppNamespace(tmpl, appId) : NULL;
}

void XnsClassifyClient(ClientPtr client, struct XnamespaceClientPriv *subj)
{
    if (subj->socketBound || subj->ns != &ns_root || !anyRules())
        return;

    struct pal_process_info info;
    if (pal_process_describe(ClientPeerIdentity(client), &info) != 0)
        return;     /* not a local client: nothing to tell */

    const struct peer_identity *id = ClientPeerIdentity(client);
    if ((id->fields & PEER_ID_PIDFD) && !info.pid_confirmed) {
        /* the process that connected is gone (its connection passed on?):
           what we read may describe another process. Trust nothing. */
        XNS_LOG("client %d: connecting process %d is gone, not trusted: namespace %s\n",
                client->index, (int) id->pid, ns_anon.name);
        XnamespaceAssignClient(subj, &ns_anon);
        goto out;
    }

    if (!info.sandbox_engine || !info.sandbox_app_id)
        goto out;

    struct Xnamespace *ns = findTarget(info.sandbox_engine, info.sandbox_app_id);
    if (!ns)
        goto out;

    struct XnsAppInfo *app = XnsAppInfoNew(info.sandbox_app_id, strlen(info.sandbox_app_id),
                                           "", 0,
                                           info.sandbox_engine, strlen(info.sandbox_engine));
    XnamespaceAssignClient(subj, ns);
    XnsAppInfoUnref(subj->app);
    subj->app = app;
    XNS_LOG("client %d (pid %d): %s application %s, namespace %s\n",
            client->index, (int) id->pid, info.sandbox_engine, info.sandbox_app_id, ns->name);
out:
    pal_process_release(&info);
}
