/*
 * Copyright (C) 2021 Weidmueller Interface GmbH & Co. KG
 * Roland Gaudig <roland.gaudig@weidmueller.com>
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

/*
 * This is a small example how to retrieve the hawkBit server status
 * from surricata.
 */

#include <stdio.h>

#if defined(CONFIG_JSON)
#include <json-c/json.h>

#include <network_ipc.h>

int main(int __attribute__ ((__unused__)) argc,
	 char __attribute__ ((__unused__)) *argv[])
{
	ipc_message msg;
	struct json_object *parsed_json;
	struct json_object *server;
	struct json_object *status;
	struct json_object *time;

	msg.type = SWUPDATE_SUBPROCESS;
	msg.data.procmsg.source = SOURCE_SURICATTA;
	msg.data.procmsg.cmd = CMD_GET_STATUS;

	msg.data.procmsg.buf[0] = '\0';
	msg.data.procmsg.len = 0;
	msg.data.procmsg.timeout = 10; /* Wait 10 s for Suricatta response */

	int rc = ipc_send_cmd(&msg);

	if (rc) {
		fprintf(stderr, "Error: ipc_send_cmd failed\n");
		exit(1);
	}

	if (msg.type == ACK) {
		parsed_json = json_tokener_parse(msg.data.procmsg.buf);
		json_object_object_get_ex(parsed_json, "server", &server);
		json_object_object_get_ex(server, "status", &status);
		json_object_object_get_ex(server, "time", &time);

		printf("status: %d, time: %s\n",
		       json_object_get_int(status),
		       json_object_get_string(time));
		exit(0);
	} else {
		printf("Error: suricatta did respond with NACK.\n");
		exit(1);
	}
}
#else
#include <stdlib.h>

#warning "swupdate-gethawkbitstatus needs json-c, replaced with dummy"

int main(int __attribute__((__unused__)) argc,
	 char __attribute__((__unused__)) **argv)
{
	fprintf(stderr, "json-c not available, exiting..\n");
	exit(1);
}
#endif
