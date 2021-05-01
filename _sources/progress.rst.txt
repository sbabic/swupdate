Getting information on running update
=====================================

It is often required to inform the operator about the status of the running
update and not just to return if the update was successful or not.
For example, if the target has a display or a remote interface,
it can be forwarded which is reached percentage of the update
to let estimate how much the update will still run.
SWUpdate has an interface for this ("progress API"). An external
process can register itself with SWUpdate, and it will receive
notifications when something in the update was changed. This is
different from the IPC API, because the last one is mainly used to transfer
the SWU image, and it is only possible to poll the interface to know
if the update is still running.


API Description
---------------

An external process registers itself to SWUpdate with a connect()
request to the domain socket "/tmp/swupdateprog" as per default
configuration of SWUpdate. There is no information to send, and
SWUpdate simply inserts the new connection into the list of processes
to be informed. SWUpdate will send a frame back after any change in
the update process with the following data (see include/progress_ipc.h):

::

        struct progress_msg {
        	unsigned int	magic;		/* Magic Number */
        	unsigned int	status;		/* Update Status (Running, Failure) */
        	unsigned int	dwl_percent;	/* % downloaded data */
        	unsigned int	nsteps;		/* No. total of steps */
        	unsigned int	cur_step;	/* Current step index */
        	unsigned int	cur_percent;	/* % in current step */
        	char		cur_image[256];	/* Name of image to be installed */
        	char		hnd_name[64];	/* Name of running handler */
        	sourcetype	source;		/* Interface that triggered the update */
        	unsigned int 	infolen;    	/* Len of data valid in info */
        	char		info[2048];   	/* additional information about install */
        };

The single fields have the following meaning:

        - *magic* is not yet used, it could be added for simply verification of the frame.
        - *status* is one of the values in swupdate_status.h (START, RUN, SUCCESS, FAILURE, DOWNLOAD, DONE).
        - *dwl_percent* is the percentage of downloaded data when status = DOWNLOAD.
        - *nsteps* is the total number of installers (handlers) to be run.
        - *cur_step* is the index of the running handler. cur_step is in the range 1..nsteps
        - *cur_percent* is the percentage of work done inside the current handler. This is useful
          when updating a slow interface, such as a slow flash, and signals which is the percentage
          of image already copied into the destination.
        - *cur_image* is the name of the image in sw-description that is currently being installed.
        - *hnd_name* reports the name of the running handler.
        - *source* is the interface that triggered the update.
        - *infolen* length of data in the following info field.
        - *info* additional information about installation.


As an example for a progress client, ``tools/swupdate-progress.c`` prints the status
on the console and drives "psplash" to draw a progress bar on a display.

