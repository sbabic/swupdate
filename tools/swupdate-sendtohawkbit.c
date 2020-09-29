/*
 * (C) Copyright 2017
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
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

static void usage(char *program) {
	printf("%s <action id> <status> <finished> <execution> <detail 1> <detail 2> ..\n", program);
}

int fd;
int verbose = 1;

/*
 * Simple example, it does nothing but calling the library
 */
int main(int argc, char *argv[]) {
	int rc, written, i;
	ipc_message msg;
	size_t size;
	char *buf;

	if (argc < 3) {
		usage(argv[0]);
		exit(1);
	}

	memset(&msg, 0, sizeof(msg));
	msg.data.instmsg.source = SOURCE_SURICATTA;
	msg.data.instmsg.cmd = CMD_ACTIVATION;
	msg.type = SWUPDATE_SUBPROCESS;

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

	if (i > 5)
		written = snprintf(buf, size, "]}");
	else
		written = snprintf(buf, size, "}");

	fprintf(stdout, "Sending: '%s'", msg.data.instmsg.buf);
	msg.data.instmsg.len = strnlen(msg.data.instmsg.buf, sizeof(msg.data.instmsg.buf));

	rc = ipc_send_cmd(&msg);

	fprintf(stdout, " returned %d\n", rc);
	if (!rc)
		fprintf(stdout, "Server returns %s\n",
				(msg.type == ACK) ? "ACK" : "NACK");

	exit(0);
}

