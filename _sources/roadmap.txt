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

There is already an example in meta-swupdate regarding
the beaglebone black. Some more examples and use cases
can better explain the different usage of the project.

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

Filesystem-based Persistent Update Status Storage
-------------------------------------------------

Currently, `U-Boot`_'s environment is used to persistently store the
update status across reboots. On systems where U-Boot is not available
or a different bootloader is used, a filesystem- or raw partition-based
persistent storage should be made available to support other bootloaders
such as, e.g., `grub`_.

.. _grub:   https://www.gnu.org/software/grub/
.. _U-Boot: http://www.denx.de/wiki/U-Boot/

Documentation
=============

Documentation should be improved.
