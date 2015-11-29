SWupdate - Software Update for Embedded Systems
===============================================

SWuPdate is a Linux Update agent with the goal to
provide an efficient and safe way to update
an embedded system. SWUpdate supports local and remote
updates, multiple update strategies and it can
be well integrated in the Yocto build system by adding
the meta-swupdate layer.

It supports the common media on embedded
such as as NOR / NAND flashes, UBI volumes, SD / eMMC, and can
be easy extended to introduce project specific update
procedures.

Pre- and postinstall scripts are supported, and a LUA
interpreter helps to customize the updates.

An update package is described by the sw-description file,
using the libconfig syntax or JSON. It is even possible to
use LUA with a custom syntax.

This software is licensed under GPL Version 2.0+

Please check inside doc directory for documentation or
the online documentation (generated from doc/) at:

	http://sbabic.github.io/swupdate


Submitting patches
------------------

You can submit your patches (or post questions reagarding
the project to the swupdate Mailing List:

	swupdate@googlegroups.com

When creating patches, please use something like:

    git format-patch -s [PATCH' <revision range>

Please use 'git send- email' to send the generated patches to the ML
to bypass changes from your mailer.
