/*
 * (C) Copyright 2015-2017
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     LGPL-2.1-or-later
 */

#ifndef _SWUPDATE_STATUS_H
#define _SWUPDATE_STATUS_H

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

#endif
