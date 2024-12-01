.. SPDX-FileCopyrightText: 2013-2021 Stefano Babic <stefano.babic@swupdate.org>
.. SPDX-License-Identifier: GPL-2.0-only

Update strategy examples
========================

SWUpdate is a building block and it allows one to design and implementing its own
update strategy.
Even if many projects have common ways for updating, it is possible to high customize
the update for each project.
The most common strategies (single-copy and dual-copy) were already described at the
beginning of this documentation and of course are well supported in SWUpdate.


Single copy - running as standalone image
-----------------------------------------

See :ref:`single_copy`.

Double copy with fall-back
--------------------------

See :ref:`double_copy`.

Combine double-copy with rescue system
--------------------------------------

This provides a recovery procedure to cover update failure in severe cases when software is damaged.
In case none of the copy can be started, the bootloader will start the rescue system (possibly stored
on another storage as the main system) to try to rescue the board.

.. image:: images/swupdate-rescue+double-copy.png

The rescue system can be updated as well during a standard update.


Split system update with application update
-------------------------------------------

Updating a whole image is quite straightforward, but this means to transfer bigger amount
of data if just a few files are updated. It is possible to split theupdate in several smaller
parts to reduce the transfer size. This requires a special care to take care of compatibility
between system and application, that can be solved with customized Lua scripts in the sw-description file.
SWUpdate supports versioning for each artefact, and anyone can add own rules to verify compatibility
between components.

.. image:: images/swupdate-application.png

Configuration update
--------------------

Thought to update the software, SWUpdate can be used to install configuration data as well.
Build system can create configuration SWU with files / data for the configuration of the system.
There is no requirements what these SWU should contains - it is duty of the integrator to build
them and make them suitable for his own project. Again, configuration data can be updated as
separate process using one of the above scenarios.
