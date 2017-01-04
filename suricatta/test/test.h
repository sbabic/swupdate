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

typedef int (*settings_callback)(void *elem, void *data);
typedef enum { LIBCFG_PARSER, JSON_PARSER } parsertype;

int read_module_settings(char *filename, const char *module,
			 settings_callback fcn, void *data)
{
	(void)filename;
	(void)module;
	(void)fcn;
	(void)data;
	return 0;
}

void get_field_string(parsertype p, void *e, const char *path, char *dest,
		      size_t n)
{
	(void)p;
	(void)e;
	(void)path;
	(void)dest;
	(void)n;
	return;
}

int get_array_length(parsertype p, void *root)
{
	(void)p;
	(void)root;
	return 0;
}

void *get_elem_from_idx(parsertype p, void *node, int idx)
{
	(void)p;
	(void)node;
	(void)idx;
	return NULL;
}

int exist_field_string(parsertype p, void *e, const char *path)
{
	(void)p;
	(void)e;
	(void)path;
	return 0;
}

void get_field(parsertype p, void *e, const char *path, void *dest)
{
	(void)p;
	(void)e;
	(void)path;
	(void)dest;
}

pthread_t start_thread(void *(* start_routine) (void *), void *arg)
{
	(void)start_routine;
	(void)arg;
	return 0;
}

int listener_create(const char *path, int type) {
	(void)path;
	(void)type;
	return 99;
}

int get_install_info(sourcetype *source, char *buf, int len) {
	(void)source;
	(void)buf;
	(void)len;
	return 0;
}
