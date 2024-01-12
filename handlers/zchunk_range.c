/*
 * (C) Copyright 2021
 * Stefano Babic, stefano.babic@swupdate.org.
 * Copyright 2018 Jonathan Dieter <jdieter@gmail.com>
 *
 * SPDX-License-Identifier:	 GPL-2.0-only
 */

/*
 * This code is mostly taken from "range.c" from the zchunk project.
 * See Copyright above. Changes here are to get more internals from Range,
 * that are not exported by zck library.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <zck.h>
#include "zchunk_range.h"
#include "util.h"

#define BUF_SIZE 32768
static zck_range_item *range_insert_new(zck_range_item *prev,
				        zck_range_item *next, uint64_t start,
					uint64_t end) {
	zck_range_item *new = calloc(1, sizeof(zck_range_item));
	if (!new) {
		ERROR("OOM in %s", __func__);
		return NULL;
	}
	new->start = start;
	new->end = end;
	if(prev) {
		new->prev = prev;
		prev->next = new;
	}
	if(next) {
		new->next = next;
		next->prev = new;
	}
	return new;
}

static zck_range_item *range_remove(zck_range_item *range) {
	zck_range_item *next = range->next;
	if(range->next)
		range->next->prev = range->prev;
	free(range);
	return next;
}

static void range_merge_combined(zck_range *info) {
	if(!info) {
		ERROR("zck_range not allocated");
		return;
	}
	for(zck_range_item *ptr=info->first; ptr;) {
		if(ptr->next && ptr->end >= ptr->next->start-1) {
			if(ptr->end < ptr->next->end)
				ptr->end = ptr->next->end;
			ptr->next = range_remove(ptr->next);
			info->count -= 1;
		} else {
			ptr = ptr->next;
		}
	}
}

static bool range_add(zck_range *info, zckChunk *chk) {
	if(info == NULL || chk == NULL) {
		ERROR("zck_range or zckChunk not allocated");
		return false;
	}

	size_t start = zck_get_chunk_start(chk);
	size_t end = zck_get_chunk_start(chk) + zck_get_chunk_comp_size(chk) - 1;
	zck_range_item *prev = info->first;
	for(zck_range_item *ptr=info->first; ptr;) {
		prev = ptr;
		if(start > ptr->start) {
			ptr = ptr->next;
			continue;
		} else if(start < ptr->start) {
			if(range_insert_new(ptr->prev, ptr, start, end) == NULL)
				return false;
			if(info->first == ptr) {
				info->first = ptr->prev;
			}
			info->count += 1;
			range_merge_combined(info);
			return true;
		} else { // start == ptr->start
			if(end > ptr->end)
				ptr->end = end;
			info->count += 1;
			range_merge_combined(info);
			return true;
		}
	}
	/* We've only reached here if we should be last item */
	zck_range_item *new = range_insert_new(prev, NULL, start, end);
	if(new == NULL)
		return false;
	if(info->first == NULL)
		info->first = new;
	info->count += 1;
	range_merge_combined(info);
	return true;
}

void zchunk_range_free(zck_range **info) {
	zck_range_item *next = (*info)->first;
	while(next) {
		zck_range_item *tmp = next;
		next = next->next;
		free(tmp);
	}
	free(*info);
	*info = NULL;
}

char *zchunk_get_range_char(zck_range *range) {
	int buf_size = BUF_SIZE;
	char *output = calloc(1, buf_size);
	if (!output) {
	   ERROR ("OOM in %s", __func__);
	   return NULL;
	}
	int loc = 0;
	int count = 0;
	zck_range_item *ri = range->first;
	while(ri) {
		int length = snprintf(output+loc, buf_size-loc, "%lu-%lu,",
				      (long unsigned)ri->start,
				      (long unsigned)ri->end);
		if(length < 0) {
			ERROR("Unable to get range: %s", strerror(errno));
			free(output);
			return NULL;
		}
		if(length > buf_size-loc) {
			buf_size = (int)(buf_size * 1.5);
			output = saferealloc(output, buf_size);
			if (!output) {
				ERROR ("OOM in %s", __func__);
				return output;
			}
			continue;
		}
		loc += length;
		count++;
		ri = ri->next;
	}
	output[loc-1]='\0'; // Remove final comma
	output = saferealloc(output, loc);
	return output;
}

zck_range *zchunk_get_missing_range(zckCtx *zck, zckChunk *first, int max_ranges) {
	if (!zck)
		return NULL;
	zck_range *range = calloc(1, sizeof(zck_range));
	if (!range) {
		ERROR ("OOM in %s", __func__);
		return NULL;
	}

	for(zckChunk *chk = first ? first : zck_get_first_chunk(zck); chk; chk = zck_get_next_chunk(chk)) {
		if (zck_get_chunk_valid(chk))
			continue;
		if(!range_add(range, chk)) {
			zchunk_range_free(&range);
			return NULL;
		}
		if(max_ranges >= 0 && range->count >= max_ranges)
			break;
	}
	return range;
}

int zchunk_get_range_count(zck_range *range) {
	return range->count;
}
