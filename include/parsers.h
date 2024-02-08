/*
 * (C) Copyright 2013-2023
 * Stefano Babic <stefano.babic@swupdate.org>
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#pragma once

#include "generated/autoconf.h"

#ifndef CONFIG_SETSWDESCRIPTION
#define SW_DESCRIPTION_FILENAME	"sw-description"
#else
#define SW_DESCRIPTION_FILENAME	CONFIG_SWDESCRIPTION
#endif

typedef int (*parser_fn)(struct swupdate_cfg *swcfg, const char *filename, char **error);

int parse(struct swupdate_cfg *swcfg, const char *filename);
int parse_cfg(struct swupdate_cfg *swcfg, const char *filename, char **error);
int parse_json(struct swupdate_cfg *swcfg, const char *filename, char **error);
int parse_external(struct swupdate_cfg *swcfg, const char *filename, char **error);
