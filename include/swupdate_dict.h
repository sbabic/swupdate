/*
 * (C) Copyright 2016
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
 */

#ifndef _SWDICT_H
#define _SWDICT_H

#include <bsdqueue.h>

struct dict_entry {
	char *key;
	char *value;
	LIST_ENTRY(dict_entry) next;
};

LIST_HEAD(dict, dict_entry);

char *dict_get_value(struct dict *dictionary, char *key);
int dict_set_value(struct dict *dictionary, char *key, char *value);
int dict_insert_entry(struct dict *dictionary, char *key, char *value);
void dict_remove(struct dict *dictionary, char *key);
void dict_remove_entry(struct dict_entry *entry);
void dict_drop_db(struct dict *dictionary);

#endif
