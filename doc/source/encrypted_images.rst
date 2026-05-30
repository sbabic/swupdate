.. SPDX-FileCopyrightText: 2013-2021 Stefano Babic <stefano.babic@swupdate.org>
.. SPDX-License-Identifier: GPL-2.0-only

.. _sym-encrypted-images:

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

Backing the PKCS#11 Token with a TPM2 (tpm2-pkcs11)
----------------------------------------------------

The `tpm2-pkcs11 <https://github.com/tpm2-software/tpm2-pkcs11>`_ project
provides a PKCS#11 library that stores all key material inside the TPM2 chip.
This means the AES image-decryption key never appears in plaintext outside the
TPM at runtime.

Prerequisites
~~~~~~~~~~~~~

The TPM 2 chip must support AES, which is optional.

Install the runtime library and the management tool (package names vary by
distribution):

::

        apt-get install libtpm2-pkcs11-tools libtpm2-pkcs11-1

Setting up the TPM2-backed Token
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The recommended workflow is:

1. Generate the AES key on a trusted offline build host.
2. Use it to encrypt the update images with OpenSSL (see above).
3. Determine the slot-id of the TPM-backed token.
4. Import the key into the TPM-backed token on the target device.
5. Delete the plaintext key file from the target.

**Step 1: Initialize the persistent store**

Run once; creates a primary key inside the TPM:

::

        tpm2_ptool init

By default the store is created in ``~/.tpm2_pkcs11``.  For a system-wide
deployment set the environment variable ``TPM2_PKCS11_STORE`` to a suitable
path before running all ``tpm2_ptool`` commands, for example:

::

        export TPM2_PKCS11_STORE=/var/lib/tpm2_pkcs11
        mkdir -p $TPM2_PKCS11_STORE
        tpm2_ptool init --path=$TPM2_PKCS11_STORE

**Step 2: Create a new PKCS#11 token:**

::

        tpm2_ptool addtoken --pid=1 --sopin=<sopin> --userpin=<userpin> --label=swupdate

**Step 3: Determine the slot-id assigned to the token:**

::

        pkcs11-tool --module /usr/lib/libtpm2_pkcs11.so --list-slots

The output lists all available slots with their slot IDs, for example::

        Available slots:
        Slot 0 (0x1): swupdate
          token label        : swupdate
          ...

**Step 4: Import the AES image-encryption key into the TPM-backed token:**

::

        echo -n "390ad54490a4a5f53722291023c19e08ffb5c4677a59e958c96ffa6e641df040" | xxd -p -r > swupdate-aes-key.bin
        pkcs11-tool --module /usr/lib/libtpm2_pkcs11.so --slot <slot-id> --login --pin <userpin> --write-object swupdate-aes-key.bin --id CAFEBABE --label swupdate-aes-key --type secrkey --key-type AES:32
        rm swupdate-aes-key.bin

**Step 5: Verify the key is accessible:**

::

        pkcs11-tool --module /usr/lib/libtpm2_pkcs11.so --slot <slot-id> --login --pin <userpin> --list-objects

Configuring SWUpdate
~~~~~~~~~~~~~~~~~~~~~

Create the key file with a PKCS#11 URL pointing to the TPM-backed slot and
append the IV as the second field (replace ``1`` with the actual slot ID
determined above):

::

        pkcs11:slot-id=1;id=%CA%FE%BA%BE?pin-value=<userpin>&module-path=/usr/lib/libtpm2_pkcs11.so 65D793B87B6724BB27954C7664F15FF3

Pass the key file to SWUpdate with the ``-K`` parameter as usual.  Because the
decryption key is sealed inside the TPM, access is subject to the TPM's own
authorization policy in addition to the PKCS#11 user PIN.
