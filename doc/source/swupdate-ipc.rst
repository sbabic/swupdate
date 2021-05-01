===================================
SWUpdate: API for external programs
===================================

Overview
========

SWUpdate contains an integrated web-server to allow remote updating.
However, which protocols are involved during an update is project
specific and differs significantly. Some projects can decide
to use FTP to load an image from an external server, or using
even a proprietary protocol.
The integrated web-server uses this interface.

SWUpdate has a simple interface to let external programs
to communicate with the installer. Clients can start an upgrade
and stream an image to the installer, querying then for the status
and the final result. The API is at the moment very simple, but it can
easy be extended in the future if new use cases will arise.

.. _install_api:

API Description
===============

The communication runs via UDS (Unix Domain Socket). The socket is created
at startup by SWUpdate in /tmp/sockinstctrl as per default configuration.
This socket should, however, not be used directly but instead by the Client
Library explained below.

The exchanged packets are described in network_ipc.h

::

	typedef struct {
		int magic;
		int type;
		msgdata data;
	} ipc_message;


Where the fields have the meaning:

- magic : a magic number as simple proof of the packet
- type : one of REQ_INSTALL, ACK, NACK,
  GET_STATUS, POST_UPDATE, SWUPDATE_SUBPROCESS, SET_AES_KEY
- msgdata : a buffer used by the client to send the image
  or by SWUpdate to report back notifications and status.

The client sends a REQ_INSTALL packet and waits for an answer.
SWUpdate sends back ACK or NACK, if for example an update is already in progress.

After the ACK, the client sends the whole image as a stream. SWUpdate
expects that all bytes after the ACK are part of the image to be installed.
SWUpdate recognizes the size of the image from the CPIO header.
Any error lets SWUpdate to leave the update state, and further packets
will be ignored until a new REQ_INSTALL will be received.

.. image:: images/API.png

It is recommended to use the client library to communicate with SWUpdate. On the lower
level with direct socket communication, it cannot be guaranteed that the structures
will remain compatible in the future. The client library was affected by this issue, too, and it is
changed to accept an opaque interface that will survive API changes. Compatibility
layers could be added on-demand in the future due to API changes.


Client Library
==============

Functions to start an update
----------------------------
A library simplifies the usage of the IPC making available a way to
start asynchronously an update.

The library consists of one function and several call-backs.

::

        int swupdate_async_start(writedata wr_func, getstatus status_func,
                terminated end_func, void *req, ssize_t size)
        typedef int (*writedata)(char **buf, int *size);
        typedef int (*getstatus)(ipc_message *msg);
        typedef int (*terminated)(RECOVERY_STATUS status);

swupdate_async_start creates a new thread and start the communication with SWUpdate,
triggering for a new update. The wr_func is called to get the image to be installed.
It is responsibility of the callback to provide the buffer and the size of
the chunk of data.

The getstatus call-back is called after the stream was downloaded to check
how upgrade is going on. It can be omitted if only the result is required.

The terminated call-back is called when SWUpdate has finished with the result
of the upgrade.

Example about using this library is in the examples/client directory.

The `req` structure is casted to void to ensure API compatibility. Am user
should instantiate it as `struct swupdate_request`. This contains fields that can control
the update process:

::

        struct swupdate_request {
                unsigned int apiversion;
                sourcetype source;
                int dry_run;
                size_t len;
                char info[512];
                char software_set[256];
                char running_mode[256];
        };

A user should first call `swupdate_prepare_req()`

::

        void swupdate_prepare_req(struct swupdate_request *req);

This fills the request structure with default values. After that, the user can fill the
other fields as:

        - *sourcetype* : one of SOURCE_UNKNOWN, SOURCE_WEBSERVER,
	  SOURCE_SURICATTA, SOURCE_DOWNLOADER, SOURCE_LOCAL
        - *dry_run* : one of RUN_DEFAULT (set from command line), RUN_DRYRUN, RUN_INSTALL.
        - *info, len* : a variable length data that can be forwarded to the progress
          interface. The installer in SWUpdate does not evaluate it.
        - *software_set* and *running_mode* : this allows one to set the `selection` for the update.

Functions to set AES keys
-------------------------

The key for decryption can be set with command line parameter (see `-K`), but it is possible
to set it via IPC. In this way, each update could have a different key.

::

        int swupdate_set_aes(char *key, char *ivt)

The key is for AES-256. The length for key and ivt are then defined by the algorithm amd they are passed as ASCII string, so the length *must* be 64 bytes for key and 32 bytes for IVT.

Functions to control SWUpdate
-----------------------------

::

        int ipc_send_cmd(ipc_message *msg);

ipc_send_cmd is used to send a command to a SWUpdate subprocess (as suricatta). The function is synchron,
that means it clocks until the subprocess has answered with ACK or NACK. This function sets `type` to SWUPDATE_SUBPROCESS.
The caller must then set the other fields in message according to the destination.
The msgdata field is a structure as:

::

     struct {
        sourcetype source; /* Who triggered the update */
        int	cmd;	   /* Optional encoded command */
        int	timeout;     /* timeout in seconds if an aswer is expected */
        unsigned int len;    /* Len of data valid in buf */
        char	buf[2048];   /*
                              * Buffer that each source can fill
                              * with additional information
                              */
        }

The caller fills `source` with the subprocess that acceps the command. Values of cmd
are in `network_ipc.h`.

Messages for suricatta
----------------------

suricatta accepts messages in JSON format. The message must be formatted in the `buf` field of
the message data.

Setting the polling time
........................

::

        { "polling" : <value in seconds, range 0..X>}

Setting it to 0 has the special meaning that the polling time is retrieved from the Backend
(if this is supported by the server).

Enable / disable Suricatta daemon
.................................

::

        { "enable" : true }
        { "enable" : false }

Set custom device attributes for Suricatta (for Hawkbit implementation)
.......................................................................

::

        { "identify" : [
            {
                "name"  : "customizableAttributeOne",
                "value" : "valueOne"
            },
            {
                "name"  : "customizableAttributeTwo",
                "value" : "valueTwo"
            }
        ]}

New attributes can be added at runtime, and existing attributes can be modified in the same
way. Changes will be reflected on the server in the next poll iteration.


Trigger a check on the server
.............................

This is useful in case the device is mostly offline, and when it is online, it should check
immediately if an update exists and run it. In fact, after enabling the suricatta daemon,
the update follows the usual states, and the daemon waits for a polling time before
loading the new software. This command forces an update (if available) without changing the
polling time.

::

        { "trigger" : true }


Activate an already installed Software
......................................

After a software was installed, the new software boots and if everything runs fine,
an acknowledge should be sent to the hawkBit server. If this feature is used, for example
to let the end user decide if the new software is accepted, the parameters used by the installation
should be stored during the update process.

::

        { "id" : <action id>,
          "finished" : "success", "failure", "none",
          "execution" : ["closed", "proceeding", canceled", "rejected", "resumed"]
          "details" : [ ]
        }

API to the integrated Webserver
===============================

The integrated Webserver provides REST resources to push a SWU package and to get inform about the update process.
This API is based on HTTP standards. There are to kind of interface:

- Install API to push a SWU and to restart the device after update.
- A WebSocket interface to send the status of the update process.

Install API
-----------

::

        POST /upload

This initiates an update: the initiator sends the request and start to stream the SWU in the same
way as described in :ref:`install_api`.

Restart API
-----------

::

        POST /restart

If configured (see post update command), this request will restart the device.


WebSocket API
-------------

The integrated Webserver exposes a WebSocket API. The WebSocket protocol specification defines ws (WebSocket) and wss (WebSocket Secure) as two new uniform resource identifier (URI) schemes that are used for unencrypted and encrypted con
nections, respectively and both of them are supported by SWUpdate.
A WebSocket provides full-duplex communication but it is used in SWUpdate to send events to an external host after
each change in the update process. The Webserver sends JSON formatted responses as results of internal events.

The response contains the field type, that defines which event is sent.

.. table:: Event Type

        +-----------+----------------------------------------------------------------+
        |  type     |   Description of event                                         |
        +===========+================================================================+
        | status    | Event sent when SWUpdate's internal state changes              |
        +-----------+----------------------------------------------------------------+
        | source    | Event to inform from which interface an update is received     |
        +-----------+----------------------------------------------------------------+
        | info      | Event with custom message to be passed to an external process  |
        +-----------+----------------------------------------------------------------+
        | message   | Event that contains the error message in case of error         |
        +-----------+----------------------------------------------------------------+
        | step      | Event to inform about the running update                       |
        +-----------+----------------------------------------------------------------+



Status Change Event
-------------------

This event is sent when the internal SWUpdate status change. Following status are supported:

::

        IDLE
        START
        RUN
        SUCCESS


Example:

::

        {
	        "type": "status",
		"status": "SUCCESS"
	}

Source Event
------------

This event informs from which interface a SWU is loaded.

::

        {
	        "type": "source",
		"source": "WEBSERVER"
	}

The field `source` can have one of the following values:

::

        UNKNOWN
        WEBSERVER
        SURICATTA
        DOWNLOADER
        LOCAL

Info Event
------------

This event forwards all internal logs sent with level=INFO.

::

        {
	        "type": "info",
		"source": < text message >
	}

Message Event
-------------

This event contains the error message in case of failure.


.. table:: Fields for message event

        +-----------+----------------------------------------------------------------+
        |  name     |   Description                                                  |
        +===========+================================================================+
        | status    | "message"                                                      |
        +-----------+----------------------------------------------------------------+
        | level     | "3" in case of error, "6" as info                              |
        +-----------+----------------------------------------------------------------+
        | text      | Message associated to the event                                |
        +-----------+----------------------------------------------------------------+

Example:

::

        {
	        "type": "message",
		"level": "3",
                "text" : "[ERROR] : SWUPDATE failed [0] ERROR core/cpio_utils.c : ",
	}

Step event
----------

This event contains which is the current step running and which percentage of this step is currently installed.

.. table:: Fields for step event

        +-----------+----------------------------------------------------------------+
        |  name     |   Description                                                  |
        +===========+================================================================+
        | number    | total number of steps N for this update                        |
        +-----------+----------------------------------------------------------------+
        | step      | running step in range [1..N]                                   |
        +-----------+----------------------------------------------------------------+
        | name      | filename of artefact to be installed                           |
        +-----------+----------------------------------------------------------------+
        | percent   | percentage of the running step                                 |
        +-----------+----------------------------------------------------------------+

Example:

::

        {
		"type": "step",
		"number": "7",
		"step": "2",
		"name": "rootfs.ext4.gz",
		"percent": "18"
	}
