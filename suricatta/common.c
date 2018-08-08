/*
 * (C) Copyright 2018
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
 */
#include <stdbool.h>
#include <swupdate_dict.h>
#include <channel.h>
#include <util.h>
#include <parselib.h>
#include <swupdate_settings.h>
#include <channel_curl.h>
#include "suricatta/suricatta.h"
#include "suricatta_private.h"

void suricatta_channel_settings(void *elem, channel_data_t *chan)
{
	char tmp[128];

	get_field(LIBCFG_PARSER, elem, "retry",
		&chan->retries);

	GET_FIELD_STRING_RESET(LIBCFG_PARSER, elem, "retrywait", tmp);
	if (strlen(tmp))
		chan->retry_sleep =
			(unsigned int)strtoul(tmp, NULL, 10);
	GET_FIELD_STRING_RESET(LIBCFG_PARSER, elem, "cafile", tmp);
	if (strlen(tmp))
		SETSTRING(chan->cafile, tmp);
	GET_FIELD_STRING_RESET(LIBCFG_PARSER, elem, "sslkey", tmp);
	if (strlen(tmp))
		SETSTRING(chan->sslkey, tmp);
	GET_FIELD_STRING_RESET(LIBCFG_PARSER, elem, "ciphers", tmp);
	if (strlen(tmp))
		SETSTRING(chan->ciphers, tmp);
	GET_FIELD_STRING_RESET(LIBCFG_PARSER, elem, "sslcert", tmp);
	if (strlen(tmp))
		SETSTRING(chan->sslcert, tmp);
	GET_FIELD_STRING_RESET(LIBCFG_PARSER, elem, "proxy", tmp);
	if (strlen(tmp))
		SETSTRING(chan->proxy, tmp);
}
