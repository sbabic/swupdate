/*
 * (C) Copyright 2015
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
} RECOVERY_STATUS;

typedef enum {
	SOURCE_UNKNOWN,
	SOURCE_WEBSERVER,
	SOURCE_SURICATTA,
	SOURCE_DOWNLOADER,
	SOURCE_LOCAL
} sourcetype;

#endif
