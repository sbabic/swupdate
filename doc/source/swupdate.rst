=================================
Sw-Management on embedded systems
=================================

Embedded Systems become more and more complex,
and their software reflects the augmentity complexity.
New features and fixes let much more as desirable that
the software on an embedded system can be updated
in a absolutely reliable way.

On a linux-based system, we can find in most cases
the following elements:
- the bootloader.
- the kernel and the DT (Device Tree) file.
- the root filesystem
- other filesystems, mounted at a later point
- customer data, in raw format or on a filesystem
- application specific software. For example, firmware
to be downloaded on connected microcontrollers, and so on.

Generally speaking, in most cases it is required to update
kernel and root filesystem, preserving user data - but cases vary.

In only a few cases it is required to update the bootloader,
too. In fact, updating the bootloader is quite always risky,
because a failure in the update breaks the board.
Restoring a broken board is possible in some cases,
but this is not left in most cases to the end user
and the system must be sent back to the manufacturer.

There are a lot of different concepts about updating
the software. I like to expone some of them, and then
explain why I have implemented this project.

Updating through the bootloader
===============================

Bootloaders do much more as simply start the kernel.
They have their own shell and can be managed using
a processor's peripheral, in most cases a serial line.
They are often scriptable, letting possible to implement
some kind of software update mechanism.

However, I found some drawbacks in this approach, that
let me search for another solution, based on an application
running on Linux:

Bootloader have limited access to peripherals.
----------------------------------------------

Not all peripherals supported by the kernel are
available with the bootloader. When it makes sense to add
support to the kernel, because the peripheral is then available
by the main application, it does not always make sense to duplicate
the effort to port the driver to the bootloader.

Bootloader's drivers are not updated
------------------------------------

Bootloader's drivers are mostly ported from the Linux kernel,
but due to adaptations they are not fixed or synchronized
with the kernel, while fixes flow regularly in the Linux kernel.
Some peripheral can then work in a not reliable ways,
and fixing the issues can be not easy. Drivers in boot loaders
are more or less a fork of the respective drivers in kernel.

Reduced filesystems
-------------------
The number of supported filesystems is limited and
porting a filesystem to the bootloader requires high effort.

Network support is limited
--------------------------
Network stack is limited, generally an update is possible via
UDP but not with TCP.

Interaction with the operator
-----------------------------

It is difficult to expone an interface to the operator,
such as a GUI with a browser or on a display.

A complex logic can be easier implemented with an application
else in the bootloader. Extending the bootloader becomes complicated
because the whole range of services and libraries are not available.

As example, the UBI / UBIFS for NAND devices contains a lot of
fixes in the kernel, that are not ported back to the bootloaders.
The same can be found for the USB stack. The effort to support
new peripherals or protocols is better to be used for the kernel
as for the bootloaders.

Bootloader's update advantages
------------------------------
However, this approach has some advantages,too:
- software for update is generally simpler.
- smaller footprint: a standalone application only for
software management requires an own kernel and a root filesystem.
Even if their size can be trimmed dropping what is not required
for updating the software, their size is not negligible.

Updating through a package manager
==================================

All Linux distributions are updating with a package manager.
Why is it not suitable for embedded ?

I cannot say it cannot be used, but there is an important drawback
using this approach. Embedded systems are well tested
with a specific software. Using a package manager
can put weirdness because the software itself
is not anymore "atomic", but split into a long
list of packages. How can be assured that an application
with library version x.y works, and also with different
versions of the same library ? How can be successful tested ?

For a manufacturer, it is generally better to say that
a new release of software (well tested by his test
engineers) is released, and the new software (or firmware)
is available for updating. Splitting in packages can
generate nightmare and high effort for the testers.


Strategies for an application doing software upgrade
====================================================

Instead of using the bootloader, an application can take
into charge to upgrade the system. The application can
use all services provided by the OS. The proposed solution
is then a stand-alone software, that follow customer rules and
performs checks to determine if a software is installable,
and then install the software on the desired storage.

The application can detect if the provided new software
is suitable for the hardware, and it is can also check if
the software is released by a verified authority. The range
of features can grow from small system to a complex one,
including the possibility to have pre- and post- install
scripts, and so on.

Different strategies can be used, depending on the system's
resources. I am listing some of them.

Double copy with fallback
-------------------------

If there are enough place on the storage to save
two copies of the whole sofware, it is possible to guarantee
that there is always a working copy even if the software update
is interrupted.

Each copy must contain the kernel, the root filesystem, and each
further component that can be updated. It is required
a mechanism to identify which version is running. A sinergy
with the bootloader is often necessary, because the bootloader must
decide which copy should be started. Again, it must be possible
to switch between the two copies.

The most evident drawback is the amount of required space. The
available space for each copy is less than half the size
of the storage. However, an update is always safe even in case of power off.

This project supports this strategy. The application as part of this project
should be installed in the rootfilesystem and started
or triggered as required. There is no
need of an own kernel, because the two copies guarantees that
it is always possible to upgrade the not running copy.

Single copy - running as standalone image
-----------------------------------------

The software upgrade application consists of kernel (maybe reduced
dropping not required drivers) and a small root filesystem, with the application
and its libraries. The whole size is much less than a single copy of
the system software. Depending on set up, I get sizes from 2.5 until 8 MB
for the standalone root filesystem. If the size is very important on small
systems, it becomes negligible on systems with a lot of storage
or big NANDs.

The system can be put in "upgrade" mode, simply signalling to the
bootloader that the upgrading software must be started. The way
can differ, for example setting a bootloader environment or using
and external GPIO.

The bootloader starts the upgrading software, starting the
swupdate kernel and the initrd image as rootfilesystem. Because it runs in RAM,
it is possible to upgrade the whole storage.

This concept consumes less space in storage as having two copies, but
it is not power off safe. However, it can be guaranteed that
the system goes automatically in upgrade mode when the productivity
software is not found or corrupted, as when the upgrade process
was interrupted. In fact, it is possible to consider
the upgrade procedure as a transaction, and only after the successful
upgrade the new software is set as "bootable". With these considerations,
an upgrade with this strategy is safe: it is always guaranteed that the
system boots and it is ready to get a new software, if the old one
is corrupted or cannot run.

=============================================
SWUPDATE: software update for embedded system
=============================================

This project is thought to help to update an embedded
system using a media (USB Pen), or from network. However,
it should be mainly considered as a framework, where
further protocols or installers can be added easy to
the application.

One use case is to update from an external local media, as
USB-Pen or SD-CArd. In this case, the update is done
without any intervention by an operator: it is thought
as "one-key-update", and the software is started at reset
simply pressing a key (or in any way that can be recognized
by the target), making all checks automatically. At the end,
the updating process reports only the status to the operator
(successful or failed).

The output can be displayed on a LCD using the framebuffer
device or directed to a serial line (Linux console).

It is generally used in the single copy approach, running
in a initrd (receipes are provided to generate with Yocto).
However, it is possible to use it in a double-copy approach.

If started in the network setup, swupdate starts an embedded
Webserver and waits for requests. The operator must upload
a suitable image, that swupdate checks and then install.
All output is notified to the operator's browser via AJAX.

Main Features
--------------

- check if a image is available. The image is built
  in a specified format (cpio) and it must contain
  a file describing the software that must be updated.

- swupdate is thought to update UBI volumes (mainly for NAND, but not only)
  and images in devices. Passing a whole image can still be updated
  as a partition on the SD card, or a MTD partition.

- new partition schema. This is bound with UBI volume.
  swupdate can recreate UBI volumes, resizing them and
  copying the new software. A special UBI volume with the name "data"
  is saved and restored after repartitioning with all data
  it contains,  to maintain user's data.

- support for compressed images, using the zlib library.
  tarball (tgz file) are supported.

- support for partitioned USB-pen or unpartitioned (mainly
  used by Windows).

- support for updating a single file inside a filesystem.
  The filesystem where to put the file must be described.

- performs chechksum for the single components of an image

- use a structured language to describe the image. This is done
  using the libconfig_ library as default parser, that uses a
  JSON-like description.

.. _libconfig:	http://www.hyperrealm.com/libconfig/

- use custom's choice for the description of the image. It is
  possible to write an own parser using the LUA language.
  An example using a XML description in LUS is provided
  in the examples directory.

- Allow to have a list of read-only devices / volumes that are not
  allowed to be updated (for example, bootloader, inventory data, etc.)

- Support for setting / erasing U-Boot variables

- Support for preinstall scripts. They run before updating the images

- Support for postinstall scripts. They run after updating the images.

- Network installer using an embedded Webserver (Mongoose Server
  was choosen, in the last version under LUA license). A different
  Webserver can be used.

- Can be configured to check for compatibility between software and hardware
  revisions. The software image must contain an entry declaring on which
  HW revision the software is allowed to run.
  swupdate refuses to install if the compatibility is not verified.

- support for image extraction. A manufacturer can require to have
  a single image that contains the software for more as one device.
  This simplifies the manufacturer's management and reduces
  their administrative costs having a single software product.
  swupdate receives the software as stream without temporary storing,
  and extracts only the required components for the device
  to be installed.

- Features are enabled / disabled using "make menuconfig".
  (Kbuild is inherited from busybox project)

Changes in bootloader code
--------------------------

The swupdate consists of kernel and a root filesystem
(image) that must be started by the bootloader.
In case using U-Boot, the following mechanism can be implemented:

U-Boot checks if a sw update is required (check gpio,
serial console, etc.). In case it is required, modifies
the variable "bootcmd" to start the swupdate kernel.

Format for the image
--------------------

cpio is used as container for its simplicity. The resulting image is very
simple to be built.
The image requires a file describing in details the single parts. This file
has a fix name ("sw-description") and must be the first file in the cpio archive.



      +-----------------+
      | sw-description  |
      +-----------------+
      |     Kernel      |
      |                 |
      +-----------------+
      |     Image 1     |
      +-----------------+
      |     Image2      |
      |                 |
      +-----------------+
      |     Image n     |
      +-----------------+




Then to produce an image, a script like this can be used:

::

	CONTAINER_VER="1.0"
	PRODUCT_NAME="my-software"
	FILES="sw-description image1.ubifs  \
	       image2.gz.u-boot uImage.bin myfile sdcard.img"
	for i in $FILES;do
		echo $i;done | cpio -ov -H crc >  ${PRODUCT_NAME}_${CONTAINER_VER}.swu


The single images can be put in any order inside the cpio container, with the exception
of sw-description, that must be the first one.


Format of the sw-description file using libconfig
-------------------------------------------------

sw-description follows the rule described in the libconfig manual. The
following example explains better the tags that are 

::

	software =
	{
		version = "0.1.0";

		hardware-compatibility: [ "1.0", "1.2", "1.3"];

		partitions: ( /* UBI Volumes */
			{
				name = "rootfs";
			  	size = 104896512; /* in bytes */
			},
			{
				name = "data";
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
				filename = "uImage.bin";
				volume = "kernel";
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
				type = "postinstall";
		 	},
			{
				filename = "display_info";
				type = "preinstall";
			}
		);

		uboot: (
			{
				name = "vram";
				value = "4M";
			},
			{
				name = "addfb";
				value = "setenv bootargs ${bootargs} vram=6M omapfb.vram=1:2M,2:2M,3:2M omapdss.def_disp=lcd"
			}
		);

	}

hardware-compatibility: [ "major.minor", "major.minor", ... ]

Example:

	hardware-compatibility: [ "1.0", "1.2", "1.3"];

This means that the software is compatible with HW-Revisions
1.0, 1.2 and 1.3, but not for 1.1 or other version not explicitely
listed here.

partitions: ( 
	{ 
		name = <volume name>;
		size = <size in bytes>;
	},
	.....
);

The volume "data", if present, is handled in a special way to save and restore
its data.

imeges (
	{
		filename = <Name in CPIO Archive>;
		volume = <destination volume>;
	}

or:
	{
		filename = <Name in CPIO Archive>;
		device = <device node >;
	}

The first format allows to update UBI-Volume. If "device" is given,
the image "filename" is copied into the specified device.

	files: (
		{
			filename = <Name in CPIO Archive>;
			path = <path in filesystem>;
			device = <device node >;
			filesystem = <filesystem for mount>;
		}
	);

Entries in "files" section are managed as single files. The attribute
"path" and "filesystem" are mandatory. Sw-Update copies the file in the path
specified after mounting the device.

	scripts: (
		{
			filename = <Name in CPIO Archive>;
			type = ["preinstall" | "postinstall"];
	 	},
	);


Scripts runs in the order they are put into the sw-description file.
The result of a script is valuated by sw-update, that stops the update
with an error if the result is <> 0.

Scripts are shell scripts, that can be run by the busybox shell. They are
copied into a temporary directory before execution and their name must
be unique inside the same cpio archive.

	uboot: (
		{
			name = <Name der Variabel>;
			value = <Wert für die Variabel>;
		},
	)

Running sw-update
-----------------

A run of swupdate consists mainly of the following steps:

- check for media (USB-pen)
- check for an image file. The extension must be .mcx
- extracts sw-description from the image and verifies it
  It parses sw-description creating a raw description in RAM
  about the activities that must be performed.
- Reads the cpio archive and proofs the checksum of each single file
  sw-update stops if the archive is not complete verified
- check for hardware-software compatibility, if any,
  reading hardware revision from hardware and matching
  with the table in sw-description.
- check that all components described in sw-description are
  really in the cpio archive.
- modify partitions, if required. This consists in a resize
  of UBI volumes, not a resize of MTD partition.
  A volume with the name "data" is saved and restored after
  resizing.
- runs pre-install scripts
- iterates through all images and save them according to
  sw-descripotion (in UBI volume, in devices, etc.)
- iterates through all "files" from sw-description and
  save them after mounting the filesystem specified.
- runs post-install scripts
- update u-boot environment, if changes are specified
  in sw-description.
- reports the status to the operator (stdout)

The first step that fails, stops the entire procedure and
an error is reported.
