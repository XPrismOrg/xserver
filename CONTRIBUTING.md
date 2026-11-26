# Contributing to XPrism

Thank you for helping with XPrism! This guide covers how to build and
test it, what a good change looks like, and how changes get in. Everyone
taking part follows the [Code of Conduct](CODE_OF_CONDUCT.md).

## Before you start

- **Questions and ideas**: ask in
  [Discussions](https://github.com/orgs/XPrismOrg/discussions).
- **Bugs and small fixes**: open an
  [issue](https://github.com/XPrismOrg/xserver/issues) or a pull request.
- **Larger changes** (a new feature, a protocol, anything touching the
  security model): open an issue or discussion first, so we agree on the
  design before you spend time on code. The design docs are in `doc/`.
- **Security issues**: never in public. Report them privately at
  <https://github.com/XPrismOrg/xserver/security/advisories/new>.

## Building

XPrism builds with Meson and Ninja and needs the same libraries as other
X servers (see the README; `.github/scripts/ubuntu/` lists the exact
packages CI installs on Ubuntu).

```
meson setup build
ninja -C build
```

WX11, the Wayland bridge, is a separate Rust program in `wx11/`:

```
cd wx11 && cargo build
```

## Testing

```
meson test -C build                 # everything
meson test -C build pyxtest-test_capability.py   # one file
```

Most tests start their own `Xvfb` and need no display, GPU or root. Tests
that drive real hardware (`test_kms.py`) only run when
`XPRISM_KMS_TESTS=1`, which CI sets in a virtual machine; don't run them
on your own desktop.

What we ask of tests:

- **Every change in behaviour comes with a test**, and every security
  check with a test that shows it refusing.
- **Make sure the test can fail.** Before you submit, break your change
  on purpose (comment out the check, return early) and watch the test
  fail, then restore it. A test that passes either way proves nothing.
- Check exit codes and counts, not just that the output looks right.
- Tests that start a server expecting it to fail should disable core
  dumps (`RLIMIT_CORE` 0).

CI runs the whole suite on Linux (with the built-in compositor off and
on), FreeBSD and OpenBSD, and builds WX11 with `clippy` and `rustfmt`.
A pull request needs a green CI.

## Writing code

- **C**: follow the style of the code around you (4 spaces, the
  existing brace style). New functions and types get a comment saying
  what they do and why; we generate documentation from them.
- **Rust** (`wx11/`): `cargo fmt` and `cargo clippy -- -D warnings`.
- **Python** (`test/pyxtest/`): pytest, in the style of the existing
  tests.
- Keep changes to code inherited from XLibre and X.Org small and
  self-contained; that keeps us able to take their fixes.
- No new OS-specific `#ifdef`s outside the platform abstraction layer
  (`os/pal/`, see `doc/pal.md`); the `os-ifdefs` test enforces this.
- Don't break drivers: the names drivers look for (`xorg-server.pc`,
  `xlibre-server.pc`, `xlibre-server.h`, the module ABI tag) stay as they
  are.
- Security-relevant changes update `doc/capabilities.md` (what is
  checked, and the known gaps).
- New files start with an `SPDX-License-Identifier` line.

## Commits

- One logical change per commit. Refactoring goes in its own commit.
- Subject line: `area: what changed`, short and precise, e.g.
  `capabilities: log legacy (unrestricted) access`. The body explains
  *why*, and what you tested. History is documentation too.
- Sign off your commits (`git commit -s`): the `Signed-off-by` line
  certifies that you may contribute the change under the project's
  license ([Developer Certificate of Origin](https://developercertificate.org/)).
- Stage files by name. Never commit build output, core dumps, logs,
  credentials or personal data; check `git status` before you commit.
- Help from tools, AI included, is fine. You are responsible for every
  line you submit, and you understand and have tested it. Credit
  substantial help in the commit, e.g. with a `Co-Authored-By` line.

## Pull requests

- Target `main`. Keep the pull request focused; several small ones are
  easier to review than one large one.
- Say what the change does, why, and how you tested it. Link the issue.
- Respond to review by pushing new commits or a cleaned-up series; say
  what you changed.
- Bug fixes for a released version also go to that release's branch
  (`release/YY.MM.NN`), as a separate pull request.

## Versions

XPrism versions are `YY.MM.NN`, with an optional `.PP` for a patch
release: `26.11.01` is the first release of November 2026, `26.11.01.01`
its first patch release. The version is set in `meson.build`;
`include/meson.build` checks it.

## License

XPrism is under the MIT/X11 licenses of the code it comes from; see
[COPYING](COPYING). By contributing, you agree that your contribution is
licensed under the same terms.
