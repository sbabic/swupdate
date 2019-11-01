==================
Project's road-map
==================

Please take into account that most of the items here are *proposals*.
I get some ideas talking with customers, some ideas are my own thoughts.
There is no plan when these features will be implemented - this depends
if there will be contribution to the project in terms of patches or
financial contributions to develop a feature.

Thanks again to all companies that have supported my work up now and to
everybody who has contributed to the project, let me bring SWUpdate
to the current status !

Main goal
=========

First goal is to reach a quite big audience, making
SWUpdate suitable for a large number of products.
This will help to build a community around the project
itself.

Disk partitions
===============

SWUpdate is able to set and handle UBI partitions, while it requires external
tools to set up disk partitions (sfdisk / fdisk). Because an update should be self-contained, it is
desirable that SWUpdate will integrate the code to be able to set up partitions
configured in sw-descriptions even for disks (eMMC / SD / Hard-disks).
This will let to use `partitions` inside sw-description to set up disk partitions
and not only UBI volumes, and add further features as restoring configuration data and so on.

Support for further compressors
===============================

SWUpdate supports image compressed with following formats: zlib, zstd. This is
a compromise between compression rate and speed to decompress the single artifact.
To reduce bandwidth or for big images, a stronger compressor could help.
Adding a new compressor must be careful done because it changes the core of
handling an image.

Binary delta updates
====================

A whole update could be very traffic intensive. Specially in case
of low-bandwidth connections, it could be interesting to introduce
a way for delta binary updates.
There was already several discussions on the Mailing List about
this. If introducing binary delta is high desired, on the other side
it is strictly required to not reduce the reliability of the update
and the feature should not introduce leaks and make the system
more vulnerable. It is accepted that different technologies could be added,
each of them solves a specific use case for a delta update.

SWUpdate is already able to perform delta updates based on librsync library. This is
currently a good compromise to reduce complexity. Anyway, this helps in case of
small changes, and it is not a general solution between two generic releases.
A general approach could be to integrate SWUpdate with CA-Sync to allow a delta upgrade
from any release. First proof of concept shows that changes in both SWUpdate and CA-Sync
are required to be conform with requirements and security concepts in SWUpdate. A design
just using CA-Sync as external fetcher without integration in SWUpdate  breaks
SWUpdate's security concept.

Integration in Linux distro
===========================

To allow an easier learning with SWUpdate and also for test purposes with the
SWU forwarder handler, it makes sense to package SWUpdate for PC Linux distro.
SWUpdate already supports debian package. Some help from community is asked to
let the package merged into Debian distro.

Handlers:
=========

New Handlers
------------

Users develop own custom handlers - I just enforce and encourage everyone
to send them and discuss how to integrate custom handler in mainline.

A handler to update a micro-controller connected via UART is introduced.
It could be enhanced to support other interfaces (SPI for example).

Some ideas for new handlers:
        - handlers to update micro-controllers
        - FPGA updater for FPGA with Flash
        - Package handler to install packages (ipk, deb)
          Packages can be inserted into the SWU and the atomicity is
          guaranteed by SWUpdate.
        - Lua handlers should be added if possible to the project
          to show how to solve custom install.


Flash handler
-------------

The flash handler for raw-devices (mainly NOR flashes) does not allow to
stream the image and an error is reported if "installed-directly" is set.
The handler can be extended to stream images.

Handlers install-able as plugin at runtime
------------------------------------------

The project supports Lua as script language for pre- and postinstall
script. It will be easy to add a way for installing a handler at run-time
written in Lua, allowing to expand SWUpdate to the cases not covered
in the design phase of a product.

Of course, this issue is related to the security features: it must be
ensured that only verified handlers can be added to the system to avoid
that malware can get the control of the target.

Current release supports verified images. That means that a handler
written in Lua could be now be part of the compound image, because
a unauthenticated handler cannot run.

Support for evaluation boards
=============================

meta-swupdate-boards contains examples with evaluation boards.
Currently, there are examples using Beaglebone Black,
Raspberri PI 3 and Wandboard. The repo is a community driven project:
patches welcome.

Back-end support (suricatta mode)
=================================

Back-end: check before installing
---------------------------------

In some cases (for example, where bandwidth is important), it is better to check
if an update must be installed instead of installing and performs checks later.
If SWUpdate provides a way to inform a checker if an update can be accepted
before downloading, a download is only done when it is really necessary.

Back-end: Hawkbit Offline support
---------------------------------

There are several discussions on Hawkbit's ML about how to synchronize
an offline update (done locally or via the internal Web-server) with
the Hawkbit's server. Currently, Hawkbit thinks to be the only one
deploying software. Hawkbit DDI API should be extended, and afterwards
changes must be implemented in SWUpdate.

Back-end: support for generic down-loader 
-----------------------------------------

SWUpdate in down-loader mode works as one-shot: it simply try to download a SWU
from a URL. For simple applications, it could be moved into `suricatta` to detect
if a new version is available before downloading and installing.

Back-end: support for Mender
----------------------------

There was several discussion how to make a stronger collaboration between
different update solution and a proposal discussed previously is to use SWUpdate as client
to upgrade from a Mender server, see `BOF at ELCE 2017 <https://elinux.org/images/0/0c/BoF_secure_ota_linux.pdf>`_

Support for multiple Servers simultaneously
-------------------------------------------

Currently, suricatta's server backends are a mutually exclusive
compile-time choice. There is no interest to have multiple OTA at the same time.
This feature won't be implemented and I will remove this from roadmap if no
interest will be waked up.

SWUpdate GUI for rescue
=======================

In case of rescue for HMI devices, it is often required to have a small GUI
for an operator to set some parameters (network,..) and start an update.
SWUpdate-GUI is released with a base set of features. The goal of this simple GUI
is to have a low footprint compared to GUI developed with state of art frameworks. 
This lets to still have a rescue that fits in small devices.
SWUpdate-GUI is already production-ready and delivered into final products. New
features coud be developped.

Test and Continuous Integration
===============================

The number of configurations and features in SWUpdate is steadily increasing and
it becomes urgent to find a way to test all incoming patch to fix regression issues.
One step in this direction is the support for Travis build - a set of configuration
files is stored with the project and should help to find fast breakages in the build.
More in this direction must be done to perform test on targets. A suitable test framework
should be found. Scope is to have a "SWUpdate factory" where patches are fast integrated
and tested on real hardware.

Documentation
=============

Documentation is a central point in SWUpdate - maintaining it up to date is a must in this project. 
