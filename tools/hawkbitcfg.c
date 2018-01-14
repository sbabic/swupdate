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
	printf("%s <polling interval 0=from server> ..\n", program);
}

/*
 * Simple example, it does nothing but calling the library
 */
int main(int argc, char *argv[]) {
	int rc;
	ipc_message msg;
	size_t size;
	char *buf;

	if (argc < 2) {
		usage(argv[0]);
		exit(1);
	}

	memset(&msg, 0, sizeof(msg));
	msg.data.instmsg.source = SOURCE_SURICATTA;
	msg.data.instmsg.cmd = CMD_CONFIG;

	size = sizeof(msg.data.instmsg.buf);
	buf = msg.data.instmsg.buf;

	/*
	 * Build a json string with the command line parameters
	 * do not check anything, let SWUpdate
	 * doing the checks
	 * An error or a NACK is returned in
	 * case of failure
	 */

	snprintf(buf, size, "{ \"polling\" : \"%lu\"}", strtoul(argv[1], NULL, 10));

	fprintf(stdout, "Sending: '%s'", msg.data.instmsg.buf);

	rc = ipc_send_cmd(&msg);

	fprintf(stdout, " returned %d\n", rc);
	if (!rc)
		fprintf(stdout, "Server returns %s\n",
				(msg.type == ACK) ? "ACK" : "NACK");

	exit(0);
}
