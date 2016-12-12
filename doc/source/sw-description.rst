=================================================
SWUpdate: syntax and tags with the default parser
=================================================

Introduction
------------

SWUpdate uses the library "libconfig"
as default parser for the image description.
However, it is possible to extend SWUpdate and add an own
parser, based on a different syntax and language as the one
supported by libconfig. In the examples directory
there is the code for a parser written in LUA, with the
description in XML.

Using the default parser, sw-description follows the
syntax rules described in the libconfig manual.
Please take a look at http://www.hyperrealm.com/libconfig/libconfig_manual.html
for an explanation of basic types.
The whole description must be contained in the sw-description file itself:
using of the #include directive is not allowed by SWUpdate.

The following example explains better the implemented tags:

::

	software =
	{
		version = "0.1.0";

		hardware-compatibility: [ "1.0", "1.2", "1.3"];

		/* partitions tag is used to resize UBI partitions */
		partitions: ( /* UBI Volumes */
			{
				name = "rootfs";
				device = "mtd4";
			  	size = 104896512; /* in bytes */
			},
			{
				name = "data";
				device = "mtd5";
		  		size = 50448384; /* in bytes */
			}
		);


		images: (
			{
				filename = "rootfs.ubifs";
				volume = "rootfs";
			},
			{
				filename = "swupdate.ext3.gz.u-boot";
				volume = "fs_recovery";
			},
			{
				filename = "sdcard.ext3.gz";
				device = "/dev/mmcblk0p1";
				compressed = true;
			},
			{
				filename = "bootlogo.bmp";
				volume = "splash";
			},
			{
				filename = "uboot-env";
				type = "uboot";
			},
			{
				filename = "uImage.bin";
				volume = "kernel";
			},
			{
				filename = "fpga.txt";
				type = "fpga";
			}
		);

		files: (
			{
				filename = "README";
				path = "/README";
				device = "/dev/mmcblk0p1";
				filesystem = "vfat"
			}
		);

		scripts: (
			{
				filename = "erase_at_end";
				type = "lua";
		 	},
			{
				filename = "display_info";
				type = "lua";
			}
		);

		uboot: (
			{
				name = "vram";
				value = "4M";
			},
			{
				name = "addfb";
				value = "setenv bootargs ${bootargs} omapfb.vram=1:2M,2:2M,3:2M omapdss.def_disp=lcd"
			}
		);
	}

The first tag is "software". The whole description is contained in
this tag. It is possible to group settings per device by using `Board
specific settings`_.

hardware-compatibility
----------------------

hardware-compatibility: [ "major.minor", "major.minor", ... ]

It lists the hardware revisions that are compatible with this software image.

Example:

	hardware-compatibility: [ "1.0", "1.2", "1.3"];

This means that the software is compatible with HW-Revisions
1.0, 1.2 and 1.3, but not for 1.1 or other version not explicitly
listed here.
It is then duty of the single project to find which is the
revision of the board where SWUpdate is running. There is no
assumption how the revision can be obtained (GPIOs, EEPROM,..)
and each project is free to select the way most appropriate.
The result must be written in the file /etc/hwrevision (or in
another file if specified as configuration option) before
SWUpdate is started.

partitions : UBI layout
-----------------------

This tag allows to change the layout of UBI volumes.
Please take care that MTDs are not touched and they are
configured by the Device Tree or in another way directly
in kernel.


::

	partitions: (
		{
			name = <volume name>;
			size = <size in bytes>;
			device = <MTD device>;
		},
	);

All fields are mandatory. SWUpdate searches for a volume of the
selected name and adjusts the size, or creates a new volume if
no volume with the given name exists.

images
------

The tag "images" collects the image that are installed to the system.
The syntax is:

::

	images: (
		{
			filename[mandatory] = <Name in CPIO Archive>;
			volume[optional] = <destination volume>;
			device[optional] = <destination volume>;
			mtdname[optional] = <destination mtd name>;
			type[optional] = <handler>;
			/* optionally, the image can be compressed if it is in raw mode */
			compressed;
		},
		/* Next Image */
		.....
	);

*volume* is only used to install the image in a UBI volume. *volume* and
*device* cannot be used at the same time. If device is set,
the raw handler is automatically selected.

The following example is to update a UBI volume:


::

		{
			filename = "core-image-base.ubifs";
			volume = "rootfs";
		}


To update an image in raw mode, the syntax is:


::

		{
			filename = "core-image-base.ext3";
			device = "/dev/mmcblk0p1";
		}


However, writing to flash in raw mode must be managed in a special
way. Flashes must be erased before copying, and writing into NAND
must take care of bad blocks and ECC errors. For this reasons, the
handler "flash" must be selected:

For example, to copy the kernel into the MTD7 of a NAND flash:

::

		{
			filename = "uImage";
			device = "mtd7";
			type = "flash";
		}

The *filename* is mandatory. It is the Name of the file extracted by the stream.
*volume* is only mandatory in case of UBI volumes. It should be not used
in other cases.

Alternatively, for the handler “flash”, the *mtdname* can be specified, instead of the device name:

::

		{
			filename = "uImage";
			mtdname = "kernel";
			type = "flash";
		}


Files
-----

It is possible to copy single files instead of images.
This is not the preferred way, but it can be used for
debugging or special purposes.

::

	files: (
		{
			filename = <Name in CPIO Archive>;
			path = <path in filesystem>;
			device[optional] = <device node >;
			filesystem[optional] = <filesystem for mount>;
		}
	);

Entries in "files" section are managed as single files. The attributes
"filename" and "path" are mandatory. Attributes "device" and "filesystem" are
optional; they tell SWUpdate to mount device (of the given filesystem type,
e.g. "ext4") before copying "filename" to "path". Without "device" and
"filesystem", the "filename" will be copied to "path" in the current rootfs.


Scripts
-------

Scripts runs in the order they are put into the sw-description file.
The result of a script is valuated by SWUpdate, that stops the update
with an error if the result is <> 0.

They are copied into a temporary directory before execution and their name must
be unique inside the same cpio archive.

If no type is given, SWUpdate default to "lua".

LUA
...

::

	scripts: (
		{
			filename = <Name in CPIO Archive>;
			type = "lua";
	 	},
	);


LUA scripts are run using the internal interpreter.

They must have at least one of the following functions:

::

	function preinst()

SWUpdate scans for all scripts and check for a preinst function. It is
called before installing the images.


::

	function postinst()

SWUpdate scans for all scripts and check for a postinst function. It is
called after installing the images.

shellscript
...........

::

	scripts: (
		{
			filename = <Name in CPIO Archive>;
			type = "shellscript";
		},
	);

Shell scripts are called via system command.
SWUpdate scans for all scripts and calls them before and after installing
the images. SWUpdate passes 'preinst' or 'postinst' as first argument to
the script.

preinstall
..........

::

	scripts: (
		{
			filename = <Name in CPIO Archive>;
			type = "preinstall";
		},
	);

preinstall are shell scripts and called via system command.
SWUpdate scans for all scripts and calls them before installing the images.

postinstall
...........

::

	scripts: (
		{
			filename = <Name in CPIO Archive>;
			type = "postinstall";
		},
	);

postinstall are shell scripts and called via system command.
SWUpdate scans for all scripts and calls them after installing the images.


uboot
-----

There are two ways to update the bootloader (U-Boot) environment.
First way is to add a file with the list of variables to be changed
and setting "uboot" as type of the image. This inform SWUpdate to
call the U-Boot handler to manage the file.

::

		{
			filename = "uboot-env";
			type = "uboot";
		},

The format of the file is described in u-boot documentation. Each line
is in the format

::

	<name of variable>	<value>

if value is missing, the variable is unset.

The second way is to define in a group setting the variables
that must be changed:

::

	uboot: (
		{
			name = <Variable name>;
			value = <Variable value>;
		},
	)

SWUpdate will internally generate a script that will be passed to the
U-Boot handler for adjusting the environment.


Board specific settings
-----------------------

Each setting can be placed under a custom tag matching the board
name. This mechanism can be used to override particular setting in
board specific fashion.

Assuming that the hardware information file `/etc/hwrevision` contains
the following entry::

  my-board 0.1.0

and the following description::

	software =
	{
	        version = "0.1.0";

	        my-board = {
	                uboot: (
	                {
	                        name = "bootpart";
	                        value = "0:2";
	                }
	                );
	        };

	        uboot: (
	        {
	                name = "bootpart";
	                value = "0:1";
	        }
	        );
	}

SWUpdate will set `bootpart` to `0:2` in U-Boot's environment for this
board. For all other boards, `bootpart` will be set to `0:1`. Board
specific settings take precedence over default scoped settings.


Software collections and operation modes
----------------------------------------

Software collections and operations modes extend the description file
syntax to provide an overlay grouping all previous configuration
tags. The mechanism is similar to `Board specific settings`_ and can
be used for implementing a dual copy strategy or delivering both
stable and unstable images within a single update file.

The mechanism uses a custom user-defined tags placed within `software`
scope. The tag names must not be any of: `version`,
`hardware-compatibility`, `uboot`, `files`, `scripts`, `partitions`,
`images`

An example description file:

::

	software =
	{
	        version = "0.1";

	        hardware-compatibility = [ "revA" ];

	        /* differentiate running image modes/sets */
	        stable:
	        {
	                main:
	                {
	                        images: (
	                        {
	                                filename = "rootfs.ext3";
	                                device = "/dev/mmcblk0p2";
	                        }
	                        );

	                        uboot: (
	                        {
	                                name = "bootpart";
	                                value = "0:2";
	                        }
	                        );
	                };
	                alt:
	                {
	                        images: (
	                        {
	                                filename = "rootfs.ext3";
	                                device = "/dev/mmcblk0p1";
	                        }
	                        );

	                        uboot: (
	                        {
	                                name = "bootpart";
	                                value = "0:1";
	                        }
	                        );
	                };

	        };
	}

The configuration describes a single software collection named
`stable`. Two distinct image locations are specified for this
collection: `/dev/mmcblk0p1` and `/dev/mmcblk0p2` for `main` mode and
`alt` mode respectively.

This feature can be used to implement a dual copy strategy by
specifying the collection and mode explicitly.

Checking version of installed software
--------------------------------------

SWUpdate can optionally verify if a sub-image is already installed
and, if the version to be installed is exactly the same, it can skip
to install it. This is very useful in case some high risky image should
be installed or to speed up the upgrade process.
One case is if the bootloader needs to be updated. In most time, there
is no need to upgrade the bootloader, but practice showed that there are
some cases where an upgrade is strictly required - the project manager
should take the risk. However, it is nicer to have always the bootloader image
as part of the .swu file, allowing to get the whole distro for the
device in a single file, but the device should install it just when needed.

SWUpdate searches for a file (/etc/sw-versions is the default location)
containing all versions of the installed images. This must be generated
before running SWUpdate.
The file must contains pairs with the name of image and his version, as:

::

	<name of component>	<version>

Version is a string and can have any value. For example:

::

        bootloader              2015.01-rc3-00456-gd4978d
        kernel                  3.17.0-00215-g2e876af

In sw-description, the optional attributes "name", "version" and
"install-if-different" provide the connection. Name and version are then
compared with the data in the versions file. install-if-different is a
boolean that enables the check for this image. It is then possible to
check the version just for a subset of the images to be installed.

Attribute reference
-------------------

There are 4 main sections inside sw-description:

- images: entries are images and SWUpdate has no knowledge
  about them.
- files: entries are files, and SWUpdate needs a filesystem for them.
  This is generally used to expand from a tar-ball or to update
  single files.
- scripts: all entries are treated as executables, and they will
  be run twice (as pre- and post- install scripts).
- uboot: entries are pair with U-Boot variable name and its value.

.. table:: Attributes
        ::widths: grid

+-------------+----------+------------+---------------------------------------+
|  Name       |  Type    | Applies to |  Description                          |
+=============+==========+============+=======================================+
| filename    | string   | images     |  filename as found in the cpio archive|
|             |          | files      |                                       |
|             |          | scripts    |                                       |
+-------------+----------+------------+---------------------------------------+
| volume      | string   | images     | Just if type = "ubivol". UBI volume   |
|             |          |            | where image must be installed.        |
+-------------+----------+------------+---------------------------------------+
| ubipartition| string   | images     | Just if type = "ubivol". Volume to be |
|             |          |            | created or adjusted with a new size   |
+-------------+----------+------------+---------------------------------------+
| device      | string   | images     | devicenode as found in /dev. Usage    |
|             |          | files      | depends on handler.                   |
|             |          |            | For files, it indicates on which      |
|             |          |            | device the "filesystem" must be       |
|             |          |            | mounted. If not specified, the current|
|             |          |            | rootfs will be used.                  |
+-------------+----------+------------+---------------------------------------+
| filesystem  | string   | files      | indicates the filesystem type where   |
|             |          |            | the file must be installed. Only      |
|             |          |            | used if "device" attribute is set.    |
+-------------+----------+------------+---------------------------------------+
| path        | string   | files      | For files: indicates the path         |
|             |          |            | (absolute) where the file must be     |
|             |          |            | installed. If "device" and            |
|             |          |            | "filesystem" are set,                 |
|             |          |            | SWUpdate will install the             |
|             |          |            | file after mounting "device" with     |
|             |          |            | "filesystem" type. (path is always    |
|             |          |            | relative to the mount point.)         |
+-------------+----------+------------+---------------------------------------+
| type        | string   | images     | string identifier for the handler,    |
|             |          | files      | as it is set by the handler when it   |
|             |          | scripts    | regitsters itself.                    |
|             |          |            | Example: "ubivol", "raw", "rawfile",  |
+-------------+----------+------------+---------------------------------------+
| compressed  | bool     | images     | flag to indicate that "filename" is   |
|             |          | files      | zlib-compressed and must be           |
|             |          |            | decompressed before being installed   |
+-------------+----------+------------+---------------------------------------+
| installed-  | bool     | images     | flag to indicate that image is        |
| directly    |          |            | streamed into the target without any  |
|             |          |            | temporary copy. Not all handlers      |
|             |          |            | support streaming.                    |
+-------------+----------+------------+---------------------------------------+
| name        | string   | uboot      | name of the U-Boot variable to be set.|
+-------------+----------+------------+---------------------------------------+
| value       | string   | uboot      | value to be assigned to the U-Boot    |
|             |          |            | variable                              |
+-------------+----------+------------+---------------------------------------+
| name        | string   | images     | name that identifies the sw-component |
|             |          | files      | it can be any string and it is        |
|             |          |            | compared with the entries in          |
|             |          |            | sw-versions                           |
+-------------+----------+------------+---------------------------------------+
| version     | string   | images     | version for the sw-component          |
|             |          | files      | it can be any string and it is        |
|             |          |            | compared with the entries in          |
|             |          |            | sw-versions                           |
+-------------+----------+------------+---------------------------------------+
| install-if  | bool     | images     | flag                                  |
| different   |          | files      | if set, name and version are          |
|             |          |            | compared with the entries in          |
+-------------+----------+------------+---------------------------------------+
| encrypted   | bool     | images     | flag                                  |
|             |          | files      | if set, file is encrypted             |
|             |          | scripts    | and must be decrypted before          |
|             |          |            | installing.                           |
+-------------+----------+------------+---------------------------------------+
| data        | string   | images     | This is used to pass arbitrary data   |
|             |          | files      | to a handler.                         |
|             |          | scripts    |                                       |
+-------------+----------+------------+---------------------------------------+
| sha256      | string   | images     | sha256 hash of image, file or script. |
|             |          | files      | Used for verification of signed       |
|             |          | scripts    | images.                               |
+-------------+----------+------------+---------------------------------------+
| mtdname     | string   | images     | name of the MTD to update. Used only  |
|             |          |            | by the flash handler to identify the  |
|             |          |            | the mtd to update, instead of         |
|             |          |            | specifying the devicenode             |
+-------------+----------+------------+---------------------------------------+
