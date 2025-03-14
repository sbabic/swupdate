<!--
SPDX-FileCopyrightText: 2025 Daniel Braunwarth <oss@braunwarth.dev>

SPDX-License-Identifier: GPL-2.0-only
-->

# Kconfiglib

This directory contains the Python Kconfig library Kconfiglib.

The library is maintained by the Zephyr project at
<https://github.com/zephyrproject-rtos/Kconfiglib>

## Update instructions

Just copy all `kconfiglib.py` and the scripts `*config.py` script we need from the official repository to this directory.

The following changes are currently not merged but applied here:

- [fix crash caused by unsupported locales](https://github.com/zephyrproject-rtos/Kconfiglib/pull/1)
