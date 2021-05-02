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
if [ "$(id -u)" = 0 ]; then
    _SUDO=
fi

# prevent tzdata from becoming interactive which causes the build to be stuck
export DEBIAN_FRONTEND=noninteractive
export TZ=Europe/London

$_SUDO apt-get -qq update
$_SUDO apt-get install -y \
    build-essential \
    automake \
    cmake \
    libzmq3-dev \
    liblua5.2-dev \
    libconfig-dev \
    libarchive-dev \
    libjson-c-dev \
    zlib1g-dev \
    git \
    uuid \
    uuid-dev \
    liblzo2-dev \
    libsystemd-dev \
    libsystemd0 \
    check \
    librsync2 \
    librsync-dev \
    libext2fs-dev \
    liburiparser-dev \
    doxygen \
    graphviz \
    autoconf-archive \
    linux-headers-generic \
    libmbedtls-dev \
    libcmocka-dev \
    libfdisk-dev \
    libwebsockets-dev \
    libgpiod-dev \
    libcurl4-openssl-dev \
    gawk \
    cpio \
    wget

$_SUDO ln -sf /usr/lib/x86_64-linux-gnu/pkgconfig/lua5.2.pc /usr/lib/x86_64-linux-gnu/pkgconfig/lua.pc
