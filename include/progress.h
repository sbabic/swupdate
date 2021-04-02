/*
 * (C) Copyright 2016
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#ifndef _INSTALL_PROGRESS_H
#define _INSTALL_PROGRESS_H

#include <swupdate_status.h>
#include <progress_ipc.h>

/*
 * Internal SWUpdate functions to drive the progress
 * interface. Common progress definitions for internal
 * as well as external use are defined in progress_ipc.h
 */
void swupdate_progress_init(unsigned int nsteps);
void swupdate_progress_update(unsigned int perc);
void swupdate_progress_inc_step(char *image, char *handler_name);
void swupdate_progress_step_completed(void);
void swupdate_progress_end(RECOVERY_STATUS status);
void swupdate_progress_done(const char *info);
void swupdate_progress_info(RECOVERY_STATUS status, int cause, const char *msg);

void swupdate_download_update(unsigned int perc, unsigned long long totalbytes);

void *progress_bar_thread (void *data);

#endif
