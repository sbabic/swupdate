/*
 * (C) Copyright 2015-2017
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * This file is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * You should have received a copy of the Less General Public License
 * along with this program; If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _SWUPDATE_STATUS_H
#define _SWUPDATE_STATUS_H

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
} RECOVERY_STATUS;

typedef enum {
	SOURCE_UNKNOWN,
	SOURCE_WEBSERVER,
	SOURCE_SURICATTA,
	SOURCE_DOWNLOADER,
	SOURCE_LOCAL
} sourcetype;

#endif
