swupdate-sendtohawkbit
======================

swupdate-sendtohawkbit is a small tool that tries to connect to a running
instance of SWUpdate and uses it as proxy to send data to the hawkBit Server.
A typical use case is acknowledgement for activation by an application or
operator after a new software has been installed. The tool can forward the
result for the activation to the hawkBit server.

SYNOPSIS
--------

swupdate-sendtohawkbit <action id> <status> <finished> <execution> <detail 1> <detail 2> ..

DESCRIPTION
-----------

See hawkBit API to get more information on the parameters.
