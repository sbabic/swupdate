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

        openssl enc -aes-256-cbc -k <PASSPHRASE> -P -md sha1

The key and initialization vector is generated based on the given ``<PASSPHRASE>``.
The output of the above command looks like this:

::

        salt=CE7B0488EFBF0D1B
        key=B78CC67DD3DC13042A1B575184D4E16D6A09412C242CE253ACEE0F06B5AD68FC
        iv =65D793B87B6724BB27954C7664F15FF3

Then, encrypt an image using this information via

::

        openssl enc -aes-256-cbc -in <INFILE> -out <OUTFILE> -K <KEY> -iv <IV> -S <SALT>

where ``<INFILE>`` is the unencrypted source image file and ``<OUTFILE>`` is the
encrypted output image file to be referenced in ``sw-description``.
``<KEY>`` is the hex value part of the 2nd line of output from the key generation
command above, ``<IV>`` is the hex value part of the 3rd line, and ``<SALT>`` is
the hex value part of the 1st line.

Then, create a key file to be supplied to SWUpdate via the `-K` switch by 
putting the key, initialization vector, and salt hex values on one line
separated by whitespace, e.g., for above example values

::

        B78CC67DD3DC13042A1B575184D4E16D6A09412C242CE253ACEE0F06B5AD68FC 65D793B87B6724BB27954C7664F15FF3 CE7B0488EFBF0D1B


Note that, while not recommended and for backwards compatibility, OpenSSL may be
used without salt. For disabling salt, add the ``-nosalt`` parameter to the key
generation command above. Accordingly, drop the ``-S <SALT>`` parameter in the
encryption command and omit the 3rd field of the key file to be supplied to
SWUpdate being the salt.


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
