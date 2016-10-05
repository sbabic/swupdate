/*
 * (C) Copyright 2016
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <assert.h>
#include "generated/autoconf.h"
#include "bsdqueue.h"
#include "util.h"
#include "swupdate_dict.h"

static struct dict_entry *get_entry(struct dictlist *dictionary, char *key)
{
	struct dict_entry *entry;

	LIST_FOREACH(entry, dictionary, next) {
		if (strcmp(key, entry->varname) == 0)
			return entry;
	}

	return NULL;
}

char *dict_get_value(struct dictlist *dictionary, char *key)
{
	struct dict_entry *entry = get_entry(dictionary, key);

	if (!entry)
		return NULL;

	return entry->value;
}

int dict_set_value(struct dictlist *dictionary, char *key, char *value)
{
	struct dict_entry *entry = get_entry(dictionary, key);

	/*
	 * Set to new value if key is already in
	 * dictionary
	 */
	if (entry) {
		LIST_REMOVE(entry, next);
		free(entry);
	}

	entry = (struct dict_entry *)malloc(sizeof(*entry) + strlen(key) + strlen(value) + 2);

	if (!entry)
		return -ENOMEM;

	entry->varname = (char *)entry + sizeof(*entry);
	entry->value = entry->varname + strlen(key) + 1;
	printf("Entry: %p %p %p\n", entry, entry->varname, entry->value);
	strncpy(entry->varname, key, strlen(key));
	strncpy(entry->value, value, strlen(value));

	LIST_INSERT_HEAD(dictionary, entry, next);

	return 0;
}

void dict_remove(struct dictlist *dictionary, char *key)
{

	struct dict_entry *entry = get_entry(dictionary, key);

	if (!entry)
		return;
	LIST_REMOVE(entry, next);
	free(entry);
}

