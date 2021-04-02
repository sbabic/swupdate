/*
 * Author: Christian Storm
 * Copyright (C) 2016, Siemens AG
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#pragma once

#include "channel_op_res.h"

/* Suricatta Main Interface.
 *
 * `start_suricatta()` is the main interface to suricatta's functionality.
 * It's implementation defines the main loop comprising polling for updates
 * and installing them. For interoperability with different server and channel
 * implementations, the valid result codes to be returned by the different
 * implementations are defined here.
 */

int start_suricatta(const char *cfgname, int argc, char *argv[]) __attribute__((noreturn));
void suricatta_print_help(void);
int suricatta_wait(int seconds);
