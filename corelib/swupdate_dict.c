/*
 * (C) Copyright 2016
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * Copyright (C) 2018 Weidmüller Interface GmbH & Co. KG
 * Stefan Herbrechtsmeier <stefan.herbrechtsmeier@weidmueller.com>
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

static int insert_list_elem(struct dict_list *list, const char *value)
{
	struct dict_list_elem *elem = (struct dict_list_elem *)malloc(sizeof(*elem));

	if (!elem)
		return -ENOMEM;

	memset(elem, 0, sizeof(*elem));
	elem->value = strdup(value);

	LIST_INSERT_HEAD(list, elem, next);

	return 0;
}

static void remove_list_elem(struct dict_list_elem *elem)
{
	LIST_REMOVE(elem, next);
	free(elem->value);
	free(elem);
}

static void remove_list(struct dict_list *list)
{
	struct dict_list_elem *elem;

	LIST_FOREACH(elem, list, next) {
		remove_list_elem(elem);
	}
}

static struct dict_entry *insert_entry(struct dict *dictionary, const char *key)
{
	struct dict_entry *entry = (struct dict_entry *)malloc(sizeof(*entry));
	if (!entry)
		return NULL;

	memset(entry, 0, sizeof(*entry));
	entry->key = strdup(key);

	LIST_INSERT_HEAD(dictionary, entry, next);

	return entry;
}

static struct dict_entry *get_entry(struct dict *dictionary, const char *key)
{
	struct dict_entry *entry;

	LIST_FOREACH(entry, dictionary, next) {
		if (strcmp(key, entry->key) == 0)
			return entry;
	}

	return NULL;
}

static void remove_entry(struct dict_entry *entry)
{
	LIST_REMOVE(entry, next);
	free(entry->key);
	remove_list(&entry->list);
	free(entry);
}

char *dict_entry_get_key(struct dict_entry *entry)
{
	if (!entry)
		return NULL;

	return entry->key;
}

char *dict_entry_get_value(struct dict_entry *entry)
{
	if (!entry || !LIST_FIRST(&entry->list))
		return NULL;

	return LIST_FIRST(&entry->list)->value;
}

struct dict_list *dict_get_list(struct dict *dictionary, const char *key)
{
	struct dict_entry *entry = get_entry(dictionary, key);

	if (!entry)
		return NULL;

	return &entry->list;
}

char *dict_get_value(struct dict *dictionary, const char *key)
{
	struct dict_entry *entry = get_entry(dictionary, key);

	if (!entry)
		return NULL;

	return dict_entry_get_value(entry);
}

int dict_insert_value(struct dict *dictionary, const char *key, const char *value)
{
	struct dict_entry *entry = get_entry(dictionary, key);

	if (!entry) {
		entry = insert_entry(dictionary, key);
		if (!entry)
			return -ENOMEM;
	}

	return insert_list_elem(&entry->list, value);
}

int dict_set_value(struct dict *dictionary, const char *key, const char *value)
{
	struct dict_entry *entry = get_entry(dictionary, key);

	if (entry)
		remove_entry(entry);

	entry = insert_entry(dictionary, key);
	if (!entry)
		return -ENOMEM;

	return insert_list_elem(&entry->list, value);
}

void dict_remove(struct dict *dictionary, const char *key)
{
	struct dict_entry *entry = get_entry(dictionary, key);

	if (!entry)
		return;

	remove_entry(entry);
}

void dict_drop_db(struct dict *dictionary)
{
	struct dict_entry *entry;

	LIST_FOREACH(entry, dictionary, next) {
		remove_entry(entry);
	}
}
