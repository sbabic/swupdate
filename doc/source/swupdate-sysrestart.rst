swupdate-sysrestart
===================

swupdate-sysrestart is a restart controller. It checks the update status
and in case of success, it reboots all devices that were updated.

SYNOPYS
-------

swupdate-sysrestart [option]

DESCRIPTION
-----------

-r
        optionally reboot the target after a successful update
-w
        waits for a SWUpdate connection instead of exit with error
-s <path>
        path to progress IPC socket
