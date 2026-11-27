Xnamespace extension v1.0
=========================

This extension separates clients into several namespaces (a bit similar to
Linux's kernel namespaces), which are isolated from each other. For example,
namespaces have their own selections and clients cannot directly interact
(send messages) or access other client's resources across namespace borders.

An exception is the `root` namespace, which completely is unrestricted.

Configuration
-------------

Namespaces are defined in a separate configuration file, which is loaded at
server startup (no dynamic provisioning in this version yet). The extension
is enabled when a namespace config is passed to the Xserver via the
`-namespace <fn>` flag.


See `Xext/namespace/ns.conf.example` for a configuration file example.

Authentication / Namespace assignment
-------------------------------------

Assignment of clients into namespaces is done by the authentication token the
client is using to authenticate. Thus, token authentication needs to enabled.

Flatpak applications
--------------------

Flatpak gives its applications the session's X socket and cookie, so by
the token alone they would land in the root namespace. A namespace with
a `classify` line takes them instead; the server recognises them by the
cgroup Flatpak runs them in:

    # every Flatpak application in a namespace of its own, made from this
    # one: "flatpak.org.example.App", removed with its last client
    namespace flatpak root
      classify flatpak
      grant clipboard.read

    # this application here, by its application id
    namespace editor root
      classify flatpak org.example.Editor
      grant clipboard.read clipboard.write

A line naming the application wins over a template. Only clients that
would otherwise be in the root namespace are moved; a client using a
namespace's own token, or a listener socket, stays where that puts it.
The cgroup can't be trusted to grant anything (doc/pal.md), but it is
only used to confine: a process that can change its cgroup isn't
sandboxed. If the process that connected has already exited when the
connection is set up, the server can't tell what it was and places the
client in the anonymous namespace.

The engine name is what the platform layer reports: `flatpak` and `snap`
on Linux.
