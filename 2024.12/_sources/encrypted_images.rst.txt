.. SPDX-FileCopyrightText: 2013-2021 Stefano Babic <stefano.babic@swupdate.org>
.. SPDX-License-Identifier: GPL-2.0-only

Symmetrically Encrypted Update Images
=====================================

SWUpdate allows one to symmetrically encrypt update images using the
AES block cipher in CBC mode. The following shows encryption with 256
bit key length but you may use other key lengths as well.


Building an Encrypted SWU Image
-------------------------------

First, create a key; for aes-256-cbc we need 32 bytes of key and 16 bytes
for an initialisation vector (IV).
A complete documentation can be found at the
`OpenSSL Website <https://www.openssl.org/docs/manmaster/man1/openssl.html>`_.

::

        openssl rand -hex 32
        # key, for example 390ad54490a4a5f53722291023c19e08ffb5c4677a59e958c96ffa6e641df040
        openssl rand -hex 16
        # IV, for example d5d601bacfe13100b149177318ebc7a4

Then, encrypt an image using this information via

::

        openssl enc -aes-256-cbc -in <INFILE> -out <OUTFILE> -K <KEY> -iv <IV>

where ``<INFILE>`` is the unencrypted source image file and ``<OUTFILE>`` is the
encrypted output image file to be referenced in ``sw-description``.
``<KEY>`` is the hex value part of the 2nd line of output from the key generation
command above and ``<IV>`` is the hex value part of the 3rd line.

Then, create a key file to be supplied to SWUpdate via the `-K` switch by 
putting the key and initialization vector hex values on one line
separated by whitespace, e.g., for above example values

::

        390ad54490a4a5f53722291023c19e08ffb5c4677a59e958c96ffa6e641df040 d5d601bacfe13100b149177318ebc7a4


Previous versions of SWUpdate allowed for a salt as third word in key file,
that was never actually used for aes and has been removed.

You should change the IV with every encryption, see CWE-329_. The ``ivt``
sw-description attribute overrides the key file's IV for one specific image.

.. _CWE-329: http://cwe.mitre.org/data/definitions/329.html

Encryption of UBI volumes
-------------------------

Due to a limit in the Linux kernel API for UBI volumes, the size reserved to be
written on disk should be declared before actually writing anything.

See the property "decrypted-size" in UBI Volume Handler's documentation.

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
				ivt = "65D793B87B6724BB27954C7664F15FF3";
        		}
        	);
        }


Running SWUpdate with Encrypted Images
--------------------------------------

Symmetric encryption support is activated by setting the ``ENCRYPTED_IMAGES``
option in SWUpdate's configuration. Use the `-K` parameter to provide the
symmetric key file generated above to SWUpdate.

Decrypting with a PKCS#11 token
-------------------------------

PKCS#11 support is activated by setting the ``PKCS11`` option in SWUpdate's
configuration. The key file has to have a PKCS#11 URL instead of the key then,
containing at least the elements of this example:

::

        pkcs11:slot-id=42;id=%CA%FE%BA%BE?pin-value=1234&module-path=/usr/lib/libsofthsm2.so 65D793B87B6724BB27954C7664F15FF3

The encryption key can be imported to the PKCS#11 token by using ``pkcs11-tool`` as follow:

::

        echo -n "390ad54490a4a5f53722291023c19e08ffb5c4677a59e958c96ffa6e641df040" |  xxd -p -r > swupdate-aes-key.bin
        pkcs11-tool --module /usr/lib/libsofthsm2.so --slot 0x42 --login --write-object swupdate-aes-key.bin  --id CAFEBABE --label swupdate-aes-key  --type secrkey --key-type AES:32
