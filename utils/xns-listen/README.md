# xns-listen

`xns-listen` gives an application its own socket into an X namespace. It
is meant to be run by sandbox launchers.

It creates a Unix socket and passes it to the X server with the
X-NAMESPACE `AddListener` request. Every client that connects through
that socket is placed in the given namespace, whatever authorization data
it sends. The launcher makes the socket the application's X display
inside the sandbox, so the application never sees the real display
socket or an authorization cookie.

```
xns-listen [options] SOCKET [-- COMMAND [ARG...]]

  -d, --display DISPLAY     X server to use (default: $DISPLAY)
  -n, --namespace NAME      namespace for clients on SOCKET (required)
  -c, --create              create the namespace, delete it on exit
  -a, --allow CAP[,CAP...]  with --create: mouse-motion, shape,
                            transparency, xinput, xkeyboard
  -g, --grant CAP[,CAP...]  with --create: capabilities for the
                            namespace (see doc/capabilities.md)
      --inject-into NS      with --create: may automate namespace NS's
                            windows (XTest, with a seat of its own)
      --ceiling CAP[,CAP...]  the most capabilities clients on SOCKET
                            may hold; "none" for none; no limit by default
      --app-id ID           application ID, e.g. org.example.Editor
      --instance-id ID      instance ID, e.g. a Flatpak instance
      --engine NAME         sandbox engine, e.g. flatpak, bubblewrap
```

If COMMAND is given, `xns-listen` runs it with `XNS_SOCKET` set to
SOCKET and exits with its status. Without COMMAND it runs until it
receives SIGINT, SIGTERM or SIGHUP. In both cases the server then stops
accepting clients on the socket and the socket file is removed. With
`--create`, the namespace is deleted as well, which disconnects any
clients still in it. `xns-listen` prints `ready` on standard output once
the socket accepts connections.

`xns-listen` uses the X-NAMESPACE extension, so it needs the root
namespace's authorization, which is normally the session's
`$XAUTHORITY`.

## Example with bubblewrap

```sh
sock=$XDG_RUNTIME_DIR/editor-x11.sock

xns-listen -n editor --create --app-id org.example.Editor --engine bubblewrap \
    "$sock" -- \
  bwrap --ro-bind /usr /usr --ro-bind /etc /etc --proc /proc --dev /dev \
        --tmpfs /tmp \
        --bind "$sock" /tmp/.X11-unix/X0 \
        --unshare-all --die-with-parent \
        --setenv DISPLAY :0 --unsetenv XAUTHORITY \
        /usr/bin/editor
```

Inside the sandbox, `/tmp/.X11-unix` contains only the application's own
socket, as display `:0`. The application connects without a cookie and
is placed in the `editor` namespace. Because `--unshare-all` also removes
network access, the application cannot reach the server over TCP or an
abstract socket either. This example has been tested with bubblewrap and
Xvfb.

## Notes

- Unix socket paths are limited to about 100 bytes, so keep SOCKET short.
  `$XDG_RUNTIME_DIR` is a suitable location.
- The socket is created with mode 0600.
- The application information (`--app-id` and the related options) is
  supplied by the launcher and reported to managers through
  `GetClientInfo`. The server cannot verify it.
