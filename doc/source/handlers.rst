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
        - UBI volumes partitioner
        - raw flashes handler (NAND, NOR, SPI-NOR, CFI interface)
        - disk partitioner
	- raw devices, such as a SD Card partition
	- bootloader (U-Boot, GRUB, EFI Boot Guard) environment
	- Lua scripts handler
        - shell scripts handler
        - rdiff handler
        - readback handler
        - archive (zo, tarballs) handler
        - remote handler
        - microcontroller update handler

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

UBI Volume Handler
------------------

The UBI volume handler will update UBI volumes without changing the
layout on the storage. Therefore, volumes must be created/adjusted
beforehand. This can be done using the ``partitions`` tag (see
:ref:`partitions-ubi-layout`).

The UBI volume handler will search for volumes in all MTD devices
(unless blacklisted, see UBIBLACKLIST) to find the volume into which
the image shall be installed. For this reason, **volume names must be
unique** within the system. Two volumes with the same name are not
supported and will lead to unpredictable results (SWUpdate will
install the image to the first volume with that name it finds, which
may not be right one!).

When updating volumes, it is guaranteed that erase counters are
preserved and not lost. The behavior of updating is identical to that
of the ``ubiupdatevol(1)`` tool from mtd-utils. In fact, the same
library from mtd-utils (libubi) is reused by SWUpdate.

atomic volume renaming
...........................

The UBI volume handler has basic support for carrying out atomic
volume renames by defining the ``replaces`` property, which must
contain a valid UBI volume name. After successfully updating the image
to ``volume``, an atomic swap of the names of ``volume`` and
``replaces`` is done. Consider the following example

::

	{
		filename ="u-boot.img";
		volume ="u-boot_r";
		properties: {
			replaces = "u-boot";
		}
	}

After u-boot.img is successfully installed into the volume "u-boot_r",
the volume "u-boot_r" is renamed to "u-boot" and "u-boot" is renamed
to "u-boot_r".

This mechanism allows one to implement a simple double copy update
approach without the need of shared state with the bootloader. For
example, the U-Boot SPL can be configured to always load U-Boot from
the volume ``u-boot`` without the need to access the environment. The
volume replace functionality will ensure that this volume name always
points to the currently valid volume.

However, please note the following limitations:

- Currently the rename takes place after *each* image was installed
  successfully. Hence, it only makes sense to use this feature for
  images that are independent of the other installed images. A typical
  example is the bootloader. This behavior may be modified in the
  future to only carry out one atomic rename after all images were
  installed successfully.

- Atomic renames are only possible and permitted for volumes residing
  on the same UBI device.

There is a handler ubiswap that allow one to do an atomic swap for several
ubi volume after all the images were flashed. This handler is a script
for the point of view of swudate, so the node that provide it the data
should be added in the section scripts.

::

	scripts: (
		{
			type = "ubiswap";
			properties: {
				swap-0 = [ "boot" , " boot_r" ];
				swap-1 = [ "kernel" , "kernel_r" ];
				swap-2 = [ "rootfs" , "rootfs_r" ];
			},
		},
	);


WARNING: if you use the property replaces on an ubi volume that is also
used with the handler ubiswap, this ubi volume will be swapped twice.
It's probably not what you want ...

volume auto resize
...........................

The UBI volume handler has support to auto resize before flashing an
image with the property ``auto-resize``. When this property is set
on an image, the ubi volume is resized to fit exactly the image.

::

	{
		filename = "u-boot.img";
		device = "mtd0";
		volume = "u-boot_r";
		properties: {
			auto-resize = "true";
		}
	}

WARNING: when this property is used, the device must be defined.

volume always remove
....................

The UBI volume handler has support to always remove ubi volume
before flashing with the property ``always-remove``. When this property
is set on an image, the ubi volume is always removed. This property
should be used with property ``auto-resize``.

::

	{
		filename = "u-boot.img";
		device = "mtd0";
		volume = "u-boot_r";
		properties: {
			always-remove = "true";
			auto-resize = "true";
		}
	}

size properties
...............
Due to a limit in the Linux kernel API for UBI volumes, the size reserved to be
written on disk should be declared before actually writing anything.
Unfortunately, the size of an encrypted or compressed image is not known until
the decryption or decompression finished. This prevents correct declaration of
the file size to be written on disk.

For this reason UBI images can declare the special properties "decrypted-size"
or "decompressed-size" like this:

::

	images: ( {
			filename = "rootfs.ubifs.enc";
			volume = "rootfs";
			encrypted = true;
			properties: {
				decrypted-size = "104857600";
			}
		},
		{
			filename = "homefs.ubifs.gz";
			volume = "homefs";
			compressed = "zlib";
			properties: {
				decompressed-size = "420000000";
			}
		}
	);

The real size of the image should be calculated and written to the
sw-description before assembling the cpio archive.
In this example, 104857600 is the size of the rootfs after the decryption: the
encrypted size is by the way larger. The decompressed size is of the homefs is
420000000.

The sizes are bytes in decimal notation.

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
technically allow one to embed them into a to be processed ``.swu``
image, this is not implemented as it carries some security
implications since the behavior of SWUpdate is changed
dynamically.

Remote handler
--------------

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


The connection is instantiated using the socket ``test_remote`` (according
to the "data" field's value) in the directory pointed to by the environment
variable ``TMPDIR`` with ``/tmp`` as fall-back if ``TMPDIR`` is not set.
If ``connect()`` fails, the  remote handler signals that the update is not
successful. Each zeromq message  from SWUpdate is a multi-part message split
into two frames:

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

SWU forwarder
---------------

The SWU forwarder handler can be used to update other systems where SWUpdate
is running. It can be used in case of master / slaves systems, where the master
is connected to the network and the "slaves" are hidden to the external world.
The master is then the only interface to the world. A general SWU can contain
embedded SWU images as single artifacts, and the SWU handler will forward it
to the devices listed in the description of the artifact.
The handler can have a single "url" properties entry with an array of urls. Each url
is the address of a secondary board where SWUpdate is running with webserver activated.
The SWU handler expects to talk with SWUpdate's embedded webserver. This helps
to update systems where an old version of SWUpdate is running, because the
embedded webserver is a common feature present in all versions.
The handler will send the embedded SWU to all URLs at the same time, and setting
``installed-directly`` is supported by this handler.

.. image:: images/SWUGateway.png

The following example shows how to set a SWU as artifact and enables
the SWU forwarder:


::

	images: (
		{
			filename = "image.swu";
			type = "swuforward";

			properties: {
				url = ["http://192.168.178.41:8080", "http://192.168.178.42:8080"];
			};
		});


rdiff handler
-------------

The rdiff handler adds support for applying binary delta patches generated by
`librsync's <http://librsync.sourcefrog.net/>`_ rdiff tool.

Naturally, the smaller the difference between the diff's source and target, the
more effective is using this handler rather than shipping the full target, e.g.,
via the image handler. Hence, the most prominent use case for the rdiff handler
is when having a read-only root filesystem and applying a small update like
security fixes or feature additions. If the sweet spot is crossed, an rdiff
patch may even exceed the full target's size due to necessary patch metadata.
Also note that in order to be most effective, an image to be processed with
rdiff should be built deterministic
(see `reproducible-builds.org <https://reproducible-builds.org>`_).

The rdiff algorithm requires no resources whatsoever on the device as the patch
is fully computed in the backend. Consequently, the backend has to have
knowledge of the current software running on the device in order to compute
a sensible patch. Alike, the patch has to be applied on the device to an
unmodified source as used in the backend for patch computation. This property is
in particular useful for resource-constrained devices as there's no need for the
device to, e.g., aid in the difference computation.

First, create the signature of the original (base) file via
``rdiff signature <basefile> <signaturefile>``.
Then, create the delta file (i.e., patch) from the original base file to the target
file via ``rdiff delta <signaturefile> <targetfile> <deltafile>``.
The ``<deltafile>`` is the artifact to be applied via this handler on the device.
Essentially, it mimics running ``rdiff patch <basefile> <deltafile> <targetfile>``
on the device. Naturally for patches, the very same ``<basefile>`` has to be used
for creating as well as for applying the patch to.

This handler registers itself for handling files and images.
An exemplary sw-description fragment for the files section is

::

    files: (
        {
            type = "rdiff_file"
            filename = "file.rdiff.delta";
            path = "/usr/bin/file";
        }
    );


Note that the file referenced to by ``path`` serves as ``<basefile>`` and
gets replaced by a temporary file serving as ``<targetfile>`` while the rdiff
patch processing.

An exemplary sw-description fragment for the images section is

::

    images: (
        {
            type = "rdiff_image";
            filename = "image.rdiff.delta";
            device = "/dev/mmcblk0p2";
            properties: {
                rdiffbase = ["/dev/mmcblk0p1"];
            };
        }
    );


Here, the property ``rdiffbase`` qualifies the ``<basefile>`` while the ``device``
attribute designates the ``<targetfile>``.
Note that there's no support for the optional ``offset`` attribute in the
``rdiff_image`` handler as there's currently no apparent use case for it and
skipping over unchanged content is handled well by the rdiff algorithm.


ucfw handler
------------

This handler allows one to update the firmware on a microcontroller connected to
the main controller via UART.
Parameters for setup are passed via sw-description file.  Its behavior can be
extended to be more general.
The protocol is ASCII based. There is a sequence to be done to put the microcontroller
in programming mode, after that the handler sends the data and waits for an ACK from the
microcontroller.

The programming of the firmware shall be:

1. Enter firmware update mode (bootloader)

        1. Set "reset line" to logical "low"
	2. Set "update line" to logical "low"
	3. Set "reset line" to logical "high"

2. Send programming message

::

        $PROG;<<CS>><CR><LF>

to the microcontroller.  (microcontroller will remain in programming state)

3. microcontroller confirms with

::

        $READY;<<CS>><CR><LF>

4. Data transmissions package based from mainboard to microcontroller package definition:

        - within a package the records are sent one after another without the end of line marker <CR><LF>
        - the package is completed with <CR><LF>

5. The microcontroller requests the next package with $READY;<<CS>><CR><LF>

6. Repeat step 4 and 5 until the complete firmware is transmitted.

7. The keypad confirms the firmware completion with $COMPLETED;<<CS>><CR><LF>

8. Leave firmware update mode
        1. Set "Update line" to logical "high"
        2. Perform a reset over the "reset line"

<<CS>> : checksum. The checksum is calculated as the two's complement of
the modulo-256 sum over all bytes of the message
string except for the start marker "$".
The handler expects to get in the properties the setup for the reset
and prog gpios. They should be in this format:

::

        properties = {
	        reset = "<gpiodevice>:<gpionumber>:<activelow>";
                prog = "<gpiodevice>:<gpionumber>:<activelow>";
        }

Example:

::

    images: (
        {
            filename = "microcontroller-image";
            type = "ucfw";
            device = "/dev/ttymxc5";

            properties: {
                reset =  "/dev/gpiochip0:38:false";
                prog =  "/dev/gpiochip0:39:false";
            };
        }
    );

SSBL Handler
------------

This implements a way to switch two software sets using a duplicated structure saved on the
flash (currently, only NOR flash is supported). Each of the two structures contains address
and size of the image to be loaded by a first loader. A field contain the "age", and it is
incremented after each switch to show which is the active set.


.. table:: Structure of SSBL Admin

   +---------------------------------------------------------------+-------------+
   |  SSBL Magic Number (29 bit)Name                               | Age (3 bit) |
   +---------------------------------------------------------------+-------------+
   |                            Image Address Offset                             |
   +-----------------------------------------------------------------------------+
   |                            Image Size                                       |
   +-----------------------------------------------------------------------------+


The handler implements a post install script. First, it checks for consistency the two
structures and find the active reading the first 32 bit value with a magic number and the age.
It increments the age and saves the new structure in the inactive copy. After a reboot,
the loader will check it and switch the software set.

::

	scripts: (
		{
		        type = "ssblswitch";
			properties: {
				device = ["mtdX", "mtdY"];
				offset = ["0", "0"];
				imageoffs = ["0x780000",  "0xA40000"];
				imagesize = ["0x800000", "0x800000"];
			}
        }


Properties in sw-description are all mandatory. They define where the SSBL Administration data
are stored for both sets. Each properties is an array of two entries, containing values for each
of the two SSBL administration.

.. table:: Properties for SSBL handler

   +-------------+----------+----------------------------------------------------+
   |  Name       |  Type    |  Description                                       |
   +=============+==========+====================================================+
   | device      | string   | MTD device where the SSBL Admin Header is stored   |
   +-------------+----------+----------------------------------------------------+
   | offset      | hex      | Offset of SSBL header inside the MTD device        |
   +-------------+----------+----------------------------------------------------+
   | imageoffset | hex      | Offset of the image to be loaded by a bootloader   |
   |             |          | when this SSBL is set.                             |
   +-------------+----------+----------------------------------------------------+
   | imagesize   | hex      | Size of the image to be loaded by a bootloader     |
   |             |          | when this SSBL is set.                             |
   +-------------+----------+----------------------------------------------------+

Readback Handler
----------------

To verify that an image was written properly, this readback handler calculates
the sha256 hash of a partition (or part of it) and compares it against a given
hash value.

The following example explains how to use this handler:

::

    scripts: (
    {
        device = "/dev/mmcblk2p1";
        type = "readback";
        properties: {
            sha256 = "e7afc9bd98afd4eb7d8325196d21f1ecc0c8864d6342bfc6b6b6c84eac86eb42";
            size = "184728576";
            offset = "0";
        };
    }
    );

Properties ``size`` and ``offset`` are optional, all the other properties are mandatory.

.. table:: Properties for readback handler

    +-------------+----------+----------------------------------------------------+
    |  Name       |  Type    |  Description                                       |
    +=============+==========+====================================================+
    | device      | string   | The partition which shall be verified.             |
    +-------------+----------+----------------------------------------------------+
    | type        | string   | Identifier for the handler.                        |
    +-------------+----------+----------------------------------------------------+
    | sha256      | string   | Expected sha256 hash of the partition.             |
    +-------------+----------+----------------------------------------------------+
    | size        | string   | Data size (in bytes) to be verified.               |
    |             |          | If 0 or not set, the handler will get the          |
    |             |          | partition size from the device.                    |
    +-------------+----------+----------------------------------------------------+
    | offset      | string   | Offset (in bytes) to the start of the partition.   |
    |             |          | If not set, default value 0 will be used.          |
    +-------------+----------+----------------------------------------------------+

Disk partitioner
----------------

This handler creates or modifies partitions using the library libfdisk. Handler must be put into
the `partitions` section of sw-description. Setup for each partition is put into the `properties` field
of sw-description.
After writing the partition table it may create a file system on selected partitions.
(Available only if CONFIG_DISKFORMAT is set.)

.. table:: Properties for diskpart handler

   +-------------+----------+----------------------------------------------------+
   |  Name       |  Type    |  Description                                       |
   +=============+==========+====================================================+
   | labeltype   | string   | "gpt" or "dos"                                     |
   +-------------+----------+----------------------------------------------------+
   | partition-X | array    | Array of values belonging to the partition number X|
   +-------------+----------+----------------------------------------------------+

For each partition, an array of couples key=value must be given. The following keys are
supported:

.. table:: Setup for a disk partition

   +-------------+----------+----------------------------------------------------+
   |  Name       |  Type    |  Description                                       |
   +=============+==========+====================================================+
   | size        | string   | Size of partition. K, M and G can be used for      |
   |             |          | Kilobytes, Megabytes and Gigabytes.                |
   +-------------+----------+----------------------------------------------------+
   | start       | integer  | First sector for the partition                     |
   +-------------+----------+----------------------------------------------------+
   | name        | string   | Name of the partition                              |
   +-------------+----------+----------------------------------------------------+
   | type        | string   | Type of partition, it has two different meanings.  |
   |             |          | It is the hex code for DOS (MBR) partition table   |
   |             |          | or it is the string identifier in case of GPT.     |
   +-------------+----------+----------------------------------------------------+
   | fstype      | string   | Optional filesystem type to be created on the      |
   |             |          | partition. If no fstype key is given, no file      |
   |             |          | will be created on the corresponding partition.    |
   |             |          | vfat / ext2 / ext3 /ext4 file system is supported  |
   +-------------+----------+----------------------------------------------------+



GPT example:

::

        partitions: (
	{
           type = "diskpart";
	   device = "/dev/sde";
           properties: {
	        labeltype = "gpt";
                partition-1 = [ "size=64M", "start=2048",
                    "name=bigrootfs", "type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B"];
                partition-2 = ["size=256M", "start=133120",
                    "name=ldata", "type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7",
		    "fstype=vfat"];
                partition-3 = ["size=512M", "start=657408",
                    "name=log", "fstype =ext4", 63DAF-8483-4772-8E79-3D69D8477DE4"];
                partition-4 = ["size=4G", "start=1705984",
                    "name=system",  "type=0FC63DAF-8483-4772-8E79-3D69D8477DE4"];
                partition-5 = ["size=512M", "start=10094592",
                    "name=part5",  "type=0FC63DAF-8483-4772-8E79-3D69D8477DE4"];
	   }
        }


MBR Example:

::

	partitions: (
	{
	   type = "diskpart";
	   device = "/dev/sde";
	   properties: {
		labeltype = "dos";
		partition-1 = [ "size=64M", "start=2048", "name=bigrootfs", "type=0x83"];
		partition-2 = ["size=256M", "start=133120", "name=ldata", "type=0x83"];
		partition-3 = ["size=256M", "start=657408", "name=log", "type=0x83"];
		partition-4 = ["size=6G", "start=1181696", "name=system",  "type=0x5"];
		partition-5 = ["size=512M", "start=1183744", "name=part5",  "type=0x83"];
		partition-6 = ["size=512M", "start=2234368", "name=part6",  "type=0x83"];
		partition-7 = ["size=16M", "start=3284992", "name=part7", "type=0x6",
		    "fstype=vfat"];
	   }
	}

Unique UUID Handler
-------------------

This handler checks if the device already has a filesystems with a provide UUID. This is helpful
in case the bootloader chooses the device to boot from the UUID and not from the partition number.
One use case is with the GRUB bootloader when GRUB_DISABLE_LINUX_UUID is not set, as usual on
Linux Distro as Debian or Ubuntu.

The handler iterates all UUIDs given in sw-description and raises error if one of them is
found on the device. It is a partition handler and it runs before any image is installed.

::

	partitions: (
	{
		type = "uniqueuuid";
		properties: {
			fs-uuid = ["21f16cae-612f-4bc6-8ef5-e68cc9dc4380",
                                   "18e12df1-d8e1-4283-8727-37727eb4261d"];
		}
	});
