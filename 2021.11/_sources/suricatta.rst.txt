.. SPDX-FileCopyrightText: 2013-2021 Stefano Babic <sbabic@denx.de>
.. SPDX-License-Identifier: GPL-2.0-only

=====================
Suricatta daemon mode
=====================

Introduction
------------

Suricatta is -- like mongoose -- a daemon mode of SWUpdate, hence the
name suricatta (engl. meerkat) as it belongs to the mongoose family.

Suricatta regularly polls a remote server for updates, downloads, and
installs them. Thereafter, it reboots the system and reports the update
status to the server, based on an update state variable currently stored
in bootloader's environment ensuring persistent storage across reboots. Some
U-Boot script logics or U-Boot's ``bootcount`` feature may be utilized
to alter this update state variable, e.g., by setting it to reflect
failure in case booting the newly flashed root file system has failed
and a switchback had to be performed.

Suricatta is designed to be extensible in terms of the servers supported
as described in Section `Supporting different Servers`_. Currently,
support for the `hawkBit`_ server is implemented via the `hawkBit Direct
Device Integration API`_ alongside a simple general purpose HTTP server.

.. _hawkBit Direct Device Integration API:  http://sp.apps.bosch-iot-cloud.com/documentation/developerguide/apispecifications/directdeviceintegrationapi.html
.. _hawkBit:  https://projects.eclipse.org/projects/iot.hawkbit


Running suricatta
-----------------

After having configured and compiled SWUpdate with enabled suricatta
support,

.. code::

  ./swupdate --help

lists the mandatory and optional arguments to be provided to suricatta
when using hawkBit as server. As an example,

.. code:: bash

    ./swupdate -l 5 -u '-t default -u http://10.0.0.2:8080 -i 25'

runs SWUpdate in suricatta daemon mode with log-level ``TRACE``, polling
a hawkBit instance at ``http://10.0.0.2:8080`` with tenant ``default``
and device ID ``25``.


Note that on startup when having installed an update, suricatta
tries to report the update status to its upstream server, e.g.,
hawkBit, prior to entering the main loop awaiting further updates.
If this initial report fails, e.g., because of a not (yet) configured
network or a currently unavailable hawkBit server, SWUpdate may exit
with an according error code. This behavior allows one to, for example,
try several upstream servers sequentially.
If suricatta should keep retrying until the update status is reported
to its upstream server irrespective of the error conditions, this has
to be realized externally in terms of restarting SWUpdate on exit.


After an update has been performed, an agent listening on the progress
interface may execute post-update actions, e.g., a reboot, on receiving
``DONE``. 
Additionally, a post-update command specified in the configuration file or
given by the ``-p`` command line option can be executed.

Note that at least a restart of SWUpdate has to be performed as post-update
action since only then suricatta tries to report the update status to its
upstream server. Otherwise, succinct update actions announced by the
upstream server are skipped with an according message until a restart of
SWUpdate has happened in order to not install the same update again.


Supporting different Servers
----------------------------

Support for servers other than hawkBit can be realized by implementing
the "interfaces" described in ``include/channel.h`` and
``include/suricatta/server.h``. The former abstracts a particular
connection to the server, e.g., HTTP-based in case of hawkBit, while
the latter implements the logics to poll and install updates.
See ``corelib/channel_curl.c``/``include/channel_curl.h`` and
``suricatta/server_hawkbit.{c,h}`` for an example implementation
targeted towards hawkBit.

``include/channel.h`` describes the functionality a channel
has to implement:

.. code:: c

    typedef struct channel channel_t;
    struct channel {
        ...
    };

    channel_t *channel_new(void);

which sets up and returns a ``channel_t`` struct with pointers to
functions for opening, closing, fetching, and sending data over
the channel.

``include/suricatta/server.h`` describes the functionality a server has
to implement:

.. code:: c

    server_op_res_t server_has_pending_action(int *action_id);
    server_op_res_t server_install_update(void);
    server_op_res_t server_send_target_data(void);
    unsigned int server_get_polling_interval(void);
    server_op_res_t server_start(const char *cfgfname, int argc, char *argv[]);
    server_op_res_t server_stop(void);
    server_op_res_t server_ipc(int fd);

The type ``server_op_res_t`` is defined in ``include/suricatta/suricatta.h``.
It represents the valid function return codes for a server's implementation.

In addition to implementing the particular channel and server, the
``suricatta/Config.in`` file has to be adapted to include a new option
so that the new implementation becomes selectable in SWUpdate's
configuration. In the simplest case, adding an option like the following
one for hawkBit into the ``menu "Server"`` section is sufficient.

.. code:: bash

    config SURICATTA_HAWKBIT
        bool "hawkBit support"
        depends on HAVE_LIBCURL
        depends on HAVE_JSON_C
        select JSON
        select CURL
        help
          Support for hawkBit server.
          https://projects.eclipse.org/projects/iot.hawkbit

Having included the new server implementation into the configuration,
edit ``suricatta/Makefile`` to specify the implementation's linkage into
the SWUpdate binary, e.g., for the hawkBit example implementation, the
following lines add ``server_hawkbit.o`` to the resulting SWUpdate binary
if ``SURICATTA_HAWKBIT`` was selected while configuring SWUpdate.

.. code:: bash

    ifneq ($(CONFIG_SURICATTA_HAWKBIT),)
    lib-$(CONFIG_SURICATTA) += server_hawkbit.o
    endif


Support for general purpose HTTP server
---------------------------------------

This is a very simple backend that uses standard HTTP response codes to signal if
an update is available. There are closed source backends implementing this interface,
but because the interface is very simple interface, this server type is also suitable
for implementing an own backend server.

The API consists of a GET with Query parameters to inform the server about the installed version.
The query string has the format:

::

        http(s)://<base URL>?param1=val1&param2=value2...

As examples for parameters, the device can send its serial number, MAC address and the running version of the software.
It is duty of the backend to interpret this - SWUpdate just takes them from the "identify" section of
the configuration file and encodes the URL.

The server answers with the following return codes:

+-----------+-------------+------------------------------------------------------------+
| HTTP Code | Text        | Description                                                |
+===========+=============+============================================================+
|    302    | Found       | A new software is available at URL in the Location header  |
+-----------+-------------+------------------------------------------------------------+
|    400    | Bad Request | Some query parameters are missing or in wrong format       |
+-----------+-------------+------------------------------------------------------------+
|    403    | Forbidden   | Client certificate not valid                               |
+-----------+-------------+------------------------------------------------------------+
|    404    | Not found   | No update is available for this device                     |
+-----------+-------------+------------------------------------------------------------+
|    503    | Unavailable | An update is available but server can't handle another     |
|           |             | update process now.                                        |
+-----------+-------------+------------------------------------------------------------+

Server's answer can contain the following headers:

+---------------+--------+------------------------------------------------------------+
| Header's name | Codes  | Description                                                |
+===============+========+============================================================+
| Retry-after   |   503  | Contains a number which tells the device how long to wait  |
|               |        | until ask the next time for updates. (Seconds)             |
+---------------+--------+------------------------------------------------------------+
| Content-MD5   |   302  | Contains the checksum of the update file which is available|
|               |        | under the url of location header                           |
+---------------+--------+------------------------------------------------------------+
| Location      |   302  | URL where the update file can be downloaded.               |
+---------------+--------+------------------------------------------------------------+

The device can send logging data to the server. Any information is transmitted in a HTTP
PUT request with the data as plain string in the message body. The Content-Type Header
need to be set to text/plain.

The URL for the logging can be set as separate URL in the configuration file or via
--logurl command line parameter:

The device sends data in a CSV format (Comma Separated Values). The format is:

::

        value1,value2,...

The format can be specified in the configuration file. A *format* For each *event* can be set.
The supported events are:

+---------------+------------------------------------------------------------+
| Event         | Description                                                |
+===============+========+===================================================+
| check         | dummy. It could send an event each time the server is      |
|               | polled.                                                    |
+---------------+------------------------------------------------------------+
| started       | A new software is found and SWUpdate starts to install it  |
+---------------+------------------------------------------------------------+
| success       | A new software was successfully installed                  |
+---------------+------------------------------------------------------------+
| fail          | Failure by installing the new software                     |
+---------------+------------------------------------------------------------+

The `general server` has an own section inside the configuration file. As example:

::

        gservice =
        {
	        url 		= ....;
	        logurl		= ;
	        logevent : (
		        {event = "check"; format="#2,date,fw,hw,sp"},
		        {event = "started"; format="#12,date,fw,hw,sp"},
		        {event = "success"; format="#13,date,fw,hw,sp"},
		        {event = "fail"; format="#14,date,fw,hw,sp"}
	        );
        }


`date` is a special field and it is interpreted as localtime in RFC 2822 format. Each
Comma Separated field is looked up inside the `identify` section in the configuration
file, and if a match is found the substitution occurs. In case of no match, the field
is sent as it is. For example, if the identify section has the following values:


::

        identify : (
        	{ name = "sp"; value = "333"; },
        	{ name = "hw"; value = "ipse"; },
        	{ name = "fw"; value = "1.0"; }
        );


with the events set as above, the formatted text in case of "success" will be:

::

        Formatted log: #13,Mon, 17 Sep 2018 10:55:18 CEST,1.0,ipse,333
