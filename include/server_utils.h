/*
 * (C) Copyright 2018
 * Stefano Babic <stefano.babic@swupdate.org>
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#pragma once
#include <stdbool.h>
#include <swupdate_dict.h>
#include <channel_curl.h>
#include "channel.h"
#include <util.h>

struct json_object;

int channel_settings(void *elem, void *data);
server_op_res_t map_channel_retcode(channel_op_res_t response);
struct json_object *server_tokenize_msg(char *buf, size_t size);
