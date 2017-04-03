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
with an according error code. This behavior allows to, for example,
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

    channel_op_res_t channel_open(channel_t *this, void *cfg);
    channel_op_res_t channel_close(channel_t *this);
    channel_op_res_t channel_put(channel_t *this, void* data);
    channel_op_res_t channel_get(channel_t *this, void* data);
    channel_op_res_t channel_get_file(channel_t *this, void* data, int file_handle);

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
        select SURICATTA
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

::

    ifneq ($(CONFIG_SURICATTA_HAWKBIT),)
    lib-$(CONFIG_SURICATTA) += channel_hawkbit.o server_hawkbit.o
    endif
