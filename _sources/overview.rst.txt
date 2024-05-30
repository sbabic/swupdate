.. SPDX-FileCopyrightText: 2013-2021 Stefano Babic <stefano.babic@swupdate.org>
.. SPDX-License-Identifier: GPL-2.0-only

=======================================
Software Management on embedded systems
=======================================

As Embedded Systems become more and more complex,
their software reflects the augmented complexity.
It is vital that the software on an embedded system
can be updated in an absolutely reliable way, as
new features and fixes are added.

On a Linux-based system, we can find in most cases
the following elements:

- the boot loader.
- the kernel and the DT (Device Tree) file.
- the root file system
- other file systems, mounted at a later point
- customer data, in raw format or on a file system
- application specific software. For example, firmware
  to be downloaded on connected micro-controllers, and so on.

Generally speaking, in most cases it is required to update
kernel and root file system, preserving user data - but cases vary.

In only a few cases it is required to update the boot loader,
too. In fact, updating the boot loader is quite always risky,
because a failure in the update breaks the board.
Restoring a broken board is possible in some cases,
but this is not left in most cases to the end user
and the system must be sent back to the manufacturer.

There are a lot of different concepts about updating
the software. I like to expose some of them, and then
explain why I have implemented this project.

Updating through the boot loader
================================

Boot loaders do much more as simply start the kernel.
They have their own shell and can be managed using
a processor's peripheral, in most cases a serial line.
They are often script-able, letting possible to implement
some kind of software update mechanism.

However, I found some drawbacks in this approach, that
let me search for another solution, based on an application
running on Linux:

Boot loaders have limited access to peripherals
-----------------------------------------------

Not all peripherals supported by the kernel are
available with the boot loader. When it makes sense to add
support to the kernel, because the peripheral is then available
by the main application, it does not always make sense to duplicate
the effort to port the driver to the boot loader.

Boot loader's drivers are not updated
-------------------------------------

Boot loader's drivers are mostly ported from the Linux kernel,
but due to adaptations they are not later fixed or synchronized
with the kernel, while bug fixes flow regularly in the Linux kernel.
Some peripherals can then work in a not reliable ways,
and fixing the issues can be not easy. Drivers in boot loaders
are more or less a fork of the respective drivers in kernel.

As example, the UBI / UBIFS for NAND devices contains a lot of
fixes in the kernel, that are not ported back to the boot loaders.
The same can be found for the USB stack. The effort to support
new peripherals or protocols is better to be used for the kernel
as for the boot loaders.

Reduced file systems
--------------------

The number of supported file systems is limited and
porting a file system to the boot loader requires high effort.

Network support is limited
--------------------------

Network stack is limited, generally an update is possible via
UDP but not via TCP.

Interaction with the operator
-----------------------------

It is difficult to expose an interface to the operator,
such as a GUI with a browser or on a display.

A complex logic can be easier implemented inside an application
else in the boot loader. Extending the boot loader becomes complicated
because the whole range of services and libraries are not available.

Boot loader's update advantages
-------------------------------
However, this approach has some advantages, too:

- software for update is generally simpler.  - smaller footprint: a stand-alone
  application only for software management requires an own kernel and a root
  file system. Even if their size can be trimmed dropping what is not required
  for updating the software, their size is not negligible.

Updating through a package manager
==================================

All Linux distributions are updating with a package manager.
Why is it not suitable for embedded ?

I cannot say it cannot be used, but there is an important drawback
using this approach. Embedded systems are well tested
with a specific software. Using a package manager
can put weirdness because the software itself
is not anymore *atomic*, but split into a long
list of packages. How can we be assured that an application
with library version x.y works, and also with different
versions of the same library? How can it be successfully tested?

For a manufacturer, it is generally better to say that
a new release of software (well tested by its test
engineers) is released, and the new software (or firmware)
is available for updating. Splitting in packages can
generate nightmare and high effort for the testers.

The ease of replacing single files can speed up the development,
but it is a software-versions nightmare at the customer site.
If a customer report a bug, how can it is possible that software
is "version 2.5" when a patch for some files were sent previously
to the customer ?

An atomic update is generally a must feature for an embedded system.


Strategies for an application doing software upgrade
====================================================

Instead of using the boot loader, an application can take
into charge to upgrade the system. The application can
use all services provided by the OS. The proposed solution
is a stand-alone software, that follow customer rules and
performs checks to determine if a software is installable,
and then install the software on the desired storage.

The application can detect if the provided new software
is suitable for the hardware, and it is can also check if
the software is released by a verified authority. The range
of features can grow from small system to a complex one,
including the possibility to have pre- and post- install
scripts, and so on.

Different strategies can be used, depending on the system's
resources. I am listing some of them.

.. _double_copy:

Double copy with fall-back
--------------------------

If there is enough space on the storage to save
two copies of the whole software, it is possible to guarantee
that there is always a working copy even if the software update
is interrupted or a power off occurs.

Each copy must contain the kernel, the root file system, and each
further component that can be updated. It is required
a mechanism to identify which version is running.

SWUpdate should be inserted in the application software, and
the application software will trigger it when an update is required.
The duty of SWUpdate is to update the stand-by copy, leaving the
running copy of the software untouched.

A synergy with the boot loader is often necessary, because the boot loader must
decide which copy should be started. Again, it must be possible
to switch between the two copies.
After a reboot, the boot loader decides which copy should run.

.. image:: images/double_copy_layout.png

Check the chapter about boot loader to see which mechanisms can be
implemented to guarantee that the target is not broken after an update.

The most evident drawback is the amount of required space. The
available space for each copy is less than half the size
of the storage. However, an update is always safe even in case of power off.

This project supports this strategy. The application as part of this project
should be installed in the root file system and started
or triggered as required. There is no
need of an own kernel, because the two copies guarantees that
it is always possible to upgrade the not running copy.

SWUpdate will set bootloader's variable to signal the that a new image is
successfully installed.

.. _single_copy:

Single copy - running as standalone image
-----------------------------------------

The software upgrade application consists of kernel (maybe reduced
dropping not required drivers) and a small root file system, with the
application and its libraries. The whole size is much less than a single copy of
the system software. Depending on set up, I get sizes from 2.5 until 8 MB
for the stand-alone root file system. If the size is very important on small
systems, it becomes negligible on systems with a lot of storage
or big NANDs.

The system can be put in "upgrade" mode, simply signaling to the
boot loader that the upgrading software must be started. The way
can differ, for example setting a boot loader environment or using
and external GPIO.

The boot loader starts "SWUpdate", booting the
SWUpdate kernel and the initrd image as root file system. Because it runs in
RAM, it is possible to upgrade the whole storage. Differently as in the
double-copy strategy, the systems must reboot to put itself in
update mode.

This concept consumes less space in storage as having two copies, but
it does not guarantee a fall-back without updating again the software.
However, it can be guaranteed that
the system goes automatically in upgrade mode when the productivity
software is not found or corrupted, as well as when the upgrade process
is interrupted for some reason.


.. image:: images/single_copy_layout.png

In fact, it is possible to consider
the upgrade procedure as a transaction, and only after the successful
upgrade the new software is set as "boot-able". With these considerations,
an upgrade with this strategy is safe: it is always guaranteed that the
system boots and it is ready to get a new software, if the old one
is corrupted or cannot run.
With U-Boot as boot loader, SWUpdate is able to manage U-Boot's environment
setting variables to indicate the start and the end of a transaction and
that the storage contains a valid software.
A similar feature for GRUB environment block modification as well as for
EFI Boot Guard has been introduced.

SWUpdate is mainly used in this configuration. The recipes for Yocto
generate an initrd image containing the SWUpdate application, that is
automatically started after mounting the root file system.

.. image:: images/swupdate_single.png

Something went wrong ?
======================

Many things can go wrong, and it must be guaranteed that the system
is able to run again and maybe able to reload a new software to fix
a damaged image. SWUpdate works together with the boot loader to identify the
possible causes of failures. Currently U-Boot, GRUB, and EFI Boot Guard
are supported.

We can at least group some of the common causes:

- damage / corrupted image during installing.
  SWUpdate is able to recognize it and the update process
  is interrupted. The old software is preserved and nothing
  is really copied into the target's storage.

- corrupted image in the storage (flash)

- remote update interrupted due to communication problem.

- power-failure

SWUpdate works as transaction process. The boot loader environment variable
"recovery_status" is set to signal the update's status to the boot loader. Of
course, further variables can be added to fine tuning and report error causes.
recovery_status can have the values "progress", "failed", or it can be unset.

When SWUpdate starts, it sets recovery_status to "progress". After an update is
finished with success, the variable is erased. If the update ends with an
error, recovery_status has the value "failed".

When an update is interrupted, independently from the cause, the boot loader
recognizes it because the recovery_status variable is in "progress" or "failed".
The boot loader can then start again SWUpdate to load again the software
(single-copy case) or run the old copy of the application
(double-copy case).

Power Failure
-------------

If a power off occurs, it must be guaranteed that the system is able
to work again - starting again SWUpdate or restoring an old copy of the software.

Generally, the behavior can be split according to the chosen scenario:

- single copy: SWUpdate is interrupted and the update transaction did not end
  with a success. The boot loader is able to start SWUpdate again, having the
  possibility to update the software again.

- double copy: SWUpdate did not switch between stand-by and current copy.
  The same version of software, that was not touched by the update, is
  started again.

To be completely safe, SWUpdate and the bootloader need to exchange some
information. The bootloader must detect if an update was interrupted due
to a power-off, and restart SWUpdate until an update is successful.
SWUpdate supports the U-Boot, GRUB, and EFI Boot Guard bootloaders.
U-Boot and EFI Boot Guard have a power-safe environment which SWUpdate is
able to read and change in order to communicate with them. In case of GRUB,
a fixed 1024-byte environment block file is used instead. SWUpdate sets
a variable as flag when it starts to update the system and resets the same
variable after completion. The bootloader can read this flag to check if an
update was running before a power-off.

.. image:: images/SoftwareUpdateU-Boot.png

What about upgrading SWUpdate itself ?
--------------------------------------

SWUpdate is thought to be used in the whole development process, replacing
customized process to update the software during the development. Before going
into production, SWUpdate is well tested for a project.

If SWUpdate itself should be updated, the update cannot be safe if there is only
one copy of SWUpdate in the storage. Safe update can be guaranteed only if
SWUpdate is duplicated.

There are some ways to circumvent this issue if SWUpdate is part of the
upgraded image:

- have two copies of SWUpdate
- take the risk, but have a rescue procedure using the boot loader.

What about upgrading the Boot loader ?
--------------------------------------

Updating the boot loader is in most cases a one-way process. On most SOCs,
there is no possibility to have multiple copies of the boot loader, and when
boot loader is broken, the board does not simply boot.

Some SOCs allow one to have multiple copies of the
boot loader. But again, there is no general solution for this because it
is *very* hardware specific.

In my experience, most targets do not allow one to update the boot loader. It
is very uncommon that the boot loader must be updated when the product
is ready for production.

It is different if the U-Boot environment must be updated, that is a
common practice. U-Boot provides a double copy of the whole environment,
and updating the environment from SWUpdate is power-off safe. Other boot loaders
can or cannot have this feature.
