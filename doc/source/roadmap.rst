=============================================
Project's road-map
=============================================

Please take into account that most of the items here are proposals.
I get some ideas talking with customers, some ideas are my own thoughts.
There is no plan when these features will be implemented - this depends
if enough interest is raised and if there will be contribution to the project.

Thanks again to all companies that have supported my work up now and to
everybody who contributes to the project, let me bring SWUpdate
to the current status !

SWUpdate's road-map
===================

Main goal
---------

First goal is to reach a quite big audience, making
SWUpdate suitable for a large number of products.
This will help to build a community around the project
itself.

Encryption of artifacts
-----------------------

Images can be verified to check for their authenticity. Anyway,
thare are cases where it is requested that the artifacts are
encrypted, and decrypted on the fly before installing.

Deployment in the IoT world
---------------------------

The thing is not just updating the single device, but all
devices in the field having a consistent status.

Interface with the world - progress status
------------------------------------------

The interface to the outside world is currently poor. If a target
has a display and desires to show the progress of the update,
it must fight with the poor status message. There is no standard way to
get a percentage.
An improvement is desired, as well as having a simple example to
display the status using the Linux framebuffer.

Binary delta updates
--------------------

A whole update could be very traffic intensive. Specially in case
of low-bandwidth connections, it could be interesting to introduce
a way for delta binary updates. Of course, the feature should not
introduce leaks and make the system more vulnerable.

Handlers desired
----------------

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
