/*
 * (C) Copyright 2015-2023
 * Stefano Babic <stefano.babic@swupdate.org>
 *
 * SPDX-License-Identifier:     LGPL-2.1-or-later
 */

#pragma once
#ifdef __cplusplus
extern "C" {
#endif

/*
 * This is used to send back the result of an update.
 * It is strictly forbidden to change the order of entries.
 * New values should be put at the end without altering the order.
 */

typedef enum {
	IDLE,
	START,
	RUN,
	SUCCESS,
	FAILURE,
	DOWNLOAD,
	DONE,
	SUBPROCESS,
	PROGRESS,
} RECOVERY_STATUS;

typedef enum {
	SOURCE_UNKNOWN,
	SOURCE_WEBSERVER,
	SOURCE_SURICATTA,
	SOURCE_DOWNLOADER,
	SOURCE_LOCAL,
	SOURCE_CHUNKS_DOWNLOADER
} sourcetype;

#ifdef __cplusplus
}   // extern "C"
#endif
