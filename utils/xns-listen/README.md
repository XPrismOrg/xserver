# xns-listen

Give an application its own socket into an X namespace.

A sandbox launcher runs `xns-listen`, which creates a Unix socket and hands
it to the X server (X-NAMESPACE 1.1 `AddListener`). Every X client that
connects through that socket is placed in the namespace, whatever
authorization data it sends. The launcher makes the socket the app's X
display inside the sandbox; the app never sees the real display socket
or any cookie.

```
xns-listen [options] SOCKET [-- COMMAND [ARG...]]

  -d, --display DISPLAY     X server to use (default: $DISPLAY)
  -n, --namespace NAME      namespace for clients on SOCKET (required)
  -c, --create              create the namespace, delete it on exit
  -a, --allow CAP[,CAP...]  with --create: mouse-motion, shape,
                            transparency, xinput, xkeyboard
      --app-id ID           e.g. org.example.Editor
      --instance-id ID      e.g. a Flatpak instance id
      --engine NAME         e.g. flatpak, bubblewrap
```

`xns-listen` runs COMMAND with `XNS_SOCKET` set to SOCKET. When COMMAND
exits (or, without COMMAND, on SIGINT/SIGTERM/SIGHUP), the server stops
accepting clients on the socket and the socket file is removed. With
`--create` the namespace is deleted too, which disconnects anything still
in it. The exit status is COMMAND's. "ready" is printed on stdout once the
socket accepts clients.

`xns-listen` itself needs the root namespace's authorization (the
session's normal `$XAUTHORITY`), since it uses X-NAMESPACE.

## Example: bubblewrap

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

Inside the sandbox `/tmp/.X11-unix` holds only the app's own socket, as
display `:0`. The app connects without a cookie and lands in `editor`.
`--unshare-all` also takes away the network, so the app can't reach the
server over TCP or an abstract socket either. (Tested with bubblewrap
against Xvfb.)

Notes:

- Unix socket paths are limited to about 100 bytes; keep SOCKET short
  (`$XDG_RUNTIME_DIR` is a good place).
- The socket is created mode 0600 (owner only).
- The app info (`--app-id`, ...) is the launcher's claim, shown to
  managers via `GetClientInfo`. The server cannot verify it.
