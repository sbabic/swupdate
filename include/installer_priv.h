/*
 * (C) Copyright 2020
 * Stefano Babic <stefano.babic@swupdate.org>
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#pragma once
#include "swupdate_status.h"
#include "network_ipc.h"

struct installer {
	int	fd;			/* install image file handle */
	RECOVERY_STATUS	status;		/* "idle" or "request source" info */
	RECOVERY_STATUS	last_install;	/* result from last installation */
	int	last_error;		/* error code if installation failed */
	char	errormsg[64];		/* error message if installation failed */
	struct swupdate_request req;
	struct swupdate_cfg *software;
};
