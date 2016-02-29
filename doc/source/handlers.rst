=============================================
Handlers
=============================================

Overview
--------

It is quite difficult to foresee all possible installation cases.
Instead of trying to find all use cases, swupdate let the
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

In mainline there are the handler for the most common cases. They includes:
	- flash devices in raw mode (both NOR and NAND)
	- UBI volumes
	- raw devices, such as a SD Card partition
	- U-Boot environment
	- LUA scripts

For example, if an image is marked to be updated into a UBI volume,
the parser must fill a supplied table setting "ubi" as required handler,
and filling the other fields required for this handler: name of volume, size,
and so on.

::

	int my_handler(struct img_type *img,
		void __attribute__ ((__unused__)) *data)


The most important parameter is the pointer to a struct img_type. It describes
a single image and inform the handler where the image must be installed. The
file descriptor of the incoming stream set to the start of the image to be installed is also
part of the structure.

The handler developer registers his own handler with a call to:

::

	__attribute__((constructor))
	void my_handler_init(void)
	{
		register_handler("mytype", my_handler, data);
	}

swupdate uses the gcc constructors, and all supplied handlers are registered
when swupdate is initialized.

register_handler has the syntax:

::

	register_handler(my_image_type, my_handler, data);

Where:

- my_image_type : string identifying the own new image type.
- my_handler : pointer to the installer to be registered.
- data : an optional pointer to an own structure, that swupdate
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
and drives to unpredictable results. swupdate will install
an image to the first volume that matches with the name, and this
maybe is not the desired behavior.
Updating volumes, it is guaranteed that the erase counters are
preserved and not lost after an update. The way for updating
is identical to the "ubiupdatevol" from the mtd-utils. In fact,
the same library from mtd-utils (libubi) is reused by swupdate.

If the storage is empty, it is required to setup the layout
and create the volumes. This can be easy done with a
preinstall script. Building with meta-swupdate, the original
mtd-utils are available and can be called by a LUA script.

Extend swupdate with handlers in LUA
------------------------------------

In an experimental phase, it is possible to add handlers
that are not linked to swupdate but that are loaded by
the LUA interpreter. The handlers must be copied into the
root filesystem and are loaded only at the startup.
These handlers cannot be integrated into the image to be installed.
Even if this can be theoretical possible, arise a lot of
security questions, because it changes swupdate's behavior.
