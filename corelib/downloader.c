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

RECOVERY_STATUS download_from_url(char *image_url)
{
	CURL *curl_handle;
	CURLcode res;
	int fd;
	int attempt = 3;
	int result;

	if (!strlen(image_url)) {
		ERROR("Image URL not provided... aborting download and update\n");
		printf("EXITING\n");
		exit(1);
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
		return -EBUSY;
	}
	errno = 0;

	notify(DOWNLOAD, 0, 0);

	curl_global_init(CURL_GLOBAL_ALL);
	curl_handle = curl_easy_init();

	/* Write all data to the image file */
	curl_easy_setopt(curl_handle, CURLOPT_URL, image_url);
	curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_data);
	curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, &fd);
	curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "swupdate");
	curl_easy_setopt(curl_handle, CURLOPT_NOPROGRESS, 1L);

	TRACE("Image download started");

	/* TODO: Convert this to a streaming download at some point such
	 * that the file doesn't need to be downloaded completely before
	 * unpacking it for updating. See stream_interface for example. */
	if ((res = curl_easy_perform(curl_handle)) != CURLE_OK) {
		ERROR("Failed to download image: %s, exiting!\n",
				curl_easy_strerror(res));
		return -1;
	}

	TRACE("Image download completed");

	curl_easy_cleanup(curl_handle);
	curl_global_cleanup();

	result = ipc_wait_for_complete(NULL);

	close(fd);

	return result;
}
