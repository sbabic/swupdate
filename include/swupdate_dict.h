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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
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
