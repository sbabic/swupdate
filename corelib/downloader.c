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
#include <getopt.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <curl/curl.h>

#include "bsdqueue.h"
#include "util.h"
#include "swupdate.h"
#include "installer.h"
#include "network_ipc.h"
#include "parselib.h"
#include "swupdate_status.h"
#include "swupdate_settings.h"
#include "download_interface.h"

#define SETSTRING(p, v) do { \
	if (p) \
		free(p); \
	p = strdup(v); \
} while (0)

/*
 * Number of seconds while below low speed
 * limit before aborting
 * it can be overwritten by -t
 */
#define DL_LOWSPEED_TIME	300

#define DL_DEFAULT_RETRIES	3

static int cnt = 0;

struct dwl_options {
	char *url;
	unsigned int retries;
	unsigned int timeout;
	char *auth;
};

/* notify download progress each second */
#define MINIMAL_PROGRESS_INTERVAL     1

struct dlprogress {
	double lastruntime;
	CURL *curl;
};

static struct option long_options[] = {
    {"url", required_argument, NULL, 'u'},
    {"retries", required_argument, NULL, 'r'},
    {"timeout", required_argument, NULL, 't'},
    {"authentification", required_argument, NULL, 'a'},
    {NULL, 0, NULL, 0}};


/*
 * Callback from the libcurl API to progress meter function
 * This function gets called by libcurl instead of its internal equivalent.
 */
static int download_info(void *p,
			 curl_off_t dltotal, curl_off_t dlnow,
			 curl_off_t __attribute__ ((__unused__)) ultotal,
			 curl_off_t __attribute__ ((__unused__)) ulnow)
{
	struct dlprogress *dlp = (struct dlprogress *)p;
	CURL *curl = dlp->curl;
	double curtime = 0;
	curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &curtime);

	if ((curtime - dlp->lastruntime) >= MINIMAL_PROGRESS_INTERVAL) {
		dlp->lastruntime = curtime;
		INFO("Received : %" CURL_FORMAT_CURL_OFF_T " / %"
		     CURL_FORMAT_CURL_OFF_T, dlnow, dltotal);
	}

	return 0;
}

/* for libcurl older than 7.32.0 (CURLOPT_PROGRESSFUNCTION) */
static int legacy_download_info(void *p,
				double dltotal, double dlnow,
				double ultotal, double ulnow)
{
	return download_info(p,
			     (curl_off_t)dltotal,
			     (curl_off_t)dlnow,
			     (curl_off_t)ultotal,
			     (curl_off_t)ulnow);
}

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

/*
 * libcurl options (see easy interface in libcurl documentation)
 * are grouped together into this function
 */
static void set_option_common(CURL *curl_handle,
				unsigned long lowspeed_time,
				struct dlprogress *prog)
{
	int ret;

	prog->lastruntime = 0;
	prog->curl = curl_handle;

	curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "swupdate");

	curl_easy_setopt(curl_handle, CURLOPT_PROGRESSFUNCTION, legacy_download_info);
	curl_easy_setopt(curl_handle, CURLOPT_PROGRESSDATA, prog);
#if LIBCURL_VERSION_NUM >= 0x072000
	/* xferinfo was introduced in 7.32.0, no earlier libcurl versions will
	   compile as they won't have the symbols around.

	   If built with a newer libcurl, but running with an older libcurl:
	   curl_easy_setopt() will fail in run-time trying to set the new
	   callback, making the older callback get used.

	   New libcurls will prefer the new callback and instead use that one
	   even if both callbacks are set. */
	curl_easy_setopt(curl_handle, CURLOPT_XFERINFOFUNCTION, download_info);
	curl_easy_setopt(curl_handle, CURLOPT_XFERINFODATA, prog);
#endif
	curl_easy_setopt(curl_handle, CURLOPT_NOPROGRESS, 0L);

	curl_easy_setopt(curl_handle, CURLOPT_LOW_SPEED_LIMIT, DL_LOWSPEED_BYTES);
	curl_easy_setopt(curl_handle, CURLOPT_LOW_SPEED_TIME, lowspeed_time);

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
static RECOVERY_STATUS download_from_url(char *image_url, unsigned int retries,
					unsigned long lowspeed_time, char *auth)
{
	CURL *curl_handle;
	CURLcode res = CURLE_GOT_NOTHING;
	int fd;
	int attempt = 10;
	int result;
	double dummy;
	unsigned long long dwlbytes = 0;
	unsigned int i;
	struct dlprogress progress;


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

	TRACE("download from url started : %s", image_url);
	if (!image_url || !strlen(image_url)) {
		ERROR("Image URL not provided... aborting download and update\n");
		return FAILURE;
	}

	/* We are starting a download */
	notify(DOWNLOAD, 0, INFOLEVEL, NULL);

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

	/* Set Authentification */
	if (auth && curl_easy_setopt(curl_handle, CURLOPT_USERPWD, auth) != CURLE_OK) {
		TRACE("Runs out of memory: serious internal error");
		return FAILURE;
	}

	curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_data);
	curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, &fd);
	set_option_common(curl_handle, lowspeed_time, &progress);

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

			/* motivation: router restart, DNS reconfiguration */
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
		if (retries && (i >= retries))
			break;

	}

	curl_easy_cleanup(curl_handle);
	curl_global_cleanup();

	if (res == CURLE_OK) {
		result = ipc_wait_for_complete(NULL);
	} else {
		INFO("Error : %s", curl_easy_strerror(res));
		result = FAILURE;
	}

	ipc_end(fd);

	return result;
}

static int download_settings(void *elem, void  __attribute__ ((__unused__)) *data)
{
	struct dwl_options *opt = (struct dwl_options *)data;
	char tmp[128];

	GET_FIELD_STRING(LIBCFG_PARSER, elem, "url", tmp);
	if (strlen(tmp)) {
		SETSTRING(opt->url, tmp);
	}

	get_field(LIBCFG_PARSER, elem, "retries",
		&opt->retries);
	get_field(LIBCFG_PARSER, elem, "timeout",
		&opt->timeout);

	return 0;
}

void download_print_help(void)
{
	fprintf(
	    stdout,
	    "\tdownload arguments (mandatory arguments are marked with '*'):\n"
	    "\t  -u, --url <url>      * <url> is a link to the .swu update image\n"
	    "\t  -r, --retries          number of retries (resumed download) if connection\n"
	    "\t                         is broken (0 means indefinitely retries) (default: %d)\n"
	    "\t  -t, --timeout          timeout to check if a connection is lost (default: %d)\n"
	    "\t  -a, --authentication   authentification information as username:password\n",
	    DL_DEFAULT_RETRIES, DL_LOWSPEED_TIME);
}

int start_download(const char *fname, int argc, char *argv[])
{
	struct dwl_options options;
	unsigned int attempt;
	int choice = 0;
	RECOVERY_STATUS result;

	memset(&options, 0, sizeof(options));

	options.retries = DL_DEFAULT_RETRIES;
	options.timeout = DL_LOWSPEED_TIME;
	options.auth = NULL;

	if (fname) {
		read_module_settings(fname, "download", download_settings,
					&options);
	}

	/* reset to optind=1 to parse download's argument vector */
	optind = 1;
	while ((choice = getopt_long(argc, argv, "t:u:r:a:",
				     long_options, NULL)) != -1) {
		switch (choice) {
		case 't':
			options.timeout = strtoul(optarg, NULL, 10);
			break;
		case 'u':
			SETSTRING(options.url, optarg);
			break;
		case 'a':
			SETSTRING(options.auth, optarg);
			break;
		case 'r':
			options.retries = strtoul(optarg, NULL, 10);
			break;
		case '?':
		default:
			return -EINVAL;
		}
	}

	result = FAILURE;

	/*
	 * Maybe we need a different option as retries
	 * to check if an updated must be retried
	 */
	for (attempt = 0;; attempt++) {
		result = download_from_url(options.url, options.retries,
						options.timeout, options.auth);
		if (result != FAILURE) {
			ipc_message msg;
			if (ipc_postupdate(&msg) != 0) {
				result = FAILURE;
			} else {
				result = msg.type == ACK ? result : FAILURE;
			}
			break;
		}

		if (options.retries > 0 && attempt >= options.retries)
			break;

		/*
		 * motivation: slow connection, download_from_url fetched half the
		 * image, then aborted due to lowspeed_timeout, if we retry immediately
		 * we would just waste our bandwidth. Useful for A/B images.
		 */
		sleep(60);
	}

	exit(result == SUCCESS ? EXIT_SUCCESS : result);
}
