/*
 * Author: Christian Storm
 * Copyright (C) 2016, Siemens AG
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc.
 */

#pragma once
#include <stdbool.h>
#include <suricatta/channel.h>

/* Have a queue for errors reported by installer */
#define HAWKBIT_MAX_REPORTED_ERRORS	10

/* hawkBit Server Implementation Private Header File.
 *
 * This is a "private" header for testability, i.e., the declarations and
 * definitions herein should be used by `server_hawkbit.c` and unit tests
 * only.
 */

typedef struct {
	char *url;
	char *device_id;
	char *tenant;
	unsigned int polling_interval;
	bool debug;
	struct dictlist configdata;
	bool has_to_send_configData;
	char *configData_url;
	update_state_t update_state;
	channel_t *channel;
	bool	cancelDuringUpdate;
	char *errors[HAWKBIT_MAX_REPORTED_ERRORS];
	int errorcnt;
} server_hawkbit_t;

extern server_hawkbit_t server_hawkbit;

static const struct {
	const char *closed;
	const char *proceeding;
	const char *canceled;
	const char *scheduled;
	const char *rejected;
	const char *resumed;
} reply_status_execution = {.closed = "closed",
			    .proceeding = "proceeding",
			    .canceled = "canceled",
			    .scheduled = "scheduled",
			    .rejected = "rejected",
			    .resumed = "resumed"};

static const struct {
	const char *success;
	const char *failure;
	const char *none;
} reply_status_result_finished = {
    .success = "success", .failure = "failure", .none = "none"};

static const struct {
	const char *skip;
	const char *attempt;
	const char *forced;
} deployment_update_action = {
    .skip = "skip", .attempt = "attempt", .forced = "forced"};
