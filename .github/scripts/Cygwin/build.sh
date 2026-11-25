#!/bin/sh
set -x
ccache -s
meson setup --prefix=/usr -Dxv=false -Dxephyr=true -Dxnest=true -Dxvfb=true -Dxorg=true -Dpciaccess=false -Dglamor=false build
meson configure build
ninja -C build
ninja -C build install
