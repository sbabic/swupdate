Suricatta daemon mode
=====================

Suricatta is -- like mongoose -- a daemon mode of SWUpdate, hence the
name suricatta (engl. meerkat) as it belongs to the mongoose family.

Suricatta regularly polls a remote server for updates, downloads, and
installs them. Thereafter, it reboots the system and reports the update
status to the server, based on an update state variable currently stored
in U-Boot's environment ensuring persistent storage across reboots. Some
U-Boot script logics or U-Boot's ``bootcount`` feature may be utilized
to alter this update state variable, e.g., by setting it to reflect
failure in case booting the newly flashed root file system has failed
and a switchback had to be performed.

Suricatta is designed to be extensible in terms of the servers supported
as described in Section `Supporting different Servers`_. Currently,
support for the `hawkBit`_ server is implemented via the `hawkBit Direct
Device Integration API`_.

.. _hawkBit Direct Device Integration API:  http://sp.apps.bosch-iot-cloud.com/documentation/developerguide/apispecifications/directdeviceintegrationapi.html
.. _hawkBit:  https://projects.eclipse.org/projects/iot.hawkbit


Running suricatta
-----------------

After having configured and compiled SWUpdate with enabled suricatta
support,

.. code::

  ./swupdate --suricatta --help
  [...]
  Arguments (mandatory arguments are marked with '*'):
  -t, --tenant      * Set hawkBit tenant ID for this device.
  -u, --url         * Host and port of the hawkBit instance, e.g., localhost:8080
  -i, --id          * The device ID to communicate to hawkBit.
  -c, --confirm       Confirm update status to server: 1=AGAIN, 2=SUCCESS, 3=FAILED
  -x, --nocheckcert   Do not abort on flawed server certificates.
  -p, --polldelay     Delay in seconds between two hawkBit poll operations (default: 45s).
  -r, --retry         Resume and retry interrupted downloads (default: 5 tries).
  -w, --retrywait     Time to wait between prior to retry and resume a download (default: 5s).
  -l, --loglevel      set log level (0=OFF, ..., 5=TRACE)
  -v, --verbose       Verbose operation, i.e., loglevel=TRACE

lists the mandatory and optional arguments to be provided to suricatta
when using hawkBit as server. As an example,

.. code:: bash

    ./swupdate -u '-t default -u http://10.0.0.2:8080 -i 25 -l 5'

runs SWUpdate in suricatta daemon mode with log-level ``TRACE``, polling
a hawkBit instance at ``http://10.0.0.2:8080`` with tenant ``default``
and device ID ``25``.


Supporting different Servers
----------------------------

Support for servers other than hawkBit can be realized by implementing
the "interfaces" described in ``include/suricatta/channel.h`` and
``include/suricatta/server.h``. The former abstracts a particular
connection to the server, e.g., HTTP-based in case of hawkBit, while
the latter implements the logics to poll and install updates.
See ``suricatta/channel_hawkbit.{c,h}`` and
``suricatta/server_hawkbit.{c,h}`` for an example implementation
targeted towards hawkBit.

``include/suricatta/channel.h`` describes the functionality a channel
has to implement:

.. code:: c

    channel_op_res_t channel_open(void);
    channel_op_res_t channel_close(void);
    channel_op_res_t channel_put(void* data);
    channel_op_res_t channel_get(void* data);
    channel_op_res_t channel_get_file(void* data);

``include/suricatta/server.h`` describes the functionality a server has
to implement:

.. code:: c

    server_op_res_t server_start(int argc, char* argv[]);
    server_op_res_t server_stop(void);
    server_op_res_t server_has_pending_action(void);
    server_op_res_t server_install_update(void);
    unsigned int    server_get_polling_interval(void);

The types ``channel_op_res_t`` and ``server_op_res_t`` are defined in
``include/suricatta/suricatta.h`` and represent the valid function
return codes for a channel's and a server's implementation,
respectively.

In addition to implementing the particular channel and server, the
``suricatta/Config.in`` file has to be adapted to include a new option
so that the new implementation becomes selectable in SWUpdate's
configuration. In the simplest case, adding an option like the following
one for hawkBit into the ``menu "Server"`` section is sufficient.

::

    config SURICATTA_HAWKBIT
        bool "hawkBit support"
        depends on HAVE_LIBCURL
        depends on HAVE_JSON_C
        default y
        help
        Support for hawkBit server.

        https://projects.eclipse.org/projects/iot.hawkbit

Note that the various server options and hence implementations should be
selectable in a mutually exclusive manner, i.e., at most one should be
active. Hence, include according ``depends on !<SERVER_OPTION>`` lines
into the configuration to specify this mutual exclusion of server
implementations. Support for multiple channels and servers
simultaneously is left for future work as outlined in suricatta's
road-map.

Having included the new server implementation into the configuration,
edit ``suricatta/Makefile`` to specify the implementation's linkage into
the SWUpdate binary, e.g., for the hawkBit example implementation, the
following lines add ``channel_hawkbit.o`` and ``server_hawkbit.o`` to
the resulting SWUpdate binary if ``SURICATTA_HAWKBIT`` was selected
while configuring SWUpdate.

.. code:: makefile

    ifneq ($(CONFIG_SURICATTA_HAWKBIT),)
    lib-$(CONFIG_SURICATTA) += channel_hawkbit.o server_hawkbit.o
    endif

