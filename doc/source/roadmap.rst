==================
Project's road-map
==================

Please take into account that most of the items here are proposals.
I get some ideas talking with customers, some ideas are my own thoughts.
There is no plan when these features will be implemented - this depends
if enough interest is raised and if there will be contribution to the project.

Thanks again to all companies that have supported my work up now and to
everybody who has contributed to the project, let me bring SWUpdate
to the current status !

Main goal
=========

First goal is to reach a quite big audience, making
SWUpdate suitable for a large number of products.
This will help to build a community around the project
itself.

SWUpdate als Updater Gateway
============================

A lot of embedded devices have small processors and maybe not a full
blown OS. Ensuring security for all of them can be a risk, and it is
easier to make sure a single device. If the device running SWUpdate is
acting as gateway, it can translate protocols from backend and send
package update to the connected small devices.

One example could be if SWUpdate runs as MQTT-broker and takes updates
from Hawkbit. SWUpdate should be able to run multiple instances of
suricatta to do this.

Binary delta updates
====================

A whole update could be very traffic intensive. Specially in case
of low-bandwidth connections, it could be interesting to introduce
a way for delta binary updates.
There was already several discussions on the Mailing List about
this. If introducing binary delta is high desired, on the other side
it is strictly required to not reduce the reliability of the update
and the feature should not introduce leaks and make the system
more vulnerable.

There are two major aspects to be considered for binary deltas
that must be investigated:

- which is the reference ? A delta means there is a untouched copy
  of the software that can be used as base. It must be verified
  that this copy is not corrupted or changed after its delivery
  and it is suitable for an update. Nevertheless, it should be
  avoided to waste resources and space on the target just to store
  a further copy of the software.
- resources applying binary deltas. Known mechanism uses a lot of
  memory because they do in-memory patching. It is required to have
  memory usage under control.
- in case of backend, do we need some sort of communication ?
  SWUpdate could communicate the version running and the backend could
  be able to compute on-the-fly the delta package, and also check
  if it is worth to transfer the delta or switch to the whole image.

Handlers:
=========

New Handlers
------------

Surely the implemented handlers cover the majority of cases. Anyway,
new methods can be considered, and new handlers can be added to mainline
to support special ways. For example, downloading an image to a separate
microprocessor/micro-controller, downloading a bit-stream to a FPGA,
and so on.
There are custom specific solutions - I will be glad if these additional
handlers will be merged into mainline in the future.

Flash handler
-------------

The flash handler for raw-devices (mainly NOR flashes) does not allow to
stream the image and an error is reported if "installed-directly" is set.
The handler can be extended to stream images.

Handlers installable as plugin at runtime
-----------------------------------------

The project supports LUA as script language for pre- and postinstall
script. It will be easy to add a way for installing a handler at runtime
written in LUA, allowing to expand SWUpdate to the cases not covered
in the design phase of a product.

Of course, this issue is related to the security features: it must be
ensured that only verified handlers can be added to the system to avoid
that malware can get the control of the target.

Current release supports verified images. That means that a handler
written in LUA could be now be part of the compound image, because
a unauthenticated handler cannot run.

Support for evaluation boards
=============================

New is meta-swupdate-boards - examples regarding evaluation boards are
put there. Currently, there are examples for Beaglebone Black and
Raspberri PI 3.

Backend support (suricatta mode)
================================

Backend: Support for other Servers
----------------------------------

SWUpdate supports Hawkbit, but SWUpdate is unaware about which
backend is used. Further connectors can be implemented to connect to
other type of backend solutions.

It is surely desired to have SWUpdate compatible with the most
deployment system, and any user project can decide which is their
backend solution.

Support for multiple Servers simultaneously
-------------------------------------------

Currently, suricatta's server backends are a mutually exclusive
compile-time choice. A proxy registrar and dispatcher should be plugged
into the architecture to allow for different channels and server
backends to be mixed and matched at run-time.

Webserver / Interface
=====================

On most systems, the interface shown in browser seems enough. However, it looks
to me quite old and it could be really improved. Some ideas:

- drop the polling mechanism and switch to a websocket implementattion. This has
  the big advantage that a nice interface with progress bars, status, and so on
  can be realized.
- website graphic is old and IMHO quite nasty. Am I the only one to think this (and I
  am the one who developped it..) ? I see a lot of devices running the website  with the 
  same graphic and just replacing the Logo. I think it could be really better as now.
- I have forwarded the traces to the browser just to show how we can debug issues and
  check what is wrong during an update - but I have not thought to let it on for
  the end products. They should be at least activated in some conditions, not always.

Documentation
=============

Documentation should be improved. There is just a little documentation for meta-swupdate
how to set it up with different configurations.
