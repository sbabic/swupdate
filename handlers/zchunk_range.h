/*
 * (C) Copyright 2021
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#pragma once

#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <zck.h>

/* Contains a single range */
typedef struct zck_range_item {
    size_t start;
    size_t end;
    struct zck_range_item *next;
    struct zck_range_item *prev;
} zck_range_item;

typedef struct zck_range {
    unsigned int count;
    zck_range_item *first;
} zck_range;

/* exported function */

/* Get a Range from a zck context */
zck_range *zchunk_get_missing_range(zckCtx *zck, zckChunk *chk, int max_ranges);

/* Return number of ranges */
int zchunk_get_range_count(zck_range *range);

/* Return string to be used in HTTP Bytes Range */
char *zchunk_get_range_char(zck_range *range);

/* Free range allocated by zchunk_get_missing_range */
void zchunk_range_free(zck_range **info);
