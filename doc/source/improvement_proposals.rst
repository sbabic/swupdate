.. SPDX-FileCopyrightText: 2013-2024 Stefano Babic <stefano.babic@swupdate.org>
.. SPDX-License-Identifier: GPL-2.0-only

=============================
Proposals to improve SWUpdate
=============================

Please take into account that most of the items here are *proposals*.
I get some ideas talking with customers, some ideas are my own thoughts.
There is no plan when these features will be implemented - this depends
if there will be contribution to the project in terms of patches or
financial contributions to develop a feature.

Each item listed here contains a status and if the feature is already planned or
I am looking for sponsor to implement it. This should avoid double work and make
more transparent what is going on with the project.

If you have further ideas about the project, just send your proposal to the
Mailing List or post a patch for this document.

Thanks again to all companies that have supported my work up now and to
everybody who has contributed to the project, let me bring SWUpdate
to the current status !

Legende
=======

.. table:: Feature's status

   +-------------+---------------------------------------------------------------+
   |  Value      | Description                                                   |
   +=============+===============================================================+
   |  Wait       |  No activity is planned, just proposal                        |
   +-------------+---------------------------------------------------------------+
   |  Design     | Design / Concept is done                                      |
   +-------------+---------------------------------------------------------------+
   |  Planned    | Feature will be implemented soon                              |
   +-------------+---------------------------------------------------------------+
   |  WIP        | Feature is current to be implemented and will be posted       |
   +-------------+---------------------------------------------------------------+
   |  Running    | Implemented                                                   |
   +-------------+---------------------------------------------------------------+

.. table:: Request for Support

   +-------------+---------------------------------------------------------------+
   |  Value      | Description                                                   |
   +=============+===============================================================+
   |  None       | No sponsoring required                                        |
   +-------------+---------------------------------------------------------------+
   |  Sponsor    | Looking for sponsors to finance the feature                   |
   +-------------+----------+----------------------------------------------------+
   |  Planned    | Feature is already sponsored                                  |
   +-------------+---------------------------------------------------------------+


.. table:: Priority

   +-------------+---------------------------------------------------------------+
   |  Value      | Description                                                   |
   +=============+===============================================================+
   |  Low        | Probably just an idea, feedback from community asked          |
   +-------------+---------------------------------------------------------------+
   |  Medium     | Not critical, but nice feature to have                        |
   +-------------+----------+----------------------------------------------------+
   |  High       | Critical feature, it should be implemented soon               |
   +-------------+---------------------------------------------------------------+



Main goal
=========

First goal is to reach a quite big audience, making
SWUpdate suitable for a large number of products.
This will help to build a community around the project
itself.

* Status : Running
* Request for Support : None

Core features
=============

Support for OpenWRT
-------------------

OpenWRT is used on many routers and has its own way for updating that is not power-cut safe.

* Status: Wait
* Request for Support : Sponsor
* Priority : Low

Software-Software compatibility
-------------------------------

SWUpdate has from the early stage a hardware to software compatibility check. In case
software is split in several components (like OS and application), it is desirable to have
a sort of software compatibility check. For example, SWUpdate verifies if a component
(like an application) is compatible with a runningOS and reject the update in case of
mismatch.

* Status: Wait
* Request for Support : Sponsor
* Priority : Medium

Support files bigger than 4GB
-----------------------------

SWUpdate currently uses CPIO to pack updates in the 'newc' and 'crc' formats.
These formats limit single files to 4GB - 1byte in size, which could become a
problem as update size grows.

* Status: Wait
* Request for Support : Sponsor
* Priority : High

Fetcher and interfaces
======================

Selective downloading
---------------------

SWUpdate starts to fetch the whole SWU and process it until an error is found. There are some requests
to have a selective downloading, that means SWUpdate will load just the chunks are needed and not
the whole SWU. An example for this use case is in case a single SWU contains software for
multiple devices, and each of them needs a subset of the whole SWU. Like the delta handler,
SWUpdate knows from sw-description which artifacts are to be installed, and reading the stream
could decide to skip unnecessary components.

* Status: Wait
* Request for Support : Sponsor
* Priority : Medium

Tools and utilities
===================

Single tool to create SWU
-------------------------

SWUGenerator is a separate project that helps to create SWUs. It is used on not OE projects. In OE,
the SWU is created using code in meta-swupdate. This leads to a duplication of code with higher
effort to maintain.

Even if it was tried to have the same features, there are some important differences:

- SWUGenerator is able to full understand a sw-description written in libconfig language and to rewrite it.
  This means it is not yet possible to write sw-description using JSON if SWUGenerator is planned.
  However, it is possible to split sw-description in small files (for example for the embedded Lua code),
  and SWUGenerator is able to write the final sw-description combined all include files.
- meta-swupdate is just able to replace variables known by bitbake, but it has no semantic knowledge.
  It is not possible to use @include directive, but it is possible to use JSON as language.

The logical step will be to use a single tool (SWUGenerator), and let meta-swupdate to use it. To do this,
SWUGenerator should be enhanced to understand and write sw-description in JSON, too.

* Status: Wait
* Request for Support : Sponsor
* Priority : Medium

Further enhancement to SWUGenerator
-----------------------------------

SWUGenerator is thought to support multiple subcommands, but it currently supports just "create".
It is thinkable, even if this can be done with other tools, to implement further commands like:

- extract: take a SWU and extracts all artifacts in a directory
- sign: take a SWU and resign with a new key. This is useful when it is required to install a new
  Software, but the certificate or the key on the device is older and rejects the installation.
- verify: just verify if the SWU is correctly signed.

SWUGenerator does not yet support all features present in meta-swupdate. As replacement for meta-swupdate
and the wish to have just one tool, SWUGenerator should align itself with meta-swupdate. It will be
then possible to drop most of code from meta-swupdate's classes, and replace with the single call
to SWUGenerator.

* Status: Wait
* Request for Support : Sponsor
* Priority : Medium

LZMA support to SWUGenerator
----------------------------

XZ (LZMA) decompression was added to SWUpdate, but SWUGenerator is not able to create XZ compressed images.

* Status: Wait
* Request for Support : Not required
* Priority : Medium

swupdate-progress start up
--------------------------

On SystemV (and compatible) systems, swupdate-progress is started from swupdate.sh via exec. This is not
the right solution and was discussed on the Mailing List.

The agreed solution is to create an own startup script for swupdate-progress, and let run it after
SWUpdate is started. This is more generic and let also to identify if swupdate-progress should be
installed or not.

* Status: Wait
* Request for Support : Not required
* Priority : Low

Lua
===

- API between SWUpdate and Lua is poorly documented.
- Store in SWUpdate's repo Lua libraries and common functions to be reused by projects.

* Status : Running
* Request for Support : None
* Priority : Medium

Handlers:
=========

New Handlers
------------

Users develop own custom handlers - I just enforce and encourage everyone
to send them and discuss how to integrate custom handler in mainline.

Some ideas for new handlers:
        - FPGA updater for FPGA with Flash
        - Package handler to install packages (ipk, deb)
          Packages can be inserted into the SWU and the atomicity is
          guaranteed by SWUpdate.
        - Lua handlers should be added if possible to the project
          to show how to solve custom install.

* Status : Running
* Request for Support : None
* Priority : Low

Internal Webserver
==================

SWUpdate make usage of the project "mongoose" as internal Webserver. It fits all
requirements and allows to stream a SWU without temporary copy.
However, upgrading the Webserver code requires to adjust the interface and code. It
will be nice to have further implementation of the Webserver, and/or to open to
Webserver that allows streaming.

* Status: Wait
* Request for Support : Sponsor
* Priority : Medium

Security / Crypto engines
=========================

- add support for asymmetric decryption

* Status: Wait
* Request for Support : Sponsor
* Priority : High

- add more algorithms for decryption, as AES-CTR can be very useful to decrypt
  chunks in delta updates.

* Status: Wait
* Request for Support : Sponsor
* Priority : High

- Support for TPM2 / HSM to store secrets (requires rework above).

* Status: Wait
* Request for Support : Sponsor
* Priority : High

Back-end support (suricatta mode)
=================================

Back-end: responsiveness for IPC
--------------------------------

Suricatta is implemented as process that launches functions for the selected module.
This means that the IPC does not answer if Suricatta is doing something, specially if it is
downloading and upgrading the system. This can be enhanced adding a separate thread for IPC and of course
all required synchronization with the main modules.

* Status: Wait
* Request for Support : Sponsor
* Priority : Medium

Back-end: check before installing
---------------------------------

In some cases (for example, where bandwidth is important), it is better to check
if an update must be installed instead of installing and performs checks later.
If SWUpdate provides a way to inform a checker if an update can be accepted
before downloading, a download is only done when it is really necessary.

* Status: Wait
* Request for Support : Sponsor
* Priority : Medium

Back-end: hawkBit Offline support
---------------------------------

There are several discussions on hawkBit's ML about how to synchronize
an offline update (done locally or via the internal Web-server) with
the hawkBit's server. Currently, hawkBit thinks to be the only one
deploying software. hawkBit DDI API should be extended, and afterwards
changes must be implemented in SWUpdate.

* Status: Wait
* Request for Support : Sponsor
* Priority : Low

Backend: hawkBit support for Delta Update
-----------------------------------------

Delta Update requires two or more files:

- the SWU
- one file ".zck" for each artifact that is upgraded via delta handler.

The .zck must be uploaded somewhere and the URL is defined inside sw-description, that
is then signed. This causes a chicken-egg issue, because the buzild cannot be completed
with hawkBit until the ".zck" files are not uploaded. In fact, hawkBit assigns to each
Software Module an "id" that is unknown at the moment of the build.

It is required to implement a mechanism that let suricatta to inform the core about URLs
passed by the hawkBit server, and they can override the URL set inside sw-description.
This lets the URL for ZCK unknown during the build and it will be detected at runtime.

The authentication to the hawkBit Server does not work in case of delta. In fact, authentication
is performed by the backend connector, but the download of .zck files is done by a different
process ("downloader") that don't use the setup from suricatta.

* Status: Wait
* Request for Support : Sponsor
* Priority : Medium

Back-end: support for generic down-loader
-----------------------------------------

SWUpdate in down-loader mode works as one-shot: it simply try to download a SWU
from a URL. For simple applications, it could be moved into `suricatta` to detect
if a new version is available before downloading and installing.

* Status: Wait
* Request for Support : Sponsor
* Priority : Medium

Back-end: further connectors
----------------------------

Further connectors could be implemented. The structure in SWUpdate
is modular, and allows to write new connectors, even in Lua. New connectors could be
added if there are requests in this direction.

* Status: Wait
* Request for Support : Sponsor
* Priority : Low


Test and Continuous Integration
===============================

The number of configurations and features in SWUpdate is steadily increasing and
it becomes urgent to find a way to test all incoming patch to fix regression issues.
One step in this direction is the support for Travis build - a set of configuration
files is stored with the project and should help to find fast breakages in the build.
More in this direction must be done to perform test on targets. A suitable test framework
should be found. Scope is to have a "SWUpdate factory" where patches are fast integrated
and tested on real hardware.

* Status: Wait
* Request for Support : Sponsor
* Priority : Medium

Bootloader interface
====================

SWUpdate has several interfaces to bootloader, but support for handling UEFI variables is still
missing. It is required to set UEFI variable exactly as done for other bootloader (like U-Boot)
via sw-description. Lua code can profit, too, becaause variables can be retrieved using the "get"
function.

* Status: Wait
* Request for Support : Sponsor
* Priority : High

Binding to languages
====================

libswupdate allows to write an application that can control SWUpdate's behavior and be informed
about a running update. There are bindings for C/C++, Lua and nodejs (just progress).

Use a JSON interface to exchange IPC messeges.
----------------------------------------------

Instead of using binary message, use JSON to exchange messages between a client and
SWUpdate. This makes adding new binding very easy, and often not necessary.

* Status: Wait
* Request for Support : Sponsor
* Priority : Low

Bindings for other languages
----------------------------

Applications can be written in other languages, and binding to Python and Rust can be
implemented, too.

* Status: Wait
* Request for Support : Sponsor
* Priority : Low

Documentation
=============

Documentation is a central point in SWUpdate - maintaining it up to date is a must in this project.
Help from any user fixing wrong sentence, bad english, adding missing topics is high
appreciated.

* Status : Running
* Request for Support : None

Already completed and mainlined
===============================

Following items were already implemented and are supported in mainlined - thanks for whom
sponsored them.

Support for BTRFS snapshot
--------------------------

BTRFS supports subvolume and delta backup for volumes - supporting subvolumes is a way
to move the delta approach to filesystems, while SWUpdate should apply the deltas
generated by BTRFS utilities.

* Status: since 2024.12

Parser
======

SWUpdate supports two parsers : libconfig and JSON. It would be nice if tools can
be used to convert from one format to the other one. Currently, due to some specialties
in libconfig, a manual conversion is still required.

* Status: since 2024.12

Support for different Update Types
----------------------------------

Update can be split and group in several components. If updating everything in one shot
is the preferred method, there are use case where different updates are provided, for example
OS and Application updates, and different SWU are generated.
SWUpdate does not recognize which is the type of the update, and uses global rules to check
the version. The feature here is thought to introduce update-types, and a set of per type setup
(min / max version, downgrading options,..) that are be used if the update of that type is recognized.
Types are not limited to a selected list, but they can be free set inside swupdate.cfg.
A flag can require that the update-type is mandatory, and SWUpdate will always check
if the type is one of the supported.

* Status: since 2025.05+
* Sponsored by iris-GmbH infrared & intelligent sensors

Handlers installable as plugin at runtime
------------------------------------------

The project supports Lua as script language for pre- and postinstall
script. It will be easy to add a way for installing a handler at run-time
written in Lua, allowing to expand SWUpdate to the cases not covered
in the design phase of a product.

Of course, this issue is related to the security features: it must be
ensured that only verified handlers can be added to the system to avoid
that malware can get the control of the target.

Current release supports verified images. That means that a handler
written in Lua could be now be part of the compound image, because
a unauthenticated handler cannot run.

* Status : since 2024.05

Security subsystem and support multiple engines
-----------------------------------------------

Rework support for crypto engine - let possible to load multiple libraries at
the same time. Currently, there is support for openSSL, WolfSSL and mbedTLS.
However, WolfSSL are missing together. There should be a way to select one or more
libraries and independently the algorithms that SWUpdate should support.
Some hacks are currently built to avoid conflicts (pkcs#7 and CMS are the same
thing, but supported by different libraries), and they should be solved.

* Status: after 2025.05
