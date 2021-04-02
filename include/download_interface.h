/*
 * (C) Copyright 2016
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */


#ifndef _DWL_INTERFACE_H
#define _DWL_INTERFACE_H

/*
 * This is used by swupdate to start the Downloader Process
 */
int start_download(const char *cfgfname, int argc, char *argv[]);

void download_print_help(void);

#endif
