/*
 * Author: Christian Storm
 * Copyright (C) 2016, Siemens AG
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#include <stdbool.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <ctype.h>
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
#include "channel_op_res.h"
#include "sslapi.h"
#include "channel.h"
#include "channel_curl.h"
#include "progress.h"
#include <json-c/json.h>

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
	char *redirect_url;
	CURL *handle;
	struct curl_slist *header;
} channel_curl_t;

typedef struct {
	channel_data_t *channel_data;
	int output;
	output_data_t *outdata;
	channel_t *this;
} write_callback_t;

typedef struct {
	curl_off_t total_download_size;
	uint8_t percent;
	sourcetype source; /* SWUpdate module that triggered the download. */
} download_callback_data_t;

static const char *method_desc[] = {
	[CHANNEL_GET] = "GET",
	[CHANNEL_POST] = "POST",
	[CHANNEL_PUT] = "PUT",
	[CHANNEL_PATCH] = "PATCH",
	[CHANNEL_DELETE] = "DELETE"
};

/* Prototypes for "internal" functions */
/* Note that they're not `static` so that they're callable from unit tests. */
size_t channel_callback_ipc(void *streamdata, size_t size, size_t nmemb,
				   write_callback_t *data);
size_t channel_callback_membuffer(void *streamdata, size_t size, size_t nmemb,
				   write_callback_t *data);
channel_op_res_t channel_map_http_code(channel_t *this, long *http_response_code);
channel_op_res_t channel_map_curl_error(CURLcode res);
channel_op_res_t channel_set_options(channel_t *this, channel_data_t *channel_data);
char *channel_get_redirect_url(channel_t *this);

static void channel_log_effective_url(channel_t *this);

/* Prototypes for "public" functions */
static channel_op_res_t channel_close(channel_t *this);
static channel_op_res_t channel_open(channel_t *this, void *cfg);
static channel_op_res_t channel_get(channel_t *this, void *data);
static channel_op_res_t channel_get_file(channel_t *this, void *data);
static channel_op_res_t channel_put(channel_t *this, void *data);
static channel_op_res_t channel_put_file(channel_t *this, void *data);
channel_op_res_t channel_curl_init(void);
channel_t *channel_new(void);


channel_op_res_t channel_curl_init(void)
{
#if defined(CONFIG_CHANNEL_CURL_SSL)
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
		newchan->put_file = &channel_put_file;
		newchan->get_redirect_url = &channel_get_redirect_url;
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
	if (channel_curl->redirect_url)
		free(channel_curl->redirect_url);
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

static channel_op_res_t result_channel_callback_ipc;
size_t channel_callback_ipc(void *streamdata, size_t size, size_t nmemb,
				   write_callback_t *data)
{
	if (!nmemb) {
		return 0;
	}
	if (!data)
		return 0;
	result_channel_callback_ipc = CHANNEL_OK;

	if (data->channel_data->usessl) {
		if (swupdate_HASH_update(data->channel_data->dgst,
					 streamdata,
					 size * nmemb) < 0) {
			ERROR("Updating checksum of chunk failed.");
			result_channel_callback_ipc = CHANNEL_EIO;
			return 0;
		}
	}

	if (!data->channel_data->http_response_code)
		channel_map_http_code(data->this, &data->channel_data->http_response_code);

	if (!data->channel_data->noipc &&
		ipc_send_data(data->output, streamdata, (int)(size * nmemb)) <
	    0) {
		ERROR("Writing into SWUpdate IPC stream failed.");
		result_channel_callback_ipc = CHANNEL_EIO;
		return 0;
	}

	if (data->channel_data->dwlwrdata) {
		return data->channel_data->dwlwrdata(streamdata, size, nmemb, data->channel_data);
	}

	/*
	 * Now check if there is a callback from the server
	 * during the download
	 */

	return size * nmemb;
}

#define BUFF_SIZE 16384
static unsigned long long int resume_cache_file(const char *fname,
					  write_callback_t *data)
{
	int fdsw;
	char *buf;
	ssize_t cnt;
	unsigned long long processed = 0;

	if (!fname || !strlen(fname))
		return 0;
	fdsw = open(fname, O_RDONLY);
	if (fdsw < 0)
		return 0; /* ignore, load from network */
	buf = calloc(1, BUFF_SIZE);
	if (!buf) {
		ERROR("Channel get operation failed with OOM");
		close(fdsw);
		return 0;
	}

	while ((cnt = read(fdsw, buf, BUFF_SIZE)) > 0) {
		if (!channel_callback_ipc(buf, cnt, 1, data))
			break;
		processed += cnt;
	}

	close(fdsw);
	free(buf);

	/*
	 * Cache file is used just once: after it is read, it is
	 * dropped automatically to avoid to reuse it again
	 * for next update
	 */
	unlink(fname);

	return processed;
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

char *channel_get_redirect_url(channel_t *this)
{
	channel_curl_t *channel_curl = this->priv;
	TRACE("Redirect URL %s", channel_curl->redirect_url);
	return channel_curl->redirect_url;
}

channel_op_res_t channel_map_http_code(channel_t *this, long *http_response_code)
{
	char *url = NULL;
	long protocol;
	channel_curl_t *channel_curl = this->priv;
	CURLcode curlrc =
	    curl_easy_getinfo(channel_curl->handle, CURLINFO_RESPONSE_CODE,
			      http_response_code);
	if (curlrc != CURLE_OK && curlrc == CURLE_UNKNOWN_OPTION) {
		ERROR("Get channel HTTP response code unsupported by libcURL "
		      "%s.\n",
		      LIBCURL_VERSION);
		/* Set to 0 as libcURL would do if no response code has been received. */
		*http_response_code = 0;
		return CHANNEL_EINIT;
	}
	switch (*http_response_code) {
	case 0:   /* libcURL: no server response code has been received yet or file:// protocol */
		curlrc = curl_easy_getinfo(channel_curl->handle,
				#if LIBCURL_VERSION_NUM >= 0x75500
					   CURLINFO_SCHEME,
				#else
					   CURLINFO_PROTOCOL,
				#endif
					   &protocol);
		if (curlrc == CURLE_OK && protocol == CURLPROTO_FILE) {
			return CHANNEL_OK;
		}
		DEBUG("No HTTP response code has been received yet!");
		return CHANNEL_EBADMSG;
	case 401: /* Unauthorized. The request requires user authentication. */
	case 403: /* Forbidden. */
	case 405: /* Method not Allowed. */
	case 407: /* Proxy Authentication Required */
	case 503: /* Service Unavailable */
		return CHANNEL_EACCES;
	case 400: /* Bad Request, e.g., invalid parameters */
	case 406: /* Not acceptable. Accept header is not response compliant */
	case 443: /* Connection refused */
	case 409: /* Conflict */
		return CHANNEL_EBADMSG;
	case 404: /* Wrong URL */
		return CHANNEL_ENOTFOUND;
	case 429: /* Bad Request, i.e., too many requests. Try again later. */
		return CHANNEL_EAGAIN;
	case 200:
	case 201:
	case 204:
	case 206:
	case 226:
		return CHANNEL_OK;
	case 302:
		curlrc = curl_easy_getinfo(channel_curl->handle, CURLINFO_REDIRECT_URL,
					   &url);
		if (curlrc == CURLE_OK) {
			if (channel_curl->redirect_url)
				free(channel_curl->redirect_url);
			channel_curl->redirect_url = strdup(url);
		} else if (curlrc == CURLE_UNKNOWN_OPTION) {
			ERROR("channel_curl_getinfo response unsupported by "
				"libcURL %s.\n",
				LIBCURL_VERSION);
		}
		return CHANNEL_EREDIRECT;
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
	case CURLE_PEER_FAILED_VERIFICATION:
#if LIBCURL_VERSION_NUM < 0x073E00
	case CURLE_SSL_CACERT:
#endif
		return CHANNEL_ESSLCERT;
	case CURLE_SSL_CONNECT_ERROR:
		return CHANNEL_ESSLCONNECT;
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
	if ((dltotal <= 0) || (dlnow > dltotal))
		return 0;

	uint8_t percent = 100.0 * ((double)dlnow / dltotal);
	download_callback_data_t *data = (download_callback_data_t*)p;

	if (data->percent >= percent)
		return 0;
	else
		data->percent = percent;

	DEBUG("Downloaded %d%% (%zu of %zu kB).", percent,
		(size_t)dlnow / 1024,
		(size_t)dltotal / 1024);
	swupdate_download_update(percent, dltotal);

	return 0;
}

#if LIBCURL_VERSION_NUM < 0x072000
static int channel_callback_xferinfo_legacy(void *p, double dltotal, double dlnow,
					    double ultotal, double ulnow)
{
	return channel_callback_xferinfo(p, (curl_off_t)dltotal, (curl_off_t)dlnow,
					 (curl_off_t)ultotal, (curl_off_t)ulnow);
}
#endif

static size_t channel_callback_headers(char *buffer, size_t size, size_t nitems, void *userdata)
{
	channel_data_t *channel_data = (channel_data_t *)userdata;
	struct dict *dict = channel_data->received_headers;
	char *info;
	char *p, *key, *val;

	if (dict) {
		info = malloc(size * nitems + 1);
		if (!info) {
			ERROR("No memory allocated for headers, headers not collected !!");
			return nitems * size;
		}
		/*
		 * Work on a local copy because the buffer is not
		 * '\0' terminated
		 */
		memcpy(info, buffer, size * nitems);
		info[size * nitems] = '\0';
		p = memchr(info, ':', size * nitems);
		if (p) {
			*p = '\0';
			key = info;
			val = p + 1; /* Next char after ':' */
			while(isspace((unsigned char)*val)) val++;
			/* Remove '\n', '\r', and '\r\n' from header's value. */
			*strchrnul(val, '\r') = '\0';
			*strchrnul(val, '\n') = '\0';
			/* For multiple same-key headers, only the last is saved. */
			dict_set_value(dict, key, val);
			DEBUG("Header processed: %s : %s", key, val);
		} else {
			DEBUG("Header not processed: '%s'", info);
		}

		free(info);
	}

	if (channel_data->headers)
		return channel_data->headers(buffer, size, nitems, userdata);

	return nitems * size;
}

static channel_op_res_t channel_set_content_type(channel_t *this,
						channel_data_t *channel_data)
{
	channel_curl_t *channel_curl = this->priv;
	const char *content;
	char *contenttype, *accept;
	assert(channel_curl->handle != NULL);

	channel_op_res_t result = CHANNEL_OK;

	if (channel_data->content_type && strlen(channel_data->content_type))
		content = channel_data->content_type;
	else
		content = "application/json";

	if (ENOMEM_ASPRINTF == asprintf(&contenttype, "Content-Type: %s%s", content,
		!strcmp(content, "application/text") ? "; charset=utf-8" : "")) {
			ERROR("OOM when setting Content-type.");
			result = CHANNEL_EINIT;
	} else {
		if ((channel_curl->header = curl_slist_append(channel_curl->header,
			contenttype)) == NULL) {
				ERROR("Setting channel header Content-type failed.");
				result = CHANNEL_EINIT;
		}
	}

	if (channel_data->accept_content_type)
		content = channel_data->accept_content_type;
	if (ENOMEM_ASPRINTF == asprintf(&accept, "Accept: %s", content)) {
		ERROR("OOM when setting Accept.");
		result = CHANNEL_EINIT;
	} else {
		if ((channel_curl->header = curl_slist_append(channel_curl->header,
			accept)) == NULL) {
				ERROR("Setting channel header Accept failed.");
				result = CHANNEL_EINIT;
		}
	}

	return result;
}

channel_op_res_t channel_set_options(channel_t *this, channel_data_t *channel_data)
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
	    (curl_easy_setopt(channel_curl->handle,
			#if LIBCURL_VERSION_NUM >= 0x75500
			      CURLOPT_REDIR_PROTOCOLS_STR,
			      "http,https"
			#else
			      CURLOPT_REDIR_PROTOCOLS,
			      CURLPROTO_HTTP | CURLPROTO_HTTPS
			#endif
			) != CURLE_OK) ||
	    (curl_easy_setopt(channel_curl->handle,
			      CURLOPT_SSLKEY,
			      channel_data->sslkey) != CURLE_OK) ||
	    (curl_easy_setopt(channel_curl->handle,
			      CURLOPT_SSLCERT,
			      channel_data->sslcert) != CURLE_OK) ||
        (curl_easy_setopt(channel_curl->handle,
                  CURLOPT_POSTREDIR,
                  CURL_REDIR_POST_ALL) != CURLE_OK)) {
		result = CHANNEL_EINIT;
		goto cleanup;
	}

	if (channel_data->connection_timeout > 0 &&
		(curl_easy_setopt(channel_curl->handle, CURLOPT_CONNECTTIMEOUT,
		 channel_data->connection_timeout) != CURLE_OK)) {
		result = CHANNEL_EINIT;
		goto cleanup;
	}

	/*
	 * If connection is via unix socket, set it
	 */
	if (channel_data->unix_socket &&
		(curl_easy_setopt(channel_curl->handle, CURLOPT_UNIX_SOCKET_PATH,
		 channel_data->unix_socket) != CURLE_OK)) {
		result = CHANNEL_EINIT;
		goto cleanup;
	}

	/* Check if sslkey or sslcert strings contains a pkcs11 URI
	 * and set curl engine and types accordingly
	 */
	bool keyUri = channel_data->sslkey ? strncasecmp(channel_data->sslkey, "pkcs11:", 7) == 0 : false;
	bool certUri = channel_data->sslkey ? strncasecmp(channel_data->sslcert, "pkcs11:", 7) == 0 : false;

	if (keyUri || certUri) {
		if (curl_easy_setopt(channel_curl->handle, CURLOPT_SSLENGINE, "pkcs11") != CURLE_OK) {
			ERROR("Error %d setting CURLOPT_SSLENGINE", result);
			result = CHANNEL_EINIT;
			goto cleanup;
		}

		if (keyUri) {
			if (curl_easy_setopt(channel_curl->handle, CURLOPT_SSLKEYTYPE, "ENG") != CURLE_OK) {
				ERROR("Error %d setting CURLOPT_SSLKEYTYPE", result);
				result = CHANNEL_EINIT;
				goto cleanup;
			}
		}

		if (certUri) {
			if (curl_easy_setopt(channel_curl->handle, CURLOPT_SSLCERTTYPE, "ENG") != CURLE_OK) {
				ERROR("Error %d setting CURLOPT_SSLCERTTYPE", result);
				result = CHANNEL_EINIT;
				goto cleanup;
			}
		}
        }

	/* Only use cafile when set, otherwise let curl use
	 * the default system location for cacert bundle
	 */
	if ((channel_data->cafile) &&
            (curl_easy_setopt(channel_curl->handle,
			       CURLOPT_CAINFO,
			       channel_data->cafile) != CURLE_OK)) {
		result = CHANNEL_EINIT;
		goto cleanup;
	}

	if (channel_data->debug) {
		(void)curl_easy_setopt(channel_curl->handle, CURLOPT_VERBOSE, 1L);
	}

	if ((!channel_data->nofollow) &&
	    (curl_easy_setopt(channel_curl->handle, CURLOPT_FOLLOWLOCATION, 1) !=
	     CURLE_OK)) {
		result = CHANNEL_EINIT;
		goto cleanup;
	}

	if (channel_data->received_headers || channel_data->headers) {
		if ((curl_easy_setopt(channel_curl->handle,
			      CURLOPT_HEADERFUNCTION,
			      channel_callback_headers) != CURLE_OK) ||
		    (curl_easy_setopt(channel_curl->handle, CURLOPT_HEADERDATA,
			      channel_data) != CURLE_OK)) {
			result = CHANNEL_EINIT;
			goto cleanup;
		}
	}

	if (channel_data->headers_to_send) {
		struct dict_entry *entry;
		char *header;
		LIST_FOREACH(entry, channel_data->headers_to_send, next)
		{
			if (ENOMEM_ASPRINTF ==
			    asprintf(&header, "%s: %s",
				     dict_entry_get_key(entry),
				     dict_entry_get_value(entry))) {
				result = CHANNEL_EINIT;
				goto cleanup;
			}
			if ((channel_curl->header = curl_slist_append(
				  channel_curl->header, header)) == NULL) {
				free(header);
				result = CHANNEL_EINIT;
				goto cleanup;
			}
			free(header);
		}
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

	if (channel_data->auth_token != NULL) {
		if (((channel_curl->header = curl_slist_append(
				channel_curl->header, channel_data->auth_token)) == NULL)) {
			result = CHANNEL_EINIT;
			goto cleanup;
		}
	}

	/*
	 * If requested, use a specific interface/IP address
	 */
	if (channel_data->iface != NULL) {
		if (curl_easy_setopt(channel_curl->handle,
		    CURLOPT_INTERFACE,
		    channel_data->iface) != CURLE_OK) {
			result = CHANNEL_EINIT;
			goto cleanup;
		}
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

	if (channel_data->range) {
		if (curl_easy_setopt(channel_curl->handle, CURLOPT_RANGE,
				     channel_data->range) != CURLE_OK) {
			ERROR("Bytes Range could not be set.");
			result = CHANNEL_EINIT;
			goto cleanup;
		}
	}

cleanup:
	return result;
}

static curl_off_t channel_get_total_download_size(channel_curl_t *this,
		const char *url)
{
	assert(this != NULL);
	assert(url  != NULL);

	curl_off_t size = -1;
	if (curl_easy_setopt(this->handle, CURLOPT_URL, url) != CURLE_OK ||
		curl_easy_setopt(this->handle, CURLOPT_NOBODY, 1L) != CURLE_OK ||
		curl_easy_perform(this->handle) != CURLE_OK)
		goto cleanup;

	if (curl_easy_getinfo(this->handle, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T,
			&size) != CURLE_OK)
		goto cleanup;

cleanup:
	if (curl_easy_setopt(this->handle, CURLOPT_NOBODY, 0L) != CURLE_OK) {
		ERROR("Failed to properly clean-up handle.");
		size = -1;
	}

	return size;
}

static channel_op_res_t channel_enable_download_progress_tracking(
		channel_curl_t *this,
		const char *url,
		download_callback_data_t *download_data)
{
	assert(url != NULL);
	assert(download_data != NULL);

	download_data->percent = 0;

	channel_op_res_t result = CHANNEL_OK;
	if ((download_data->total_download_size = channel_get_total_download_size(
				this, url)) <= 0) {
		result = CHANNEL_EINIT;
		goto cleanup;
	}

#if LIBCURL_VERSION_NUM >= 0x072000
	if ((curl_easy_setopt(this->handle, CURLOPT_XFERINFOFUNCTION,
			      channel_callback_xferinfo) != CURLE_OK) ||
	    (curl_easy_setopt(this->handle, CURLOPT_XFERINFODATA,
				  download_data) != CURLE_OK)) {
		result = CHANNEL_EINIT;
		goto cleanup;
	}
#else
	if ((curl_easy_setopt(this->handle, CURLOPT_PROGRESSFUNCTION,
			      channel_callback_xferinfo_legacy) != CURLE_OK) ||
	    (curl_easy_setopt(this->handle, CURLOPT_PROGRESSDATA,
				  download_data) != CURLE_OK)) {
		result = CHANNEL_EINIT;
		goto cleanup;
	}
#endif
	if (curl_easy_setopt(this->handle, CURLOPT_NOPROGRESS, 0L) != CURLE_OK) {
		result = CHANNEL_EINIT;
		goto cleanup;
	}
cleanup:
	return result;
}

static size_t read_callback(char *ptr, size_t size, size_t nmemb, void *data)
{
	channel_data_t *channel_data = (channel_data_t *)data;
	ssize_t nbytes;
	size_t n = 0;

	/*
	 * Check if data is stored in a buffer or should be read
	 * form the input pipe
	 */
	if (channel_data->request_body) {
		/* Check data to be sent */
		nbytes = strlen(channel_data->request_body) - channel_data->offs;

		if (!nbytes)
			return 0;

		n = min(nbytes, size * nmemb);

		memcpy(ptr, &channel_data->request_body[channel_data->offs], n);
		channel_data->offs += n;
	} else {
		if (nmemb * size > channel_data->upload_filesize)
			nbytes =  channel_data->upload_filesize;
		else
			nbytes = nmemb * size;

		nbytes = read(channel_data->read_fifo, ptr, nbytes);
		if (nbytes < 0) {
			if (errno == EAGAIN) {
				TRACE("READ EAGAIN");
				nbytes = 0;
			} else {
				ERROR("Cannot read from FIFO");
				return CURL_READFUNC_ABORT;
			}
		}

		n = nbytes / size;
		channel_data->upload_filesize -= nbytes;
	}

	return n;
}

static void channel_log_reply(channel_op_res_t result, channel_data_t *channel_data,
			      output_data_t *chunk)
{
	if (result != CHANNEL_OK) {
		ERROR("Channel operation returned HTTP error code %ld.",
			channel_data->http_response_code);
		switch (channel_data->http_response_code) {
			case 403:
			case 404:
			case 500:
				DEBUG("The error message is: '%s'", chunk ? chunk->memory : "N/A");
				break;
			default:
				break;
		}
		return;
	}
	if (channel_data->debug) {
		TRACE("Channel operation returned HTTP status code %ld.",
		      channel_data->http_response_code);
	}
}

static channel_op_res_t setup_reply_buffer(CURL *handle, write_callback_t *wrdata)
{
	wrdata->outdata->memory = NULL;
	wrdata->outdata->size = 0;

	if ((wrdata->outdata->memory = malloc(1)) == NULL) {
		ERROR("Channel buffer reservation failed with OOM.");
		return CHANNEL_ENOMEM;
	}
	*wrdata->outdata->memory = '\0';

	if ((curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION,
			      channel_callback_membuffer) != CURLE_OK) ||
	    (curl_easy_setopt(handle, CURLOPT_WRITEDATA, (void *)wrdata) != CURLE_OK)) {
		ERROR("Cannot setup memory buffer writer callback function.");
		return CHANNEL_EINIT;
	}

	return CHANNEL_OK;
}

static channel_op_res_t parse_reply(channel_data_t *channel_data, output_data_t *chunk)
{
	if (!chunk->memory) {
		ERROR("Channel reply buffer was not filled.");
		return CHANNEL_ENOMEM;
	}

	if (!chunk->size)
		return CHANNEL_OK;

	if (channel_data->format == CHANNEL_PARSE_JSON) {
		assert(channel_data->json_reply == NULL);
		enum json_tokener_error json_res;
		struct json_tokener *json_tokenizer = json_tokener_new();
		do {
			channel_data->json_reply = json_tokener_parse_ex(
			    json_tokenizer, chunk->memory, (int)chunk->size);
		} while ((json_res = json_tokener_get_error(json_tokenizer)) ==
			 json_tokener_continue);
		/* json_tokenizer is not used for json_tokener_error_desc, hence can be freed here. */
		json_tokener_free(json_tokenizer);
		if (json_res != json_tokener_success) {
			ERROR("Error while parsing channel's returned JSON data: %s",
			      json_tokener_error_desc(json_res));
			return CHANNEL_EBADMSG;
		}
	}

	if (channel_data->format == CHANNEL_PARSE_RAW) {
		/* strndup is strnlen + malloc + memcpy, seems more appropriate than just malloc + memcpy. */
		if ((channel_data->raw_reply = strndup(chunk->memory, chunk->size)) == NULL) {
			ERROR("Channel reply buffer memory allocation failed with OOM.");
			return CHANNEL_ENOMEM;
		}
	}

	if (channel_data->debug) {
		/* If reply is not \0 terminated, impose an upper bound. */
		TRACE("Got channel reply: %s", strndupa(chunk->memory, chunk->size));
	}
	return CHANNEL_OK;
}

static CURLcode channel_set_read_callback(channel_curl_t *handle, channel_data_t *channel_data)
{

	return curl_easy_setopt(handle, CURLOPT_READFUNCTION, read_callback) ||
		curl_easy_setopt(handle, CURLOPT_INFILESIZE_LARGE,
				  channel_data->request_body ? (curl_off_t)strlen(channel_data->request_body) : (curl_off_t)channel_data->upload_filesize) ||
		curl_easy_setopt(handle, CURLOPT_READDATA, channel_data);
}

static channel_op_res_t channel_post_method(channel_t *this, void *data, int method)
{
	channel_curl_t *channel_curl = this->priv;
	assert(data != NULL);
	assert(channel_curl->handle != NULL);

	channel_op_res_t result = CHANNEL_OK;
	channel_data_t *channel_data = (channel_data_t *)data;
	channel_data->offs = 0;
	output_data_t outdata = {};
	write_callback_t wrdata = { .this = this, .channel_data = channel_data, .outdata = &outdata };

	if ((result = channel_set_content_type(this, channel_data)) !=
	    CHANNEL_OK) {
		ERROR("Set content-type option failed.");
		goto cleanup_header;
	}

	if ((result = channel_set_options(this, channel_data)) != CHANNEL_OK) {
		ERROR("Set channel option failed.");
		goto cleanup_header;
	}

	if ((result = setup_reply_buffer(channel_curl->handle, &wrdata)) != CHANNEL_OK) {
		goto cleanup_header;
	}

	CURLcode curl_result = CURLE_OK;
	switch (method)  {
	case CHANNEL_PATCH:
	case CHANNEL_POST:
		if (method == CHANNEL_PATCH)
			curl_result = curl_easy_setopt(channel_curl->handle, CURLOPT_CUSTOMREQUEST, "PATCH");
		else
			curl_result = curl_easy_setopt(channel_curl->handle, CURLOPT_POST, 1L);

		curl_result |= curl_easy_setopt(channel_curl->handle,
					       CURLOPT_POSTFIELDS,
					       channel_data->request_body);
		if (channel_data->read_fifo)
			curl_result |= channel_set_read_callback(channel_curl->handle, channel_data);
		break;

	case CHANNEL_DELETE:
		curl_result = curl_easy_setopt(channel_curl->handle, CURLOPT_CUSTOMREQUEST, "DELETE");
		break;

	case CHANNEL_PUT:
		curl_result = curl_easy_setopt(channel_curl->handle,
						#if LIBCURL_VERSION_NUM >= 0x70C01
						CURLOPT_UPLOAD,
						#else
						CURLOPT_PUT,
						#endif
						1L) || channel_set_read_callback(channel_curl->handle, channel_data);
		break;
	}

	if (curl_result != CURLE_OK) {
		result = CHANNEL_EINIT;
		ERROR("Set %s channel method option failed.", method_desc[method]);
		goto cleanup_header;
	}

	if (channel_data->debug) {
		TRACE("%s to %s: %s", method_desc[method], channel_data->url, channel_data->request_body);
	}

	CURLcode curlrc = curl_easy_perform(channel_curl->handle);
	if (curlrc != CURLE_OK) {
		ERROR("Channel %s operation failed (%d): '%s'", method_desc[method], curlrc,
		      curl_easy_strerror(curlrc));
		result = channel_map_curl_error(curlrc);
		goto cleanup_header;
	}

	channel_log_effective_url(this);

	result = channel_map_http_code(this, &channel_data->http_response_code);

	if (channel_data->nocheckanswer)
		goto cleanup_header;

	channel_log_reply(result, channel_data, &outdata);

	if (result == CHANNEL_OK) {
	    result = parse_reply(channel_data, &outdata);
	}

cleanup_header:
	outdata.memory != NULL ? free(outdata.memory) : (void)0;
	curl_easy_reset(channel_curl->handle);
	curl_slist_free_all(channel_curl->header);
	channel_curl->header = NULL;

	return result;
}

channel_op_res_t channel_put(channel_t *this, void *data)
{
	assert(data != NULL);

	channel_data_t *channel_data = (channel_data_t *)data;

	channel_data->http_response_code = 0;
	switch (channel_data->method) {
	case CHANNEL_PUT:
	case CHANNEL_POST:
	case CHANNEL_PATCH:
	case CHANNEL_DELETE:
		return channel_post_method(this, data, channel_data->method);
	default:
		TRACE("Channel method (POST, PUT, PATCH) is not set !");
		return CHANNEL_EINIT;
	}
}

channel_op_res_t channel_put_file(channel_t *this, void *data)
{
	CURLcode curl_result = CURLE_OK;
	channel_curl_t *channel_curl = this->priv;
	assert(data != NULL);
	assert(channel_curl->handle != NULL);

	channel_op_res_t result = CHANNEL_OK;
	channel_data_t *channel_data = (channel_data_t *)data;
	channel_data->offs = 0;
	output_data_t outdata = {};
	write_callback_t wrdata = { .this = this, .channel_data = channel_data, .outdata = &outdata };

	if ((result = channel_set_content_type(this, channel_data)) !=
	    CHANNEL_OK) {
		ERROR("Set content-type option failed.");
		goto cleanup_header;
	}

	if ((result = channel_set_options(this, channel_data)) != CHANNEL_OK) {
		ERROR("Set channel option failed.");
		goto cleanup_header;
	}

	if ((result = setup_reply_buffer(channel_curl->handle, &wrdata)) != CHANNEL_OK) {
		goto cleanup_header;
	}

	if (!channel_data->method)
		channel_data->method = CHANNEL_POST;

	switch (channel_data->method)  {
	case CHANNEL_PATCH:
	case CHANNEL_POST:
		if (channel_data->method == CHANNEL_PATCH)
			curl_result |= curl_easy_setopt(channel_curl->handle, CURLOPT_CUSTOMREQUEST, "PATCH");
		else
			curl_result |= curl_easy_setopt(channel_curl->handle, CURLOPT_POST, 1L);

		break;

	case CHANNEL_PUT:
		curl_result |= curl_easy_setopt(channel_curl->handle,
						#if LIBCURL_VERSION_NUM >= 0x70C01
						CURLOPT_UPLOAD,
						#else
						CURLOPT_PUT,
						#endif
						1L);
		break;
	}

	curl_result |= channel_set_read_callback(channel_curl->handle, channel_data);
	if (curl_result != CURLE_OK) {
		result = CHANNEL_EINIT;
		ERROR("Set %s channel method option failed.", method_desc[channel_data->method]);
		goto cleanup_header;
	}

	CURLcode curlrc = curl_easy_perform(channel_curl->handle);
	if (curlrc != CURLE_OK) {
		ERROR("Channel %s operation failed (%d): '%s'", method_desc[channel_data->method], curlrc,
		      curl_easy_strerror(curlrc));
		result = channel_map_curl_error(curlrc);
		goto cleanup_header;
	}

	channel_log_effective_url(this);

	result = channel_map_http_code(this, &channel_data->http_response_code);

	if (channel_data->nocheckanswer)
		goto cleanup_header;

	channel_log_reply(result, channel_data, &outdata);

	if (result == CHANNEL_OK) {
	    result = parse_reply(channel_data, &outdata);
	}

cleanup_header:
	outdata.memory != NULL ? free(outdata.memory) : (void)0;
	curl_easy_reset(channel_curl->handle);
	curl_slist_free_all(channel_curl->header);
	channel_curl->header = NULL;

	return result;
}

channel_op_res_t channel_get_file(channel_t *this, void *data)
{
	channel_curl_t *channel_curl = this->priv;
	int file_handle = -1;
	struct swupdate_request req;
	assert(data != NULL);
	assert(channel_curl->handle != NULL);

	channel_op_res_t result = CHANNEL_OK;
	channel_data_t *channel_data = (channel_data_t *)data;
	channel_data->http_response_code = 0;

	if (channel_data->usessl) {
		memset(channel_data->sha1hash, 0x0, SWUPDATE_SHA_DIGEST_LENGTH * 2 + 1);
		channel_data->dgst = swupdate_HASH_init("sha1");
		if (!channel_data->dgst) {
			result = CHANNEL_EINIT;
			ERROR("Cannot initialize sha1 checksum context.");
			return result;
		}
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

	if ((result = channel_set_options(this, channel_data)) != CHANNEL_OK) {
		ERROR("Set channel option failed.");
		goto cleanup_header;
	}

	if (channel_data->max_download_speed &&
			curl_easy_setopt(channel_curl->handle,
				CURLOPT_MAX_RECV_SPEED_LARGE,
				channel_data->max_download_speed) != CURLE_OK) {
		ERROR("Set channel download speed limit failed.");
		result = CHANNEL_EINIT;
		goto cleanup_header;
	}

	download_callback_data_t download_data;
	download_data.source = channel_data->source;

	/*
	 * In case of range do not ask the server for file size
	 */
	if (!channel_data->range)  {
		if (channel_enable_download_progress_tracking(channel_curl,
								channel_data->url,
								&download_data) == CHANNEL_EINIT) {
			WARN("Failed to get total download size for URL %s.",
				channel_data->url);
	} else
		INFO("Total download size is %" CURL_FORMAT_CURL_OFF_TU " kB.",
			download_data.total_download_size / 1024);

	}

	if (curl_easy_setopt(channel_curl->handle, CURLOPT_CUSTOMREQUEST, "GET") !=
	    CURLE_OK) {
		ERROR("Set GET channel method option failed.");
		result = CHANNEL_EINIT;
		goto cleanup_header;
	}

	write_callback_t wrdata = { .this = this };
	wrdata.channel_data = channel_data;
	if (!channel_data->noipc) {
		swupdate_prepare_req(&req);
		req.dry_run = channel_data->dry_run;
		req.source = channel_data->source;
		if (channel_data->info) {
			strncpy(req.info, channel_data->info,
				sizeof(req.info) - 1 );
			req.len = strlen(channel_data->info);
		}
		for (int retries = 3; retries >= 0; retries--) {
			file_handle = ipc_inst_start_ext( &req, sizeof(struct swupdate_request));
			if (file_handle > 0)
				break;
			sleep(1);
		}
		if (file_handle < 0) {
			ERROR("Cannot open SWUpdate IPC stream: %s", strerror(errno));
			result = CHANNEL_EIO;
			goto cleanup_header;
		}
	}

	wrdata.output = file_handle;
	result_channel_callback_ipc = CHANNEL_OK;

	if ((curl_easy_setopt(channel_curl->handle, CURLOPT_WRITEFUNCTION,
			      channel_callback_ipc) != CURLE_OK) ||
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

	if (channel_data->cached_file) {

		total_bytes_downloaded = resume_cache_file(channel_data->cached_file,
							   &wrdata);
		if (total_bytes_downloaded > 0) {
			TRACE("Resume from cache file %s, restored %lld bytes",
				channel_data->cached_file,
				total_bytes_downloaded);
	                /*
			 * Simulate that a partial download was already done,
			 * and tune parameters if retries is not set
			 */
			channel_data->retries++;
			try_count++;
		}
	}

	/*
	 * If there is a cache file, read data from cache first
	 * and load from URL the remaining data
	 */
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
		if (result == CHANNEL_ENONET) {
			WARN("Lost connection. Retrying after %d seconds.",
					channel_data->retry_sleep);
			if (sleep(channel_data->retry_sleep) > 0) {
				TRACE("Channel's sleep got interrupted, "
					"retrying nonetheless now.");
			}
		} else if ((result != CHANNEL_OK) && (result != CHANNEL_EAGAIN)) {
			ERROR("Channel operation returned error (%d): '%s'",
			      curlrc, curl_easy_strerror(curlrc));
			goto cleanup_file;
		}

	#if LIBCURL_VERSION_NUM >= 0x73700
		curl_off_t bytes_downloaded;
		CURLcode resdlprogress = curl_easy_getinfo(
		    channel_curl->handle, CURLINFO_SIZE_DOWNLOAD_T,
		    &bytes_downloaded);
	#else
		double bytes_downloaded;
		CURLcode resdlprogress = curl_easy_getinfo(
		    channel_curl->handle, CURLINFO_SIZE_DOWNLOAD,
		    &bytes_downloaded);
	#endif
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

	result = channel_map_http_code(this, &channel_data->http_response_code);

	channel_log_reply(result, channel_data, NULL);

	if (result_channel_callback_ipc != CHANNEL_OK) {
		result = CHANNEL_EIO;
		goto cleanup_file;
	}

	if (channel_data->usessl) {
		unsigned char sha1hash[SWUPDATE_SHA_DIGEST_LENGTH];
		unsigned int md_len;
		(void)md_len;
		if (swupdate_HASH_final(channel_data->dgst, sha1hash, &md_len) != 1) {
			ERROR("Cannot compute checksum.");
			goto cleanup_file;
		}

		char sha1hexchar[3];
		for (int i = 0; i < SWUPDATE_SHA_DIGEST_LENGTH; i++) {
			sprintf(sha1hexchar, "%02x", sha1hash[i]);
			strcat(channel_data->sha1hash, sha1hexchar);
		}
	}

cleanup_file:
	/* NOTE ipc_end() calls close() but does not return its error code,
	 *      so use close() here directly to issue an error in case.
	 *      Also, for a given file handle, calling ipc_end() would make
	 *      no semantic sense. */
	if (file_handle >= 0 && close(file_handle) != 0) {
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
	assert(channel_curl->handle != NULL);

	channel_op_res_t result = CHANNEL_OK;
	channel_data_t *channel_data = (channel_data_t *)data;
	channel_data->http_response_code = 0;
	output_data_t outdata = {};
	write_callback_t wrdata = { .this = this, .channel_data = channel_data, .outdata = &outdata };

	if ((result = channel_set_content_type(this, channel_data)) !=
	    CHANNEL_OK) {
		ERROR("Set content-type option failed.");
		goto cleanup_header;
	}

	if ((result = channel_set_options(this, channel_data)) != CHANNEL_OK) {
		ERROR("Set channel option failed.");
		goto cleanup_header;
	}

	if (curl_easy_setopt(channel_curl->handle, CURLOPT_CUSTOMREQUEST, "GET") !=
	    CURLE_OK) {
		ERROR("Set GET channel method option failed.");
		result = CHANNEL_EINIT;
		goto cleanup_header;
	}

	if ((result = setup_reply_buffer(channel_curl->handle, &wrdata)) != CHANNEL_OK) {
		goto cleanup_header;
	}

	if (channel_data->debug) {
		DEBUG("Trying to GET %s", channel_data->url);
	}
	CURLcode curlrc = curl_easy_perform(channel_curl->handle);
	if (curlrc != CURLE_OK) {
		ERROR("Channel get operation failed (%d): '%s'", curlrc,
		      curl_easy_strerror(curlrc));
		result = channel_map_curl_error(curlrc);
		goto cleanup_header;
	}

	channel_log_effective_url(this);

	result = channel_map_http_code(this, &channel_data->http_response_code);

	if (channel_data->nocheckanswer)
		goto cleanup_header;

	channel_log_reply(result, channel_data, &outdata);

	if (result == CHANNEL_OK) {
	    result = parse_reply(channel_data, &outdata);
	}

cleanup_header:
	outdata.memory != NULL ? free(outdata.memory) : (void)0;
	curl_easy_reset(channel_curl->handle);
	curl_slist_free_all(channel_curl->header);
	channel_curl->header = NULL;

	return result;
}
