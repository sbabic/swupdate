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
    autoconf-archive \
    automake \
    build-essential \
    check \
    cmake \
    cpio \
    curl \
    doxygen \
    gawk \
    git \
    graphviz \
    libarchive-dev \
    libbtrfsutil-dev \
    libcmocka-dev \
    libconfig-dev \
    libcurl4-openssl-dev \
    libext2fs-dev \
    libfdisk-dev \
    libgpiod-dev \
    libjson-c-dev \
    liblua5.2-dev \
    libluajit-5.1-dev \
    liblzo2-dev \
    libmbedtls-dev \
    libpci-dev \
    librsync-dev \
    librsync2 \
    libsystemd-dev \
    libsystemd0 \
    liburiparser-dev \
    libwebsockets-dev \
    libyaml-dev \
    libzmq3-dev \
    libzstd-dev \
    linux-headers-generic \
    meson \
    ninja-build \
    python3 \
    uuid \
    uuid-dev \
    wget \
    zlib1g-dev

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
