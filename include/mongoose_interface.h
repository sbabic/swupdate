/*
 * (C) Copyright 2012-2014
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
 * Foundation, Inc.
 */


#ifndef _MONGOOSE_INTERFACE_H
#define _MONGOOSE_INTERFACE_H

/*
 * Max number of command line options
 * to be passed to the mongoose webserver
 */
#define MAX_OPTIONS 40
/*
 * This is used by swupdate to start the Webserver
 */
int start_mongoose(const char *cfgfname, int argc, char *argv[]);

void mongoose_print_help(void);

#endif
