#!/bin/sh
# SPDX-License-Identifier: MIT
#
# XPrism CI, inside a FreeBSD or OpenBSD VM: upstream's package install,
# then build and run the unit tests (PAL generic backend, peer identity,
# capabilities). The pytest suite needs Xvfb plumbing upstream hasn't got
# working on the BSDs yet, so only "unit" runs here (where it is built).
#   $1 = freebsd | openbsd
set -e

os=$1
./.github/scripts/$os/install-pkg.sh
# the unit tests compile XKB keymaps; upstream's FreeBSD list lacks the data
if [ "$os" = freebsd ]; then
    pkg install -y xkeyboard-config
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
# the unit tests are only built along with the Xorg server
if meson test -C _build --list 2>/dev/null | grep -q ':unit$'; then
    meson test -C _build --print-errorlogs unit
else
    echo "--> no unit tests in this configuration (needs -Dxorg=true); build only"
fi
