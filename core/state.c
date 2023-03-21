/*
 * Author: Christian Storm
 * Copyright (C) 2016, Siemens AG
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <util.h>
#include <bootloader.h>
#include <state.h>
#include <network_ipc.h>
#include <sys/types.h>
#include <unistd.h>
#include "pctl.h"

/*
 * This check is to avoid to corrupt the environment
 * An empty key is accepted, but U-Boot reports a corrupted
 * environment/
 */
#define CHECK_STATE_VAR(v) do { \
	if (v[0] == 0) { \
		WARN("Update Status Storage Key " \
			"is empty, setting it to 'ustate'"); \
		v = (char *)"ustate"; \
	} \
} while(0)

static int do_save_state(char *key, char* value)
{
	CHECK_STATE_VAR(key);
	if (!value)
		return -EINVAL;
	char c = *value;
	if (c < STATE_OK || c > STATE_LAST)
		return -EINVAL;

	return bootloader_env_set(key, value);
}

int save_state(update_state_t value)
{
	char value_str[2] = {value, '\0'};
	ipc_message msg;
	if (pid == getpid()) {
		memset(&msg, 0, sizeof(msg));
		msg.magic = IPC_MAGIC;
		msg.type = SET_UPDATE_STATE;
		msg.data.msg[0] = (char)value;
		return !(ipc_send_cmd(&msg) == 0 && msg.type == ACK);
	} else {
		/* Main process */
		return do_save_state((char *)STATE_KEY, value_str);
	}
}

static update_state_t read_state(char *key)
{
	CHECK_STATE_VAR(key);

	char *envval = bootloader_env_get(key);
	if (envval == NULL) {
		INFO("Key '%s' not found in Bootloader's environment.", key);
		return STATE_NOT_AVAILABLE;
	}
	/* TODO It's a bit whacky just to cast this but as we're the only */
	/*      ones touching the variable, it's maybe OK for a PoC now. */

	update_state_t val = (update_state_t)*envval;
	/* bootloader get env allocates space for the value */
	free(envval);

	return val;
}

static update_state_t do_get_state(void) {
	update_state_t state = read_state((char *)STATE_KEY);

	if (state == STATE_NOT_AVAILABLE) {
		ERROR("Cannot read stored update state.");
		return STATE_NOT_AVAILABLE;
	}

	if (is_valid_state(state)) {
		TRACE("Read state=%c from persistent storage.", state);
		return state;
	}

	ERROR("Unknown update state=%c", state);
	return STATE_NOT_AVAILABLE;
}

update_state_t get_state(void) {
	if (pid == getpid())
	{
		ipc_message msg;
		memset(&msg, 0, sizeof(msg));

		msg.type = GET_UPDATE_STATE;

		if (ipc_send_cmd(&msg) || msg.type == NACK) {
			ERROR("Failed to get current bootloader update state.");
			return STATE_NOT_AVAILABLE;
		}

		return (update_state_t)msg.data.msg[0];
	} else {
		// Main process
		return do_get_state();
	}
}
