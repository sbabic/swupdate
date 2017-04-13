/*
 * (C) Copyright 2017
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
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

/*
 * This is a simple example how to send a command to
 * a SWUpdate's subprocess. It sends a "feedback"
 * to the suricatta module and waits for the answer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include "network_ipc.h"

void usage(char *program) {
	printf("%s <action id> <status> <finished> <execution> <detail 1> <detail 2> ..\n", program);
}

char buf[256];
int fd;
int verbose = 1;

/*
 * Simple example, it does nothing but calling the library
 */
int main(int argc, char *argv[]) {
	int c;
	const char *fn;
	int rc, written, i;
	char *progname;
	ipc_message msg;
	size_t size;
	char *buf;

	if (argc < 3) {
		usage(argv[0]);
		exit(1);
	}

	memset(&msg, 0, sizeof(msg));
	msg.data.instmsg.source = SOURCE_SURICATTA;
	msg.data.instmsg.cmd = 0;

	size = sizeof(msg.data.instmsg.buf);
	buf = msg.data.instmsg.buf;

	/*
	 * Build a json string with the command line parameters
	 * do not check anything, let SWUpdate
	 * doing the checks
	 * An error or a NACK is returned in
	 * case of failure
	 */
	for (i = 1; i < argc; i++) {
		switch (i) {
		case 1:
			written = snprintf(buf, size, "{ \"id\" : \"%lu\"", strtoul(argv[i], NULL, 10));
			break;
		case 2:
			written = snprintf(buf, size, ", \"status\" : \"%s\"", argv[i]);
			break;
		case 3:
			written = snprintf(buf, size, ",\"finished\" : \"%s\"", argv[i]);
			break;
		case 4:
			written = snprintf(buf, size, ",\"execution\" : \"%s\"", argv[i]);
			break;
		case 5:
			written = snprintf(buf, size, ",\"details\" : [ \"%s\"", argv[i]);
			break;
		default:
			written = snprintf(buf, size, ",\"%s\"", argv[i]);
			break;
		}

		buf += written;
		size -= written;

		if (size <= 0)
			break;
	}

	if (i > 4)
		written = snprintf(buf, size, "]}");
	else
		written = snprintf(buf, size, "}");

	fprintf(stdout, "Sending: '%s'", msg.data.instmsg.buf);

	rc = ipc_send_cmd(&msg);

	fprintf(stdout, " returned %d\n", rc);
	if (!rc)
		fprintf(stdout, "Server returns %s\n",
				(msg.type == ACK) ? "ACK" : "NACK");

	exit(0);
}

