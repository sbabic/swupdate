/*
 * Author: Christian Storm
 * Copyright (C) 2016, Siemens AG
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc.
 */

#pragma once
#include <swupdate_status.h>
#include <util.h>

/* Disable swupdate core's logging output to not clutter test output */
int loglevel = OFF;

/* mock notify() to spare linking against swupdate core's notifier.c */
void notify(RECOVERY_STATUS status, int level, const char *msg)
{
	(void)status;
	(void)level;
	(void)msg;
	return;
}
