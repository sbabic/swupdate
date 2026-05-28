.. SPDX-FileCopyrightText: 2025 Michael Glembotzki <michael.glembotzki@iris-sensing.com>
.. SPDX-License-Identifier: GPL-2.0-only

Asymmetrically Encrypted Update Images
======================================

Asymmetrically encrypted update images are realized by an asymmetrical
encrypted sw-description, making it possible to decrypt images device specific.
The artifacts themselves are still encrypted symmetrically. An AES key must be
provided in the sw-description. At the moment only Cryptographic Message Syntax
(CMS) is available for asymmetic decryption.


Use Cases
---------

- Asymmetrically encrypted update images, with individual device key pairs, are
  inherently more secure than a purely symmetrical solution, because one
  compromised private device key does not affect the security of the others.
- If ``CONFIG_SIGNED_IMAGES`` is enabled too and a device's private key is
  compromised, the key pair can be excluded from the list of eligible devices
  for receiving new update images.
- The AES key can be securely **exchanged** with each new update image, as it is
  part of the sw-description, even in the absence of direct access to the
  device.


Create a Self-Signed Device Key Pair
------------------------------------

As an example, an elliptic curve key pair (PEM) is generated for a single
device. These steps must be repeated for all other devices. An RSA key pair
could be used in the same way.

::

        # Create a private key and a self-signed certificate
        openssl ecparam -name secp521r1 -genkey -noout -out device-key-001.pem
        openssl req -new -x509 -key device-key-001.pem -out device-cert-001.pem -subj "/O=SWUpdate /CN=target"

        # Combine the private key and the certificate into a single file
        # Note: The certificate is optional, but it speeds up key matching,
        # especially when there are many different device keys.
        cat device-key-001.pem device-cert-001.pem > device-001.pem


Symmetric Encryption of Artifacts
---------------------------------

Generate an AES key and IV, as familiar from
:ref:`symmetric image encryption <sym-encrypted-images>`. The encryption
process for the artifacts remains unchanged.


Encryption of sw-description for Multiple Devices
-------------------------------------------------

All device certificates togther are used for encryption.

::

        # Encrypt sw-description for multiple devices
        openssl cms -encrypt -aes-256-cbc -in sw-description.in -out sw-description -outform DER $(printf ' -recip %q' certs/device-cert-*.pem)

Assume `sw-description.in` is the plaintext input and `sw-description` is the
encrypted output. After `-recip`, a list of all device certificates is expected.
One for each device you want to encrypt for.


Decryption of sw-description for a Single Device
------------------------------------------------

The combined key pair (private key and certificate) is used for decryption.
SWUpdate handles the decryption process autonomously. Manually executing this
step is not necessary and is provided here solely for development purposes.

::

        # Decrypt sw-description for a single device
        openssl cms -decrypt -in sw-description  -out sw-description.out -inform DER -inkey device-privatekey-001.pem -recip device-cert-001.pem

Assume `sw-description` is the encrypted sw-description and `sw-description.out`
is the plaintext output. The device private key is required for decryption,
while the certificate itself is optional, but helps speed up the key matching
process.


Example Asymmetrically Encrypted Image
--------------------------------------

The image artifacts should be symmetrically encrypted and signed in advance.
Now, create a plain `sw-description.in` file. The attributes: ``encrypted``,
``aes-key`` and ``ivt`` are necessary for artifact decryption. It is okay that
the AES key is included here, because the sw-description file will be encrypted
afterwards.

::

        software =
        {
            version = "1.0.0";
            images: ({
                filename = "rootfs.ext4.enc";
                device = "/dev/mmcblk0p3";
                sha256 = "131159df3a4efaa890ff80173664a125c496c458dd432a8a6acae18872e35822";
                encrypted = "aes-cbc";  // former: encrypted = true;
                aes-key = "ed73b9d3bf9c655d5a0b04836d8be48660a4a4bb6f4aa07c6778e00e342881ac";
                ivt = "ea34a55a0c3476ed78f238ac87a7970c";
            });
        }


:ref:`Sign<signed-images>` `sw-description.in` and create the signature file `sw-description.sig`:
::

        openssl cms -sign -in sw-description.in -out sw-description.sig -signer signing-cert.pem -inkey signing-key.pem -outform DER -nosmimecap -binary


Asymmetrically encrypt the `sw-description` for multiple devices:
::

        openssl cms -encrypt -aes-256-cbc -in sw-description.in -out sw-description -outform DER $(printf ' -recip %q' certs/device-cert-*.pem)


Create the new update image (SWU):

::

        #!/bin/sh

        FILES="sw-description sw-description.sig rootfs.ext4.enc"

        for i in $FILES; do
            echo $i;done | cpio -ov -H crc >  firmware.swu


Running SWUpdate with Asymmetrically Encrypted Images
-----------------------------------------------------

Asymmetric encryption support can be enabled by configuring the compile-time
option ``CONFIG_ASYM_ENCRYPTED_SW_DESCRIPTION``, which depends on
``CONFIG_ENCRYPTED_SW_DESCRIPTION``. To pass the combined key pair
(PEM) generated earlier to SWUpdate, use the ``-K`` argument. Alternatively,
use the ``decryption-key`` parameter in the ``swupdate.cfg``.


Security Considerations
-----------------------
- Ideally, generate the private key on the device during factory provisioning,
  ensuring it never leaves the device. Only the public certificate leaves the
  device for encrypting future update packages.
- This feature should be used in conjunction with signature verification
  (``CONFIG_SIGNED_IMAGES``) to ensure data integrity. In principle, anyone
  with the corresponding device certificate can create update packages.
- As a side effect, the size of the update package may significantly increase
  in a large-scale deployment. To enhance scalability, consider using group
  keys. Smaller groups should be preferred over larger ones. For example,
  1000 device keys (using secp521r1) increase the sw-description size to
  0.35 MB. This means that by forming groups of 1000, it is possible to support
  1 million devices. Alternatively, groups of 100 increase the sw-description
  size to 3.5 MB accordingly.
- Exchange the AES key in the sw-description with each update package.
- Avoid encrypting new update packages for compromised devices, if there is no
  direct access to the device or if unauthorized users have access to new update
  packages.
