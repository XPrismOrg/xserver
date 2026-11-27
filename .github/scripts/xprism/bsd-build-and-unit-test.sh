#!/bin/sh
# SPDX-License-Identifier: MIT
#
# XPrism CI, inside a FreeBSD, OpenBSD, NetBSD or DragonFly BSD VM: upstream's package install,
# then build and run the unit tests and the pytest suite (Xvfb).
#   $1 = freebsd | openbsd | netbsd | DragonFlyBSD (upstream's script dirs)
set -e

os=$1
./.github/scripts/$os/install-pkg.sh
# the unit tests compile XKB keymaps; upstream's FreeBSD and DragonFly
# lists lack the data
if [ "$os" = freebsd ] || [ "$os" = DragonFlyBSD ]; then
    pkg install -y xkeyboard-config
fi

# the pytest suite (test/pyxtest), run against Xvfb
case "$os" in
    # pytest itself for whichever Python the repository builds it for: a
    # glob also matched plugins such as py3xx-aiomisc-pytest, which pytest
    # then loads (and which needs ssl, broken in DragonFly's Python)
    freebsd|DragonFlyBSD) pkg install -y -x '^py3[0-9]+-pytest$' ;;
    openbsd) pkg_add -I py3-test ;;
    netbsd)
        PATH="$PATH:/usr/pkg/bin:/usr/pkg/sbin"
        pkgin -y install py313-test || pkgin -y install py312-test
        # pkgsrc installs it as pytest-3.1x only
        if ! command -v pytest >/dev/null 2>&1; then
            ln -s "$(ls /usr/pkg/bin/pytest-3.* | head -1)" /usr/pkg/bin/pytest
        fi
        ;;
esac

if [ "$os" = netbsd ]; then
    # upstream's install script puts pkgsrc in /usr/pkg and X in /usr/X11R7
    PATH="$PATH:/usr/pkg/bin:/usr/pkg/sbin"
    PKG_CONFIG_PATH="/usr/X11R7/lib/pkgconfig:/usr/pkg/lib/pkgconfig:/usr/pkg/share/pkgconfig:${PKG_CONFIG_PATH:-}"
    export PATH PKG_CONFIG_PATH
fi
if [ "$os" = DragonFlyBSD ]; then
    CFLAGS="${CFLAGS:-} -Wno-typedef-redefinition"
    export CFLAGS
fi

if [ "$os" = openbsd ]; then
    PKG_CONFIG_PATH="/usr/X11R6/lib/pkgconfig:/usr/local/lib/pkgconfig:/usr/local/share/pkgconfig:/usr/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
    CPPFLAGS="-I/usr/X11R6/include -I/usr/local/include ${CPPFLAGS:-}"
    LDFLAGS="-L/usr/X11R6/lib -L/usr/local/lib ${LDFLAGS:-}"
    export PKG_CONFIG_PATH CPPFLAGS LDFLAGS
fi

rm -rf _build
meson setup _build $MESON_ARGS
meson compile -C _build
status=0
# the unit tests are only built along with the Xorg server
if meson test -C _build --list 2>/dev/null | grep -q ':unit$'; then
    meson test -C _build --print-errorlogs unit || status=1
else
    echo "--> no unit tests in this configuration (needs -Dxorg=true)"
fi
meson test -C _build --print-errorlogs --suite pyxtest || status=1
if [ -f _build/meson-logs/testlog.txt ]; then
    grep -E '^ *[0-9]+/[0-9]+ .*(FAIL|TIMEOUT)' _build/meson-logs/testlog.txt || true
fi
exit $status
