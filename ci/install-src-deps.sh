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
    git clone https://github.com/sigma-star/mtd-utils
    cd mtd-utils
    git checkout -b tmp v2.0.0
    ./autogen.sh
    ./configure
    make
    $_SUDO install -m 644 include/libubi.h /usr/local/include
    $_SUDO install -m 644 include/libmtd.h /usr/local/include
    $_SUDO install -m 644 include/mtd/ubi-media.h /usr/local/include/mtd
    $_SUDO install -m 644 *.a /usr/local/lib
    cd ..
}

install_libubootenv() {
    git clone https://github.com/sbabic/libubootenv.git
    cd libubootenv
    cmake .
    make
    $_SUDO make install
    cd ..
}

install_efibootguard() {
    git clone https://github.com/siemens/efibootguard.git
    cd efibootguard
    autoreconf -fi
    ./configure
    make libebgenv.a
    $_SUDO install -m 644 libebgenv.a /usr/local/lib/libebgenv.a
    $_SUDO install -m 755 -d /usr/include/efibootguard
    $_SUDO install -m 644 include/ebgenv.h /usr/include/efibootguard/ebgenv.h
    cd ..
}

install_mtd_utils
install_libubootenv
install_efibootguard
$_SUDO ldconfig
