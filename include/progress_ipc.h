/*
 * Author: Christian Storm
 * Copyright (C) 2017, Siemens AG
 *
 * SPDX-License-Identifier:     LGPL-2.1-or-later
 */

#pragma once

#include <stdbool.h>
#include <swupdate_status.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PRINFOSIZE	2048

typedef enum progress_cause {
	CAUSE_NONE,
	CAUSE_REBOOT_MODE,
} progress_cause_t;

extern char* SOCKET_PROGRESS_PATH;

/*
 * Versioning of API
 * it is defined as
 * bits 31..24 : unused, set to 0
 * bits 23..16 : Major Version
 * bits 15..8  : Minor version
 * bits 7..0   : small changes not relevant for compatibility
 *
 * The following policy is followed:
 * - changes in minor version mean that the API was enhanced and it has
 *   new features, but it is compatible with the older. It is suggested
 *   that clients are updated, but they still work.
 * - changes in major mean an incompatibility and clients do not work anymore
 */

#define PROGRESS_API_MAJOR	1
#define PROGRESS_API_MINOR	0
#define PROGRESS_API_PATCH	0

#define PROGRESS_API_VERSION 	((PROGRESS_API_MAJOR & 0xFFFF) << 16 | \
				(PROGRESS_API_MINOR & 0xFF) << 8 | \
				(PROGRESS_API_PATCH & 0xFF))
/*
 * Message sent via progress socket.
 * Data is sent in LE if required.
 */
struct progress_msg {
	unsigned int	apiversion;	/* API Version for compatibility check */
	RECOVERY_STATUS	status;		/* Update Status (Running, Failure) */
	unsigned int	dwl_percent;	/* % downloaded data */
	unsigned long long dwl_bytes;   /* total of bytes to be downloaded */
	unsigned int	nsteps;		/* No. total of steps */
	unsigned int	cur_step;	/* Current step index */
	unsigned int	cur_percent;	/* % in current step */
	char		cur_image[256];	/* Name of image to be installed */
	char		hnd_name[64];	/* Name of running handler */
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
