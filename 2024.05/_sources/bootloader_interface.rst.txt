.. SPDX-FileCopyrightText: 2022 Christian Storm <christian.storm@siemens.com>
.. SPDX-License-Identifier: GPL-2.0-only

====================
Bootloader Interface
====================

Overview
========

SWUpdate has bindings to various bootloaders in order to store persistent
state information across reboots. Currently, the following bootloaders are
supported:

* A fake bootloader called "Environment in RAM",
* `EFI Boot Guard <https://github.com/siemens/efibootguard>`_,
* `U-Boot <https://www.denx.de/wiki/U-Boot>`_, and
* `GRUB <https://www.gnu.org/software/grub/>`_.

The actual (sub)set of bootloaders supported is a compile-time choice. At
run-time, the compile-time set default bootloader interface implementation
is used unless overruled to use another bootloader interface implementation
via the ``-B`` command line switch or a configuration file (via the
``bootloader`` setting in the ``globals`` section, see
``examples/configuration/swupdate.cfg``).

Note that the run-time support for some bootloaders, currently U-Boot and
EFI Boot Guard, relies on loading the respective bootloader's environment
modification shared library at run-time. Hence, even if support for
a particular bootloader is compiled-in, the according shared library must
be present and loadable on the target system at run-time for using this
bootloader interface implementation.
This allows, e.g., distributions to ship a generic SWUpdate package and
downstream integrators to combine this generic package with the appropriate
bootloader by just providing its environment modification shared library.


Bootloader Interface Description
================================

The bootloader interface implementations are located in ``bootloader/``.
Each bootloader has to implement the interface functions as defined in
``include/bootloader.h``, more precisely

.. code-block:: c

    char *env_get(const char *name);
    int env_set(const char *name, const char *value);
    int env_unset(const char *name);
    int apply_list(const char *filename);

which
retrieve a key's value from the bootloader environment,
set a key to a value in the bootloader environment,
delete a key-value pair from the bootloader environment, and
apply the ``key=value`` pairs found in a file.


Then, each bootloader interface implementation has to register itself to
SWUpdate at run-time by calling the ``register_bootloader(const char *name,
bootloader *bl)`` function that takes the bootloader's name and a pointer
to ``struct bootloader`` as in ``include/bootloader.h`` which is filled
with pointers to the respective above mentioned interface functions.
If the bootloader setup fails and hence it cannot be successfully registered,
e.g., because the required shared library for environment modification cannot
be loaded, ``NULL`` is to be returned as pointer to ``struct bootloader``.

For example, assuming a bootloader named "trunk" and (static) interface
functions implementations ``do_env_{get,set,unset}()`` as well as
``do_apply_list()`` in a ``bootloader/trunk.c`` file, the following snippet
registers this bootloader to SWUpdate at run-time:

.. code-block:: c

    static bootloader trunk = {
        .env_get = &do_env_get,
        .env_set = &do_env_set,
        .env_unset = &do_env_unset,
        .apply_list = &do_apply_list
    };

    __attribute__((constructor))
    static void trunk_probe(void)
    {
        (void)register_bootloader(BOOTLOADER_TRUNK, &trunk);
    }

with 

.. code-block:: c

    #define BOOTLOADER_TRUNK "trunk"

added to ``include/bootloader.h`` as a single central "trunk" bootloader
name definition aiding in maintaining the uniqueness of bootloader names.
This new "trunk" bootloader should also be added to the Suricatta Lua
Module interface specification's bootloader Table
``suricatta.bootloader.bootloaders = { ... }`` in
``suricatta/suricatta.lua``.


.. attention:: Take care to uniquely name the bootloader.


See, e.g., ``bootloader/{uboot,ebg}.c`` for examples of a bootloader using
a shared environment modification library and ``bootloader/{grub,none}.c``
for a simpler bootloader support example.


Bootloader Build System Integration
===================================

A bootloader support implementation needs to be registered to the kconfig
build system.

First, the bootloader support implementation, named "trunk" and implemented
in ``bootloader/trunk.c`` for example, needs to be added to
``bootloader/Config.in`` in the ``Bootloader Interfaces`` menu as
follows:

.. code-block:: kconfig

    ...

    menu "Bootloader"

    menu "Bootloader Interfaces"

    ...

    config BOOTLOADER_TRUNK
        bool "TrUnK Bootloader"
        help
          Support for the TrUnK Bootloader
          https://github.com/knurt/trunk


Then, in order to enable the compile-time selection of the "trunk" bootloader
as default, add a section to the ``Default Bootloader Interface`` choice
submenu of the ``Bootloader`` menu as follows:

.. code-block:: kconfig

    choice
    	prompt "Default Bootloader Interface"
    	help
    	  Default bootloader interface to use if not explicitly
    	  overridden via configuration or command-line option
    	  at run-time.

    ...

    config BOOTLOADER_DEFAULT_TRUNK
        bool "TrUnK"
        depends on BOOTLOADER_TRUNK
        help
          Use TrUnK as default bootloader interface.


Finally, ``bootloader/Makefile`` needs to be adapted to build the "trunk"
bootloader support code, given ``BOOTLOADER_TRUNK`` was enabled:

.. code-block:: makefile

    obj-$(CONFIG_BOOTLOADER_TRUNK) += trunk.o


If the "trunk" bootloader, for example, requires loading a shared
environment modification library, then ``Makefile.flags`` needs to be
adapted as well, e.g., as follows:

.. code-block:: makefile

    ifeq ($(CONFIG_BOOTLOADER_TUNK),y)
    LDLIBS += dl
    endif


