/*
 * (C) Copyright 2016
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
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

int dict_insert_entry(struct dictlist *dictionary, char *key, char *value)
{
	struct dict_entry *entry = (struct dict_entry *)malloc(sizeof(*entry));

	if (!entry)
		return -ENOMEM;

	memset(entry, 0, sizeof(*entry));
	entry->varname = strdup(key);
	entry->value = strdup(value);

	LIST_INSERT_HEAD(dictionary, entry, next);

	return 0;
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

	return dict_insert_entry(dictionary, key, value);
}

void dict_remove_entry(struct dict_entry *entry)
{
	LIST_REMOVE(entry, next);
	free(entry->varname);
	free(entry->value);
	free(entry);
}

void dict_remove(struct dictlist *dictionary, char *key)
{

	struct dict_entry *entry = get_entry(dictionary, key);

	if (!entry)
		return;

	dict_remove_entry(entry);
}

void dict_drop_db(struct dictlist *dictionary)
{
	struct dict_entry *var;

	LIST_FOREACH(var, dictionary, next) {
		dict_remove_entry(var);
	}
}
