/*
 * Author: Christian Storm
 * Copyright (C) 2016, Siemens AG
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#pragma once
#include <stdbool.h>
#include <state.h>
#include <swupdate_dict.h>
#include <channel.h>

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
	bool polling_interval_from_server;
	bool debug;
	struct dict configdata;
	bool has_to_send_configData;
	char *configData_url;
	char *cancel_url;
	update_state_t update_state;
	int stop_id;
	channel_t *channel;
	bool	cancelDuringUpdate;
	char *errors[HAWKBIT_MAX_REPORTED_ERRORS];
	int errorcnt;
	const char *update_action;
	char *targettoken;
	char *gatewaytoken;
	char *cached_file;
	bool usetokentodwl;
	unsigned int initial_report_resend_period;
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
