/*
 * (C) Copyright 2021
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#pragma once

#include <sys/types.h>
#include <stdint.h>

#define RANGE_PAYLOAD_SIZE (32 * 1024)
typedef enum {
	RANGE_GET,
	RANGE_HEADERS,
	RANGE_DATA,
	RANGE_COMPLETED,
	RANGE_ERROR
} request_type;

typedef struct {
	uint32_t id;
	request_type type;
	size_t urllen;
	size_t rangelen;
	uint32_t crc;
	char data[RANGE_PAYLOAD_SIZE]; /* URL + RANGE */
} range_request_t;

typedef struct {
	uint32_t id;
	request_type type;
	size_t len;
	uint32_t crc;
	char data[RANGE_PAYLOAD_SIZE]; /* Payload */
} range_answer_t;
