/*
 * (C) Copyright 2018
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
 */

#pragma once
#include <stdbool.h>
#include <swupdate_dict.h>
#include <channel_curl.h>
#include <util.h>

void suricatta_channel_settings(void *elem, channel_data_t *chan);
