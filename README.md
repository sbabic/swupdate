<!--
SPDX-FileCopyrightText: 2013 Stefano Babic <sbabic@denx.de>

SPDX-License-Identifier: GPL-2.0-only
-->

<p align ="center"><img src=SWUpdate.svg width=200 height=200 /></p>

SWUpdate - Software Update for Embedded Linux Devices
=====================================================

[![Build Status](https://travis-ci.org/sbabic/swupdate.svg?branch=master)
](https://travis-ci.org/sbabic/swupdate)
[![Coverity Scan Build Status](https://scan.coverity.com/projects/20753/badge.svg)](https://scan.coverity.com/projects/20753)
![License](https://img.shields.io/github/license/sbabic/swupdate)

[SWUpdate](https://swupdate.org) is a Linux Update agent with the goal to
provide an efficient and safe way to update
an embedded Linux system in field. SWUpdate supports local and OTA
updates, multiple update strategies and it is designed with security
in mind.

## Getting started

To start with SWUpdate, it is suggested you look at the documentation
and build for one evaluation board (or you run SWUpdate on your host
for a first overview).

## Features

SWUpdate is a framework with a lot of configurable options:

- Update of all components of device (rootfs, kernel, bootloader, microcontroller FW)
- Install on embedded media (eMMC, SD, Raw NAND, UBIFS, NOR and SPI-NOR flashes)
- Partitioner for GPT and MBR partition table
- Allow single image delivery on multiple devices
- Streaming mode without temporary copies of artifacts
- Multiple interfaces (local and OTA) for getting software
    - local storage (USB, etc.)
    - integrated web server
    - integrated REST client connector to [hawkBit](https://projects.eclipse.org/projects/iot.hawkbit) for fleet updates.
    - remote server download
- Software delivered as images, gzipped tarball, etc.
- Allow custom handlers for installing FPGA firmware, microcontroller firmware via custom protocols.
- Delta updates based on librsync.
- Fail safe and atomic update
- Lua interpreter to extend the update rules on your needs
- Hardware / Software compatibility.
- Small footprint to generate a rescue system to restore the device.
- Cryptographic sign and verification of updates
	- support for OpenSSL
	- support for mbedTLS
	- support for WolfSSL
- Encryption of artifacts via symmetric AES key.
- pre and post update scripts
- small resources required.
- controllable via library
- progress interface to report update status to an application / HMI.
- ...and many others.

Take a look at [features](https://swupdate.org/features).

## Technical documentation

Documentation is part of the project and can be generated, or you access
to the [Online Documentation](https://sbabic.github.io/swupdate/swupdate.html).

## Building

SWUpdate is well integrated in the [Yocto](https://www.yoctoproject.org) build system by adding
the [meta-swupdate](https://layers.openembedded.org/layerindex/branch/master/layer/meta-swupdate/) layer.
It is also integrated in [Buildroot](https://github.com/buildroot/buildroot/blob/master/package/swupdate/swupdate.config).
Debian (and Debian-like distributions) has merged a [package](https://packages.debian.org/unstable/swupdate).

Examples using meta-swupdate with evaluation boards (Beaglebone, RPI3) are provided in
[meta-swupdate-boards](https://layers.openembedded.org/layerindex/branch/master/layer/meta-swupdate-boards/) layer.

## License

SWUpdate is released under GPLv2. A library to control SWUpdate is part of the
project and it is released under LGPLv2.1.
License information for any file is either explicitly stated
or defaults to GPL version 2.0. Extension written in Lua are subjected to
Lua license (MIT).

## Community support

A community support takes place on the SWUpdate's Mailing List:

	swupdate@googlegroups.com

The Mailing List is open without need to be registered.

## Contributing to the project

Contributions are welcome !  You can submit your patches (or post questions
regarding the project) to the Mailing List.

Please read the [contributing](http://sbabic.github.io/swupdate/contributing.html)
chapter in the documentation how to contribute to the project.

Patches are collected by [Patchwork](https://patchwork.ozlabs.org/project/swupdate/list)
