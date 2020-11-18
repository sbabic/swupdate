Symmetrically Encrypted Update Images
=====================================

SWUpdate allows one to symmetrically encrypt update images using the
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

        openssl enc -aes-256-cbc -in <INFILE> -out <OUTFILE> -K <KEY> -iv <IV>

where ``<INFILE>`` is the unencrypted source image file and ``<OUTFILE>`` is the
encrypted output image file to be referenced in ``sw-description``.
``<KEY>`` is the hex value part of the 2nd line of output from the key generation
command above and ``<IV>`` is the hex value part of the 3rd line.

Then, create a key file to be supplied to SWUpdate via the `-K` switch by 
putting the key and initialization vector hex values on one line
separated by whitespace, e.g., for above example values

::

        B78CC67DD3DC13042A1B575184D4E16D6A09412C242CE253ACEE0F06B5AD68FC 65D793B87B6724BB27954C7664F15FF3


For earlier versions of SWUpdate it was falsely noted that passing the SALT as a
3rd parameter would increase security. Key and IV are enough for maximum security,
salt doesn't add any value.

Encryption of UBI volumes
-------------------------

Due to a limit in the Linux kernel api for UBI volumes, the size reserved to be
written on disk should be declared before actually write anything.
Unfortunately, the size of an encrypted image is not know until the complete
decryption, thus preventing to correctly declare the size of the file to be
written on disk.

For this reason UBI images can declare the special property "decrypted-size" like
this:

::

	images: ( {
			filename = "rootfs.ubifs.enc";
			volume = "rootfs";
			encrypted = true;
			properties = {decrypted-size = "104857600";}
		}
	);

The real size of the decrypted image should be calculated and written to the
sw-description before assembling the cpio archive.
In this example, 104857600 is the size of the rootfs after the decryption: the
encrypted size is by the way larger.

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
