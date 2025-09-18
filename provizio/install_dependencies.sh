#!/bin/bash
# SPDX-FileCopyrightText: 2025 Provizio <support@provizio.ai>
#
# SPDX-License-Identifier: GPL-2.0-only

# Usage (Ubuntu 20.04+):
#   sudo ./install_dependencies.sh
#
# Minimal dependencies installer for SWUpdate builds
# No longer installs libubootenv since it's not needed after source code changes

set -euo pipefail
trap 'echo "[install_dependencies] failed at line $LINENO" >&2' ERR

# Run from this script's directory
cd "$(cd -P -- "$(dirname -- "$0")" && pwd -P)"

if [[ $EUID -ne 0 ]]; then
  echo "Root permissions required" >&2
  exit 1
fi

echo "=== Environment ==="
env | sort

echo "=== Install minimal build dependencies ==="
export DEBIAN_FRONTEND=noninteractive

# Update package list only
apt-get update

# Install only the essential build dependencies for SWUpdate
# No libstdc++ manipulation needed since we don't build libubootenv anymore
apt-get install -y --no-install-recommends \
  build-essential \
  libjson-c-dev \
  libssl-dev \
  libarchive-dev \
  libconfig-dev \
  zlib1g-dev

# Ensure cmake exists (minimal version check)
if ! command -v cmake >/dev/null 2>&1; then
  apt-get install -y --no-install-recommends cmake
fi

echo "=== Installation complete ==="
echo "Build tools and libraries installed successfully"
echo "No system library modifications were made"
