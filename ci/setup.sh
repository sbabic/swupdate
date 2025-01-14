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

$_SUDO apt-get -qq update && apt-get install --yes --no-install-recommends \
        cpio \
        curl \
        gawk \
        gcc \
        git \
        gulp \
        libarchive-dev \
        libblkid-dev \
        libbtrfsutil-dev \
        libcmocka-dev \
        libconfig-dev \
        libcurl4-openssl-dev \
        libczmq-dev \
        libext2fs-dev \
        libfdisk-dev \
        libgpiod-dev \
        libjson-c-dev \
        liblua5.2-dev \
        libluajit-5.1-dev \
        libmbedtls-dev \
        librsync-dev \
        libssl-dev \
        libsystemd-dev \
        libudev-dev \
        liburiparser-dev \
        libwebsockets-dev \
        libzstd-dev \
        make \
        npm \
        python3 \
        uuid-dev \
        zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

# packages are too old in Ubuntu Jammy and Debian Bookworm
if ! grep -qP "VERSION_CODENAME=(jammy|bookworm)" /etc/os-release; then
    $_SUDO apt-get -qq update && apt-get install --yes --no-install-recommends \
            libebgenv-dev \
            libmtd-dev \
            libubi-dev \
            libubootenv-dev \
            libzck-dev \
        && rm -rf /var/lib/apt/lists/*
else
    $_SUDO apt-get -qq update && apt-get install --yes --no-install-recommends \
            autoconf \
            autoconf-archive \
            automake \
            check \
            cmake \
            liblzo2-dev \
            libtool \
            libyaml-dev \
            meson \
        && rm -rf /var/lib/apt/lists/*
    "$SCRIPT_DIR/install-src-deps.sh"
fi
