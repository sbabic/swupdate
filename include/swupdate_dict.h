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
	char *varname;
	char *value;
	LIST_ENTRY(dict_entry) next;
};

LIST_HEAD(dictlist, dict_entry);

char *dict_get_value(struct dictlist *dictionary, char *key);
int dict_set_value(struct dictlist *dictionary, char *key, char *value);
int dict_insert_entry(struct dictlist *dictionary, char *key, char *value);
void dict_remove(struct dictlist *dictionary, char *key);
void dict_remove_entry(struct dict_entry *entry);
void dict_drop_db(struct dictlist *dictionary);

#endif
