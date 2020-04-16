<p align ="center"><img src=SWUpdate.svg width=400 height=400 /></p>

SWUpdate - Software Update for Embedded Systems
===============================================

[![Build Status](https://travis-ci.org/sbabic/swupdate.svg?branch=master)](https://travis-ci.org/sbabic/swupdate)
[![Coverity Scan Build Status](https://scan.coverity.com/projects/20753/badge.svg)](https://scan.coverity.com/projects/20753)

SWUpdate is a Linux Update agent with the goal to
provide an efficient and safe way to update
an embedded system. SWUpdate supports local and remote
updates, multiple update strategies and it can
be well integrated in the [Yocto](https://www.yoctoproject.org) build system by adding
the [meta-swupdate](https://layers.openembedded.org/layerindex/branch/master/layer/meta-swupdate/) layer.

Examples using this layer with evaluation boards (Beaglebone, RPI3) are provided in 
[meta-swupdate-boards](https://layers.openembedded.org/layerindex/branch/master/layer/meta-swupdate-boards/) layer.

It supports the common media on embedded devices 
such as NOR / NAND flashes, UBI volumes, SD / eMMC, and can
be easily extended to introduce project specific update
procedures.

Pre- and postinstall scripts are supported, and a Lua
interpreter helps to customize the update process.

An update package is described by the sw-description file,
using the libconfig syntax or JSON. It is even possible to
use Lua with a custom syntax.

Here a short list of the main features:

- Install on embedded media (eMMC, SD, Raw NAND, NOR and SPI-NOR flashes)
- Allow delivery single image for multiple devices
- Multiple interfaces for getting software
    - local storage
    - integrated web server
    - integrated REST client connector to [hawkBit](https://projects.eclipse.org/projects/iot.hawkbit)
    - remote server download
- Software delivered as images, gzipped tarball, etc.
- Allow custom handlers for installing FPGA firmware, microcontroller firmware via custom protocols.
- Power-Off safe
- Hardware / Software compatibility.

More on http://sbabic.github.io/swupdate/swupdate.html

Different components of this software are under different licenses (a mix
of MIT, GPLv2 and GPLv2+). License information for any file is either explicitly stated
or defaults to GPL version 2.0+.

Please check inside doc directory for documentation or
the online documentation (generated from doc/) at:
http://sbabic.github.io/swupdate


Contributing to the project
---------------------------

Contributions are welcome !  You can submit your patches (or post questions
regarding the project) to the swupdate Mailing List:

	swupdate@googlegroups.com

Please read the [contributing](http://sbabic.github.io/swupdate/contributing.html)
chapter in the documentation how to contribute to the project.
