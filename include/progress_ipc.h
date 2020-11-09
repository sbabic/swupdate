/*
 * Author: Christian Storm
 * Copyright (C) 2017, Siemens AG
 *
 * SPDX-License-Identifier:     LGPL-2.1-or-later
 */

#ifndef _PROGRESS_IPC_H
#define _PROGRESS_IPC_H

#include <stdbool.h>
#include <swupdate_status.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PRINFOSIZE	2048

extern char* SOCKET_PROGRESS_PATH;

/*
 * Message sent via progress socket.
 * Data is sent in LE if required.
 */
struct progress_msg {
	unsigned int	magic;		/* Magic Number */
	RECOVERY_STATUS	status;		/* Update Status (Running, Failure) */
	unsigned int	dwl_percent;	/* % downloaded data */
	unsigned long long dwl_bytes;   /* total of bytes to be downloaded */
	unsigned int	nsteps;		/* No. total of steps */
	unsigned int	cur_step;	/* Current step index */
	unsigned int	cur_percent;	/* % in current step */
	char		cur_image[256];	/* Name of image to be installed */
	char		hnd_name[64];	/* Name of running hanler */
	sourcetype	source;		/* Interface that triggered the update */
	unsigned int 	infolen;    	/* Len of data valid in info */
	char		info[PRINFOSIZE]; /* additional information about install */
};

char *get_prog_socket(void);

/* Standard function to connect to progress interface */
int progress_ipc_connect(bool reconnect);

/*
 * In case more as an instance of SWUpdate is running, this allows to select
 * which should be taken
 */
int progress_ipc_connect_with_path(const char *socketpath, bool reconnect);

/* Retrieve messages from progress interface (it blocks) */
int progress_ipc_receive(int *connfd, struct progress_msg *msg);

#ifdef __cplusplus
}   // extern "C"
#endif

#endif
