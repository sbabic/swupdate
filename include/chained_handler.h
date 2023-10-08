/*
 * (C) Copyright 2022
 * Stefano Babic <stefano.babic@swupdate.org>
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#pragma once

#include "swupdate_image.h"
struct chain_handler_data {
	struct img_type img;
};

extern void *chain_handler_thread(void *data);
