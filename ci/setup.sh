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

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)

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
    curl \
    libzmq3-dev \
    liblua5.2-dev \
    libluajit-5.1-dev \
    libconfig-dev \
    libarchive-dev \
    libbtrfsutil-dev \
    libjson-c-dev \
    libyaml-dev \
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
    libpci-dev \
    gawk \
    cpio \
    meson \
    ninja-build \
    libzstd-dev \
    wget \
    python3

# packages are too old in Ubuntu Jammy
if ! grep -q UBUNTU_CODENAME=jammy /etc/os-release; then
    apt-get install -y \
        libebgenv-dev \
        libmtd-dev \
        libubi-dev \
        libubootenv-dev \
        libzck-dev
else
    "$SCRIPT_DIR/install-src-deps.sh"
fi

PC_FILE_DIR=$(pkg-config --variable=pcfiledir lua5.2)
$_SUDO ln -sf "$PC_FILE_DIR/lua5.2.pc" "$PC_FILE_DIR/lua.pc"
