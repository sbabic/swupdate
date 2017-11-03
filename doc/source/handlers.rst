=============================================
Handlers
=============================================

Overview
--------

It is quite difficult to foresee all possible installation cases.
Instead of trying to find all use cases, SWUpdate let the
developer free to add his own installer (that is, a new **handler**),
that must be responsible to install an image of a certain type.
An image is marked to be of a defined type to be installed with
a specific handler.

The parser make the connection between 'image type' and 'handler'.
It fills a table containing the list of images to be installed
with the required handler to execute the installation. Each image
can have a different installer.

Supplied handlers
-----------------

In mainline there are the handlers for the most common cases. They include:
	- flash devices in raw mode (both NOR and NAND)
	- UBI volumes
	- raw devices, such as a SD Card partition
	- bootloader (U-Boot, GRUB) environment
	- Lua scripts

For example, if an image is marked to be updated into a UBI volume,
the parser must fill a supplied table setting "ubi" as required handler,
and filling the other fields required for this handler: name of volume, size,
and so on.

Creating own handlers
---------------------

SWUpdate can be extended with new handlers. The user needs to register his own
handler with the core and he must provide the callback that SWUpdate uses when
an image required to be installed with the new handler.

The prototype for the callback is:

::

	int my_handler(struct img_type *img,
		void __attribute__ ((__unused__)) *data)


The most important parameter is the pointer to a struct img_type. It describes
a single image and inform the handler where the image must be installed. The
file descriptor of the incoming stream set to the start of the image to be installed is also
part of the structure.

The structure *img_type* contains the file descriptor of the stream pointing to the first byte
of the image to be installed. The handler must read the whole image, and when it returns
back SWUpdate can go on with the next image in the stream.

SWUpdate provides a general function to extract data from the stream and copy
to somewhere else:

::

        int copyfile(int fdin, int fdout, int nbytes, unsigned long *offs,
                int skip_file, int compressed, uint32_t *checksum, unsigned char *hash);

fdin is the input stream, that is img->fdin from the callback. The *hash*, in case of
signed images, is simply passed to copyfile() to perform the check, exactly as the *checksum*
parameter. copyfile() will return an error if checksum or hash do not match. The handler
does not need to bother with them.
How the handler manages the copied data, is specific to the handler itself. See
supplied handlers code for a better understanding.

The handler's developer registers his own handler with a call to:

::

	__attribute__((constructor))
	void my_handler_init(void)
	{
		register_handler("mytype", my_handler, my_mask, data);
	}

SWUpdate uses the gcc constructors, and all supplied handlers are registered
when SWUpdate is initialized.

register_handler has the syntax:

::

	register_handler(my_image_type, my_handler, my_mask, data);

Where:

- my_image_type : string identifying the own new image type.
- my_handler : pointer to the installer to be registered.
- my_mask : ``HANDLER_MASK`` enum value(s) specifying what
  input type(s) my_handler can process.
- data : an optional pointer to an own structure, that SWUpdate
  saves in the handlers' list and pass to the handler when it will
  be executed.

Handler for UBI Volumes
-----------------------

The handler for UBI volumes is thought to update UBI volumes
without changing the layout of the storage.
Volumes must be set before: the handler does not create volumes
itself. It searches for a volume in all MTD (if they are not
blacklisted: see UBIBLACKLIST) to find the volume where the image
must be installed. For this reason, volumes must be unique inside
the system. Two volumes with the same names are not supported
and drives to unpredictable results. SWUpdate will install
an image to the first volume that matches with the name, and this
maybe is not the desired behavior.
Updating volumes, it is guaranteed that the erase counters are
preserved and not lost after an update. The way for updating
is identical to the "ubiupdatevol" from the mtd-utils. In fact,
the same library from mtd-utils (libubi) is reused by SWUpdate.

If the storage is empty, it is required to setup the layout
and create the volumes. This can be easy done with a
preinstall script. Building with meta-SWUpdate, the original
mtd-utils are available and can be called by a Lua script.

Lua Handlers
------------

In addition to the handlers written in C, it is possible to extend
SWUpdate with handlers written in Lua that get loaded at SWUpdate
startup. The Lua handler source code file may either be embedded
into the SWUpdate binary via the ``CONFIG_EMBEDDED_LUA_HANDLER``
config option or has to be installed on the target system in Lua's
search path as ``swupdate_handlers.lua`` so that it can be loaded
by the embedded Lua interpreter at run-time.

In analogy to C handlers, the prototype for a Lua handler is

::

        function lua_handler(image)
            ...
        end

where ``image`` is a Lua table (with attributes according to
:ref:`sw-description's attribute reference <sw-description-attribute-reference>`)
that describes a single artifact to be processed by the handler. 

Note that dashes in the attributes' names are replaced with
underscores for the Lua domain to make them idiomatic, e.g.,
``installed-directly`` becomes ``installed_directly`` in the
Lua domain.

To register a Lua handler, the ``swupdate`` module provides the
``swupdate.register_handler()`` method that takes the handler's
name, the Lua handler function to be registered under that name,
and, optionally, the types of artifacts for which the handler may
be called. If the latter is not given, the Lua handler is registered
for all types of artifacts. The following call registers the
above function ``lua_handler`` as *my_handler* which may be
called for images:

::

        swupdate.register_handler("my_handler", lua_handler, swupdate.HANDLER_MASK.IMAGE_HANDLER)


A Lua handler may call C handlers ("chaining") via the
``swupdate.call_handler()`` method. The callable and registered
C handlers are available (as keys) in the table
``swupdate.handler``. The following Lua code is an example of
a simple handler chain-calling the ``rawfile`` C handler:

::

        function lua_handler(image)
            if not swupdate.handler["rawfile"] then
                swupdate.error("rawfile handler not available")
                return 1
            end
            image.path = "/tmp/destination.path"
            local err, msg = swupdate.call_handler("rawfile", image)
            if err ~= 0 then
                swupdate.error(string.format("Error chaining handlers: %s", msg))
                return 1
            end
            return 0
        end

Note that when chaining handlers and calling a C handler for
a different type of artifact than the Lua handler is registered
for, the ``image`` table's values must satisfy the called
C handler's expectations: Consider the above Lua handler being
registered for "images" (``swupdate.HANDLER_MASK.IMAGE_HANDLER``)
via the ``swupdate.register_handler()`` call shown above. As per the 
:ref:`sw-description's attribute reference <sw-description-attribute-reference>`,
the "images" artifact type doesn't have the ``path`` attribute
but the "file" artifact type does. So, for calling the ``rawfile``
handler, ``image.path`` has to be set prior to chain-calling the
``rawfile`` handler, as done in the example above. Usually, however,
no such adaptation is necessary if the Lua handler is registered for
handling the type of artifact that ``image`` represents.

In addition to calling C handlers, the ``image`` table passed as
parameter to a Lua handler has a ``image:copy2file()`` method that
implements the common use case of writing the input stream's data
to a file, which is passed as this method's argument. On success,
``image:copy2file()`` returns ``0`` or ``-1`` plus an error
message on failure. The following Lua code is an example of
a simple handler calling ``image:copy2file()``:

::

        function lua_handler(image)
            local err, msg = image:copy2file("/tmp/destination.path")
            if err ~= 0 then
                swupdate.error(string.format("Error calling copy2file: %s", msg))
                return 1
            end
            return 0
        end

Beyond using ``image:copy2file()`` or chain-calling C handlers,
the ``image`` table passed as parameter to a Lua handler has
a ``image:read(<callback()>)`` method that reads from the input
stream and calls the Lua callback function ``<callback()>`` for
every chunk read, passing this chunk as parameter. On success,
``0`` is returned by ``image:read()``. On error, ``-1`` plus an
error message is returned. The following Lua code is an example
of a simple handler printing the artifact's content:

::

        function lua_handler(image)
            err, msg = image:read(function(data) print(data) end)
            if err ~= 0 then
                swupdate.error(string.format("Error reading image: %s", msg))
                return 1
            end
            return 0
        end

Using the ``image:read()`` method, an artifact's contents may be
(post-)processed in and leveraging the power of Lua without relying
on preexisting C handlers for the purpose intended.


Just as C handlers, a Lua handler must consume the artifact 
described in its ``image`` parameter so that SWUpdate can 
continue with the next artifact in the stream after the Lua handler
returns. Chaining handlers, calling ``image:copy2file()``, or using 
``image:read()`` satisfies this requirement.


Note that although the dynamic nature of Lua handlers would
technically allow to embed them into a to be processed ``.swu``
image, this is not implemented as it carries some security
implications since the behavior of SWUpdate is changed
dynamically.

Remote handlers
---------------

Remote handlers are thought for binding legacy installers
without having the necessity to rewrite them in Lua. The remote
handler forward the image to be installed to another process,
waiting for an acknowledge to be sure that the image is installed
correctly.
The remote handler makes use of the zeromq library - this is
to simplify the IPC with Unix Domain Socket. The remote handler
is quite general, describing in sw-description with the
"data" attribute how to communicate with the external process.
The remote handler always acts as client, and try a connect()
using the socket identified by the "data" attribute. For example,
a possible setup using a remote handler could be:

::

        images: (
                {
                    filename = "myimage"";
                    type = "remote";
                    data = "test_remote";
                 }
        )


The connection is instantiated using the socket "/tmp/test_remote". If
connect() fails, the remote handler signals that the update is not successful.
Each Zeromq Message from SWUpdate is a multi-part message split into two frames:

        - first frame contains a string with a command.
        - second frame contains data and can be of 0 bytes.

There are currently just two possible commands: INIT and DATA. After
a successful connect, SWUpdate sends the initialization string in the
format:


::
        
        INIT:<size of image to be installed>

The external installer is informed about the size of the image to be
installed, and it can assign resources if it needs. It will answer
with the string *ACK* or *NACK*. The first NACK received by SWUpdate
will interrupt the update. After sending the INIT command, the remote
handler will send a sequence of *DATA* commands, where the second
frame in message will contain chunks of the image to be installed.
It is duty of the external process to take care of the amount of
data transferred and to release resources when the last chunk
is received. For each DATA message, the external process answers with a
*ACK* or *NACK* message.
