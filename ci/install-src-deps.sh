#!/bin/sh
# Copyright (c) Siemens AG, 2021
#
# Authors:
#  Michael Adler <michael.adler@siemens.com>
#
# This work is licensed under the terms of the GNU GPL, version 2.  See
# the COPYING file in the top-level directory.
#
# SPDX-License-Identifier:	GPL-2.0-only
set -eu

_SUDO=sudo
if [ $(id -u) = 0 ]; then
    _SUDO=
fi

install_mtd_utils() {
    $_SUDO mkdir -p /usr/local/lib
    $_SUDO mkdir -p /usr/local/include
    $_SUDO mkdir -p /usr/local/include/mtd
    rm -rf /tmp/mtd-utils
    git clone https://github.com/sigma-star/mtd-utils /tmp/mtd-utils
    cd /tmp/mtd-utils
    git checkout v2.0.0
    ./autogen.sh
    ./configure
    make -j$(nproc)
    $_SUDO install -m 644 include/libubi.h /usr/local/include
    $_SUDO install -m 644 include/libmtd.h /usr/local/include
    $_SUDO install -m 644 include/mtd/ubi-media.h /usr/local/include/mtd
    $_SUDO install -m 644 *.a /usr/local/lib
}

install_libubootenv() {
    rm -rf /tmp/libubootenv
    git clone https://github.com/sbabic/libubootenv.git /tmp/libubootenv
    cd /tmp/libubootenv
    cmake .
    make -j$(nproc)
    $_SUDO make install
}

install_efibootguard() {
    rm -rf /tmp/efibootguard
    git clone https://github.com/siemens/efibootguard.git /tmp/efibootguard
    cd /tmp/efibootguard
    git submodule update --init
    autoreconf -fi
    ./configure --disable-bootloader
    make -j$(nproc)
    $_SUDO make install
}

install_zchunk() {
    rm -rf /tmp/zchunk
    git clone https://github.com/zchunk/zchunk /tmp/zchunk
    cd /tmp/zchunk
    meson build
    cd build
    ninja
    $_SUDO ninja install
}

install_mtd_utils
install_libubootenv
install_efibootguard
install_zchunk
$_SUDO ldconfig
