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
containing the release of the product. As described in Yocto's documentation
about layers, you should include it in your *bblayers.conf* file to use it.

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
  Setting the variable for each artifact telles the class which file must
  be packed into the SWU image.


::

        SWUPDATE_IMAGES_FSTYPES[core-image-full-cmdline] = ".ubifs"

- **SWUPDATE_IMAGES_NOAPPEND_MACHINE** : flag to use drop the machine name from the
  artifact file. Most images in *deploy* have the name of the Yocto's machine in the
  filename. The class adds automatically the name of the MACHINE to the file, but some
  artifacts can be deploes without it.

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
