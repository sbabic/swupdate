/*
 * (C) Copyright 2017
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
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
#include <getopt.h>
#include "network_ipc.h"

static struct option long_options[] = {
	{"help", no_argument, NULL, 'h'},
	{"polling-time", required_argument, NULL, 'p'},
	{"enable", no_argument, NULL, 'e'},
	{"disable", no_argument, NULL, 'd'},
	{NULL, 0, NULL, 0}
};

static void usage(char *programname)
{
	fprintf(stdout, "%s (compiled %s)\n", programname, __DATE__);
	fprintf(stdout, "Usage %s [OPTION]\n",
			programname);
	fprintf(stdout,
		" -p, --polling-time      : Set polling time (0=from server) to ask the backend server\n"
		" -e, --enable            : Enable polling of backend server\n"
		" -d, --disable           : Disable polling of backend server\n"
		" -h, --help              : print this help and exit\n"
		);
}

static void send_msg(ipc_message *msg)
{
	int rc;

	fprintf(stdout, "Sending: '%s'", msg->data.procmsg.buf);
	rc = ipc_send_cmd(msg);

	fprintf(stdout, " returned %d\n", rc);
	if (rc == 0) {
		fprintf(stdout, "Server returns %s\n",
				(msg->type == ACK) ? "ACK" : "NACK");
		if (msg->data.procmsg.len > 0) {
			fprintf(stdout, "Returned message: %s\n",
					msg->data.procmsg.buf);
		}
	}
}

/*
 * Simple example, it does nothing but calling the library
 */
int main(int argc, char *argv[]) {
	ipc_message msg;
	size_t size;
	char *buf;
	int c;
	unsigned long polling_time;
	bool enable = false;
	int opt_e = 0;
	int opt_p = 0;

	if (argc < 2) {
		usage(argv[0]);
		exit(1);
	}

	memset(&msg, 0, sizeof(msg));
	msg.data.procmsg.source = SOURCE_SURICATTA;
	msg.type = SWUPDATE_SUBPROCESS;

	size = sizeof(msg.data.procmsg.buf);
	buf = msg.data.procmsg.buf;

	/* Process options with getopt */
	while ((c = getopt_long(argc, argv, "p:edh",
				long_options, NULL)) != EOF) {
		switch (c) {
		case 'p':
			opt_p = 1;
			msg.data.procmsg.cmd = CMD_CONFIG;
			polling_time = strtoul(optarg, NULL, 10);
			break;
		case 'e':
		case 'd':
			msg.data.procmsg.cmd = CMD_ENABLE;
			opt_e = 1;
			enable = (c == 'e');
			break;
		case 'h':
			usage(argv[0]);
			exit(0);
			break;
		default:
			usage(argv[0]);
			exit(1);
			break;
		}
	}

	/*
	 * Build a json string with the command line parameters
	 * do not check anything, let SWUpdate
	 * doing the checks
	 * An error or a NACK is returned in
	 * case of failure
	 */
	if (opt_p) {
		snprintf(buf, size, "{ \"polling\" : \"%lu\"}", polling_time);
		msg.data.procmsg.len = strnlen(buf, size);
		send_msg(&msg);
	}
	if (opt_e) {
		snprintf(buf, size, "{ \"enable\" : %s}", enable ? "true" : "false");
		msg.data.procmsg.len = strnlen(buf, size);
		send_msg(&msg);
	}

	exit(0);
}
