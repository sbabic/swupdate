/*
 * (C) Copyright 2016
 * Stefano Babic <stefano.babic@swupdate.org>
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */


#pragma once

/*
 * This is used by swupdate to start the Downloader Process
 */
int start_download_server(const char *cfgfname, int argc, char *argv[]);

void download_print_help(void);
