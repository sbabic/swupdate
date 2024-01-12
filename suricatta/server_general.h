/*
 * (C) Copyright 2018
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#pragma once
#include <stdbool.h>
#include <state.h>
#include <swupdate_dict.h>
#include <channel.h>

/*
 * General Server Implementation Private Header File.
 *
 */

typedef struct {
	char *url;
	char *logurl;
	unsigned int polling_interval;
	bool debug;
	char *cached_file;
	struct dict configdata;
	struct dict received_httpheaders;
	struct dict httpheaders_to_send;
	update_state_t update_state;
	channel_t *channel;
} server_general_t;

