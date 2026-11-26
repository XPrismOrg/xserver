# xprism-permission-agent

Asks the user when a confined application needs a capability it was not
granted, for example a sandboxed app that wants a screenshot of the whole
screen. Without an agent, such requests are refused.

It needs:

- a server started with `-namespace CONFIG -capmode enforce`, since
  prompts happen only where capabilities are enforced;
- the session's own X authorization (`$XAUTHORITY`), which puts it in the
  root namespace; sandboxed applications must not have that.

For each request, it runs the `--ask` command through `/bin/sh` with:

| Variable | Value |
|---|---|
| `XPRISM_PROMPT` | a sentence to show: "org.example.Editor in sandbox wants to take pictures of the whole screen." |
| `XPRISM_CAPABILITY` | `capture.screen`, `input.inject`, ... (see [doc/capabilities.md](../../doc/capabilities.md)) |
| `XPRISM_APP_ID` | the app id its launcher gave (`xns-listen --app-id`), or empty |
| `XPRISM_NAMESPACE` | its X namespace |
| `XPRISM_PID`, `XPRISM_COMMAND` | its process id and command name, where known |

The command's exit status is the answer:

| Exit | Answer |
|---|---|
| 0 | allow this once |
| 1 | deny this once |
| 2 | allow for as long as the application runs |
| 3 | deny, and do not ask again while it runs |
| other | deny this once |

Several prompts can be open at once. The application waits for the
answer, at most 60 seconds, after which it is refused.

## Examples

KDE:

```sh
xprism-permission-agent --ask '
  kdialog --title "Permission" --yesnocancel "$XPRISM_PROMPT" \
          --yes-label "Allow" --no-label "Allow once" --cancel-label "Deny"
  case $? in 0) exit 2;; 1) exit 0;; *) exit 1;; esac'
```

GNOME and others with zenity:

```sh
xprism-permission-agent --ask '
  zenity --question --title "Permission" --text "$XPRISM_PROMPT" \
         --ok-label "Allow" --cancel-label "Deny" && exit 2 || exit 1'
```

A fixed policy without dialogs, here allowing only screenshots:

```sh
xprism-permission-agent --ask '[ "$XPRISM_CAPABILITY" = capture.screen ] && exit 2; exit 3'
```

`--verbose` logs each prompt and answer to stderr. The protocol is
described in [doc/Xnamespace-protocol.md](../../doc/Xnamespace-protocol.md),
sections 6.14 to 6.16.
