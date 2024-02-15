.. SPDX-FileCopyrightText: 2013-2021 Stefano Babic <stefano.babic@swupdate.org>
.. SPDX-License-Identifier: GPL-2.0-only

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
there is the code for a parser written in Lua, with the
description in XML.

Using the default parser, sw-description follows the
syntax rules described in the libconfig manual.
Please take a look at http://www.hyperrealm.com/libconfig/libconfig_manual.html
for an explanation of basic types.
The whole description must be contained in the sw-description file itself:
using of the #include directive is not allowed by SWUpdate.

The following example explains the implemented tags:

::

	software =
	{
		version = "0.1.0";
		description = "Firmware update for XXXXX Project";

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
				compressed = "zlib";
			},
			{
				filename = "bootlogo.bmp";
				volume = "splash";
			},
			{
				filename = "uImage.bin";
				volume = "kernel";
			},
			{
				filename = "fpga.txt";
				type = "fpga";
			},
			{
				filename = "bootloader-env";
				type = "bootloader";
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

		bootenv: (
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

Handling configuration differences
----------------------------------

The concept can be extended to deliver a single image
containing the release for multiple devices. Each device has its own
kernel, dtb, and root filesystem, or they can share some parts.

Currently this is managed (and already used in a real project) by
writing an own parser, that checks which images must be installed
after recognizing which is the device where software is running.

Because the external parser can be written in Lua and it is
completely customizable, everybody can set his own rules.
For this specific example, the sw-description is written in XML format,
with tags identifying the images for each device. To run it, the liblxp
library is needed.

::

	<?xml version="1.0" encoding="UTF-8"?>
	<software version="1.0">
	  <name>Update Image</name>
	  <version>1.0.0</version>
	  <description>Firmware for XXXXX Project</description>

	  <images>
	    <image device="firstdevice" version="0.9">
	      <stream name="dev1-uImage" type="ubivol" volume="kernel" />
	      <stream name="dev1.dtb" type="ubivol" volume="dtb" />
	      <stream name="dev1-rootfs.ubifs" type="ubivol" volume="rootfs"/>
	      <stream name="dev1-uboot-env" type="uboot" />
	      <stream name="raw_vfat" type="raw" dest="/dev/mmcblk0p4" />
	      <stream name="sdcard.lua" type="lua" />
	    </image>

	    <image device="seconddevice" version="0.9">
	      <stream name="dev2-uImage" type="ubivol" volume="kernel" />
	      <stream name="dev2.dtb" rev="0.9" type="ubivol" volume="dtb" />
	      <stream name="dev2-rootfs.ubifs" type="ubivol" volume="rootfs"/>
	    </image>
	  </images>
	</software>


The parser for this is in the /examples directory.
By identifying which is the running device, the parser return
a table containing the images that must be installed and their associated
handlers.
By reading the delivered image, SWUpdate will ignore all images that
are not in the list processed by the parser. In this way, it is possible
to have a single delivered image for the update of multiple devices.

Multiple devices are supported by the default parser, too.

::

    software =
    {
        version = "0.1.0";

        target-1 = {
                images: (
                        {
                                ...
                        }
                );
        };

        target-2 = {
                images: (
                        {
                                ...
                        }
                );
        };
    }

In this way, it is possible to have a single image providing software
for each device you have.

By default, the hardware information is extracted from
`/etc/hwrevision` file. The file should contain a single line in the
following format:

  <boardname> <revision>

Where:

- `<revision>` will be used for matching with hardware compatibility
  list

- `<boardname>` can be used for grouping board specific settings

.. _collections:

Software collections
--------------------

Software collections and operation modes can be used to implement a
dual copy strategy. The simplest case is to define two installation
locations for the firmware image and call `SWUpdate` selecting the
appropriate image.

::

    software =
    {
            version = "0.1.0";

            stable = {
                    copy-1: {
                            images: (
                            {
                                    device = "/dev/mtd4"
                                    ...
                            }
                            );
                    }
                    copy-2: {
                            images: (
                            {
                                    device = "/dev/mtd5"
                                    ...
                            }
                            );
                    }
            };
    }

In this way it is possible to specify that `copy-1` gets installed to
`/dev/mtd4`, while `copy-2` to `/dev/mtd5`. By properly selecting the
installation locations, `SWUpdate` will update the firmware in the
other slot.

The method of image selection is out of the scope of SWUpdate and user
is responsible for calling `SWUpdate` passing proper settings.

Priority finding the elements in the file
-----------------------------------------

SWUpdate search for entries in the sw-description file according to the following priority:

1. Try <boardname>.<selection>.<mode>.<entry>
2. Try <selection>.<mode>.<entry>
3. Try <boardname>.<entry>
4. Try <entry>

Take an example. The following sw-description describes the release for a set of boards.

::

    software =
    {
            version = "0.1.0";

            myboard = {
                stable = {
                    copy-1: {
                            images: (
                            {
                                    device = "/dev/mtd4"
                                    ...
                            }
                            );
                    }
                    copy-2: {
                            images: (
                            {
                                    device = "/dev/mtd5"
                                    ...
                            }
                            );
                    }
                }
            }

            stable = {
                copy-1: {
                      images: (
                          {
                               device = "/dev/mtd6"
                                    ...
                          }
                       );
                }
                copy-2: {
                       images: (
                       {
                               device = "/dev/mtd7"
                                    ...
                       }
                       );
                }
            }
    }

On *myboard*, SWUpdate searches and finds myboard.stable.copy1(2). When running on different
boards, SWUpdate does not find an entry corresponding to the boardname and it falls back to the
version without boardname. This allows to realize the same release for different boards having
a completely different hardware. `myboard` could have an eMMC and an ext4 filesystem,
while another device can have raw flash and install an UBI filesystem. Nevertheless, they are
both just a different format of the same release and they could be described together in sw-description.
It is important to understand the priorities how SWUpdate scans for entries during the parsing.

Using links
-----------

sw-description can become very complex. Let's think to have just one board, but in multiple
hw revision and they differ in Hardware. Some of them can be grouped together, some of them
require a dedicated section. A way (but not the only one !) could be to add *mode* and selects
the section with `-e stable,<rev number>`.

::

	software =
	{
		version = "0.1.0";

		myboard = {
	            stable = {

			hardware-compatibility: ["1.0", "1.2", "2.0", "1.3", "3.0", "3.1"];
			rev-1.0: {
				images: (
					...
				);
				scripts: (
					...
				);
			}
			rev-1.2: {
				hardware-compatibility: ["1.2"];
				images: (
					...
				);
				scripts: (
					...
				);
			}
			rev-2.0: {
				hardware-compatibility: ["2.0"];
				images: (
					...
				);
				scripts: (
                                   ...
				);
			}
			rev-1.3: {
				hardware-compatibility: ["1.3"];
				images: (
                                    ...
				);
				scripts: (
                                    ...
				);
			}

			rev-3.0:
			{
				hardware-compatibility: ["3.0"];
				images: (
					...
				);
				scripts: (
					...
				);
	                }
			rev-3.1:
			{
				hardware-compatibility: ["3.1"];
				images: (
					...
				);
				scripts: (
					...
				);
			}
		     }
	        }
	}

If each of them requires an own section, it is the way to do. Anyway, it is more probable
than revisions can be grouped together, for example board with the same major revision
number could have the same installation instructions. This leads in the example to 3 groups
for rev1.X, rev2.X, and rev3.X. Links allow one to group section together. When a "ref" is found
when SWUpdate searches for a group (images, files, script, bootenv), it replaces the current path
in the tree with the value of the string. In this way, the example above can be written in this way:

::

	software =
	{
                version = "0.1.0";

                myboard = {
	            stable = {

                        hardware-compatibility: ["1.0", "1.2", "2.0", "1.3", "3.0", "3.1"];
                        rev-1x: {
                                images: (
                                   ...
                                );
                                scripts: (
                                    ...
                                );
                        }
                        rev1.0 = {
                                ref = "#./rev-1x";
                        }
                        rev1.2 = {
                                ref = "#./rev-1x";
                        }
                        rev1.3 = {
                                ref = "#./rev-1x";
                        }
                        rev-2x: {
                                images: (
                                     ...
                                );
                                scripts: (
                                     ...
                                );
                        }
                        rev2.0 = {
                                ref = "#./rev-2x";
                        }

                        rev-3x: {
                                images: (
                                     ...
                                );
                                scripts: (
                                      ...
                                );
	                }
                        rev3.0 = {
                                ref = "#./rev-3x";
                        }
                        rev3.1 = {
                                ref = "#./rev-3x";
                        }
		     }
	        }
       }

The link can be absolute or relative. The keyword *"ref"* is used to indicate a link. If this is found, SWUpdate
will traverse the tree and replaces the current path with the values find in the string pointed by "ref". There are
simple rules for a link:

       - it must start with the character '#'
       - "." points to the current level in the tree, that means the parent of "ref"
       - ".." points to the parent level in the tree
       - "/" is used as filed separator in the link


A relative path has a number of
leading "../" to move the current cursor to the parent leaf of the tree.
In the following example, rev40 sets a link to a "common" section, where `images`
is found. This is sets via a link, too, to a section in the parent node.
The path `software.myboard.stable.common.images`  is then replaced by
`software.myboard.stable.trythis`

::

	software =
	{
	  version = {
		  ref = "#./commonversion";
	  }

	  hardware-compatibility = ["rev10", "rev11", "rev20"];

	  commonversion = "0.7-linked";

	pc:{
	  stable:{

	    common:{
		images =
		{
		  ref = "#./../trythis";
		}
	      };

	    trythis:(
		{
		filename = "rootfs1.ext4";
		device = "/dev/mmcblk0p8";
		type = "raw";
		} ,
		{
		filename = "rootfs5.ext4";
		device = "/dev/mmcblk0p7";
		type = "raw";
		}
	      );
	    pdm3rev10:
	      {
	      images:(
		  {
		  filename = "rootfs.ext3"; device = "/dev/mmcblk0p2";}
		);
	      uboot:(
		  { name = "bootpart";
		  value = "0:2";}
		);
	      };
	      pdm3rev11 =
	      {
		ref = "#./pdm3rev10";
	      }
	      pdm3rev20 =
	      {
		ref = "#./pdm3rev10";
	      }
	      pdm3rev40 =
	      {
		ref = "#./common";
	      }
	    };
	  };
	}


Each entry in sw-description can be redirect by a link as in the above example for the
"version" attribute.

hardware-compatibility
----------------------

``hardware-compatibility: [ "major.minor", "major.minor", ... ]``

This entry lists the hardware revisions that are compatible with this
software image.

Example:

::

	hardware-compatibility: [ "1.0", "1.2", "1.3"];

This defines that the software is compatible with HW-Revisions 1.0,
1.2, and 1.3, but not with 1.1 or any other version not explicitly
listed here. In the above example, compatibility is checked by means
of string comparison. If the software is compatible with a large
number of hardware revisions, it may get cumbersome to enumerate all
compatible versions. To allow more compact specifications, regular
expressions (POSIX extended) can be used by adding a prefix ``#RE:``
to the entry. Rewriting the above example would yield:

::

	hardware-compatibility: [ "#RE:^1\.[023]$" ];

It is in the responsibility of the respective project to find the
revision of the board on which SWUpdate is running. No assumptions are
made about how the revision can be obtained (GPIOs, EEPROM,..) and
each project is free to select the most appropriate way. In the end
the result must be written to the file ``/etc/hwrevision`` (or in
another file if specified as configuration option) before SWUpdate is
started.

.. _partitions-ubi-layout:

partitions : UBI layout
-----------------------

This tag allows one to change the layout of UBI volumes.
Please take care that MTDs are not touched and they are
configured by the Device Tree or in another way directly
in kernel.


::

	partitions: (
		{
			name = <volume name>;
			size = <size in bytes>;
			device = <MTD device>;
		}
	);

All fields are mandatory. SWUpdate searches for a volume of the given
name and if necessary adjusts size or type (see below). If no volume
with the given name is found, a new volume is created on the UBI
device attached to the MTD device given by ``device``. ``device`` can
be specified by number (e.g. "mtd4") or by name (the name of the MTD
device, e.g. "ubi_partition"). The UBI device is attached
automatically.

The default behavior of swupdate is to create a dynamic UBI volume. To
create a static volume, add a line ``data = "static";`` to the
respective partition entry.

If a size of 0 is given, the volume will be deleted if it exists. This
can be used to remove orphan volumes possibly created by older software
versions which are not required anymore.

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
			/* optionally, the image can be copied at a specific offset */
			offset[optional] = <offset>;
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

To flash an image at a specific offset, the syntax is:


::

		{
			filename = "u-boot.bin";
			device = "/dev/mmcblk0p1";
			offset = "16K";
		}

The offset handles the following multiplicative suffixes: K=1024 and M=1024*1024.

However, writing to flash in raw mode must be managed in a special
way. Flashes must be erased before copying, and writing into NAND
must take care of bad blocks and ECC errors. For these reasons, the
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
			properties[optional] = {create-destination = "true";}
		}
	);

Entries in "files" section are managed as single files. The attributes
"filename" and "path" are mandatory. Attributes "device" and "filesystem" are
optional; they tell SWUpdate to mount device (of the given filesystem type,
e.g. "ext4") before copying "filename" to "path". Without "device" and
"filesystem", the "filename" will be copied to "path" in the current rootfs.

As a general rule, swupdate doesn't copy out a file if the destination path
doesn't exists. This behavior could be changed using the special property
"create-destination".

As another general rule, the raw file handler installs the file directly to the
specified path. If the target file already exists and the raw file handler
is interrupted, the existing file may be replaced by an empty or partially
written file. A use case can exist where having an empty or corrupted file is
worse than the existing file. For this reason, the raw file handler supports an
"atomic-install" property. Setting the property to "true" installs the file to
the specified path with ".tmp" appended to the filename. Once the contents of
the file have been written and the buffer is flushed, the ".tmp" file is renamed
to the target file. This minimizes chances that an empty or corrupted file is
created by an interrupted raw file handler.

Scripts
-------

Scripts runs in the order they are put into the sw-description file.
The result of a script is valuated by SWUpdate, that stops the update
with an error if the result is <> 0.

They are copied into a temporary directory before execution and their name must
be unique inside the same cpio archive.

If no type is given, SWUpdate default to "lua". Please note that running a shell script
opens a set of different security issues, check also chapter "Best practise".


Lua
...

::

	scripts: (
		{
			filename = <Name in CPIO Archive>;
			type = "lua";
	 	}
	);


Lua scripts are run using the internal interpreter.

They must have at least one of the following functions:

::

	function preinst()

SWUpdate scans for all scripts and check for a preinst function. It is
called before installing the images.


::

	function postinst()


SWUpdate scans for all scripts and check for a postinst function. It is
called after installing the images.

::

	function postfailure()

Only in case an update fails, SWUpdate scans for all scripts and check
for a postfailure function. This could be useful in case it is necessary
to restore a previous state, for example, in case the application was
stop, it should run again.

shellscript
...........

SWUpdate will run the binary shell "/bin/sh" to execute the script.

::

	scripts: (
		{
			filename = <Name in CPIO Archive>;
			type = "shellscript";
		}
	);

Shell scripts are called by forking the process and running the shell as /bin/sh.
SWUpdate scans for all scripts and calls them before and after installing
the images. SWUpdate passes 'preinst', 'postinst' or 'postfailure' as first argument to
the script.
If the data attribute is defined, its value is passed as the last argument(s)
to the script.

preinstall
..........

::

	scripts: (
		{
			filename = <Name in CPIO Archive>;
			type = "preinstall";
		}
	);

preinstall are shell scripts and called via system command.
SWUpdate scans for all scripts and calls them before installing the images.
If the data attribute is defined, its value is passed as the last argument(s)
to the script.

Note that cannot be ensured that preinstall scripts run before an artifact is
installed in streaming mode. In fact, if streaming is activated, the artifact must
be installed as soon as it is received from network because there is no temporary
copy. Because there is no fix order in the SWU, an artifact can be packed before any
script in the SWU. The right way is to write an "embedded-script" in Lua inside
sw-description: because it becomes part of sw-description, it runs when sw-description is
parsed and before any handler runs, even before a partition handler.

postinstall
...........

::

	scripts: (
		{
			filename = <Name in CPIO Archive>;
			type = "postinstall";
		}
	);

postinstall are shell scripts and called via system command.
SWUpdate scans for all scripts and calls them after installing the images.
If the data attribute is defined, its value is passed as the last argument(s)
to the script.

Update Transaction and Status Marker
------------------------------------

By default, SWUpdate sets the bootloader environment variable "recovery_status"
to "in_progress" prior to an update operation and either unsets it or sets it to
"failed" after the update operation. This is an interface for SWUpdate-external
tooling: If there is no "recovery_status" variable in the bootloader's
environment, the update operation has been successful. Else, if there is
a "recovery_status" variable with the value "failed", the update operation has
not been successful.

While this is in general essential behavior for firmware updates, it needn't be
for less critical update operations. Hence, whether or not the update
transaction marker is set by SWUpdate can be controlled by the boolean switch
"bootloader_transaction_marker" which is global per `sw-description` file.
It defaults to ``true``. The following example snippet disables the update
transaction marker:

::

	software =
	{
		version = "0.1.0";
		bootloader_transaction_marker = false;
		...


It is also possible to disable setting of the transaction marker
entirely (and independently of the setting in `sw-description`) by
starting SWUpdate with the `-M` option.


The same applies to setting the update state in the bootloader via its
environment variable "ustate" (default) to `STATE_INSTALLED=1` or
`STATE_FAILED=3` after an installation. This behavior can be turned off
globally via the `-m` option to SWUpdate or per `sw-description` via the
boolean switch "bootloader_state_marker".

reboot flag
-----------

It is possible to signal that a reboot for a specific update is not required.
This information is evaluated by SWUpdate just to inform a backend about the
transaction result. If a postinstall script (command line parameter -p) is
passed at the startup to perform a reboot, it will be executed anyway because
SWUpdate cannot know the nature of this script.

SWUpdate sends this information to the progress interface and it is duty of the
listeners to interprete the information. The attribute is a boolean:

::

        reboot = false;

Attribute belongs to the general section, where also version belongs. It is
not required to activate the flag with `reboot = true` because it is the
default behavior, so just disabling makes sense.

The tool `swupdate-progress` interprets the flag: if it was started with
reboot support (-r parameter), it checks if a "no-reboot" message is received
and disables to reboot the device for this specific update. When the transaction
completes, the reboot feature is activated again in case a new update will require to
reboot the device. This allows to have on the fly updates, where not the whole
software is updated and a reboot is not required.

bootloader
----------

There are two ways to update the bootloader (currently U-Boot, GRUB, and
EFI Boot Guard) environment. First way is to add a file with the list of
variables to be changed and setting "bootloader" as type of the image. This
informs SWUpdate to call the bootloader handler to manage the file
(requires enabling bootloader handler in configuration). There is one
bootloader handler for all supported bootloaders. The appropriate bootloader
must be chosen from the bootloader selection menu in `menuconfig`.

::

	images: (
		{
			filename = "bootloader-env";
			type = "bootloader";
		}
	)

The format of the file is described in U-boot documentation. Each line
is in the format

::

	<name of variable>=<value>

if value is missing, the variable is unset.

The format is compatible with U-Boot "env import" command. It is possible
to produce the file from target as result of "env export".

Comments are allowed in the file to improve readability, see this example:

::

        # Default variables
        bootslot=0
        board_name=myboard
        baudrate=115200

        ## Board Revision dependent
        board_revision=1.0


The second way is to define in a group setting the variables
that must be changed:

::

	bootenv: (
		{
			name = <Variable name>;
			value = <Variable value>;
		}
	)

SWUpdate will internally generate a script that will be passed to the
bootloader handler for adjusting the environment.

For backward compatibility with previously built `.swu` images, the
"uboot" group name is still supported as an alias. However, its usage
is deprecated.

SWUpdate persistent variables
-----------------------------

Not all updates require to inform the bootloader about the update, and in many cases a
reboot is not required. There are also cases where changing bootloader's environment
is unwanted due to restriction for security.
SWUpdate needs then some information after new software is running to understand if
everything is fine or some actions like a fallback are needed. SWUpdate can store
such as information in variables (like shell variables), that can be stored persistently.
The library `libubootenv` provide a way for saving such kind as database in a power-cut safe mode.
It uses the algorythm originally implemented in the U-Boot bootloader. It is then guaranteed
that the system will always have a valid instance of the environment. The library supports multiple
environment databases at the same time, identifies with `namespaces`.
SWUpdate should be configured to set the namespace used for own variables. This is done by setting
the attribute *namespace-vars* in the runtime configuration file (swupdate.cfg). See also
example/configuration/swupdate.cfg for details.

The format is the same used with bootloader for single variable:

::

	vars: (
		{
			name = <Variable name>;
			value = <Variable value>;
		}
	)

SWUpdate will set these variables all at once like the bootloader variables. These environment
is stored just before writing the bootloader environment, that is always the last step in an update.

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
	                bootenv: (
	                {
	                        name = "bootpart";
	                        value = "0:2";
	                }
	                );
	        };

	        bootenv: (
	        {
	                name = "bootpart";
	                value = "0:1";
	        }
	        );
	}

SWUpdate will set `bootpart` to `0:2` in bootloader's environment for this
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
`hardware-compatibility`, `uboot`, `bootenv`, `files`, `scripts`, `partitions`,
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

	                        bootenv: (
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

	                        bootenv: (
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

Versioning schemas in SWUpdate
------------------------------

SWUpdate can perform version comparisons for the whole Software by checking
the `version` attribute in the common part of sw-description and / or
for single artifacts. SWUpdate supports two different version schemas,
and they must be followed if version comparison is requested.

Numbering schema (default)
..........................

SWUpdate supports a version based on the schema:

::

        <major>.<minor>.<revision>.<build>

where each field is a plain number (no alphanumeric) in the range 0..65535.
User can add further fields using the dot separator, but they are not
considered for version comparison. SWUpdate will check if a version
number is set according to this rule and fall back to semantic version
upon failure. The version is converted to a 64 bit number (each field is 16 bit)
and compared against the running version of the same artifact.

Please consider that, because additional fields are descriptive only, for the
comparison they are not considered. This example contains version numbers
that are interpreted as the same version number by SWUpdate:

::

        1.2.3.4
        1.2.3.4.5
        1.2.3.4.5.6

But the following is different:

::

        1.2.3.4-alpha

And it is treated as semantic version.

Semantic version
----------------

SWUpdate supports semantic_ version. See official documentation
for more details.

.. _semantic: https://semver.org/

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
The file must contain pairs with the name of image and version, as:

::

	<name of component>	<version>

In sw-description, the optional attributes "name", "version", and
"install-if-different" provide the connection. Name and version are then
compared with the data in the versions file. install-if-different is a
boolean that enables the check for this image. It is then possible to
check the version just for a subset of the images to be installed.

If used with "install-if-different", then version can be any string.
For example:

::

        bootloader              2015.01-rc3-00456-gd4978d
        kernel                  3.17.0-00215-g2e876af

There is also an attribute "install-if-higher" that checks if the version
of the new software is higher than the version of the installed software.
If it's false, the new software isn't installed. The goal is to avoid
installing an older version of software.

In this case, version can be any of 2 formats. Either the version consists
of *up to* 4 numbers in the range 0..65535 separated by a dot,
e.g. `<major>.<minor>.<rev>.<build>`,
or it is a `semantic version <https://semver.org>`_.

::

        bootloader              2018.03.01
        kernel                  3.17.0-pre1+g2e876af
        rfs                     0.17-foo3.bar5+2020.07.01
        app                     1.7

It is advised not to mix version formats! Semantic versions only support 3
numbers (major, minor, patch) and the fourth number will be silently dropped
if present.

Embedded Script
---------------

It is possible to embed a script inside sw-description. This is useful in a lot
of conditions where some parameters are known just by the target at runtime. The
script is global to all sections, but it can contain several functions that can be specific
for each entry in the sw-description file.

These attributes are used for an embedded-script:

::

		embedded-script = "<Lua code>"

It must be taken into account that the parser has already run and usage of double quotes can
interfere with the parser. For this reason, each double quote in the script must be escaped.

That means a simple Lua code as:

::

        print ("Test")

must be changed to:

::

        print (\"Test\")

If not, the parser thinks to have the closure of the script and this generates an error.
See the examples directory for examples how to use it.
Any entry in files or images can trigger one function in the script. The "hook" attribute
tells the parser to load the script and to search for the function pointed to by the hook
attribute. For example:

::

		files: (
			{
				filename = "examples.tar";
				type = "archive";
				path = "/tmp/test";
				hook = "set_version";
				preserve-attributes = true;
			}
		);

After the entry is parsed, the parser runs the Lua function pointed to by hook. If Lua is not
activated, the parser raises an error because a sw-description with an embedded script must
be parsed, but the interpreter is not available.

Each Lua function receives as parameter a table with the setup for the current entry. A hook
in Lua is in the format:

::

        function lua_hook(image)

image is a table where the keys are the list of available attributes. If an attribute contains
a "-", it is replaced with "_", because "-" cannot be used in Lua. This means, for example, that:

::

        install-if-different ==> install_if_different
        installed-directly   ==> installed_directly

Attributes can be changed in the Lua script and values are taken over on return.
The Lua function must return 2 values:

        - a boolean, to indicate whether the parsing was correct
        - the image table or nil to indicate that the image should be skipped

Example:

::

        function set_version(image)
	        print (\"RECOVERY_STATUS.RUN: \".. swupdate.RECOVERY_STATUS.RUN)
                for k,l in pairs(image) do
                        swupdate.trace(\"image[\" .. tostring(k) .. \"] = \" .. tostring(l))
                end
	        image.version = \"1.0\"
        	image.install_if_different = true
        	return true, image
        end


The example sets a version for the installed image. Generally, this is detected at runtime
reading from the target.

.. _sw-description-attribute-reference:

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
- bootenv: entries are pair with bootloader environment variable name and its
  value.


.. tabularcolumns:: |p{1.5cm}|p{1.5cm}|p{1.5cm}|L|
.. table:: Attributes in sw-description


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
   | device      | string   | images     | devicenode as found in /dev or a      |
   |             |          | files      | symlink to it. Can be specified as    |
   |             |          |            | absolute path or a name in /dev folder|
   |             |          |            | For example if /dev/mtd-dtb is a link |
   |             |          |            | to /dev/mtd3 "mtd3", "mtd-dtb",       |
   |             |          |            | "/dev/mtd3" and "/dev/mtd-dtb" are    |
   |             |          |            | valid names.                          |
   |             |          |            | Usage depends on handler.             |
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
   | preserve-\  | bool     | files      | flag to control whether the following |
   | attributes  |          |            | attributes will be preserved when     |
   |             |          |            | files are unpacked from an archive    |
   |             |          |            | (assuming destination filesystem      |
   |             |          |            | supports them, of course):            |
   |             |          |            | timestamp, uid/gid (numeric), perms,  |
   |             |          |            | file attributes, extended attributes  |
   +-------------+----------+------------+---------------------------------------+
   | type        | string   | images     | string identifier for the handler,    |
   |             |          | files      | as it is set by the handler when it   |
   |             |          | scripts    | registers itself.                     |
   |             |          |            | Example: "ubivol", "raw", "rawfile",  |
   +-------------+----------+------------+---------------------------------------+
   | compressed  | string   | images     | string to indicate the "filename" is  |
   |             |          | files      | compressed and must be decompressed   |
   |             |          |            | before being installed. the value     |
   |             |          |            | denotes the compression type.         |
   |             |          |            | currently supported values are "zlib" |
   |             |          |            | and "zstd".                           |
   +-------------+----------+------------+---------------------------------------+
   | compressed  | bool (dep| images     | Deprecated. Use the string form. true |
   |             | recated) | files      | is equal to 'compressed = "zlib"'.    |
   +-------------+----------+------------+---------------------------------------+
   | installed-\ | bool     | images     | flag to indicate that image is        |
   | directly    |          |            | streamed into the target without any  |
   |             |          |            | temporary copy. Not all handlers      |
   |             |          |            | support streaming.                    |
   +-------------+----------+------------+---------------------------------------+
   | name        | string   | bootenv    | name of the bootloader variable to be |
   |             |          |            | set.                                  |
   +-------------+----------+------------+---------------------------------------+
   | value       | string   | bootenv    | value to be assigned to the           |
   |             |          |            | bootloader variable                   |
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
   | description | string   |            | user-friendly description of the      |
   |             |          |            | swupdate archive (any string)         |
   +-------------+----------+------------+---------------------------------------+
   | reboot      | bool     |            | allows to disable reboot for the      |
   |             |          |            | current running update                |
   +-------------+----------+------------+---------------------------------------+
   | install-if\ | bool     | images     | flag                                  |
   | -different  |          | files      | if set, name and version are          |
   |             |          |            | compared with the entries in          |
   |             |          |            | sw-versions                           |
   +-------------+----------+------------+---------------------------------------+
   | install-if\ | bool     | images     | flag                                  |
   | -higher     |          | files      | if set, name and version are          |
   |             |          |            | compared with the entries in          |
   |             |          |            | sw-versions                           |
   +-------------+----------+------------+---------------------------------------+
   | encrypted   | bool     | images     | flag                                  |
   |             |          | files      | if set, file is encrypted             |
   |             |          | scripts    | and must be decrypted before          |
   |             |          |            | installing.                           |
   +-------------+----------+------------+---------------------------------------+
   | ivt         | string   | images     | IVT in case of encrypted artefact     |
   |             |          | files      | It has no value if "encrypted" is not |
   |             |          | scripts    | set. Each artefact can have an own    |
   |             |          |            | IVT to avoid attacker can guess the   |
   |             |          |            | the key.                              |
   |             |          |            | It is an ASCII string of 32 chars     |
   +-------------+----------+------------+---------------------------------------+
   | data        | string   | images     | This is used to pass arbitrary data   |
   |             |          | files      | to a handler.                         |
   |             |          | scripts    |                                       |
   +-------------+----------+------------+---------------------------------------+
   | sha256      | string   | images     | sha256 hash of image, file or script. |
   |             |          | files      | Used for verification of signed       |
   |             |          | scripts    | images.                               |
   +-------------+----------+------------+---------------------------------------+
   | embedded-\  | string   |            | Lua code that is embedded in the      |
   | script      |          |            | sw-description file.                  |
   +-------------+----------+------------+---------------------------------------+
   | offset      | string   | images     | Optional destination offset           |
   +-------------+----------+------------+---------------------------------------+
   | hook        | string   | images     | The name of the function (Lua) to be  |
   |             |          | files      | called when the entry is parsed.      |
   +-------------+----------+------------+---------------------------------------+
   | mtdname     | string   | images     | name of the MTD to update. Used only  |
   |             |          |            | by the flash handler to identify the  |
   |             |          |            | the mtd to update, instead of         |
   |             |          |            | specifying the devicenode             |
   +-------------+----------+------------+---------------------------------------+
