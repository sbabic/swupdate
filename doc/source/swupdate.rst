=============================================
swupdate: software update for embedded system
=============================================

Overview
========

This project is thought to help to update an embedded
system from a storage media or from network. However,
it should be mainly considered as a framework, where
further protocols or installers (in swupdate they are called handlers)
can be easily added to the application.

One use case is to update from an external local media, as
USB-Pen or SD-Card. In this case, the update is done
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

If started for a remote update, swupdate starts an embedded
Webserver and waits for requests. The operator must upload
a suitable image, that swupdate checks and then install.
All output is notified to the operator's browser via AJAX 
notifications.

Single image delivery
---------------------

The main concept is that the manufacturer delivers a single
big image. All single images are packed together (cpio was chosen
for its simplicity and because can be streamed) together with
an additional file (sw-description), that contains meta
information about each single image.

The format of sw-description can be customized: swupdate can be
configured to use its internal parser (based on libconfig), or calling
an external parser in LUA.



.. image:: images/image_format.png


Changing the rules to accept images with an external parser,
let to extend to new image types and how they are installed.
In fact, the scope of the parser is to retrieve which single
images must be installed and how.
swupdate implements "handlers" to install a single image:
there are handlers to install images into UBI volumes,
or to a SD card, a CFI Flash, and so on. It is then easy to
add an own handler if a very special installer is required.

For example we can think at a project with a main processor and
one or several microcontrollers. Let's say for simplicity that
the main processor communicates with the microcontrollers via
UARTS using a proprietary protocol. The software on the microcontrollers
can be updated using the proprietary protocol.

It is possible to extend swupdate writing a handler, that implements
the part of the proprietary protocol to perform the upgrade
on the microcontroller. The parser must recognize which image must be
installed with the new handler, and swupdate will call the handler
during the installation process.

Handling configuration differences
----------------------------------

The concept can be extended to deliver a single image
containing the release for multiple devices. Each device has its own
kernel, dtb and root filesystem, or they can share some parts.

Currently this is managed (and already used in a real project) by
writing an own parser, that checks which images must be installed
after recognizing which is the device where software is running.

Because the external parser can be written in LUA and it is
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
By reading the delivered image, swupdate will ignore all images that
are not in the list processed by the parser. In this way, it is possible
to have a single delivered image for the update of multiple devices.

Streaming feature
-----------------

Even if not yet fully implemented, swupdate is thought to be able
to stream the received image directly into the target, without
any temporary copy. In fact, the single installer (handler) receive
as input the file descriptor set at the beginning of the image
that must be installed.

The reason why this feature is not yet fully implemented and some parts
are temporary extracted into /tmp is due to the fact that it is then not
possible to make checks on the whole delivered software before installing.
By streaming, it can happen that only a part is delivered, for example because
the network connection is suddenly broken. At the moment, the images making
a software release for a device are extracted from the stream and copied into
/tmp, and then a check runs before installing.

The temporary copy is done only when updated from network. When the image
is stored on an external storage, there is no need of that copy.

List of supported features
--------------------------

- check if a image is available. The image is built
  in a specified format (cpio) and it must contain
  a file describing the software that must be updated.

- swupdate is thought to update UBI volumes (mainly for NAND, but not only)
  and images on devices. Passing a whole image can still be updated
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

- chechksum for the single components of an image

- use a structured language to describe the image. This is done
  using the libconfig_ library as default parser, that uses a
  JSON-like description.

.. _libconfig:	http://www.hyperrealm.com/libconfig/

- use custom's choice for the description of the image. It is
  possible to write an own parser using the LUA language.
  An example using a XML description in LUS is provided
  in the examples directory.

- Support for setting / erasing U-Boot variables

- Support for preinstall scripts. They run before updating the images

- Support for postinstall scripts. They run after updating the images.

- Network installer using an embedded Webserver (Mongoose Server
  was choosen, in the version under LUA license). A different
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

Configuration and installation
==============================

swupdate is configurable via "make menu config". The small footprint
is reached using the internal parser and disabling the webserver.

To compile, you have to follow the steps:

- configure the options

	make menuconfig

- generate the code

	make

To cross-compile, set the CC and CXX variables before running make.
It is also possible to set the cross-compiler prefix as option with
make menuconfig.

The result is the binary "swupdate". 

To start it expecting the image from a file:

::

	        swupdate -i <filename>

To start with the embedded webserver:

::

	         swupdate -w "<webserver options"

The main important parameter for the webserver is "document_root".

::

	         swupdate -w "-document_root ./www"

The embedded webserver is taken from the Mongoose project (last release
with LUA license). Additional paramters can be found in mongoose
documentation.
This uses as website the pages delivered with the code. Of course,
they can be customized and replaced. The website uses AJAX to communicate
with swupdate, and to show the progress of the update to the operator.

The default port of the Webserver is 8080. You can then connect to the target with:


::

	http://<target_ip>:8080

If it works, the start page should be displayed as in next figure.

.. image:: images/website.png

If a correct image is downloaded, swupdate starts to process the received image.
All notifications are sent back to the browser. swupdate provides a mechanism
to send to a receiver the progress of the installation. In fact, swupdate
takes a list of objects that registers itself with the application
and they will be informed any time the application calls the notify() function.
This allows also for self-written handlers to inform the upper layers about
error conditions or simply return the status. It is then simply to add
own receivers to implement customized way to display the results: displaying
on a LCD (if the target has one), or sending back to another device via
network.
An example of the notifications sent back to the browser is in the next figure:

.. image:: images/webprogress.png

Changes in bootloader code
==========================

The swupdate consists of kernel and a root filesystem
(image) that must be started by the bootloader.
In case using U-Boot, the following mechanism can be implemented:

- U-Boot checks if a sw update is required (check gpio, serial console, etc.).
- the script "altbootcmd" sets the rules to start swupdate
- in case swupdate is required, u-boot run the script "altbootcmd"

Is it safe to change U-Boot environment ? Well, it is, but U-Boot must
be configured correctly. U-Boot supports two copies of the environment
to be power-off safe during a an evironment update. The board's
configuration file must have defined CONFIG_ENV_OFFSET_REDUND or
CONFIG_ENV_ADDR_REDUND. Check in U-Boot documentation for these
constants and how to use them.

There are a further enhancement that can be optionally integrated
into u-boot to make the system safer. The most important I will
suggest is to add support for boot counter in u-boot (documentation
is in U-Boot docs). This allows U-Boot to track for attempts to
successfully run the application, and if the boot counter is
greater as a limit, can start automatically swupdate to replace
a corrupt software.

Building a single image
=======================

cpio is used as container for its simplicity. The resulting image is very
simple to be built.
The file describing the images ("sw-description", but the name can be configured)
must be the first file in the cpio archive.

To produce an image, a script like this can be used:

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

Using the default parser, sw-description follows the rule described
in the libconfig manual.
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
		 	},
			{
				filename = "display_info";
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

The single tags have this meaning:

hardware-compatibility: [ "major.minor", "major.minor", ... ]

Example:

	hardware-compatibility: [ "1.0", "1.2", "1.3"];

This means that the software is compatible with HW-Revisions
1.0, 1.2 and 1.3, but not for 1.1 or other version not explicitely
listed here.

::

	partitions: ( 
		{ 
			name = <volume name>;
			size = <size in bytes>;
		},
	);

The volume "data", if present, is handled in a special way to save and restore
its data.

::

	images (
		{
			filename = <Name in CPIO Archive>;
			volume = <destination volume>;
		},
		{
			filename = <Name in CPIO Archive>;
			device = <device node >;
		},
	);


The first format allows to update UBI-Volume. If "device" is given,
the image "filename" is copied into the specified device.

::

	files: (
		{
			filename = <Name in CPIO Archive>;
			path = <path in filesystem>;
			device = <device node >;
			filesystem = <filesystem for mount>;
		}
	);

Entries in "files" section are managed as single files. The attribute
"path" and "filesystem" are mandatory. swupdate copies the file in the path
specified after mounting the device.

::

	scripts: (
		{
			filename = <Name in CPIO Archive>;
	 	},
	);

Scripts runs in the order they are put into the sw-description file.
The result of a script is valuated by swupdate, that stops the update
with an error if the result is <> 0.

Scripts are LUA scripts and they are run using the internal interpreter.
They are copied into a temporary directory before execution and their name must
be unique inside the same cpio archive.
Scripts must have at least one of the following functions:

::
	function preinst()

swupdate scans for all scripts and check for a preinst function. It is
called before installing the images.


::
	function postnst()

swupdate scans for all scripts and check for a postinst function. It is
called after installing the images.

::

	uboot: (
		{
			name = <Name der Variabel>;
			value = <Wert für die Variabel>;
		},
	)

Running swupdate
-----------------

A run of swupdate consists mainly of the following steps:

- check for media (USB-pen)
- check for an image file. The extension must be .swu
- extracts sw-description from the image and verifies it
  It parses sw-description creating a raw description in RAM
  about the activities that must be performed.
- Reads the cpio archive and proofs the checksum of each single file
  swupdate stops if the archive is not complete verified
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
- iterates through all images and call the corresponding
  handler for installing on target.
- runs post-install scripts
- update u-boot environment, if changes are specified
  in sw-description.
- reports the status to the operator (stdout)

The first step that fails, stops the entire procedure and
an error is reported.
