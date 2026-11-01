/*
 * (C) Copyright 2023-2026
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */
#pragma once
#include <stdbool.h>
#include <state.h>
#include <channel.h>
#include <util.h>

/*
 * Suricatta server for the OTA updates of ThingsBoard
 * (https://thingsboard.io), through its HTTP device API.
 */

#define TB_STRING_SIZE	SWUPDATE_GENERAL_STRING_SIZE

typedef struct {
	char *url;
	char *token;
	unsigned int polling_interval;
	char *cached_file;
	/* "fw" or "sw": firmware or software packages */
	const char *prefix;
	/* state given with --confirm, else STATE_NOT_AVAILABLE */
	update_state_t update_state;
	/* the state after the last update must still be reported */
	bool initial_report;
	/* the package running on the device was reported */
	bool announced;
	channel_t *channel;
	/* package running on the device */
	char title[TB_STRING_SIZE];
	char version[TB_STRING_SIZE];
	/* package assigned on the server */
	char target_title[TB_STRING_SIZE];
	char target_version[TB_STRING_SIZE];
	char *target_url;
	/* package that failed: not tried again until the assignment changes */
	char failed_title[TB_STRING_SIZE];
	char failed_version[TB_STRING_SIZE];
} server_thingsboard_t;
