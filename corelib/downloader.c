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
 * Foundation, Inc.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <curl/curl.h>

#include "bsdqueue.h"
#include "util.h"
#include "swupdate.h"
#include "installer.h"
#include "network_ipc.h"

static int cnt = 0;

/*
 * Callback from the libcurl API
 * It is called any time a chunk of data is received
 * to transfer it via IPC to the installer
 */
static size_t write_data(void *buffer, size_t size, size_t nmemb, void *userp)
{
	int ret;
	int fd;

	if (!nmemb)
		return 0;
	if (!userp) {
		ERROR("Failure IPC stream file descriptor \n");
		return -EFAULT;
	}

	fd = *(int *)userp;
	ret = ipc_send_data(fd, buffer, size * nmemb);
	if (ret < 0) {
		ERROR("Failure writing into IPC Stream\n");
		return ret;
	}
	cnt += size * nmemb;

	return nmemb;
}

/* Minimum bytes/sec, else connection is broken */
#define DL_LOWSPEED_BYTES	8
/* Number of seconds while below low speed limit before aborting */
#define DL_LOWSPEED_TIME	300

/*
 * libcurl options (see easy interface in libcurl documentation)
 * are grouped together into this function
 */
static void set_option_common(CURL *curl_handle)
{
	int ret;

	curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "swupdate");
	curl_easy_setopt(curl_handle, CURLOPT_NOPROGRESS, 1L);

	curl_easy_setopt(curl_handle, CURLOPT_LOW_SPEED_LIMIT, DL_LOWSPEED_BYTES);
	curl_easy_setopt(curl_handle, CURLOPT_LOW_SPEED_TIME, DL_LOWSPEED_TIME);

	/* enable TCP keep-alive for this transfer */
	ret = curl_easy_setopt(curl_handle, CURLOPT_TCP_KEEPALIVE, 1L);
	if (ret == CURLE_UNKNOWN_OPTION) {
		TRACE("No keep alive (unsupported in curl)");
		return;
	}

	/* keep-alive idle time to 240 seconds */
	curl_easy_setopt(curl_handle, CURLOPT_TCP_KEEPIDLE, 240L);

	/* interval time between keep-alive probes: 120 seconds */
	curl_easy_setopt(curl_handle, CURLOPT_TCP_KEEPINTVL, 120L);
}

/*
 * This provide a pull from an external server
 * It si not thought to work with local (file://)
 * for that, the -i option is used.
 */
RECOVERY_STATUS download_from_url(char *image_url, int retries)
{
	CURL *curl_handle;
	CURLcode res = CURLE_GOT_NOTHING;
	int fd;
	int attempt = 3;
	int result;
	double dummy;
	unsigned long long dwlbytes = 0;
	int i;

	if (!strlen(image_url)) {
		ERROR("Image URL not provided... aborting download and update\n");
		return FAILURE;
	}

	/*
	 * Maybe daemon is not yet started,
	 * try several times
	 */
	while (--attempt) {
		fd = ipc_inst_start();
		if (fd > 0)
			break;
		sleep(1);
	}

	if (fd < 0) {
		ERROR("Error getting IPC: %s\n", strerror(errno));
		return FAILURE;
	}
	errno = 0;

	/* We are starting a download */
	notify(DOWNLOAD, 0, 0);

	curl_global_init(CURL_GLOBAL_ALL);
	curl_handle = curl_easy_init();
	if (!curl_handle) {
		/* something very bad, it should never happen */
		ERROR("FAULT: no handle from libcurl");
		return FAILURE;
	}

	/* Set URL */
	if (curl_easy_setopt(curl_handle, CURLOPT_URL, image_url) != CURLE_OK) {
		TRACE("Runs out of memory: serious internal error");
		return FAILURE;
	}

	curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_data);
	curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, &fd);
	set_option_common(curl_handle);

	TRACE("Image download started : %s", image_url);

	for (i = 0; (res != CURLE_OK); i++) {
		/* if resume, set the offset */
		if (i) {
			TRACE("Connection with server interrupted, try RESUME after %llu",
					dwlbytes);
			if (curl_easy_setopt(curl_handle,CURLOPT_RESUME_FROM_LARGE,
					dwlbytes) != CURLE_OK) {
				TRACE("CURLOPT_RESUME_FROM_LARGE not implemented");
				break;
			}

			/* Let some time before tries */
			sleep(20);
		}
		res = curl_easy_perform(curl_handle);

		/* if size cannot be determined, exit because no resume is possible */
		if (curl_easy_getinfo(curl_handle,
				      CURLINFO_SIZE_DOWNLOAD,
				      &dummy) != CURLE_OK)
			break;

		dwlbytes += dummy;

		/* Check if max attempts is reached */
		if (retries && (i > retries))
			break;

	}

	if (res == CURLE_OK) {
		TRACE("Image download completed %s : %llu bytes", image_url, dwlbytes);
	} else {
		ERROR("Image not downloaded: %s", curl_easy_strerror(res));
	}

	curl_easy_cleanup(curl_handle);
	curl_global_cleanup();

	result = ipc_wait_for_complete(NULL);

	close(fd);

	return result;
}
