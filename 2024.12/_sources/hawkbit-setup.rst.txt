.. SPDX-FileCopyrightText: 2013-2021 Stefano Babic <stefano.babic@swupdate.org>
.. SPDX-License-Identifier: GPL-2.0-only

==========================================================
Config for hawkBit under SSL/TLS using private CA / sub CA
==========================================================

A user-contributed recipe based on hawkBit (0.2.0-SNAPSHOT) + swupdate (v2018.03)

Purpose
-------

Use HTTPS on a hawkBit server to avoid server spoofing. Anonymous client connections are authorized.

Recipe
------

1. On the PKI:

 * Create a pkcs#12 (``.p12``) file, rolling server key, server, private CA, sub CA certs into a single file.
 * Use a password on the server key you won't be ashamed of.
 * Also create a single ``.pem`` file for the private CA + sub-CA

2. On the hawkBit host:

 * hawkBit uses the Java KeyStore to access credentials, but a JKS is not designed apparently to hold CA certs, which is a problem for private CAs. The workaround is to make it gulp an entire pkcs#12 file.
 * It looks like a JKS like this cannot have a password different from the one protecting the ``.p12``. Keytool also seems to have a little tendency to destruct the ``.jks`` if you change your mind and want to change the password... Basically do everything you need with openssl and use only keytool for generating the ``.jks`` file.

 The following command imports a ``.p12`` into a "pkcs12 Java keystore", keeping the same password:

 .. code:: bash

  keytool -importkeystore -srckeystore hb-pass.p12 -srcstoretype pkcs12 \
          -destkeystore hb-pass.jks -deststoretype pkcs12 \
          -alias 1 -deststorepass <password_of_p12>

 Then you need to adapt ``application.properties`` of the hawkBit server to make use of the keystore.
 There are extra requirements to make hawkBit send artifacts via HTTPS.

 This is the relevant part of ``<hawkBit dir>/hawkbit-runtime/hawkbit-update-server/src/main/resources/application.properties``::

  # HTTPS mode working w/ swupdate
  # See also https://docs.spring.io/spring-boot/docs/1.4.7.RELEASE/reference/html/howto-embedded-servlet-containers.html#howto-configure-ssl
  #          https://github.com/eclipse/hawkbit/issues/618
  #
  # Need to run as root to use port 443
  server.hostname=hb.domain
  server.port=8443
  #
  # Overriding some of hawkbit-artifactdl-defaults.properties is required
  hawkbit.artifact.url.protocols.download-http.protocol=https
  hawkbit.artifact.url.protocols.download-http.port=8443
  #
  # Upgrades http:8443 to https:8443
  # Would redirect + upgrade http:80 to https:443
  security.require-ssl=true
  server.use-forward-headers=true
  #
  # Server cert+key w/ private CA + subCA
  # See also https://stackoverflow.com/questions/906402/how-to-import-an-existing-x509-certificate-and-private-key-in-java-keystore-to-u
  #          http://cunning.sharp.fm/2008/06/importing_private_keys_into_a.html (2008, still relevant!?)
  #
  # File .jks is a .p12 imported via keytool. Only one password supported, set from openssl.
  server.ssl.key-store=hb-pass.jks
  server.ssl.key-password=password
  server.ssl.key-store-password=password-yes_the_same_one
  ...

3. On the swupdate client host(s):

 * The client needs the private CA certificate(s) to authenticate the server.
 * There is a setting in swupdate to specify the path to a single CA cert, not a directory. Beyond that, libcurl looks into ``/etc/ssl/certs``. So we're using a compound "CA chain" ``.pem`` file to hold both private CA and sub-CA in our preferred location.

 This is the relevant part of ``/etc/swupdate/swupdate.conf``::

  ...
  suricatta :
  {
   tenant = "default";
   id = "machineID";
   url = "https://hb.domain:8443";
   nocheckcert = false;
   cafile = "/etc/swupdate/priv-cachain.pem"; /* CA + sub CA in one file */
   /* sslkey = anon client: do not set; */
   /* sslcert = anon client: do not set; */
  ...
