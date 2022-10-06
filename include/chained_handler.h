/*
 * (C) Copyright 2022
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#include "swupdate.h"
struct chain_handler_data {
	struct img_type img;
};

extern void *chain_handler_thread(void *data);
