.. SPDX-FileCopyrightText: 2013-2021 Stefano Babic <stefano.babic@swupdate.org>
.. SPDX-License-Identifier: GPL-2.0-only

==================================
meta-swupdate: building with Yocto
==================================

Overview
========

The Yocto-Project_ is a community project under the umbrella of the Linux
Foundation that provides tools and template to create the own custom Linux-based
software for embedded systems.

.. _Yocto-Project: http://www.yoctoproject.org
.. _meta-SWUpdate:  https://github.com/sbabic/meta-swupdate.git

Add-on features can be added using *layers*. meta-swupdate_ is the layer to
cross-compile the SWUpdate application and to generate the compound SWU images
containing the release of the product.  It contains the required changes
for mtd-utils and for generating Lua. Using meta-SWUpdate is a
straightforward process. As described in Yocto's documentation
about layers, you should include it in your *bblayers.conf* file to use it.

Add meta-SWUpdate as usual to your bblayers.conf. You have also
to add meta-oe to the list.

In meta-SWUpdate there is a recipe to generate an initrd with a
rescue system with SWUpdate. Use:

::

	MACHINE=<your machine> bitbake swupdate-image

You will find the result in your tmp/deploy/<your machine> directory.
How to install and start an initrd is very target specific - please
check in the documentation of your bootloader.

What about libubootenv ?
========================

This is a common issue when SWUpdate is built. SWUpdate depends on this library,
that is generated from the U-Boot's sources. This library allows one to safe modify
the U-Boot environment. It is not required if U-Boot is not used as bootloader.
If SWUpdate cannot be linked, you are using an old version of U-Boot (you need
at least 2016.05). If this is the case, you can add your own recipe for
the package u-boot-fw-utils, adding the code for the library.

It is important that the package u-boot-fw-utils is built with the same
sources of the bootloader and for the same machine. In fact, the target
can have a default environment linked together with U-Boot's code,
and it is not (yet) stored into a storage. SWUpdate should be aware of
it, because it cannot read it: the default environment must be linked
as well to SWUpdate's code. This is done inside the libubootenv.

If you build for a different machine, SWUpdate will destroy the
environment when it tries to change it the first time. In fact,
a wrong default environment is taken, and your board won't boot again.

To avoid possible mismatch, a new library was developed to be hardware independent.
A strict match with the bootloader is not required anymore. The meta-swupdate layer
contains recipes to build the new library (`libubootenv`) and adjust SWUpdate to be linked
against it. To use it as replacement for u-boot-fw-utils:

        - set PREFERRED_PROVIDER_u-boot-fw-utils = "libubootenv"
        - add to SWUpdate config:

::

                CONFIG_UBOOT=y

With this library, you can simply pass the default environment as file (u-boot-initial-env).
It is recommended for new project to switch to the new library to become independent from
the bootloader.

The swupdate class
==================

meta-swupdate contains a class specific for SWUpdate. It helps to generate the
SWU image starting from images built inside the Yocto. It requires that all
components, that means the artifacts that are part of the SWU image, are present
in the Yocto's deploy directory.  This class should be inherited by recipes
generating the SWU. The class defines new variables, all of them have the prefix
*SWUPDATE_* in the name.

- **SWUPDATE_IMAGES** : this is a list of the artifacts to be packaged together.
  The list contains the name of images without any extension for MACHINE or
  filetype, that are added automatically.
  Example :

::

        SWUPDATE_IMAGES = "core-image-full-cmdline uImage"

- **SWUPDATE_IMAGES_FSTYPES** : extension of the artifact. Each artifact can
  have multiple extension according to the IMAGE_FSTYPES variable.
  For example, an image can be generated as tarball and as UBIFS for target.
  Setting the variable for each artifact tells the class which file must
  be packed into the SWU image.


::

        SWUPDATE_IMAGES_FSTYPES[core-image-full-cmdline] = ".ubifs"

- **SWUPDATE_IMAGES_NOAPPEND_MACHINE** : flag to use drop the machine name from the
  artifact file. Most images in *deploy* have the name of the Yocto's machine in the
  filename. The class adds automatically the name of the MACHINE to the file, but some
  artifacts can be deployed without it.

::

        SWUPDATE_IMAGES_NOAPPEND_MACHINE[my-image] = "1"

- **SWUPDATE_SIGNING** : if set, the SWU is signed. There are 3 allowed values:
  RSA, CMS, CUSTOM. This value determines used signing mechanism.
- **SWUPDATE_SIGN_TOOL** : instead of using openssl, use SWUPDATE_SIGN_TOOL to sign
  the image. A typical use case is together with a hardware key. It is
  available if SWUPDATE_SIGNING is set to CUSTOM
- **SWUPDATE_PRIVATE_KEY** : this is the file with the private key used to sign the
  image using RSA mechanism. Is available if SWUPDATE_SIGNING is set to RSA.
- **SWUPDATE_PASSWORD_FILE** : an optional file containing the password for the private
  key. It is available if SWUPDATE_SIGNING is set to RSA.
- **SWUPDATE_CMS_KEY** : this is the file with the private key used in signing
  process using CMS mechanism. It is available if SWUPDATE_SIGNING is set to
  CMS.
- **SWUPDATE_CMS_CERT** : this is the file with the certificate used in signing
  process using CMS method. It is available if SWUPDATE_SIGNING is
  set to CMS.

- **SWUPDATE_AES_FILE** : this is the file with the AES password to encrypt artifact. A new `fstype` is
  supported by the class (type: `enc`). SWUPDATE_AES_FILE is generated as output from openssl to create
  a new key with

  ::

                openssl enc -aes-256-cbc -k <PASSPHRASE> -P -md sha1 -nosalt > $SWUPDATE_AES_FILE

  To use it, it is enough to add IMAGE_FSTYPES += "enc" to the  artifact. SWUpdate supports decryption of
  compressed artifact, such as

  ::

        IMAGE_FSTYPES += ".ext4.gz.enc"


Automatic sha256 in sw-description
----------------------------------

The swupdate class takes care of computing and inserting sha256 hashes in the
sw-description file. The attribute *sha256* **must** be set in case the image
is signed. Each artifact must have the attribute:

::

        sha256 = "$swupdate_get_sha256(artifact-file-name)"

For example, to add sha256 to the standard Yocto core-image-full-cmdline:

::

        sha256 = "$swupdate_get_sha256(core-image-full-cmdline-machine.ubifs)";


The name of the file must be the same as in deploy directory.

BitBake variable expansion in sw-description
--------------------------------------------

To insert the value of a BitBake variable into the update file, pre- and
postfix the variable name with "@@".
For example, to automatically set the version tag:

::

        version = "@@DISTRO_VERSION@@";

Automatic versions in sw-description
------------------------------------

By setting the version tag in the update file to `$swupdate_get_pkgvar(<package-name>)` it is
automatically replaced with `PV` from BitBake's package-data-file for the package
matching the name of the provided <package-name> tag.
For example, to set the version tag to `PV` of package `u-boot`:

::

        version = "$swupdate_get_pkgvar(u-boot)";

To automatically insert the value of a variable from BitBake's package-data-file
different to `PV` (e.g. `PKGV`) you can append the variable name to the tag:
`$swupdate_get_pkgvar(<package-name>@<package-data-variable>)`
For example, to set the version tag to `PKGV` of package `u-boot`:

::

        version = "$swupdate_get_pkgvar(u-bootPKGV)";

Using checksum for version
--------------------------

It is possible to use the hash of an artifact as the version in order to use
"install-if-different".  This allows versionless artifacts to be skipped if the
artifact in the update matches the currently installed artifact.

In order to use the hash as the version, the sha256 hash file placeholder
described above in Automatic sha256 in sw-description must be used for version.

Each artifact must have the attribute:

::

        version = "@artifact-file-name"

The name of the file must be the same as in deploy directory.

Template for recipe using the class
-----------------------------------

::

        DESCRIPTION = "Example recipe generating SWU image"
        SECTION = ""

        LICENSE = ""

        # Add all local files to be added to the SWU
        # sw-description must always be in the list.
        # You can extend with scripts or wahtever you need
        SRC_URI = " \
            file://sw-description \
            "

        # images to build before building swupdate image
        IMAGE_DEPENDS = "core-image-full-cmdline virtual/kernel"

        # images and files that will be included in the .swu image
        SWUPDATE_IMAGES = "core-image-full-cmdline uImage"

        # a deployable image can have multiple format, choose one
        SWUPDATE_IMAGES_FSTYPES[core-image-full-cmdline] = ".ubifs"
        SWUPDATE_IMAGES_FSTYPES[uImage] = ".bin"

        inherit swupdate

Simplified version for just image
---------------------------------

In many cases there is a single image in the SWU. This is for example when
just rootfs is updated. The generic case described above required an additional
recipe that must be written and maintained. For this reason, a simplified version
of the class is introduced that allowed to build the SWU from the image recipe.

Users just need to import the `swupdate-image` class. This already sets some variables.
A sw-description must still be added into a `files` directory, that is automatically searched by the class.
User still needs to set SWUPDATE_IMAGE_FSTYPES[`your image`] to the fstype that should be packed
into the SWU - an error is raised if the flag is not set.

In the simple way, your recipe looks like

::
        <your original recipe code>

        SWUPDATE_IMAGES_FSTYPES[<name of your image>] = <fstype to be put into SWU>
        inherit swupdate-image

What about grub ?
=================
In order to use swupdate with grub, swupdate needs to be configured to use grub. Some of
the imporatant configurations are **CONFIG_GRUBENV_PATH="/path/to/grubenv"**,
where **"/path/to/grubenv"** is thepath to grub environment.
Example: "/boot/EFI/BOOT/grubenv".

The grubenv file should be created using grub-editenv tool, because it is a **1024-byte file**, therefore,
any modification using nano or vim will only corrupt the file, and grub will not be able to use it.

You can create a grubenv file using these commands for instance:
::

        GRUBENV="/path/to/grubenv"
        grub-editenv $GRUBENV create
        grub-editenv $GRUBENV set rootfs=2
        grub-editenv $GRUBENV set kernel=2

grub-editenv is a tool that is integrated to yocto.

When the grubenv file is created, grub should be configured to use it.
This configuration should be in the configuration file grub.cfg.
Here is an example of grub.cfg that loads the environment file before booting:
::

        # Take a kernel and a rootfs by default in case grubenv is corrupted
        rootfs=1
        kernel=1
        serial --unit=0 --speed=115200 --word=8 --parity=no --stop=1
        default=boot
        # set timeout to zero to boot without timeout
        timeout=0
        # load grubenv the environment file that contains the value of rootfs and kernel variables
        load_env -f "/path/to/grubenv"
        # detect which memory contains 5 partitions
        for i in 1 2 3 4 5; do  if [ -d (hd${i},gpt5)/ ]; then drive=${i};fi ; done
        # detect which rootfs should we boot with
        if [ ${rootfs} = "1" ]; then rootfs_part=4 ; elif [ ${rootfs} = "2" ]; then rootfs_part=5 ; fi
        # detect which kernel should we boot with
        if [ ${kernel} = "1" ]; then kernel_part="(hd${drive},gpt2)" ; elif [ ${kernel} = "2" ]; then kernel_part="(hd${drive},gpt3)" ; fi

        # The menuentry that is used to boot (more can be added if it is wanted)
        menuentry 'boot'{
        linux ${kernel_part}/bzImage root=/dev/mmcblk1p${rootfs_part} rw rootwait quiet console=ttyS2,115200 console=tty0 panic=5
        }

The grub.cfg above is merely an example, and can be modified as the user wants to,
as long as it loads the environment variables,and it boots correctly using these environment
variables. 
