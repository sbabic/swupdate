/*
 * Author: Christian Storm
 * Copyright (C) 2016, Siemens AG
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc.
 */

#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <stdarg.h>
#include <unistd.h>
#include <curl/curl.h>
#include <json-c/json.h>
#include <generated/autoconf.h>
#include <unistd.h>
#include <network_ipc.h>
#include <util.h>
#ifdef CONFIG_SURICATTA_SSL
#include <openssl/sha.h>
#endif
#include "suricatta/channel.h"
#include "channel_hawkbit.h"
#include "suricatta/suricatta.h"

#define SPEED_LOW_BYTES_SEC 8
#define SPEED_LOW_TIME_SEC 300
#define KEEPALIVE_DELAY 204L
#define KEEPALIVE_INTERVAL 120L

typedef struct {
	char *memory;
	size_t size;
} output_data_t;

typedef struct {
	char *proxy;
	char *effective_url;
	CURL *handle;
	struct curl_slist *header;
} channel_curl_t;

typedef struct {
	channel_data_t *channel_data;
	int output;
	output_data_t *outdata;
} write_callback_t;

#ifdef CONFIG_SURICATTA_SSL
static SHA_CTX checksum_ctx;
#endif

/* Prototypes for "internal" functions */
/* Note that they're not `static` so that they're callable from unit tests. */
size_t channel_callback_write_file(void *streamdata, size_t size, size_t nmemb,
				   write_callback_t *data);
size_t channel_callback_membuffer(void *streamdata, size_t size, size_t nmemb,
				   write_callback_t *data);
channel_op_res_t channel_map_http_code(channel_t *this, long *http_response_code);
channel_op_res_t channel_map_curl_error(CURLcode res);
channel_op_res_t channel_set_options(channel_t *this, channel_data_t *channel_data,
				     channel_method_t method);
static void channel_log_effective_url(channel_t *this);

/* Prototypes for "public" functions */
channel_op_res_t channel_hawkbit_init(void);
channel_op_res_t channel_close(channel_t *this);
channel_op_res_t channel_open(channel_t *this, void *cfg);
channel_op_res_t channel_get(channel_t *this, void *data);
channel_op_res_t channel_get_file(channel_t *this, void *data, int file_handle);
channel_op_res_t channel_put(channel_t *this, void *data);
channel_t *channel_new(void);


channel_op_res_t channel_hawkbit_init(void)
{
#ifdef CONFIG_SURICATTA_SSL
#define CURL_FLAGS CURL_GLOBAL_SSL
#else
#define CURL_FLAGS CURL_GLOBAL_NOTHING
#endif
	CURLcode curlrc = curl_global_init(CURL_FLAGS);
	if (curlrc != CURLE_OK) {
		ERROR("Initialization of channel failed (%d): '%s'\n", curlrc,
		      curl_easy_strerror(curlrc));
		return CHANNEL_EINIT;
	}
#undef CURL_FLAGS
	return CHANNEL_OK;
}

channel_t *channel_new(void)
{
	channel_t *newchan = (channel_t *)calloc(1, sizeof(*newchan) +
				sizeof(channel_curl_t));

	if (newchan) {
		newchan->priv = (void *)newchan +  sizeof(*newchan);
		newchan->open = &channel_open;
		newchan->close = &channel_close;
		newchan->get = &channel_get;
		newchan->get_file = &channel_get_file;
		newchan->put = &channel_put;
	}

	return newchan;
}

channel_op_res_t channel_close(channel_t *this)
{
	channel_curl_t *channel_curl = this->priv;

	if ((channel_curl->proxy != NULL) &&
	    (channel_curl->proxy != USE_PROXY_ENV)) {
		free(channel_curl->proxy);
	}
	if (channel_curl->handle == NULL) {
		return CHANNEL_OK;
	}
	curl_easy_cleanup(channel_curl->handle);
	channel_curl->handle = NULL;

	return CHANNEL_OK;
}

channel_op_res_t channel_open(channel_t *this, void *cfg)
{
	assert(this != NULL);
	channel_curl_t *channel_curl = this->priv;
	assert(channel_curl->handle == NULL);

	channel_data_t *channel_cfg = (channel_data_t *)cfg;

	if ((channel_cfg != NULL) && (channel_cfg->proxy != NULL)) {
		channel_curl->proxy = channel_cfg->proxy == USE_PROXY_ENV
					 ? USE_PROXY_ENV
					 : strdup(channel_cfg->proxy);
		if (channel_curl->proxy == NULL) {
			return CHANNEL_EINIT;
		}
	}

	if ((channel_curl->handle = curl_easy_init()) == NULL) {
		ERROR("Initialization of channel failed.\n");
		return CHANNEL_EINIT;
	}

	return CHANNEL_OK;
}

static channel_op_res_t result_channel_callback_write_file;
size_t channel_callback_write_file(void *streamdata, size_t size, size_t nmemb,
				   write_callback_t *data)
{
	if (!nmemb) {
		return 0;
	}
	if (!data)
		return 0;
	result_channel_callback_write_file = CHANNEL_OK;
#ifdef CONFIG_SURICATTA_SSL
	if (SHA1_Update(&checksum_ctx, streamdata, size * nmemb) != 1) {
		ERROR("Updating checksum of chunk failed.\n");
		result_channel_callback_write_file = CHANNEL_EIO;
		return 0;
	}
#endif
	if (ipc_send_data(data->output, streamdata, (int)(size * nmemb)) <
	    0) {
		ERROR("Writing into SWUpdate IPC stream failed.\n");
		result_channel_callback_write_file = CHANNEL_EIO;
		return 0;
	}

	if (data->channel_data->checkdwl && data->channel_data->checkdwl())
		return 0;
	/*
	 * Now check if there is a callback from the server
	 * during the download
	 */

	return size * nmemb;
}

size_t channel_callback_membuffer(void *streamdata, size_t size, size_t nmemb,
				  write_callback_t *data)
{
	if (!nmemb) {
		return 0;
	}
	if (!data) {
		return 0;
	}

	size_t realsize = size * nmemb;
	output_data_t *mem = data->outdata;

	mem->memory = realloc(mem->memory, mem->size + realsize + 1);
	if (mem->memory == NULL) {
		ERROR("Channel get operation failed with OOM\n");
		return 0;
	}
	memcpy(&(mem->memory[mem->size]), streamdata, realsize);
	mem->size += realsize;
	mem->memory[mem->size] = 0;
	return realsize;
}

static void channel_log_effective_url(channel_t *this)
{
	channel_curl_t *channel_curl = this->priv;

	CURLcode curlrc =
	    curl_easy_getinfo(channel_curl->handle, CURLINFO_EFFECTIVE_URL,
			      &channel_curl->effective_url);
	if (curlrc != CURLE_OK && curlrc == CURLE_UNKNOWN_OPTION) {
		ERROR("Get channel's effective URL response unsupported by "
		      "libcURL %s.\n",
		      LIBCURL_VERSION);
		return;
	}
	TRACE("Channel's effective URL resolved to %s\n",
	      channel_curl->effective_url);
}

channel_op_res_t channel_map_http_code(channel_t *this, long *http_response_code)
{
	channel_curl_t *channel_curl = this->priv;
	CURLcode curlrc =
	    curl_easy_getinfo(channel_curl->handle, CURLINFO_RESPONSE_CODE,
			      http_response_code);
	if (curlrc != CURLE_OK && curlrc == CURLE_UNKNOWN_OPTION) {
		ERROR("Get channel HTTP response code unsupported by libcURL "
		      "%s.\n",
		      LIBCURL_VERSION);
		return CHANNEL_EINIT;
	}
	switch (*http_response_code) {
	case 0:   /* libcURL: no server response code has been received yet */
		DEBUG("No HTTP response code has been received yet!\n");
		return CHANNEL_EBADMSG;
	case 401: /* Unauthorized. The request requires user authentication. */
	case 403: /* Forbidden. */
	case 405: /* Method not Allowed. */
	case 407: /* Proxy Authentication Required */
	case 503: /* Service Unavailable */
		return CHANNEL_EACCES;
	case 400: /* Bad Request, e.g., invalid parameters */
	case 404: /* Wrong URL */
	case 406: /* Not acceptable. Accept header is not response compliant */
	case 443: /* Connection refused */
		return CHANNEL_EBADMSG;
	case 429: /* Bad Request, i.e., too many requests. Try again later. */
		return CHANNEL_EAGAIN;
	case 200:
	case 206:
		return CHANNEL_OK;
	case 500:
		return CHANNEL_EBADMSG;
	default:
		ERROR("Channel operation returned unhandled HTTP error code "
		      "%ld\n",
		      *http_response_code);
		return CHANNEL_EBADMSG;
	}
}

channel_op_res_t channel_map_curl_error(CURLcode res)
{
	switch (res) {
	case CURLE_NOT_BUILT_IN:
	case CURLE_BAD_FUNCTION_ARGUMENT:
	case CURLE_UNKNOWN_OPTION:
	case CURLE_SSL_ENGINE_NOTFOUND:
	case CURLE_SSL_ENGINE_SETFAILED:
	case CURLE_SSL_CERTPROBLEM:
	case CURLE_SSL_CIPHER:
	case CURLE_SSL_ENGINE_INITFAILED:
	case CURLE_SSL_CACERT_BADFILE:
	case CURLE_SSL_CRL_BADFILE:
	case CURLE_SSL_ISSUER_ERROR:
#if LIBCURL_VERSION_NUM >= 0x072900
	case CURLE_SSL_INVALIDCERTSTATUS:
#endif
#if LIBCURL_VERSION_NUM >= 0x072700
	case CURLE_SSL_PINNEDPUBKEYNOTMATCH:
#endif
		return CHANNEL_EINIT;
	case CURLE_COULDNT_RESOLVE_PROXY:
	case CURLE_COULDNT_RESOLVE_HOST:
	case CURLE_COULDNT_CONNECT:
	case CURLE_INTERFACE_FAILED:
	case CURLE_SSL_CONNECT_ERROR:
	case CURLE_PEER_FAILED_VERIFICATION:
	case CURLE_SSL_CACERT:
	case CURLE_USE_SSL_FAILED:
		return CHANNEL_ENONET;
	case CURLE_OPERATION_TIMEDOUT:
	case CURLE_SEND_ERROR:
	case CURLE_RECV_ERROR:
	case CURLE_GOT_NOTHING:
	case CURLE_HTTP_POST_ERROR:
	case CURLE_PARTIAL_FILE:
		return CHANNEL_EAGAIN;
	case CURLE_OUT_OF_MEMORY:
		return CHANNEL_ENOMEM;
	case CURLE_REMOTE_FILE_NOT_FOUND:
		return CHANNEL_ENOENT;
	case CURLE_FILESIZE_EXCEEDED:
	case CURLE_ABORTED_BY_CALLBACK:
	case CURLE_WRITE_ERROR:
	case CURLE_CHUNK_FAILED:
	case CURLE_SSL_SHUTDOWN_FAILED:
		return CHANNEL_EIO;
	case CURLE_TOO_MANY_REDIRECTS:
		return CHANNEL_ELOOP;
	case CURLE_BAD_CONTENT_ENCODING:
	case CURLE_CONV_FAILED:
	case CURLE_CONV_REQD:
		return CHANNEL_EILSEQ;
	case CURLE_REMOTE_ACCESS_DENIED:
	case CURLE_LOGIN_DENIED:
		return CHANNEL_EACCES;
	case CURLE_OK:
		return CHANNEL_OK;
	default:
		return CHANNEL_EINIT;
	}
}

channel_op_res_t channel_set_options(channel_t *this,
					channel_data_t *channel_data,
					channel_method_t method)
{
	channel_curl_t *channel_curl = this->priv;
	channel_op_res_t result = CHANNEL_OK;
	if ((curl_easy_setopt(channel_curl->handle, CURLOPT_URL,
			      channel_data->url) != CURLE_OK) ||
	    (curl_easy_setopt(channel_curl->handle, CURLOPT_USERAGENT,
			      "libcurl-agent/1.0") != CURLE_OK) ||
	    (curl_easy_setopt(channel_curl->handle, CURLOPT_LOW_SPEED_LIMIT,
			      SPEED_LOW_BYTES_SEC) != CURLE_OK) ||
	    (curl_easy_setopt(channel_curl->handle, CURLOPT_LOW_SPEED_TIME,
			      SPEED_LOW_TIME_SEC) != CURLE_OK) ||
	    (curl_easy_setopt(channel_curl->handle, CURLOPT_HTTPHEADER,
			      channel_curl->header) != CURLE_OK) ||
	    (curl_easy_setopt(channel_curl->handle, CURLOPT_MAXREDIRS, -1) !=
	     CURLE_OK) ||
	    (curl_easy_setopt(channel_curl->handle, CURLOPT_FOLLOWLOCATION, 1) !=
	     CURLE_OK) ||
	    (curl_easy_setopt(channel_curl->handle, CURLOPT_REDIR_PROTOCOLS,
			      CURLPROTO_HTTP | CURLPROTO_HTTPS) != CURLE_OK) ||
	    (curl_easy_setopt(channel_curl->handle,
			      CURLOPT_CAINFO,
			      channel_data->cafile) != CURLE_OK) ||
	    (curl_easy_setopt(channel_curl->handle,
			      CURLOPT_SSLKEY,
			      channel_data->sslkey) != CURLE_OK) ||
	    (curl_easy_setopt(channel_curl->handle,
			      CURLOPT_SSLCERT,
			      channel_data->sslcert) != CURLE_OK)) {
		result = CHANNEL_EINIT;
		goto cleanup;
	}

	if (channel_data->strictssl == true) {
		if ((curl_easy_setopt(channel_curl->handle,
				      CURLOPT_SSL_VERIFYHOST,
				      2L) != CURLE_OK) ||
		    (curl_easy_setopt(channel_curl->handle,
				      CURLOPT_SSL_VERIFYPEER,
				      1L) != CURLE_OK)) {
			result = CHANNEL_EINIT;
			goto cleanup;
		}
	}
	else {
		if ((curl_easy_setopt(channel_curl->handle,
				      CURLOPT_SSL_VERIFYHOST,
				      0L) != CURLE_OK) ||
		    (curl_easy_setopt(channel_curl->handle,
				      CURLOPT_SSL_VERIFYPEER,
				      0L) != CURLE_OK)) {
			result = CHANNEL_EINIT;
			goto cleanup;
		}
	}

	switch (method) {
	case CHANNEL_GET:
		if (curl_easy_setopt(channel_curl->handle, CURLOPT_CUSTOMREQUEST,
				     "GET") != CURLE_OK) {
			result = CHANNEL_EINIT;
			goto cleanup;
		}
		break;
	case CHANNEL_PUT:
		if ((curl_easy_setopt(channel_curl->handle, CURLOPT_PUT, 1L) !=
		     CURLE_OK) ||
		     (curl_easy_setopt(channel_curl->handle, CURLOPT_UPLOAD, 1L) !=
		      CURLE_OK)) {
			result = CHANNEL_EINIT;
			goto cleanup;
		}
		break;
	case CHANNEL_POST:
		if ((curl_easy_setopt(channel_curl->handle, CURLOPT_POST, 1L) !=
		     CURLE_OK) ||
		    (curl_easy_setopt(channel_curl->handle, CURLOPT_POSTFIELDS,
				      channel_data->json_string) != CURLE_OK)) {
			result = CHANNEL_EINIT;
			goto cleanup;
		}
		if (channel_data->debug) {
			TRACE("Post JSON: %s\n", channel_data->json_string);
		}
		break;
	}

	if (channel_curl->proxy != NULL) {
		if (channel_curl->proxy != USE_PROXY_ENV) {
			if (curl_easy_setopt(
				channel_curl->handle, CURLOPT_PROXY,
				channel_curl->proxy) != CURLE_OK) {
				result = CHANNEL_EINIT;
				goto cleanup;
			}
		}
		if (curl_easy_setopt(channel_curl->handle, CURLOPT_NETRC,
				     CURL_NETRC_OPTIONAL) != CURLE_OK) {
			result = CHANNEL_EINIT;
			goto cleanup;
		}
	}

	CURLcode curlrc =
	    curl_easy_setopt(channel_curl->handle, CURLOPT_TCP_KEEPALIVE, 1L);
	if (curlrc == CURLE_OK) {
		if ((curl_easy_setopt(channel_curl->handle, CURLOPT_TCP_KEEPIDLE,
				      KEEPALIVE_DELAY) != CURLE_OK) ||
		    (curl_easy_setopt(channel_curl->handle,
				      CURLOPT_TCP_KEEPINTVL,
				      KEEPALIVE_INTERVAL) != CURLE_OK)) {
			ERROR("TCP Keep-alive interval and delay could not be "
			      "configured.\n");
			result = CHANNEL_EINIT;
			goto cleanup;
		}
	} else {
		if (curlrc != CURLE_UNKNOWN_OPTION) {
			ERROR("Channel could not be configured to sent "
			      "keep-alive probes.\n");
			result = CHANNEL_EINIT;
			goto cleanup;
		}
	}

cleanup:
	return result;
}

static size_t put_read_callback(void *ptr, size_t size, size_t nmemb, void *data)
{
	channel_data_t *channel_data = (channel_data_t *)data;
	unsigned int bytes;
	size_t n;

	/* Check data to be sent */
	bytes = strlen(channel_data->json_string) - channel_data->offs;

	if (!bytes)
		return 0;

	n = min(bytes, size * nmemb);

	memcpy(ptr, &channel_data->json_string[channel_data->offs], n);
	channel_data->offs += n;

	return n;
}

static channel_op_res_t channel_post_method(channel_t *this, void *data)
{
	channel_curl_t *channel_curl = this->priv;
	assert(data != NULL);
	assert(channel_curl->handle != NULL);

	channel_op_res_t result = CHANNEL_OK;
	channel_data_t *channel_data = (channel_data_t *)data;

	if (channel_data->debug) {
		curl_easy_setopt(channel_curl->handle, CURLOPT_VERBOSE, 1L);
	}

	if (((channel_curl->header = curl_slist_append(
		  channel_curl->header, "Content-Type: application/json")) ==
	     NULL) ||
	    ((channel_curl->header = curl_slist_append(
		  channel_curl->header, "Accept: application/json")) == NULL) ||
	    ((channel_curl->header = curl_slist_append(
		  channel_curl->header, "charsets: utf-8")) == NULL)) {
		ERROR("Set channel header failed.\n");
		result = CHANNEL_EINIT;
		goto cleanup_header;
	}

	if ((result = channel_set_options(this, channel_data, CHANNEL_POST)) !=
	    CHANNEL_OK) {
		ERROR("Set channel option failed.\n");
		goto cleanup_header;
	}

	CURLcode curlrc = curl_easy_perform(channel_curl->handle);
	if (curlrc != CURLE_OK) {
		ERROR("Channel put operation failed (%d): '%s'\n", curlrc,
		      curl_easy_strerror(curlrc));
		result = channel_map_curl_error(curlrc);
		goto cleanup_header;
	}

	channel_log_effective_url(this);

	long http_response_code;
	if ((result = channel_map_http_code(this, &http_response_code)) !=
	    CHANNEL_OK) {
		ERROR("Channel operation returned HTTP error code %ld.\n",
		      http_response_code);
		goto cleanup_header;
	}
	TRACE("Channel put operation returned HTTP status code %ld.\n",
	      http_response_code);

cleanup_header:
	curl_easy_reset(channel_curl->handle);
	curl_slist_free_all(channel_curl->header);
	channel_curl->header = NULL;

	return result;
}

static channel_op_res_t channel_put_method(channel_t *this, void *data)
{
	channel_curl_t *channel_curl = this->priv;
	assert(data != NULL);
	assert(channel_curl->handle != NULL);

	channel_op_res_t result = CHANNEL_OK;
	channel_data_t *channel_data = (channel_data_t *)data;
	channel_data->offs = 0;

	if (channel_data->debug) {
		curl_easy_setopt(channel_curl->handle, CURLOPT_VERBOSE, 1L);
	}

	if (((channel_curl->header = curl_slist_append(
		  channel_curl->header, "Content-Type: application/json")) ==
	     NULL) ||
	    ((channel_curl->header = curl_slist_append(
		  channel_curl->header, "Accept: application/json")) == NULL) ||
	    ((channel_curl->header = curl_slist_append(
		  channel_curl->header, "charsets: utf-8")) == NULL)) {
		ERROR("Set channel header failed.\n");
		result = CHANNEL_EINIT;
		goto cleanup_header;
	}

	if ((result = channel_set_options(this, channel_data, CHANNEL_PUT)) !=
	    CHANNEL_OK) {
		ERROR("Set channel option failed.\n");
		goto cleanup_header;
	}

	if ((curl_easy_setopt(channel_curl->handle, CURLOPT_READFUNCTION, put_read_callback) !=
		CURLE_OK) ||
	   (curl_easy_setopt(channel_curl->handle, CURLOPT_INFILESIZE_LARGE,
			     (curl_off_t)strlen(channel_data->json_string)) != CURLE_OK) ||
	   (curl_easy_setopt(channel_curl->handle, CURLOPT_READDATA, channel_data) !=
			CURLE_OK)) {
		ERROR("Set channel option failed.\n");
		goto cleanup_header;
	}

	CURLcode curlrc = curl_easy_perform(channel_curl->handle);
	if (curlrc != CURLE_OK) {
		ERROR("Channel put operation failed (%d): '%s'\n", curlrc,
		      curl_easy_strerror(curlrc));
		result = channel_map_curl_error(curlrc);
		goto cleanup_header;
	}

	channel_log_effective_url(this);

	long http_response_code;
	if ((result = channel_map_http_code(this, &http_response_code)) != CHANNEL_OK) {
		ERROR("Channel operation returned HTTP error code %ld.\n",
		      http_response_code);
		goto cleanup_header;
	}
	TRACE("Channel put operation returned HTTP error code %ld.\n",
	      http_response_code);

cleanup_header:
	curl_easy_reset(channel_curl->handle);
	curl_slist_free_all(channel_curl->header);
	channel_curl->header = NULL;

	return result;
}

channel_op_res_t channel_put(channel_t *this, void *data)
{
	assert(data != NULL);

	channel_data_t *channel_data = (channel_data_t *)data;

	switch (channel_data->method) {
	case CHANNEL_PUT:
		return channel_put_method(this, data);
	case CHANNEL_POST:
		return channel_post_method(this, data);
	default:
		TRACE("Channel method (POST, PUT) is not set !\n");
		return CHANNEL_EINIT;
	}
}

channel_op_res_t channel_get_file(channel_t *this, void *data, int file_handle)
{
	channel_curl_t *channel_curl = this->priv;
	assert(data != NULL);
	assert(channel_curl->handle != NULL);

	channel_op_res_t result = CHANNEL_OK;
	channel_data_t *channel_data = (channel_data_t *)data;

#ifdef CONFIG_SURICATTA_SSL
	memset(channel_data->sha1hash, 0x0, SHA_DIGEST_LENGTH * 2 + 1);
	if (SHA1_Init(&checksum_ctx) != 1) {
		result = CHANNEL_EINIT;
		ERROR("Cannot initialize sha1 checksum context.\n");
		goto cleanup;
	}
#endif

	if (channel_data->debug) {
		curl_easy_setopt(channel_curl->handle, CURLOPT_VERBOSE, 1L);
	}

	if (((channel_curl->header = curl_slist_append(
		  channel_curl->header,
		  "Content-Type: application/octet-stream")) == NULL) ||
	    ((channel_curl->header = curl_slist_append(
		  channel_curl->header, "Accept: application/octet-stream")) ==
	     NULL)) {
		result = CHANNEL_EINIT;
		ERROR("Set channel header failed.\n");
		goto cleanup_header;
	}

	if ((result = channel_set_options(this, channel_data, CHANNEL_GET)) !=
	    CHANNEL_OK) {
		ERROR("Set channel option failed.\n");
		goto cleanup_header;
	}

	if (file_handle == FD_USE_IPC) {
		for (int retries = 3; retries >= 0; retries--) {
			file_handle = ipc_inst_start_ext(SOURCE_SURICATTA,
				channel_data->info == NULL ? 0 : strlen(channel_data->info),
				channel_data->info);
			if (file_handle > 0)
				break;
			sleep(1);
		}
		if (file_handle < 0) {
			ERROR("Cannot open SWUpdate IPC stream: %s\n", strerror(errno));
			result = CHANNEL_EIO;
			goto cleanup_header;
		}
	} else {
		assert(file_handle > 0);
	}

	write_callback_t wrdata;
	wrdata.channel_data = channel_data;
	wrdata.output = file_handle;
	result_channel_callback_write_file = CHANNEL_OK;
	if ((curl_easy_setopt(channel_curl->handle, CURLOPT_WRITEFUNCTION,
			      channel_callback_write_file) != CURLE_OK) ||
	    (curl_easy_setopt(channel_curl->handle, CURLOPT_WRITEDATA,
			      &wrdata) != CURLE_OK)) {
		ERROR("Cannot setup file writer callback function.\n");
		result = CHANNEL_EINIT;
		goto cleanup_file;
	}

	/* Depending on the architecture and libcurl, this is at
	 * least 2^32 - 1 bytes, i.e., enough to account for a 4GiB
	 * download. Although 32-Bit curl defines `curl_off_t` as
	 * (signed) long long, use unsigned here, but it wastes some
	 * bits on 64-Bit.
	 */
	unsigned long long int total_bytes_downloaded = 0;
	unsigned char try_count = 0;
	CURLcode curlrc = CURLE_OK;
	do {
		if (try_count > 0) {
			if (channel_data->retries == 0) {
				ERROR(
				    "Channel get operation failed (%d): '%s'\n",
				    curlrc, curl_easy_strerror(curlrc));
				result = channel_map_curl_error(curlrc);
				goto cleanup_file;
			}

			if (try_count > channel_data->retries) {
				ERROR("Channel get operation aborted because "
				      "of too many failed download attempts "
				      "(%d).\n",
				      channel_data->retries);
				result = CHANNEL_ELOOP;
				goto cleanup_file;
			}

			DEBUG("Channel connection interrupted, trying resume "
			      "after %llu bytes.",
			      total_bytes_downloaded);
			if (curl_easy_setopt(
				channel_curl->handle, CURLOPT_RESUME_FROM_LARGE,
				total_bytes_downloaded) != CURLE_OK) {
				ERROR("Could not set Channel resume seek (%d): "
				      "'%s'\n",
				      curlrc, curl_easy_strerror(curlrc));
				result = channel_map_curl_error(curlrc);
				goto cleanup_file;
			}
			TRACE("Channel sleeps for %d seconds now.",
			      channel_data->retry_sleep);
			if (sleep(channel_data->retry_sleep) > 0) {
				TRACE("Channel's sleep got interrupted, "
				      "retrying nonetheless now.");
			}
			TRACE("Channel awakened from sleep.");
		}

		curlrc = curl_easy_perform(channel_curl->handle);
		result = channel_map_curl_error(curlrc);
		if ((result != CHANNEL_OK) && (result != CHANNEL_EAGAIN)) {
			ERROR("Channel operation returned error (%d): '%s'\n",
			      curlrc, curl_easy_strerror(curlrc));
			goto cleanup_file;
		}

		double bytes_downloaded;
		CURLcode resdlprogress = curl_easy_getinfo(
		    channel_curl->handle, CURLINFO_SIZE_DOWNLOAD,
		    &bytes_downloaded);
		if (resdlprogress != CURLE_OK) {
			ERROR("Channel does not report bytes downloaded (%d): "
			      "'%s'\n",
			      resdlprogress, curl_easy_strerror(resdlprogress));
			result = channel_map_curl_error(resdlprogress);
			goto cleanup_file;
		}
		total_bytes_downloaded += bytes_downloaded;

	} while (++try_count && (result != CHANNEL_OK));

	channel_log_effective_url(this);

	DEBUG("Channel downloaded %llu bytes ~ %llu MiB.\n",
	      total_bytes_downloaded, total_bytes_downloaded / 1024 / 1024);

	long http_response_code;
	if ((result = channel_map_http_code(this, &http_response_code)) !=
	    CHANNEL_OK) {
		ERROR("Channel operation returned HTTP error code %ld.\n",
		      http_response_code);
		goto cleanup_file;
	}
	TRACE("Channel operation returned HTTP status code %ld.\n",
	      http_response_code);

	if (result_channel_callback_write_file != CHANNEL_OK) {
		result = CHANNEL_EIO;
		goto cleanup_file;
	}

#ifdef CONFIG_SURICATTA_SSL
	unsigned char sha1hash[SHA_DIGEST_LENGTH];
	if (SHA1_Final(sha1hash, &checksum_ctx) != 1) {
		ERROR("Cannot compute checksum.\n");
		goto cleanup_file;
	}
	char sha1hexchar[3];
	for (int i = 0; i < SHA_DIGEST_LENGTH; i++) {
		sprintf(sha1hexchar, "%02x", sha1hash[i]);
		strcat(channel_data->sha1hash, sha1hexchar);
	}
#endif

cleanup_file:
	/* NOTE ipc_end() calls close() but does not return its error code,
	 *      so use close() here directly to issue an error in case.
	 *      Also, for a given file handle, calling ipc_end() would make
	 *      no semantic sense. */
	if (close(file_handle) != 0) {
		ERROR("Channel error while closing download target handle: '%s'\n",
		      strerror(errno));
	}
cleanup_header:
	curl_easy_reset(channel_curl->handle);
	curl_slist_free_all(channel_curl->header);
	channel_curl->header = NULL;

#ifdef CONFIG_SURICATTA_SSL
cleanup:
#endif
	return result;
}

channel_op_res_t channel_get(channel_t *this, void *data)
{
	channel_curl_t *channel_curl = this->priv;
	assert(data != NULL);
	assert(channel_curl.handle != NULL);

	channel_op_res_t result = CHANNEL_OK;
	channel_data_t *channel_data = (channel_data_t *)data;

	if (channel_data->debug) {
		curl_easy_setopt(channel_curl->handle, CURLOPT_VERBOSE, 1L);
	}

	if (((channel_curl->header = curl_slist_append(
		  channel_curl->header, "Content-Type: application/json")) ==
	     NULL) ||
	    ((channel_curl->header = curl_slist_append(
		  channel_curl->header, "Accept: application/json")) == NULL) ||
	    ((channel_curl->header = curl_slist_append(
		  channel_curl->header, "charsets: utf-8")) == NULL)) {
		result = CHANNEL_EINIT;
		ERROR("Set channel header failed.\n");
		goto cleanup_header;
	}

	if ((result = channel_set_options(this, channel_data, CHANNEL_GET)) !=
	    CHANNEL_OK) {
		ERROR("Set channel option failed.\n");
		goto cleanup_header;
	}

	output_data_t chunk = {.memory = NULL, .size = 0};
	if ((chunk.memory = malloc(1)) == NULL) {
		ERROR("Channel buffer reservation failed with OOM.\n");
		result = CHANNEL_ENOMEM;
		goto cleanup_header;
	}

	write_callback_t wrdata;
	wrdata.channel_data = channel_data;
	wrdata.outdata = &chunk;

	if ((curl_easy_setopt(channel_curl->handle, CURLOPT_WRITEFUNCTION,
			      channel_callback_membuffer) != CURLE_OK) ||
	    (curl_easy_setopt(channel_curl->handle, CURLOPT_WRITEDATA,
			      (void *)&wrdata) != CURLE_OK)) {
		ERROR("Cannot setup memory buffer writer callback function.\n");
		result = CHANNEL_EINIT;
		goto cleanup_chunk;
	}

	DEBUG("Trying to GET %s", channel_data->url);
	CURLcode curlrc = curl_easy_perform(channel_curl->handle);
	if (curlrc != CURLE_OK) {
		ERROR("Channel get operation failed (%d): '%s'\n", curlrc,
		      curl_easy_strerror(curlrc));
		result = channel_map_curl_error(curlrc);
		goto cleanup_chunk;
	}

	channel_log_effective_url(this);

	long http_response_code;
	if ((result = channel_map_http_code(this, &http_response_code)) !=
	    CHANNEL_OK) {
		ERROR("Channel operation returned HTTP error code %ld.\n",
		      http_response_code);
		if (http_response_code == 500) {
			DEBUG("The error's message is: '%s'\n", chunk.memory);
		}
		if (http_response_code == 404) {
			DEBUG("The error's message is: '%s'\n", chunk.memory);
		}
		goto cleanup_chunk;
	}
	TRACE("Channel operation returned HTTP status code %ld.\n",
	      http_response_code);

	assert(channel_data->json_reply == NULL);
	enum json_tokener_error json_res;
	struct json_tokener *json_tokenizer = json_tokener_new();
	do {
		channel_data->json_reply = json_tokener_parse_ex(
		    json_tokenizer, chunk.memory, (int)chunk.size);
	} while ((json_res = json_tokener_get_error(json_tokenizer)) ==
		 json_tokener_continue);
	if (json_res != json_tokener_success) {
		ERROR("Error while parsing channel's returned JSON data: %s\n",
		      json_tokener_error_desc(json_res));
		result = CHANNEL_EBADMSG;
		goto cleanup_json_tokenizer;
	}
	if (channel_data->debug) {
		TRACE("Get JSON: %s\n", chunk.memory);
	}

cleanup_json_tokenizer:
	json_tokener_free(json_tokenizer);
cleanup_chunk:
	chunk.memory != NULL ? free(chunk.memory) : (void)0;
cleanup_header:
	curl_easy_reset(channel_curl->handle);
	curl_slist_free_all(channel_curl->header);
	channel_curl->header = NULL;

	return result;
}
