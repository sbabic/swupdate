..
        SPDX-FileCopyrightText: 2021 Stefano Babic <sbabic@denx.de>
        SPDX-License-Identifier: GPL-2.0-only

**********************
Swupdate Best Practice
**********************

This is intended as general rule to integrate SWUpdate into a custom project.

SWUpdate is an update agent and it is thought to be a framework. This means it is highly
configurable and SWUpdate should be configured to fit into a project, and not
viceversa. SWUpdate makes just few requirements on the system and it has no fix schema.
There is no restrictions about how many partitions, or which storage you are using.
In some more complex cases, the update depends on a lot of conditions,
and SWUpdate can run differently according to the `mode` a device is started.
Think that SWUpdate is not a ready-to-use updater, and you need first to
write a meaningful:

Update Concept
==============

Take your time and write first an update concept for your device.  It is not wasted time.
You have to imagine conditions when an update is not working, and try to write
down the use cases when an update can fail and how the device can be restored.
SWUpdate installs new software, but a successful update means that the new software
is started and runs flawlessly. The interface with the bootloader (or the one that start the
SW) must be checked in details.
A successful update means:
- SWUpdate runs with success
- Device reboots
- Bootloader can start the new software
- The new software runs, makes some consistency checks, and declare that the transaction (that is from old to new sw) is terminated.

This means that some coordination between bootloader and update agent is necessary.
In most cases, this is done via persistent variables that are available to both
SWUpdate and bootloader. SWUpdate has two built-in variables:

- *recovery_status*: this is set when SWUpdate starts to write to the device, and it is
  reset after the installation completed with success or it is set to `failure` in case
  of error. A bootloader can use it to check if an update was interrupted.
  This is a must in case a single copy of software is on the device.

- *ustate*: this sets a state machine. SWUpdate sets it to `1` (INSTALL) after an update.
  The bootloader can use it to check if a new software must be tested.
  Bootloader should implement a counter mechanism to check how many time it tried to start
  a new software. When a threshold is reached, the bootloader should declare the new sofwtare
  as buggy, and proceed with a fallback in case thew old software is available.

A *fallback* is always initiated by the bootloader, because it knows
if the new sofware is running. It should toggle the copies and start the old software set.
To communicate this to the user space and to SWUpdate, the bootloader sets the ustate variable to
`FAILED`. SWUpdate uses it in case the result must be forwarded to an external server (like a backend).
There is a time window when a fallback can take place. In fact, after a reboot and some attempts,
the update transaction is declared successful or failed, and ater then new update can be executed.
When a new update runs, the status of the stand-by copy is unknown, because it could be
the result of an interrupted update. Running an uncomplete software can lead to unpredictable
results and must be strongly avoided.
A common pattern for a toggling in the bootloader is:

- ustate is not set or set to '0' (no update pending). The bootloader runs the configured
  copy and won't ever toggle. In case of failure, a rescue can be started if available.
- ustate is set to *installed* ('1'): the new software is under test. The bootloader initiates
  a fallback if new software is not running and sets ustate to failed. If new software runs,
  it is duty of user space (often SWUpdate or the init script for SWUpdate) to reset the variable
  to '0'. Note that resetting the variable is project specific, and it could be set as last
  action after checking as much as possible that the new SW is running. This includes
  performing in the application a database migration, starts to communicate with peers, whatever.

Check in advance which security topics are relevant on your project. This includes:

- signed images (SWU is verified before installing), and then which crypto mechanismus is used
  (RSA keys, certificates, PKI)
- encryption for artifacts
- under which user and group SWUpdate and other components are allowed to run, see
  `privilege separation`. Set user id and group id if not root in swupdate.cfg.
- if any version can be installed or if you forbid a downgrade, and then be sure to pass
  the range of versions you allow via --M, -R and --max-version.
- hardware-software compatibility check and how your device knows which hardware
  revision is running.

Reduce dependencies at minimum
------------------------------

An update should be possible in any condition. Even if the system is degraded or in a bad shape,
if an update can work, the device can be functional again without returning it back to the
factory.
SWUpdate is thought to be self contained: that means it does not make usage of external
tools. If your system is degraded and filesystems gets corrupted, there are less chance to restore it
if the update calls external tools. SWUpdate is started at boot time and there are good chance
it goes on even if your system has some (software) flaws.
Be careful to make an update depending on your application or try to reduce the dependencies.
In fact, application is updated often and an introduction of a new bugs can make the device not
updatable anymore. Take the dependencies under control, and if you have any, be sure that the
update is still working. You can fix any bugs if the update works, but not anymore if the device
cannot be updated.

Make a risk analyses
--------------------

A more accurate analyses brings less surprises in field. Think about twice what you want to update,
and which components should be updated, and which risks in case of update of single point of failure.
Very often, this means the bootloader. Compare risks and benefits: it happens in many projects that
having the possibility (with some risk) to update the bootloader is better that returning the devices
back to service. A cost / benefits analyses should be part of the integration of the update agent.

SWUpdate builtin configuration
==============================

SWUpdate has a compile time configuration. The default configuration delivered with `meta-swupdate`
is not suitable for most project. The easy way to check configuration in Yocto is to run:

::

        bitbake -c menuconfig swupdate

Outside Yocto, just run in SWUpdate's sources:

::

        make menuconfig

Check security, bootloader and which handlers should be installed. They depend strongly from
your project.
If you build with OE, add a swupdate_%.bbappend to one of your layers, and put the resulting
configuration file as `defconfig` that can be fetched.
Please review the following configuration:
- security settings

- interfaces required (where the SW is coming from). Disable the interface you do not need.

- handlers required for you project. Disable again what you do not need, but consider if
  you could need some of them in future. As example, you can safely disable *ubivol* if
  you do not use raw NAND, but you can let *archive* if you plan to install artifacts
  from tarball in future.

- it is highly recommended to enable Lua to extend runtime behavior.

SWUpdate startup
================

An easy way to start SWUpdate is provided only with meta-swupdate and Yocto. A generi SystemV init script or a
systemd unit for SWUpdate are executing a script `swupdate.sh`, that is delivered together with SWUpdate
binaries.
The script goes through /etc/swupdate/conf.d/ and source all found files. The integrator can use
a set of predefined variable to configure SWUpdate's command line parameters. 

- *SWUPDATE_WEBSERVER_ARGS* : this string is passed if the webserver must be started. They are the Webserver
  specific parameter. If this variable is set, the script will add `-w` to the list of parameters.
  Note: meta-swupdate contains a default configuration for SWUPDATE_WEBSERVER_ARGS, that use /www as document root
  for the Website and default port 8080.
- *SWUPDATE_SURICATTA_ARGS* : Suricatta (backend) specific parameters. There is no default.
- *SWUPDATE_ARGS* : parameteres not belonging to Webserver or Suricatta.

Note that swupdate.sh sources the file in sorted order, so it is possible to overwrite the variables
with a configuration file whose filename is loaded at the end. Preferred style is to use SystemV like
names, for example `10-webserver`, `11-suricatta`, and so on.

Write sw-description
====================

sw-description is the central file that describe a new release and how a release must be installed.
It should be a consequence of the update concept. There is not a single right way to do. SWupdate
uses heaviliy `selections` and links to extract just one part of the whole sw-description, that
can be used for different situsations and different way to run the device. One use case for
selection is to implement the dual-copy (often reported as A/B) mode: one section contains instructions
for one copy, the other for the second copy. Which copy is the stand-by must be detected
before running SWUpdate and passed via the `-e <selection,mode>` switch.
Other methods set up link to the standby storage (like /dev/standby) during boot. Or the standby
device can be detected at runtime with an `embedded-script`, as part of sw-description, with Lua code.
Please note that for last case, SWUpdate is extended with functions exported to the Lua context that
simplify the detection. SWUpdate exports a `getroot` that returns type and value for the device used
for rootfs. See SWUpdate documentation for a complete list of functions exported by SWUpdate that can be
used in Lua. An embedded Lua script must just start with:

::

        require ('swupdate')

to make usage of them.

Use as much as possible OE variables
------------------------------------

meta-swupdate replaces special construct in sw-description with the values of build variables.
The recognized construct in sw-description is delimited by *@@*, that is *@@VARIABLE-NAME@@*.
The exception (for compatibility reason) is the automatic generation of sha256. The syntax in that case
is :

::

        sha256 = "@<name of artifact>"

You can use again variable substituion for artifact name. Example:

::

        sha256 = "@@@SYSTEM_IMAGE@@-@@MACHINE@@@@SWUPDATE_IMAGES_FSTYPES[@@SYSTEM_IMAGE@@]@@";

Please note that each variable is double delimited (at the begin and at the end) by `@@`.

Deliver your scripts instead of relying on installed
----------------------------------------------------

You have the freedom to call any tools during an update. However, take care if you are using
some tools from the running rootfs / current software. This implies that the current software is running
flawlessly, as well as the tools you are calling. And this is not maybe always the case.


Prefer Lua to shell scripts
---------------------------

Shell scripts are very popular, and they are often used even when they are not strictly required. 
They can raise security issues. In fact, take as example a simple
shell script. Goal of rootkits is often the shell, because taking control of the shell
means to control the whole device. If the shell is compromised, the whole system is compromised.
Running a shell script means that SWUpdate should call "fork" followed by an "exec". This means
also that many resources are duplicated in the child process, and it could cause a further
problem if system is getting rid of resources.
A better approach is to use Lua and to deliver the scripts inside the SWU. In fact, the Lua
interpreter is linked to SWUpdate and runs in context of the SWUpdate process without forking
a child process. Shell is not involved at all. Of course, Lua scripts should be written
to be selfcontained, too, and executing external tools should be done only if unavoidable.

Use installed-directly when possible
------------------------------------

SWUpdate can be enabled for zero-copy (or streaming mode), that is the incoming SWU is analyzed on the fly and it is
installed by the related handler without any temporary copy. If this is not set, SWUpdate creates
a temporary copy on $TMPDIR before passing it to the handlers. Note that $TMPDIR generally points to
a RAMDISK and storing files there reduce the amount of memory available for the application.
It makes sense to disable the flag in case the artifact is a single point of failure.
A typical example could be the bootloader (not duplicated on the devices), and if the SWU
is corrupted or the connection get broken, lets the board in a bricked state. It makes sense
then to download the whole artifact before installing.

Always enable sha256 verification
---------------------------------

The SWU image is a CPIO archive with CRC (new ASCII format), but the check in CPIO is very
weak. Do not trust it, but enable sha256 for each artifact.

Always set the "type" attribute
-------------------------------

SWUpdate sets some default handler if the type is not set. Do not use it, but set explicitely
the type (that is, which handler should install the artifact) in sw-description.

Do not rely on install order
----------------------------

SWUpdate does not require that artifacts must be put in CPIO in a specific order. The exception is
sw-description, that must be the first file in SWU. Avoid dependencies inside the SWU, that is an artifact
that can be installed only after another one was installed before. If you really need it, for example
you want to install a file into a filesystem provided as image, disable `installed-directy` for the file
and enable it for the filesystem image.

Do not drop atomicity !
-----------------------

SWUpdate guarantees atomicity until you do not do something that simply breaks it. As example,
think at the bootloader's environment. In sw-description there is a specific section where
the environment can be set, adding / modifying / deleting variables. SWUpdate does not change
the single variable, but generates for the supported bootloader the resulting new environment and
this is written in one shot in a way (for U-Boot / EFIBootguard, not for GRUB) that is powercut safe. 
You can of course change the environment in a postinstall script, like in the following way (for U-Boot):

::

        fw_setenv var1 val1
        fw_setenv var2 val2
        fw_setenv var3 val3
        fw_setenv var4 val4
        fw_setenv var5 val5

If a power cut happens during two calls of fw_setenv, the environment is in a middled of a change and this
can brick the device.

Plan to have a rescue system
============================

Even if you have a double-copy, something can go wrong. Plan to have a rescue system (swupdate-image in meta-swupdate)
and to install it on a separate storage as the main system, if it is possible. This helps when the main
storage is corrupted, and device can be restored in field without returning it back to the factory.
Plan to update the rescue as well: it is software, too, and its bugs should be fixed, too.
