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


#ifndef _INSTALL_PROGRESS_H
#define _INSTALL_PROGRESS_H

#include <swupdate_status.h>

#define SOCKET_PROGRESS_PATH "/tmp/swupdateprog"

/*
 * Message sent via socket
 * Data are sent in LE when required
 */
struct progress_msg {
	unsigned int	magic;		/* Magic Number */
	RECOVERY_STATUS	status;		/* Update Status (Running, Failure) */
	unsigned int	dwl_percent;	/* % downloaded data */
	unsigned int	nsteps;		/* No. total of steps */
	unsigned int	cur_step;	/* Current step index */
	unsigned int	cur_percent;	/* % in current step */
	char		cur_image[256];	/* Name of image to be installed */
	char		hnd_name[64];	/* Name of running hanler */
	sourcetype	source;		/* Interface that triggered the update */
	unsigned int 	infolen;    	/* Len of data valid in info */
	char		info[2048];   	/* additional information about install */
};

void swupdate_progress_init(unsigned int nsteps);
void swupdate_progress_update(unsigned int perc);
void swupdate_progress_inc_step(char *image);
void swupdate_progress_step_completed(void);
void swupdate_progress_end(RECOVERY_STATUS status);
void swupdate_progress_done(void);

void *progress_bar_thread (void *data);

#endif
