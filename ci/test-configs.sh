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

BUILD_DIR=$(mktemp --directory)

find configs -type f | while read -r fname; do
    echo "*** Testing config: $fname"
    rm -rf "${BUILD_DIR}"
    mkdir -p "${BUILD_DIR}"
    make O="${BUILD_DIR}" "$(basename "$fname")"
    make O="${BUILD_DIR}" "-j$(nproc)"
    make O="${BUILD_DIR}" tests
done
