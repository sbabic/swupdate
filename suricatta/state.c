/*
 * Author: Christian Storm
 * Copyright (C) 2016, Siemens AG
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <util.h>
#include <bootloader.h>
#include "suricatta/state.h"

/*
 * This check is to avoid to corrupt the environment
 * An empty key is accepted, but U-Boot reports a corrupted
 * environment/
 */
#define CHECK_STATE_VAR(v) do { \
	if (strnlen(v, BOOTLOADER_VAR_LENGTH) == 0) { \
		WARN("Update Status Storage Key " \
			"is empty, setting it to 'ustate'\n"); \
		v = (char *)"ustate"; \
	} \
} while(0)

bool is_state_valid(update_state_t state) {
	if ((state < STATE_OK) || (state > STATE_ERROR)) {
		ERROR("Unknown update state=%c", state);
		return false;
	}
	return true;
}

#ifndef CONFIG_SURICATTA_STATE_CHOICE_BOOTLOADER
/*
 * This is just if the state is not stored persistently, that is
 * it does not survive after a reboot.
 * Save it in a variable and use setter / getter to retrieve it
 */
static int suricatta_state = STATE_NOT_AVAILABLE;
server_op_res_t save_state(char *key, update_state_t value)
{
	(void)key;
	suricatta_state = value;
	return SERVER_OK;
}

server_op_res_t read_state(char *key, update_state_t *value)
{
	(void)key;

	*value =  suricatta_state;
	return SERVER_OK;
}

server_op_res_t reset_state(char *key)
{
	(void)key;
	suricatta_state = STATE_NOT_AVAILABLE;
	return SERVER_OK;
}
#else

server_op_res_t save_state(char *key, update_state_t value)
{
	int ret;
	char value_str[2] = {value, '\0'};

	CHECK_STATE_VAR(key);

	ret = bootloader_env_set(key, value_str);

	return ret == 0 ? SERVER_OK : SERVER_EERR;
}

server_op_res_t read_state(char *key, update_state_t *value)
{
	char *envval;
	CHECK_STATE_VAR(key);

	envval = bootloader_env_get(key);
	if (envval == NULL) {
		INFO("Key '%s' not found in Bootloader's environment.", key);
		*value = STATE_NOT_AVAILABLE;
		return SERVER_OK;
	}
	/* TODO It's a bit whacky just to cast this but as we're the only */
	/*      ones touching the variable, it's maybe OK for a PoC now. */
	*value = (update_state_t)*envval;

	/* bootloader get env allocates space for the value */
	free(envval);

	return SERVER_OK;
}
server_op_res_t reset_state(char *key)
{
	int ret;

	CHECK_STATE_VAR(key);
	ret = bootloader_env_unset(key);
	return ret == 0 ? SERVER_OK : SERVER_EERR;
}
#endif
