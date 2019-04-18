==================
Project's road-map
==================

Please take into account that most of the items here are proposals.
I get some ideas talking with customers, some ideas are my own thoughts.
There is no plan when these features will be implemented - this depends
if enough interest is raised and if there will be contribution to the project
in terms of patches or financial contribution to develop a feature.

Thanks again to all companies that have supported my work up now and to
everybody who has contributed to the project, let me bring SWUpdate
to the current status !

Main goal
=========

First goal is to reach a quite big audience, making
SWUpdate suitable for a large number of products.
This will help to build a community around the project
itself.

SWUpdate as Updater Gateway
===========================

This feature was introduced with the "swuforward" handler. It is already
possible to update a tree of devices if each of them runs SWUpdate. This
feature is already implemented in SWUpdate for embedded linux.

Anyway, a lot of embedded devices have small processors and maybe not a full
blown OS. Ensuring security for all of them can be a risk, and it is
easier to make sure on a single device. If the device running SWUpdate is
acting as gateway, it can translate protocols from backend and send
package updates to the connected small devices.

One example could be if SWUpdate runs as MQTT-broker and takes updates
from Hawkbit. SWUpdate should be able to run multiple instances of
suricatta to do this.

One other examples is using LWM2M. The gateway should be generic enough
to allow to add further protocols in future.

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

A handler to update a microcontroller connected via UART is introduced.
It could be enhanced to support other interfaces (SPI for example).

Some ideas for new handlers:
        - handlers to update microcontrollers
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

Handlers installable as plugin at runtime
-----------------------------------------

The project supports Lua as script language for pre- and postinstall
script. It will be easy to add a way for installing a handler at runtime
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

New is meta-swupdate-boards - examples regarding evaluation boards are
put there. Currently, there are examples for Beaglebone Black,
Raspberri PI 3 and Wandboard. Maybe some more boards ? Patches welcome.

Backend support (suricatta mode)
================================

Backend: Hawkbit Offline support
--------------------------------

There are several discussions on Hawkbit's ML about how to synchronize
an offline update (done locally or via the internal Webserver) with
the Hawkbit's server. Currently, Hawkbit thinks to be the only one
deploying software. Hawkbit DDI API should be extended, and afterwards
changes must be implemented in SWUpdate.

Backend: Consolidate "general server"
-------------------------------------

A second OTA server was introduced with 2018.11, but there is not
an open source solution for a server. Anyway, the very simple interface
of the "general server" can be used by anyone to introduce an own server
instead of a more complicate solution with a backend like Hawkbit.

Backend: support for Mender
---------------------------

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
A first version of SWUpdate-GUI was released with a base set of features. The goal of this simple GUI
is to have a low footprint compared to GUI developed with state of art frameworks. 
This lets to still have a rescue that fits in small devices.

Test and Continuos Integration
==============================

The number of configurations and features in SWUpdate is steadily increasing and
it becomes urgent to find a way to test all incoming patch to fix regression issues.
One step in this direction is the support for Travis build - a set of configuration
files is stored with the project and should help to find fast breakages in the build.
More in this direction must be done to perform test on targets. A suitable test framework
should be found. Scope is to have a "SWUpdate factory" where patches are fast integrated
and tested on real hardware.

Documentation
=============

Documentation should be improved. There is just a little documentation for meta-swupdate
how to set it up with different configurations.
