/* SPDX-License-Identifier: X11 OR MIT OR AGPL-3.0-or-later */
/* Copyright (C) 2026 Enrico Weigelt, metux IT consult <info@metux.net> */
#define HOOK_NAME "selection"

#include <dix-config.h>

#include <stdio.h>

#include "dix/capability_priv.h"
#include "dix/dix_priv.h"
#include "dix/selection_priv.h"

#include "namespace.h"
#include "hooks.h"

static inline const char *stripNS(const char* name) {
    if ((!name) || (name[0] != '<'))
        return name; // can this ever happen ?
    const char *got = strchr(name, '>');
    if (!got)
        return name;
    return ++got;
}

/*
 * The session's selections (PRIMARY, SECONDARY, CLIPBOARD) stay shared with
 * the rest of the session for clients whose grants (namespace grants AND
 * listener ceiling) include clipboard.read / clipboard.write:
 *
 *   SetSelectionOwner                      clipboard.write
 *   ConvertSelection, XFixes listening     clipboard.read
 *   GetSelectionOwner                      either (a writer must see that it
 *                                          owns the selection, a reader who
 *                                          to ask)
 *
 * Everything else, and every selection for clients without the grant, is
 * renamed into the namespace as below. A grant revoked at runtime applies
 * from the client's next request.
 */
static Bool sessionSelectionGranted(struct XnamespaceClientPriv *subj,
                                    Atom selection, int op)
{
    static Atom primary, secondary, clipboard;
    if (!clipboard) {
        primary = dixAddAtom("PRIMARY");
        secondary = dixAddAtom("SECONDARY");
        clipboard = dixAddAtom("CLIPBOARD");
    }
    if (selection != primary && selection != secondary && selection != clipboard)
        return FALSE;

    uint32_t grants = XnsClientGrants(subj);
    const uint32_t rd = XP_CAP_BIT(XP_CAP_CLIPBOARD_READ);
    const uint32_t wr = XP_CAP_BIT(XP_CAP_CLIPBOARD_WRITE);

    switch (op) {
        case SELECTION_FILTER_SETOWNER:
            return (grants & wr) != 0;
        case SELECTION_FILTER_CONVERT:
        case SELECTION_FILTER_LISTEN:
            return (grants & rd) != 0;
        case SELECTION_FILTER_GETOWNER:
            return (grants & (rd | wr)) != 0;
        default:
            return FALSE;
    }
}

/*
 * Reply tokens. A selection owner answers a SelectionRequest by setting a
 * property on the requestor's window and sending it SelectionNotify. When
 * the requestor is in another namespace (e.g. the session pasting from a
 * confined app that holds CLIPBOARD through clipboard.write), Xnamespace
 * would block both. So delivering a SelectionRequest to a confined owner
 * records a token: that owner, that requestor window, that property. It
 * allows ChangeProperty of exactly that property on that window, and one
 * SendEvent of a matching SelectionNotify to it, which uses the token up.
 *
 * Not covered: INCR transfers (large data in chunks; the owner would also
 * need PropertyNotify from the requestor's window).
 */
#define SEL_REPLY_MAX 64

static struct {
    ClientPtr owner;        /* NULL: free slot */
    Window requestor;
    Atom selection;
    Atom property;
} selReplies[SEL_REPLY_MAX];
static unsigned selReplyNext;

static void selReplyAdd(ClientPtr owner, Window requestor, Atom selection, Atom property)
{
    /* full: overwrite the oldest; a flood only breaks pastes, it can't
       grant more than one property on a window that asked */
    for (unsigned i = 0; i < SEL_REPLY_MAX; i++) {
        if (!selReplies[i].owner) {
            selReplyNext = i;
            break;
        }
    }
    selReplies[selReplyNext].owner = owner;
    selReplies[selReplyNext].requestor = requestor;
    selReplies[selReplyNext].selection = selection;
    selReplies[selReplyNext].property = property;
    selReplyNext = (selReplyNext + 1) % SEL_REPLY_MAX;
}

Bool XnsSelReplyMayChangeProperty(ClientPtr owner, Window requestor, Atom property)
{
    for (unsigned i = 0; i < SEL_REPLY_MAX; i++)
        if (selReplies[i].owner == owner && selReplies[i].requestor == requestor &&
            selReplies[i].property == property)
            return TRUE;
    return FALSE;
}

Bool XnsSelReplyPending(ClientPtr owner, Window requestor)
{
    for (unsigned i = 0; i < SEL_REPLY_MAX; i++)
        if (selReplies[i].owner == owner && selReplies[i].requestor == requestor)
            return TRUE;
    return FALSE;
}

Bool XnsSelReplyConsume(ClientPtr owner, WindowPtr dest, const xEvent *events, int count)
{
    if (count != 1 || (events[0].u.u.type & 0x7f) != SelectionNotify)
        return FALSE;
    const Window requestor = events[0].u.selectionNotify.requestor;
    const Atom selection = events[0].u.selectionNotify.selection;
    const Atom property = events[0].u.selectionNotify.property;
    if (requestor != dest->drawable.id)
        return FALSE;

    for (unsigned i = 0; i < SEL_REPLY_MAX; i++) {
        if (selReplies[i].owner == owner && selReplies[i].requestor == requestor &&
            selReplies[i].selection == selection &&
            (property == None || property == selReplies[i].property)) {
            selReplies[i].owner = NULL;
            return TRUE;
        }
    }
    return FALSE;
}

void XnsSelReplyDropClient(ClientPtr client)
{
    for (unsigned i = 0; i < SEL_REPLY_MAX; i++)
        if (selReplies[i].owner == client)
            selReplies[i].owner = NULL;
}

static void selReplyOnRequest(SelectionFilterParamRec *param)
{
    struct XnamespaceClientPriv *owner = XnsClientPriv(param->recvClient);
    if (!owner || !owner->ns || owner->ns->superPower)
        return;     /* the owner isn't confined: nothing to open */

    ClientPtr reqClient = dixLookupXIDOwner(param->requestor);
    if (!reqClient || XnsClientSameNS(owner, XnsClientPriv(reqClient)))
        return;     /* same namespace: allowed anyway */

    /* obsolete clients pass property None and get the reply in target */
    Atom property = param->property != None ? param->property : param->target;
    selReplyAdd(param->recvClient, param->requestor, param->selection, property);
}

/*
 * This hook is rewriting the client visible selection names to internally used,
 * per namespace ones. Whenever a client is asking for a selection, it's name
 * is replaced by a namespaced one, e.g. asking for "PRIMARY" while being in
 * namespace "foo" will become "<foo>PRIMARY"
 *
 * A malicious client could still send specially crafted messages to others,
 * asking them to send their selection data to him. This needs to be solved
 * separately, by a send hook.
 */
void hookSelectionFilter(CallbackListPtr *pcbl, void *unused, void *calldata)
{
    XNS_HOOK_HEAD(SelectionFilterParamRec);

    if (param->op == SELECTION_FILTER_EV_REQUEST)
        selReplyOnRequest(param);

    /* no rewrite if client is in root namespace */
    if (subj->ns->superPower)
        return;

    if (sessionSelectionGranted(subj, param->selection, param->op))
        return;

    const char *origSelectionName = NameForAtom(param->selection);

    char selname[PATH_MAX] = { 0 };
    snprintf(selname, sizeof(selname)-1, "<%s>%s", subj->ns->name, origSelectionName);
    Atom realSelection = dixAddAtom(selname);

    switch (param->op) {
        case SELECTION_FILTER_GETOWNER:
        case SELECTION_FILTER_SETOWNER:
        case SELECTION_FILTER_CONVERT:
        case SELECTION_FILTER_LISTEN:
            // TODO: check whether window really belongs to the client
            param->selection = realSelection;
        break;

        case SELECTION_FILTER_NOTIFY:
        {
            // need to translate back, since we're having the ns-prefixed name here
            const char *stripped = stripNS(origSelectionName);
            param->selection = dixAddAtom(stripped);
            break;
        }

        // nothing to do here: already having the client visible name
        case SELECTION_FILTER_EV_REQUEST:
        case SELECTION_FILTER_EV_CLEAR:
        break;
    }
}
