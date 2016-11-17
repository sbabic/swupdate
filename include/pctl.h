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

#ifndef _SWUPDATE_PCTL_H
#define _SWUPDATE_PCTL_H

extern int pid;

pthread_t start_thread(void *(* start_routine) (void *), void *arg);

typedef int (*swupdate_process)(char *cfgname, int argc, char **argv);

int spawn_process(uid_t userid, gid_t groupid, char *cfgname, int ac, char **av,
			swupdate_process start);

#endif
