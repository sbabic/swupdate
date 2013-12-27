=============================================
Handlers
=============================================

Overview
--------

It is quite difficult to foresee all possible installation cases.
Instead of trying to find all use cases, swupdate let the
developer free to add his own installer (that is, a new **handler**),
that must be responsibleto install an image of a certain type.
An image is marked to be of a defined type to be installed with
a specific handler.

The parser make the connection between 'image type' nad 'handler'.
It fills a table containing the list of images to be installed
with the required handler to execute the installation. Each image
can have a different installer.

Supplied handlers
-----------------

In mainline there are the handler for the most common cases. They includes:
	- NOR flashes
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
