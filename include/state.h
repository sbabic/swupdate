/*
 * Author: Christian Storm
 * Copyright (C) 2016, Siemens AG
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#pragma once
#include "util.h"
#include <stdbool.h>

#ifdef CONFIG_UPDATE_STATE_CHOICE_BOOTLOADER
#define EXPANDTOKL2(token) token
#define EXPANDTOK(token) EXPANDTOKL2(token)
#define STATE_KEY EXPANDTOK(CONFIG_UPDATE_STATE_BOOTLOADER)
#else
#define STATE_KEY "none"
#endif

/* (Persistent) Update State Management Functions.
 *
 * The SWUpdate core or a module such as suricatta may want to persistently
 * store the update status to communicate it to the server instance after,
 * e.g., a successful reboot into the new firmware.
 * The `{save,get}_state()` functions are called to manage the update status
 * via, e.g., U-Boot's environment.
 *
 */

typedef enum {
	STATE_OK = '0',
	STATE_INSTALLED = '1',
	STATE_TESTING = '2',
	STATE_FAILED = '3',
	STATE_NOT_AVAILABLE = '4',
	STATE_ERROR = '5',
	STATE_WAIT = '6',
	STATE_IN_PROGRESS = '7',
	STATE_LAST = STATE_IN_PROGRESS
} update_state_t;

static inline bool is_valid_state(update_state_t state) {
	return (state >= STATE_OK && state <= STATE_LAST);
}

static inline char* get_state_string(update_state_t state) {
	switch (state) {
		case STATE_IN_PROGRESS: return (char*)"in_progress";
		case STATE_FAILED: return (char*)"failed";
		case STATE_OK: return (char*)"ok";
		case STATE_INSTALLED: return (char*)"installed";
		case STATE_TESTING: return (char*)"testing";
		case STATE_NOT_AVAILABLE: return (char*)"not_available";
		case STATE_ERROR: return (char*)"error";
		case STATE_WAIT: return (char*)"wait";
		default: break;
	}
	return (char*)"<nil>";
}

int save_state(update_state_t value);
update_state_t get_state(void);
