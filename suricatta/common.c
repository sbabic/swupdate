/*
 * (C) Copyright 2018
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */
#include <unistd.h>
#include <stdbool.h>
#include <stdlib.h>
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
	GET_FIELD_STRING_RESET(LIBCFG_PARSER, elem, "interface", tmp);
	if (strlen(tmp))
		SETSTRING(chan->iface, tmp);
}

server_op_res_t map_channel_retcode(channel_op_res_t response)
{
	switch (response) {
	case CHANNEL_ENONET:
	case CHANNEL_EAGAIN:
		return SERVER_EAGAIN;
	case CHANNEL_EACCES:
		return SERVER_EACCES;
	case CHANNEL_ENOENT:
	case CHANNEL_EIO:
	case CHANNEL_EILSEQ:
	case CHANNEL_ENOMEM:
	case CHANNEL_EINIT:
	case CHANNEL_ELOOP:
		return SERVER_EERR;
	case CHANNEL_EBADMSG:
	case CHANNEL_ENOTFOUND:
		return SERVER_EBADMSG;
	case CHANNEL_OK:
	case CHANNEL_EREDIRECT:
		return SERVER_OK;
	}
	return SERVER_EERR;
}

struct json_object *server_tokenize_msg(char *buf, size_t size)
{

	struct json_tokener *json_tokenizer = json_tokener_new();
	enum json_tokener_error json_res;
	struct json_object *json_root;
	do {
		json_root = json_tokener_parse_ex(
		    json_tokenizer, buf, size);
	} while ((json_res = json_tokener_get_error(json_tokenizer)) ==
		 json_tokener_continue);
	if (json_res != json_tokener_success) {
		ERROR("Error while parsing channel's returned JSON data: %s",
		      json_tokener_error_desc(json_res));
		json_tokener_free(json_tokenizer);
		return NULL;
	}

	json_tokener_free(json_tokenizer);

	return json_root;
}
