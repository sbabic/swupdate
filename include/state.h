/*
 * Author: Christian Storm
 * Copyright (C) 2016, Siemens AG
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
 */

#pragma once
#include <stdbool.h>

#ifdef CONFIG_SURICATTA_STATE_CHOICE_BOOTLOADER
#define EXPANDTOKL2(token) token
#define EXPANDTOK(token) EXPANDTOKL2(token)
#define STATE_KEY EXPANDTOK(CONFIG_SURICATTA_STATE_BOOTLOADER)
#else
#define STATE_KEY "none"
#endif

/* (Persistent) Update State Management Functions.
 *
 * Suricatta may persistently store the update status to communicate it to the
 * server instance after, e.g., a successful reboot into the new firmware. The
 * `{save,read,reset}_state()` functions are called by a server implementation
 * to persistently manage the update state via, e.g., U-Boot's environment.
 */

typedef enum {
	STATE_OK = '0',
	STATE_INSTALLED = '1',
	STATE_TESTING = '2',
	STATE_FAILED = '3',
	STATE_NOT_AVAILABLE = '4',
	STATE_ERROR = '5',
	STATE_WAIT = '6',
	STATE_LAST = STATE_WAIT
} update_state_t;

static inline bool is_valid_state(update_state_t state) {
	return (state >= STATE_OK && state <= STATE_LAST);
}

server_op_res_t save_state(char *key, update_state_t value);
server_op_res_t read_state(char *key, update_state_t *value);
server_op_res_t reset_state(char *key);
update_state_t get_state(void);
