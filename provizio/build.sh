#!/bin/bash

# SPDX-FileCopyrightText: 2025 Provizio <support@provizio.ai>
#
# SPDX-License-Identifier: GPL-2.0-only

# SWUpdate Build Script
# This script builds SWUpdate with Provizio-specific configuration
# Usage: ./build.sh [config_file]

set -euo pipefail

# Default configuration
DEFAULT_CONFIG="configs/provizio-jetson-nano.config"
CONFIG_FILE="${1:-$DEFAULT_CONFIG}"

# Change to repository root
cd "$(dirname "$0")/.."

echo "=== SWUpdate Build Script ==="
echo "Configuration: $CONFIG_FILE"
echo "Working directory: $(pwd)"
echo "Build started at: $(date)"

# Clean previous build
echo "=== Cleaning previous build ==="
make mrproper || true

# Configure SWUpdate
echo "=== Configuring SWUpdate ==="
if [ ! -f "$CONFIG_FILE" ]; then
    echo "ERROR: Configuration file '$CONFIG_FILE' not found"
    exit 1
fi

make KCONFIG_ALLCONFIG="$CONFIG_FILE" allnoconfig

# Verify critical configuration settings
echo "=== Verifying configuration settings ==="
echo "SWUPDATE_VARS: $(grep 'CONFIG_SWUPDATE_VARS' .config || echo 'not set')"
echo "UBOOT: $(grep 'CONFIG_UBOOT' .config || echo 'not set')"
echo "HAVE_LIBUBOOTENV: $(grep 'CONFIG_HAVE_LIBUBOOTENV' .config || echo 'not set')"
echo "WEBSERVER: $(grep 'CONFIG_WEBSERVER' .config || echo 'not set')"
echo "MONGOOSE: $(grep 'CONFIG_MONGOOSE' .config || echo 'not set')"
echo "SSL_IMPL: $(grep 'CONFIG_SSL_IMPL' .config || echo 'not set')"

# Build SWUpdate
echo "=== Building SWUpdate ==="
NPROC=$(nproc)
echo "Building with $NPROC parallel jobs"
make -j"$NPROC" swupdate_unstripped

# Verify build output
echo "=== Verifying build output ==="
if [ ! -f "swupdate_unstripped" ]; then
    echo "ERROR: Build failed - swupdate_unstripped not found"
    exit 1
fi

# Check binary dependencies
echo "=== Checking binary dependencies ==="
echo "DT_NEEDED:"
readelf -d swupdate_unstripped | awk -F'[][]' '/Shared library/{print $2}' || true

# Verify no libubootenv dependency (critical for Provizio config)
if readelf -d swupdate_unstripped | grep -q 'libubootenv\.so'; then
    echo "ERROR: swupdate links to libubootenv - this violates Provizio configuration requirements" >&2
    exit 1
fi

# Create output directory and copy artifacts
echo "=== Preparing build artifacts ==="
mkdir -p out
cp -v swupdate_unstripped out/
cp -v .config out/build.config
make savedefconfig && mv -v defconfig out/defconfig.generated

# Build summary
echo "=== Build Summary ==="
echo "Build completed successfully at: $(date)"
echo "Binary: $(ls -lh swupdate_unstripped)"
echo "Artifacts saved to: out/"
echo "Configuration used: $CONFIG_FILE"
echo ""
echo "Build artifacts:"
ls -la out/

echo "=== Build Complete ==="
