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
                CONFIG_UBOOT_NEWAPI=y

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
  process using using CMS method. It is available if SWUPDATE_SIGNING is
  set to CMS.

- **SWUPDATE_AES_FILE** : this is the file with the AES password to encrypt artifact. A new `fstype` is
  supported by the class (type: `enc`). SWUPDATE_AES_FILE is generated as output from openssl to create
  a new key with

  ::

                openssl enc -aes-256-cbc -k <PASSPHRASE> -P -md sha1 > $SWUPDATE_AES_FILE

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

        sha256 = "@artifact-file-name"

For example, to add sha256 to the standard Yocto core-image-full-cmdline:

::

        sha256 = "@core-image-full-cmdline-machine.ubifs";


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

By setting the version tag in the update file to `@SWU_AUTO_VERSION` it is
automatically replaced with `PV` from BitBake's package-data-file for the package
matching the name of the provided filename tag.
For example, to set the version tag to `PV` of package `u-boot`:

::

        filename = "u-boot";
        ...
        version = "@SWU_AUTO_VERSION";

Since the filename can differ from package name (deployed with another name or
the file is a container for the real package) you can append the correct package
name to the tag: `@SWU_AUTO_VERSION:<package-name>`.
For example, to set the version tag of the file `packed-bootloader` to `PV` of
package `u-boot`:

::

        filename = "packed-bootloader";
        ...
        version = "@SWU_AUTO_VERSION:u-boot";

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
