===================================
swupdate: API for external programs
===================================

Overview
========

swupdate contains an integrated web-server to allow remote updating.
However, which protocols are involved during an update is project
specific and differs significantly. Some projects can decide
to use FTP to load an image from an external server, or using
even a proprietary protocol.
The integrated web-server uses this interface.

swupdate has a simple interface to let external programs
to communicate with the installer. Clients can start an upgrade
and stream an image to the installer, querying then for the status
and the final result. The API is at the moment very simple, but it can
easy be extended in the future if new use cases will arise.

API Description
---------------

The communication runs via UDS (Unix Domain Socket). The socket is created
at the startup by swupdate in /tmp/sockinstdata.

The exchanged packets are described in network_ipc.h

::

	typedef struct {
		int magic;
		int type;
		msgdata data;
	} ipc_message;


Where the fields have the meaning:

- magic : a magic number as simple proof of the packet
- type : one of REQ_INSTALL, ACK, NACK, GET_STATUS
- msgdata : a buffer used by the client to send the image
  or by swupdate to report back notifications and status.

The client sends a REQ_INSTALL packet and waits for an answer.
swupdate sends back ACK or NACK, if for example an update is already in progress.

After the ACK, the client sends the whole image as a stream. swupdate
expects that all bytes after the ACK are part of the image to be installed.
swupdate recognizes the size of the image from the CPIO header.
Any error lets swupdate to leave the update state, and further packets
will be ignored until a new REQ_INSTALL will be received.

.. image:: images/API.png

Client Library
--------------

A library simplifies the usage of the IPC making available a way to
start asynchrounosly an update.

The library consists of one function and several call-backs.

::

        int swupdate_async_start(writedata wr_func, getstatus status_func,
                terminated end_func)
        typedef int (*writedata)(char **buf, int *size);
        typedef int (*getstatus)(ipc_message *msg);
        typedef int (*terminated)(RECOVERY_STATUS status);

swupdate_async_start creates a new thread and start the communication with swupdate,
triggering for a new update. The wr_func is called to get the image to be installed.
It is responsibility of the callback to provide the buffer and the size of
the chunk of data.

The getstatus call-back is called after the stream was downloaded to check
how upgrade is going on. It can be omitted if only the result is required.

The terminated call-back is called when swupdate has finished with the result
of the upgrade.

Example about using this library is in the examples/client directory.
