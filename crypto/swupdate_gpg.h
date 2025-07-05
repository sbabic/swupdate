/*
 * (C) Copyright 2016-2024
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#pragma once

#include <stdint.h>
#include "util.h"

struct gpg_digest {
	char *gpg_home_directory;
	bool verbose;
	char *gpgme_protocol;
};
