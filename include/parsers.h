/*
 * (C) Copyright 2008-2013
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#ifndef _RECOVERY_PARSERS_H
#define _RECOVERY_PARSERS_H

#include "generated/autoconf.h"

#ifndef CONFIG_SETSWDESCRIPTION
#define SW_DESCRIPTION_FILENAME	"sw-description"
#else
#define SW_DESCRIPTION_FILENAME	CONFIG_SWDESCRIPTION
#endif

typedef int (*parser_fn)(struct swupdate_cfg *swcfg, const char *filename);

int parse(struct swupdate_cfg *swcfg, const char *filename);
int parse_cfg (struct swupdate_cfg *swcfg, const char *filename);
int parse_json(struct swupdate_cfg *swcfg, const char *filename);
int parse_external(struct swupdate_cfg *swcfg, const char *filename);
#endif

