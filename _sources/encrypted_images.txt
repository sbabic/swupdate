Symmetrically Encrypted Update Images
=====================================

SWUpdate allows to symmetrically encrypt update images using the
256 bit AES block cipher in CBC mode.


Building an Encrypted SWU Image
-------------------------------

First, create a key via `openssl` which is part of the OpenSSL project.
A complete documentation can be found at the
`OpenSSL Website <https://www.openssl.org/docs/manmaster/man1/openssl.html>`_.

::

        openssl enc -aes-256-cbc -k <PASSPHRASE> -nosalt -P -md sha1

The key and initialization vector is generated based on the given ``<PASSPHRASE>``.
The output of the above command looks like this:

::

        key=B60D121B438A380C343D5EC3C2037564B82FFEF3542808AB5694FA93C3179140
        iv =20578C4FEF1AEE907B1DC95C776F8160


Then, encrypt an image using this information via

::

        openssl enc -aes-256-cbc -in <INFILE> -out <OUTFILE> -K <KEY> -iv <IV>

where ``<INFILE>`` is the unencrypted source image file and ``<OUTFILE>`` is the
encrypted output image file to be referenced in ``sw-description``.
``<KEY>`` is the hex value part of the first line of output from the key generation
command above and ``<IV>`` is the hex value part of the second line. 

Then, create a key file to be supplied to SWUpdate via the `-K` switch by 
putting the key and initialization vector hex values on one line separated by
whitespace, e.g., for above example values

::

        B60D121B438A380C343D5EC3C2037564B82FFEF3542808AB5694FA93C3179140 20578C4FEF1AEE907B1DC95C776F8160


Example sw-description with Encrypted Image
-------------------------------------------

The following example is a (minimal) ``sw-description`` for installing
a Yocto image onto a Beaglebone. Pay attention to the ``encrypted = true;``
setting.

::

        software =
        {
        	version = "0.0.1";
        	images: ( {
        			filename = "core-image-full-cmdline-beaglebone.ext3.enc";
        			device = "/dev/mmcblk0p3";
        			encrypted = true;
        		}
        	);
        }


Running SWUpdate with Encrypted Images
--------------------------------------

Symmetric encryption support is activated by setting the ``ENCRYPTED_IMAGES``
option in SWUpdate's configuration. Use the `-K` parameter to provide the
symmetric key file generated above to SWUpdate.
