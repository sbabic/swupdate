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
	 * Foundation, Inc.
	 */


#ifndef _DWL_INTERFACE_H
#define _DWL_INTERFACE_H

/*
 * This is used by swupdate to start the Downloader Process
 */
int start_download(const char *cfgfname, int argc, char *argv[]);

void download_print_help(void);

#endif
