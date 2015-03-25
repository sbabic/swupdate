/*
 * (C) Copyright 2008
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 * 	on behalf of ifm electronic GmbH
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

#ifndef _IPC_H
#define _IPC_H

#define IPC_MAGIC		0x14052001
#define SOCKET_CTRL_PATH	"/tmp/sockinstctrl"

typedef enum {
	REQ_INSTALL,
	ACK,
	NACK,
	GET_STATUS
} msgtype;

typedef union {
	char msg[128];
	struct { 
		int current;
		int last_result;
		int error;
		char desc[1024];
	} status;
} msgdata;
	
typedef struct {
	int magic;	/* magic number */
	int type;
	msgdata data;
} ipc_message;

int ipc_inst_start(void);
int ipc_send_data(int connfd, char *buf, int size);
void ipc_end(int connfd);
int ipc_get_status(ipc_message *msg);

void *network_thread(void *data);

extern pthread_mutex_t stream_mutex;
extern pthread_cond_t stream_wkup;

#endif
