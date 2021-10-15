.. SPDX-FileCopyrightText: 2013-2021 Stefano Babic <sbabic@denx.de>
.. SPDX-License-Identifier: GPL-2.0-only

swupdate-sysrestart
===================

swupdate-sysrestart is a restart controller. It checks the update status
and in case of success, it reboots all devices that were updated.

SYNOPSIS
--------

swupdate-sysrestart [option]

DESCRIPTION
-----------

-r
        optionally reboot the target after a successful update
-w
        waits for a SWUpdate connection instead of exit with error
-s <path>
        path to progress IPC socket
