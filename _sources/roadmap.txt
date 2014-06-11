=============================================
Project's roadmap
=============================================

swupdate's roadmap
==================

Main goal
---------

First goal is to reach a quite big audience, making
swupdate suitable for a large number of products.
This will help to build a community around the project
itself.

Security features
-----------------

There is currently no check if the images to be installed
are coming from a verified source. Any image originally built
according to the rules in the documentation is accepted.

However, there is some draft for using signed images. This will
guarantee that only images from an autenticated source (that is
the manufacturer of the product) can be installed on the target.

Handlers desired
----------------

Surely the implemented handlers cover the majority of cases. Anyway,
new methods can be considered, and new handlers can be added to mainline
to support special ways. For example, downloading an image to a separate
microprocessor/microcontroller, downloading a bitstream to a FPGA,
and so on.

Handlers installable as plugin at runtime
-----------------------------------------

The project supports LUA as script language for pre- and postinstall
script. It will be easy to add a way for installing a handler at runtime
written in LUA, allowing to expand swupdate to the cases not covered
in the design phase of a product.

Of course, this issue is related to the security features: it must be
ensured that only verified handlers can be added to the system to avoid
that malware can get the control of the target.

Images fully streamed
---------------------

In case of remote update, swupdate extracts relevant images
from the stream and copy them into /tmp before calling the handlers.
This guarantee that an update is initiated only if all parts are present and correct.
However, on some systems with less resources, the amount of RAM
to copy the images could be not enough, for example if the filesystem on
an attached SD Card must be updated. In this case, it will help if the images
are installed directly as stream by the corresponding handler, without temporary
copies. The API of the handlers is already thought to support a full stream
(the file descriptor of the stream is passed to the handler and set to the start
of the image to be installed).
