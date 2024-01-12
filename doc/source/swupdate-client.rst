.. SPDX-FileCopyrightText: 2013-2021 Stefano Babic <stefano.babic@swupdate.org>
.. SPDX-License-Identifier: GPL-2.0-only

swupdate-client
===============

swupdate-client is a small tool that sends a SWU image to a running instance of
SWUpdate. It can be used if the update package (SWU) is downloaded by another
application external to SWUpdate. It is an example how to use the IPC to forward
an image to SWUpdate.

SYNOPSIS
--------

swupdate-client [OPTIONS] <image.swu to be installed>...

DESCRIPTION
-----------

-h
       print help and exit
-d
       ask the server to only perform a dry-run
-e     <software>,<mode>
       select software image set and source (for example: stable,main)
-q
       go quiet, resets verbosity
-v
       go verbose, essentially print upgrade status messages from server
-p
       ask the server to run post-update commands if upgrade succeeds
