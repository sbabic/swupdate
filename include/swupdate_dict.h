/*
 * (C) Copyright 2016
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#ifndef _SWDICT_H
#define _SWDICT_H

#include <bsdqueue.h>

struct dict_list_elem {
	char *value;
	LIST_ENTRY(dict_list_elem) next;
};

LIST_HEAD(dict_list, dict_list_elem);

struct dict_entry {
	char *key;
	struct dict_list list;
	LIST_ENTRY(dict_entry) next;
};

LIST_HEAD(dict, dict_entry);

char *dict_entry_get_key(struct dict_entry *entry);
char *dict_entry_get_value(struct dict_entry *entry);

struct dict_list *dict_get_list(struct dict *dictionary, const char *key);
char *dict_get_value(struct dict *dictionary, const char *key);
int dict_set_value(struct dict *dictionary, const char *key, const char *value);
int dict_insert_value(struct dict *dictionary, const char *key, const char *value);
void dict_remove(struct dict *dictionary, const char *key);
void dict_drop_db(struct dict *dictionary);
int dict_parse_script(struct dict *dictionary, const char *script);

#endif
