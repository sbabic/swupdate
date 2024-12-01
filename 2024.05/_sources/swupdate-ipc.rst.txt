.. SPDX-FileCopyrightText: 2021 Stefano Babic <stefano.babic@swupdate.org>
.. SPDX-License-Identifier: GPL-2.0-only

swupdate-ipc
============

swupdate-ipc sends a specific command to SWUpdate. The following commands are
enabled:

aes
---
send a new aes key to SWUpdate

setversion
----------
sends a range of versions that can be accepted.

gethawkbit
----------
return status of the connection to Hawkbit.

sendtohawkbit
-------------
send data to the Hawkbit Server.  A typical use case is acknowledgement
for activation by an application or operator after a new software has been installed.
The tool can forward the result for the activation to the hawkBit server.

sysrestart
----------
send a restart command after a network update

SYNOPSIS
--------

swupdate-ipc <cmd> [option]

Where cmd is one of the listed above.

DESCRIPTION
-----------

aes <key> <ivt>
        AES key to be used for decryption

setversion <min> <max> <current>
        configure the accepted range of versions

hawkbitcfg
        configuration for Hawkbit Module

-h
        help
-p
        allows one to set the polling time (in seconds)
-e
        enable suricatta mode
-d
        disable suricatta mode

swupdate-sendtohawkbit <action id> <status> <finished> <execution> <detail 1> <detail 2> ..
        Send Acknolwedge to Hawkbit server after an update if SWUpdate is set to wait for.

sysrestart
        Used with SWU handler, allow one to perform a network restart

-r
        optionally reboot the target after a successful update
-w
        waits for a SWUpdate connection instead of exit with error
-s <path>
        path to progress IPC socket
