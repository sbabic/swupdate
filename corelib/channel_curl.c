/*
 * Author: Christian Storm
 * Copyright (C) 2016, Siemens AG
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
 */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <stdarg.h>
#include <unistd.h>
#include <math.h>
#include <curl/curl.h>
#include <generated/autoconf.h>
#include <unistd.h>
#include <network_ipc.h>
#include <util.h>
#include "sslapi.h"
#include "channel.h"
#include "channel_curl.h"
#ifdef CONFIG_JSON
#include <json-c/json.h>
#endif

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
channel_op_res_t channel_curl_init(void);
channel_op_res_t channel_close(channel_t *this);
channel_op_res_t channel_open(channel_t *this, void *cfg);
channel_op_res_t channel_get(channel_t *this, void *data);
channel_op_res_t channel_get_file(channel_t *this, void *data);
channel_op_res_t channel_put(channel_t *this, void *data);
channel_t *channel_new(void);


channel_op_res_t channel_curl_init(void)
{
#if defined(CONFIG_SURICATTA_SSL) || defined(CONFIG_CHANNEL_CURL_SSL)
#define CURL_FLAGS CURL_GLOBAL_SSL
#else
#define CURL_FLAGS CURL_GLOBAL_NOTHING
#endif
	CURLcode curlrc = curl_global_init(CURL_FLAGS);
	if (curlrc != CURLE_OK) {
		ERROR("Initialization of channel failed (%d): '%s'", curlrc,
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
		ERROR("Initialization of channel failed.");
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

	if (data->channel_data->usessl) {
		if (swupdate_HASH_update(data->channel_data->dgst,
					 streamdata,
					 size * nmemb) < 0) {
			ERROR("Updating checksum of chunk failed.");
			result_channel_callback_write_file = CHANNEL_EIO;
			return 0;
		}
	}

	if (ipc_send_data(data->output, streamdata, (int)(size * nmemb)) <
	    0) {
		ERROR("Writing into SWUpdate IPC stream failed.");
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
		ERROR("Channel get operation failed with OOM");
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
	TRACE("Channel's effective URL resolved to %s",
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
		DEBUG("No HTTP response code has been received yet!");
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

static int channel_callback_xferinfo(void *p, curl_off_t dltotal, curl_off_t dlnow,
				     curl_off_t __attribute__((__unused__)) ultotal,
				     curl_off_t __attribute__((__unused__)) ulnow)
{
	if ((dltotal <= 0) || (dlnow > dltotal)) {
		return 0;
	}
	double percent = 100.0 * (dlnow/1024.0) / (dltotal/1024.0);
	double *last_percent = (double*)p;
	if ((int)*last_percent == (int)percent) {
		return 0;
	}
	*last_percent = percent;
	char *info;
	if (asprintf(&info,
					"{\"percent\": %d, \"msg\":\"Received %" CURL_FORMAT_CURL_OFF_T "B "
					"of %" CURL_FORMAT_CURL_OFF_T "B\"}",
					(int)percent, dlnow, dltotal) != ENOMEM_ASPRINTF) {
		notify(SUBPROCESS, RECOVERY_NO_ERROR, TRACELEVEL, info);
		free(info);
	}
	return 0;
}

static int channel_callback_xferinfo_legacy(void *p, double dltotal, double dlnow,
					    double ultotal, double ulnow)
{
	return channel_callback_xferinfo(p, (curl_off_t)dltotal, (curl_off_t)dlnow,
					 (curl_off_t)ultotal, (curl_off_t)ulnow);
}

channel_op_res_t channel_set_options(channel_t *this,
					channel_data_t *channel_data,
					channel_method_t method)
{
	if (channel_data->low_speed_timeout == 0) {
		channel_data->low_speed_timeout = SPEED_LOW_TIME_SEC;
		DEBUG("cURL's low download speed timeout is disabled, "
			  "this is most probably not what you want. "
			  "Adapted it to %us instead.\n", SPEED_LOW_TIME_SEC);
	}
	channel_curl_t *channel_curl = this->priv;
	channel_op_res_t result = CHANNEL_OK;
	if ((curl_easy_setopt(channel_curl->handle, CURLOPT_URL,
			      channel_data->url) != CURLE_OK) ||
	    (curl_easy_setopt(channel_curl->handle, CURLOPT_USERAGENT,
			      "libcurl-agent/1.0") != CURLE_OK) ||
	    (curl_easy_setopt(channel_curl->handle, CURLOPT_LOW_SPEED_LIMIT,
			      SPEED_LOW_BYTES_SEC) != CURLE_OK) ||
	    (curl_easy_setopt(channel_curl->handle, CURLOPT_LOW_SPEED_TIME,
			      channel_data->low_speed_timeout) != CURLE_OK) ||
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

	double percent = -INFINITY;
	if ((curl_easy_setopt(channel_curl->handle, CURLOPT_PROGRESSFUNCTION,
			      channel_callback_xferinfo_legacy) != CURLE_OK) ||
	    (curl_easy_setopt(channel_curl->handle, CURLOPT_PROGRESSDATA,
				  &percent) != CURLE_OK)) {
		result = CHANNEL_EINIT;
		goto cleanup;
	}
#if LIBCURL_VERSION_NUM >= 0x072000
	if ((curl_easy_setopt(channel_curl->handle, CURLOPT_XFERINFOFUNCTION,
			      channel_callback_xferinfo) != CURLE_OK) ||
	    (curl_easy_setopt(channel_curl->handle, CURLOPT_XFERINFODATA,
				  &percent) != CURLE_OK)) {
		result = CHANNEL_EINIT;
		goto cleanup;
	}
#endif
	if (curl_easy_setopt(channel_curl->handle, CURLOPT_NOPROGRESS, 0L) != CURLE_OK) {
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

	/*
	 * Check if there is a restricted list of ciphers to be used
	 */
	if (channel_data->ciphers) {
		if (curl_easy_setopt(channel_curl->handle,
				      CURLOPT_SSL_CIPHER_LIST,
				      channel_data->ciphers) != CURLE_OK) {
			result = CHANNEL_EINIT;
			goto cleanup;
		}
	}

	if (channel_data->header != NULL) {
		if (((channel_curl->header = curl_slist_append(
				channel_curl->header, channel_data->header)) == NULL)) {
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
				      channel_data->request_body) != CURLE_OK)) {
			result = CHANNEL_EINIT;
			goto cleanup;
		}
		if (channel_data->debug) {
			TRACE("Posted: %s", channel_data->request_body);
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

	if (channel_data->auth) {
		if (curl_easy_setopt(channel_curl->handle, CURLOPT_USERPWD,
				     channel_data->auth) != CURLE_OK) {
			ERROR("Basic Auth credentials could not be set.");
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
	bytes = strlen(channel_data->request_body) - channel_data->offs;

	if (!bytes)
		return 0;

	n = min(bytes, size * nmemb);

	memcpy(ptr, &channel_data->request_body[channel_data->offs], n);
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
		ERROR("Set channel header failed.");
		result = CHANNEL_EINIT;
		goto cleanup_header;
	}

	if ((result = channel_set_options(this, channel_data, CHANNEL_POST)) !=
	    CHANNEL_OK) {
		ERROR("Set channel option failed.");
		goto cleanup_header;
	}

	CURLcode curlrc = curl_easy_perform(channel_curl->handle);
	if (curlrc != CURLE_OK) {
		ERROR("Channel put operation failed (%d): '%s'", curlrc,
		      curl_easy_strerror(curlrc));
		result = channel_map_curl_error(curlrc);
		goto cleanup_header;
	}

	channel_log_effective_url(this);

	long http_response_code;
	if ((result = channel_map_http_code(this, &http_response_code)) !=
	    CHANNEL_OK) {
		ERROR("Channel operation returned HTTP error code %ld.",
		      http_response_code);
		goto cleanup_header;
	}
	if (channel_data->debug) {
		TRACE("Channel put operation returned HTTP status code %ld.",
			http_response_code);
	}

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
		ERROR("Set channel header failed.");
		result = CHANNEL_EINIT;
		goto cleanup_header;
	}

	if ((result = channel_set_options(this, channel_data, CHANNEL_PUT)) !=
	    CHANNEL_OK) {
		ERROR("Set channel option failed.");
		goto cleanup_header;
	}

	if ((curl_easy_setopt(channel_curl->handle, CURLOPT_READFUNCTION, put_read_callback) !=
		CURLE_OK) ||
	   (curl_easy_setopt(channel_curl->handle, CURLOPT_INFILESIZE_LARGE,
			     (curl_off_t)strlen(channel_data->request_body)) != CURLE_OK) ||
	   (curl_easy_setopt(channel_curl->handle, CURLOPT_READDATA, channel_data) !=
			CURLE_OK)) {
		ERROR("Set channel option failed.");
		goto cleanup_header;
	}

	CURLcode curlrc = curl_easy_perform(channel_curl->handle);
	if (curlrc != CURLE_OK) {
		ERROR("Channel put operation failed (%d): '%s'", curlrc,
		      curl_easy_strerror(curlrc));
		result = channel_map_curl_error(curlrc);
		goto cleanup_header;
	}

	channel_log_effective_url(this);

	long http_response_code;
	if ((result = channel_map_http_code(this, &http_response_code)) != CHANNEL_OK) {
		ERROR("Channel operation returned HTTP error code %ld.",
		      http_response_code);
		goto cleanup_header;
	}
	TRACE("Channel put operation returned HTTP error code %ld.",
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
		TRACE("Channel method (POST, PUT) is not set !");
		return CHANNEL_EINIT;
	}
}

channel_op_res_t channel_get_file(channel_t *this, void *data)
{
	channel_curl_t *channel_curl = this->priv;
	int file_handle;
	assert(data != NULL);
	assert(channel_curl->handle != NULL);

	channel_op_res_t result = CHANNEL_OK;
	channel_data_t *channel_data = (channel_data_t *)data;

	if (channel_data->usessl) {
		memset(channel_data->sha1hash, 0x0, SHA_DIGEST_LENGTH * 2 + 1);
		channel_data->dgst = swupdate_HASH_init("sha1");
		if (!channel_data->dgst) {
			result = CHANNEL_EINIT;
			ERROR("Cannot initialize sha1 checksum context.");
			return result;
		}
	}

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
		ERROR("Set channel header failed.");
		goto cleanup_header;
	}

	if ((result = channel_set_options(this, channel_data, CHANNEL_GET)) !=
	    CHANNEL_OK) {
		ERROR("Set channel option failed.");
		goto cleanup_header;
	}

	for (int retries = 3; retries >= 0; retries--) {
		file_handle = ipc_inst_start_ext(channel_data->source,
			channel_data->info == NULL ? 0 : strlen(channel_data->info),
			channel_data->info,
			false /*no dryrun */);
		if (file_handle > 0)
			break;
		sleep(1);
	}
	if (file_handle < 0) {
		ERROR("Cannot open SWUpdate IPC stream: %s", strerror(errno));
		result = CHANNEL_EIO;
		goto cleanup_header;
	}

	write_callback_t wrdata;
	wrdata.channel_data = channel_data;
	wrdata.output = file_handle;
	result_channel_callback_write_file = CHANNEL_OK;
	if ((curl_easy_setopt(channel_curl->handle, CURLOPT_WRITEFUNCTION,
			      channel_callback_write_file) != CURLE_OK) ||
	    (curl_easy_setopt(channel_curl->handle, CURLOPT_WRITEDATA,
			      &wrdata) != CURLE_OK)) {
		ERROR("Cannot setup file writer callback function.");
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
			ERROR("Channel operation returned error (%d): '%s'",
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

	DEBUG("Channel downloaded %llu bytes ~ %llu MiB.",
	      total_bytes_downloaded, total_bytes_downloaded / 1024 / 1024);

	long http_response_code;
	if ((result = channel_map_http_code(this, &http_response_code)) !=
	    CHANNEL_OK) {
		ERROR("Channel operation returned HTTP error code %ld.",
		      http_response_code);
		goto cleanup_file;
	}
	if (channel_data->debug) {
		TRACE("Channel operation returned HTTP status code %ld.",
			http_response_code);
	}

	if (result_channel_callback_write_file != CHANNEL_OK) {
		result = CHANNEL_EIO;
		goto cleanup_file;
	}

	if (channel_data->usessl) {
		unsigned char sha1hash[SHA_DIGEST_LENGTH];
		unsigned int md_len;
		if (swupdate_HASH_final(channel_data->dgst, sha1hash, &md_len) != 1) {
			ERROR("Cannot compute checksum.");
			goto cleanup_file;
		}

		char sha1hexchar[3];
		for (int i = 0; i < SHA_DIGEST_LENGTH; i++) {
			sprintf(sha1hexchar, "%02x", sha1hash[i]);
			strcat(channel_data->sha1hash, sha1hexchar);
		}
	}

cleanup_file:
	/* NOTE ipc_end() calls close() but does not return its error code,
	 *      so use close() here directly to issue an error in case.
	 *      Also, for a given file handle, calling ipc_end() would make
	 *      no semantic sense. */
	if (close(file_handle) != 0) {
		ERROR("Channel error while closing download target handle: '%s'",
		      strerror(errno));
	}
	if (channel_data->dgst) {
		swupdate_HASH_cleanup(channel_data->dgst);
	}

cleanup_header:
	curl_easy_reset(channel_curl->handle);
	curl_slist_free_all(channel_curl->header);
	channel_curl->header = NULL;

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
		ERROR("Set channel header failed.");
		goto cleanup_header;
	}

	if ((result = channel_set_options(this, channel_data, CHANNEL_GET)) !=
	    CHANNEL_OK) {
		ERROR("Set channel option failed.");
		goto cleanup_header;
	}

	output_data_t chunk = {.memory = NULL, .size = 0};
	if ((chunk.memory = malloc(1)) == NULL) {
		ERROR("Channel buffer reservation failed with OOM.");
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
		ERROR("Cannot setup memory buffer writer callback function.");
		result = CHANNEL_EINIT;
		goto cleanup_chunk;
	}

	if (channel_data->debug) {
		DEBUG("Trying to GET %s", channel_data->url);
	}
	CURLcode curlrc = curl_easy_perform(channel_curl->handle);
	if (curlrc != CURLE_OK) {
		ERROR("Channel get operation failed (%d): '%s'", curlrc,
		      curl_easy_strerror(curlrc));
		result = channel_map_curl_error(curlrc);
		goto cleanup_chunk;
	}

	if (channel_data->debug) {
		channel_log_effective_url(this);
	}

	long http_response_code;
	if ((result = channel_map_http_code(this, &http_response_code)) !=
	    CHANNEL_OK) {
		ERROR("Channel operation returned HTTP error code %ld.",
		      http_response_code);
		if (http_response_code == 500) {
			DEBUG("The error's message is: '%s'", chunk.memory);
		}
		if (http_response_code == 404) {
			DEBUG("The error's message is: '%s'", chunk.memory);
		}
		goto cleanup_chunk;
	}
	if (channel_data->debug) {
		TRACE("Channel operation returned HTTP status code %ld.",
			http_response_code);
	}

#ifdef CONFIG_JSON
	assert(channel_data->json_reply == NULL);
	enum json_tokener_error json_res;
	struct json_tokener *json_tokenizer = json_tokener_new();
	do {
		channel_data->json_reply = json_tokener_parse_ex(
		    json_tokenizer, chunk.memory, (int)chunk.size);
	} while ((json_res = json_tokener_get_error(json_tokenizer)) ==
		 json_tokener_continue);
	if (json_res != json_tokener_success) {
		ERROR("Error while parsing channel's returned JSON data: %s",
		      json_tokener_error_desc(json_res));
		result = CHANNEL_EBADMSG;
		goto cleanup_json_tokenizer;
	}
	if (channel_data->debug) {
		TRACE("Get JSON: %s", chunk.memory);
	}

cleanup_json_tokenizer:
	json_tokener_free(json_tokenizer);
#endif
cleanup_chunk:
	chunk.memory != NULL ? free(chunk.memory) : (void)0;
cleanup_header:
	curl_easy_reset(channel_curl->handle);
	curl_slist_free_all(channel_curl->header);
	channel_curl->header = NULL;

	return result;
}
