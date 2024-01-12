/*
 * (C) Copyright 2021
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

/*
 * This handler computes the difference between an artifact
 * and an image on the device, and download the missing chunks.
 * The resulting image is then passed to a chained handler for
 * installing.
 * The handler uses own properties and it shares th same
 * img struct with the chained handler. All other fields
 * in sw-description are reserved for the chain handler, that
 * works as if there is no delta handler in between.
 */

#include <stdbool.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <handler.h>
#include <signal.h>
#include <zck.h>
#include <zlib.h>
#include <util.h>
#include <pctl.h>
#include <pthread.h>
#include <fs_interface.h>
#include <sys/mman.h>
#include "delta_handler.h"
#include "multipart_parser.h"
#include "installer.h"
#include "zchunk_range.h"
#include "chained_handler.h"
#include "swupdate_image.h"

#define DEFAULT_MAX_RANGES	150	/* Apache has default = 200 */

const char *handlername = "delta";
void delta_handler(void);

/*
 * Structure passed to callbacks
 */
/*
 * state machine when answer from
 * server is parsed.
 */
typedef enum {
	NOTRUNNING,
	WAITING_FOR_HEADERS,
	WAITING_FOR_BOUNDARY,
	WAITING_FOR_FIRST_DATA,
	WAITING_FOR_DATA,
	END_TRANSFER
} dwl_state_t;

/*
 * There are two kind of answer from an HTTP Range request:
 * - if just one range is selected, the server sends a
 *   content-range header with the delivered bytes as
 *   <start>-<end>/<totalbytes>
 * - if multiple ranges are requested, the server sends
 *   a multipart answer and sends a header with
 *   Content-Type: multipart/byteranges; boundary=<boundary>
 */
typedef enum {
	NONE_RANGE,	/* Range not found in Headers */
	SINGLE_RANGE,
	MULTIPART_RANGE
} range_type_t;

struct dwlchunk {
	unsigned char *buf;
	size_t chunksize;
	size_t nbytes;
	bool completed;
};

struct hnd_priv {
	/* Attributes retrieved from sw-descritpion */
	char *url;			/* URL to get full ZCK file */
	char *srcdev;			/* device as source for comparison */
	char *chainhandler;		/* Handler to pass the decompressed image */
	struct chain_handler_data chain_handler_data;
	zck_log_type zckloglevel;	/* if found, set log level for ZCK to this */
	bool detectsrcsize;		/* if set, try to compute size of filesystem in srcdev */
	size_t srcsize;			/* Size of source */
	unsigned long max_ranges;	/* Max allowed ranges (configured via sw-description) */
	/* Data to be transferred to chain handler */
	struct img_type img;
	int fdout;
	int fdsrc;
	zckCtx *tgt;
	/* Structures for downloading chunks */
	bool dwlrunning;
	range_type_t range_type;	/* Single or multipart */
	char boundary[SWUPDATE_GENERAL_STRING_SIZE];
	int pipetodwl;			/* pipe to downloader process */
	dwl_state_t dwlstate;		/* for internal state machine */
	range_answer_t *answer;			/* data from downloader */
	uint32_t reqid;			/* Current request id to downloader */
	struct dwlchunk current;	/* Structure to collect data for working chunk */
	zckChunk *chunk;		/* Current chunk to be processed */
	size_t rangelen;		/* Value from Content-range header */
	size_t rangestart;		/* Value from Content-range header */
	bool content_range_received;	/* Flag to indicate that last header is content-range */
	bool error_in_parser;		/* Flag to report if an error occurred */
	multipart_parser *parser;	/* pointer to parser, allocated at any download */
	/* Some nice statistics */
	size_t bytes_to_be_reused;
	size_t bytes_to_download;
	size_t totaldwlbytes;		/* bytes downloaded, including headers */
	/* flags to improve logging */
	bool debugchunks;
};

static bool copy_existing_chunks(zckChunk **dstChunk, struct hnd_priv *priv);

/*
 * Callbacks for multipart parsing.
 */
static int network_process_data(multipart_parser* p, const char *at, size_t length)
{
	struct hnd_priv *priv = (struct hnd_priv *)multipart_parser_get_data(p);
	size_t nbytes = length;
	const char *bufSrc = at;
	int ret;

	/* Stop if previous error occurred */
	if (priv->error_in_parser)
		return -EFAULT;

	while (nbytes) {
		size_t to_be_filled = priv->current.chunksize - priv->current.nbytes;
		size_t tobecopied = min(nbytes, to_be_filled);
		memcpy(&priv->current.buf[priv->current.nbytes], bufSrc, tobecopied);
		priv->current.nbytes += tobecopied;
		nbytes -= tobecopied;
		bufSrc += tobecopied;
		/*
		 * Chunk complete, it must be copied
		 */
		if (priv->current.nbytes == priv->current.chunksize) {
			char *sha = zck_get_chunk_digest(priv->chunk);
			unsigned char hash[SHA256_HASH_LENGTH];	/* SHA-256 is 32 byte */
			ascii_to_hash(hash, sha);
			free(sha);

			if (priv->debugchunks)
				TRACE("Copying chunk %ld from NETWORK, size %ld",
					zck_get_chunk_number(priv->chunk),
					priv->current.chunksize);
			if (priv->current.chunksize != 0) {
				ret = copybuffer(priv->current.buf,
						 &priv->fdout,
						 priv->current.chunksize,
						 COMPRESSED_ZSTD,
						 hash,
						 0,
						 NULL,
						 NULL);
			} else
				ret = 0; /* skipping, nothing to be copied */
			/* Buffer can be discarged */
			free(priv->current.buf);
			priv->current.buf = NULL;
			/*
			 * if an error occurred, stops
			 */
			if (ret) {
				ERROR("copybuffer failed !");
				priv->error_in_parser = true;
				return -EFAULT;
			}
			/*
			 * Set the chunk as completed and switch to next one
			 */
			priv->chunk = zck_get_next_chunk(priv->chunk);
			if (!priv->chunk && nbytes > 0) {
				WARN("Still data in range, but no chunks anymore !");
				close(priv->fdout);
			}
			if (!priv->chunk)
				break;

			size_t current_chunk_size = zck_get_chunk_comp_size(priv->chunk);
			priv->current.buf = (unsigned char *)malloc(current_chunk_size);
			if (!priv->current.buf) {
				ERROR("OOM allocating new chunk %lu!", current_chunk_size);
				priv->error_in_parser = true;
				return -ENOMEM;
			}

			priv->current.nbytes = 0;
			priv->current.chunksize = current_chunk_size;
		}
	}
	return 0;
}

/*
 * This is called after headers are processed. Allocate a
 * buffer big enough to contain the next chunk to be processed
 */
static int multipart_data_complete(multipart_parser* p)
{
	struct hnd_priv *priv = (struct hnd_priv *)multipart_parser_get_data(p);
	size_t current_chunk_size;

	current_chunk_size = zck_get_chunk_comp_size(priv->chunk);
	priv->current.buf = (unsigned char *)malloc(current_chunk_size);
	priv->current.nbytes = 0;
	priv->current.chunksize = current_chunk_size;
	/*
	 * Buffer check should be done in each callback
	 */
	if (!priv->current.buf) {
		ERROR("OOM allocating new chunk !");
		return -ENOMEM;
	}

	return 0;
}

/*
 * This is called after a range is completed and before next range
 * is processed. Between two ranges, chunks are taken from SRC.
 * Checks which chunks should be copied and copy them until a chunk must
 * be retrieved from network
 */
static int multipart_data_end(multipart_parser* p)
{
	struct hnd_priv *priv = (struct hnd_priv *)multipart_parser_get_data(p);
	free(priv->current.buf);
	priv->current.buf = NULL;
	priv->content_range_received = true;
	copy_existing_chunks(&priv->chunk, priv);
	return 0;
}

/*
 * Set multipart parser callbacks.
 * No need at the moment to process multipart headers
 */
static multipart_parser_settings multipart_callbacks = {
	.on_part_data = network_process_data,
	.on_headers_complete = multipart_data_complete,
	.on_part_data_end = multipart_data_end
};

/*
 * Debug function to output all chunks and show if the chunk
 * can be copied from current software or must be downloaded
 */
static size_t get_total_size(zckCtx *zck, struct hnd_priv *priv) {
	zckChunk *iter = zck_get_first_chunk(zck);
	size_t pos = 0;
	priv->bytes_to_be_reused = 0;
	priv->bytes_to_download = 0;
	if (priv->debugchunks)
		TRACE("Index        Typ HASH %*c START(chunk) SIZE(uncomp) Pos(Device) SIZE(comp)",
			(((int)zck_get_chunk_digest_size(zck) * 2) - (int)strlen("HASH")), ' '
		);
	while (iter) {
		if (priv->debugchunks)
			TRACE("%12lu %s %s %12lu %12lu %12lu %12lu",
				zck_get_chunk_number(iter),
				zck_get_chunk_valid(iter) ? "SRC" : "DST",
				zck_get_chunk_digest_uncompressed(iter),
				zck_get_chunk_start(iter),
				zck_get_chunk_size(iter),
				pos,
				zck_get_chunk_comp_size(iter));

		pos += zck_get_chunk_size(iter);
		if (!zck_get_chunk_valid(iter)) {
			priv->bytes_to_download += zck_get_chunk_comp_size(iter);
		} else {
			priv->bytes_to_be_reused += zck_get_chunk_size(iter);
		}
		iter = zck_get_next_chunk(iter);
	}

	INFO("Total bytes to be reused     : %12lu\n", priv->bytes_to_be_reused);
	INFO("Total bytes to be downloaded : %12lu\n", priv->bytes_to_download);

	return pos;
}

/*
 * Get attributes from sw-description
 */
static int delta_retrieve_attributes(struct img_type *img, struct hnd_priv *priv) {
	if (!priv)
		return -EINVAL;

	priv->zckloglevel = ZCK_LOG_DDEBUG;
	priv->url = dict_get_value(&img->properties, "url");
	priv->srcdev = dict_get_value(&img->properties, "source");
	priv->chainhandler = dict_get_value(&img->properties, "chain");
	if (!priv->url || !priv->srcdev ||
		!priv->chainhandler || !strcmp(priv->chainhandler, handlername)) {
		ERROR("Wrong Attributes in sw-description: url=%s source=%s, handler=%s",
			priv->url, priv->srcdev, priv->chainhandler);
		free(priv->url);
		free(priv->srcdev);
		free(priv->chainhandler);
		return -EINVAL;
	}
	errno = 0;
	if (dict_get_value(&img->properties, "max-ranges"))
		priv->max_ranges = strtoul(dict_get_value(&img->properties, "max-ranges"), NULL, 10);
	if (errno || priv->max_ranges == 0)
		priv->max_ranges = DEFAULT_MAX_RANGES;

	char *srcsize;
	srcsize = dict_get_value(&img->properties, "source-size");
	if (srcsize) {
		if (!strcmp(srcsize, "detect"))
			priv->detectsrcsize = true;
		else
			priv->srcsize = ustrtoull(srcsize, NULL, 10);
	}

	char *zckloglevel = dict_get_value(&img->properties, "zckloglevel");
	if (!zckloglevel)
		return 0;
	if (!strcmp(zckloglevel, "debug"))
		priv->zckloglevel = ZCK_LOG_DEBUG;
	else if (!strcmp(zckloglevel, "info"))
		priv->zckloglevel = ZCK_LOG_INFO;
	else if (!strcmp(zckloglevel, "warn"))
		priv->zckloglevel = ZCK_LOG_WARNING;
	else if (!strcmp(zckloglevel, "error"))
		priv->zckloglevel = ZCK_LOG_ERROR;
	else if (!strcmp(zckloglevel, "none"))
		priv->zckloglevel = ZCK_LOG_NONE;

	char *debug = dict_get_value(&img->properties, "debug-chunks");
	if (debug) {
		priv->debugchunks = true;
	}

	return 0;
}

/*
 * Prepare a request for the chunk downloader process
 * It fills a range_request structure with data for the
 * connection
 */

static range_request_t *prepare_range_request(const char *url, const char *range, size_t *len)
{
	range_request_t *req = NULL;

	if (!url || !len)
		return NULL;

	if (strlen(range) > RANGE_PAYLOAD_SIZE - 1) {
		ERROR("RANGE request too long !");
		return NULL;
	}
	req = (range_request_t *)calloc(1, sizeof(*req));
	if (req) {
		req->id = rand();
		req->type = RANGE_GET;
		req->urllen = strlen(url);
		req->rangelen = strlen(range);
		if (req->urllen + req->rangelen > RANGE_PAYLOAD_SIZE - 2) {
			ERROR("Range exceeds maximum %d bytes !", RANGE_PAYLOAD_SIZE - 1);
			free(req);
			return NULL;
		}
		strcpy(req->data, url);
		strcpy(&req->data[strlen(url) + 1], range);
	} else {
		ERROR("OOM preparing internal IPC !");
		return NULL;
	}

	return req;
}

/*
 * ZCK and SWUpdate have different levels for logging
 * so map them
 */
static zck_log_type map_swupdate_to_zck_loglevel(LOGLEVEL level) {

	switch (level) {
	case OFF:
		return ZCK_LOG_NONE;
	case ERRORLEVEL:
		return ZCK_LOG_ERROR;
	case WARNLEVEL:
		return ZCK_LOG_WARNING;
	case INFOLEVEL:
		return ZCK_LOG_INFO;
	case TRACELEVEL:
		return ZCK_LOG_DEBUG;
	case DEBUGLEVEL:
		return ZCK_LOG_DDEBUG;
	}
	return ZCK_LOG_ERROR;
}

static LOGLEVEL map_zck_to_swupdate_loglevel(zck_log_type lt) {
	switch (lt) {
	case ZCK_LOG_NONE:
		return OFF;
	case ZCK_LOG_ERROR:
		return ERRORLEVEL;
	case ZCK_LOG_WARNING:
		return WARNLEVEL;
	case ZCK_LOG_INFO:
		return INFOLEVEL;
	case ZCK_LOG_DEBUG:
		return TRACELEVEL;
	case ZCK_LOG_DDEBUG:
		return DEBUGLEVEL;
	}
	return loglevel;
}

/*
 * Callback for ZCK to send ZCK logs to SWUpdate instead of writing
 * into a file
 */
static void zck_log_toswupdate(const char *function, zck_log_type lt,
				const char *format, va_list args) {
	LOGLEVEL l = map_zck_to_swupdate_loglevel(lt);
	char buf[NOTIFY_BUF_SIZE];
	int pos;

	pos = snprintf(buf, NOTIFY_BUF_SIZE - 1, "(%s) ", function);
	vsnprintf(buf + pos, NOTIFY_BUF_SIZE - 1 - pos, format, args);

	switch(l) {
	case ERRORLEVEL:
		ERROR("%s", buf);
		return;
	case WARNLEVEL:
		WARN("%s", buf);
		return;
	case INFOLEVEL:
		INFO("%s", buf);
		return;
	case TRACELEVEL:
		TRACE("%s", buf);
		return;
	case DEBUGLEVEL:
		TRACE("%s", buf);
		return;
	default:
		return;
	}
}

/*
 * Create a zck Index from a file
 */
static bool create_zckindex(zckCtx *zck, int fd, size_t maxbytes)
{
	const size_t bufsize = 16384;
	char *buf = malloc(bufsize);
	ssize_t n;
	int ret;

	if (!buf) {
		ERROR("OOM creating temporary buffer");
		return false;
	}
	while ((n = read(fd, buf, bufsize)) > 0) {
		ret = zck_write(zck, buf, n);
		if (ret < 0) {
			ERROR("ZCK returns %s", zck_get_error(zck));
			free(buf);
			return false;
		}
		if (maxbytes && n > maxbytes)
			break;
	}

	free(buf);

	return true;
}

/*
 * Chunks must be retrieved from network, prepare an send
 * a request for the downloader
 */
static bool trigger_download(struct hnd_priv *priv)
{
	range_request_t *req = NULL;
	zckCtx *tgt = priv->tgt;
	size_t reqlen;
	zck_range *range;
	char *http_range;
	bool status = true;


	priv->boundary[0] = '\0';

	range = zchunk_get_missing_range(tgt, priv->chunk, priv->max_ranges);
	if (!range)
		return false;
	http_range = zchunk_get_range_char(range);
	TRACE("Range request : %s", http_range);

	req = prepare_range_request(priv->url, http_range, &reqlen);
	if (!req) {
		ERROR(" Internal chunk request cannot be prepared");
		free(range);
		free(http_range);
		return false;
	}

	/* Store request id to compare later */
	priv->reqid = req->id;
	priv->range_type = NONE_RANGE;

	if (write(priv->pipetodwl, req, sizeof(*req)) != sizeof(*req)) {
		ERROR("Cannot write all bytes to pipe");
		status = false;
	}

	free(req);
	free(range);
	free(http_range);
	priv->dwlrunning = true;
	return status;
}

/*
 * drop all temporary data collected during download
 */
static void dwl_cleanup(struct hnd_priv *priv)
{
	multipart_parser_free(priv->parser);
	priv->parser = NULL;
}

static bool read_and_validate_package(struct hnd_priv *priv)
{
	ssize_t nbytes = sizeof(range_answer_t);
	range_answer_t *answer;
	int count = -1;
	uint32_t crc;

	do {
		count++;
		if (count == 1)
			DEBUG("id does not match in IPC, skipping..");

		char *buf = (char *)priv->answer;
		do {
			ssize_t ret;
			ret = read(priv->pipetodwl, buf, sizeof(range_answer_t));
			if (ret < 0)
				return false;
			buf += ret;
			nbytes -= ret;
		} while (nbytes > 0);
		answer = priv->answer;

		if (nbytes < 0)
			return false;
	} while (answer->id != priv->reqid);


	if (answer->type == RANGE_ERROR) {
	    ERROR("Transfer was unsuccessful, aborting...");
	    priv->dwlrunning = false;
	    dwl_cleanup(priv);
	    return false;
	}

	if (answer->type == RANGE_DATA) {
		crc = crc32(0, (unsigned char *)answer->data, answer->len);
		if (crc != answer->crc) {
			ERROR("Corrupted package received !");
			exit(1);
			return false;
		}
	}

	priv->totaldwlbytes += answer->len;

	return true;
}

/*
 * This is called to parse the HTTP headers
 * It searches for content-ranges and select a SINGLE or
 * MULTIPARTS answer.
 */
static bool parse_headers(struct hnd_priv *priv)
{
	int nconv;
	char *header = NULL, *value = NULL, *boundary_string = NULL;
	char **pair;
	int cnt;

	range_answer_t *answer = priv->answer;
	answer->data[answer->len] = '\0';
	/* Converto to lower case to make comparison easier */
	string_tolower(answer->data);

	/* Check for multipart */
	nconv = sscanf(answer->data, "%ms %ms %ms", &header, &value, &boundary_string);

	if (nconv == 3) {
		if (!strncmp(header, "content-type", strlen("content-type")) &&
			!strncmp(boundary_string, "boundary", strlen("boundary"))) {
			pair = string_split(boundary_string, '=');
			cnt = count_string_array((const char **)pair);
			if (cnt == 2) {
				memset(priv->boundary, '-', 2);
				strlcpy(&priv->boundary[2], pair[1], sizeof(priv->boundary) - 2);
				priv->range_type = MULTIPART_RANGE;
			}
			free_string_array(pair);
		}

		if (!strncmp(header, "content-range", strlen("content-range")) &&
		    !strncmp(value, "bytes", strlen("bytes"))) {
			pair = string_split(boundary_string, '-');
			priv->range_type = SINGLE_RANGE;
			size_t start = strtoul(pair[0], NULL, 10);
			size_t end = strtoul(pair[1], NULL, 10);
			free_string_array(pair);
			priv->rangestart = start;
			priv->rangelen = end - start;
		}
		free(header);
		free(value);
		free(boundary_string);
	} else if (nconv == 1) {
		free(header);
	} else if (nconv == 2) {
		free(header);
		free(value);
	}

	return true;
}

static bool search_boundary_in_body(struct hnd_priv *priv)
{
	char *s;
	range_answer_t *answer = priv->answer;
	size_t i;

	if (priv->range_type == NONE_RANGE) {
		ERROR("Malformed body, no boundary found");
		return false;
	}

	if (priv->range_type == SINGLE_RANGE) {
		/* Body contains just one range, it is data, do nothing */
		return true;
	}
	s = answer->data;
	for (i = 0; i < answer->len; i++, s++) {
		if (!strncmp(s, priv->boundary, strlen(priv->boundary))) {
			DEBUG("Boundary found in body");
			/* Reset buffer to start from here */
			if (i != 0)
				memcpy(answer->data, s, answer->len - i);
			answer->len -=i;
			return true;
		}
	}

	return false;
}

static bool fill_buffers_list(struct hnd_priv *priv)
{
	range_answer_t *answer = priv->answer;
	/*
	 * If there is a single range, all chunks
	 * are consecutive. Same processing can be done
	 * as with multipart and data is received.
	 */
	if (priv->range_type == SINGLE_RANGE) {
		return network_process_data(priv->parser, answer->data, answer->len) == 0;
	}

	multipart_parser_execute(priv->parser, answer->data, answer->len);

	return true;
}

/*
 * copy_network_chunk() retrieves chunks from network and triggers
 * a network transfer if no one is running.
 * It collects data in a buffer until the chunk is fully
 * downloaded, and then copies to the pipe to the installer thread
 * starting the chained handler.
 */
static bool copy_network_chunks(zckChunk **dstChunk, struct hnd_priv *priv)
{
	range_answer_t *answer;

	priv->chunk = *dstChunk;
	priv->error_in_parser = false;
	while (1) {
		switch (priv->dwlstate) {
		case NOTRUNNING:
			if (!trigger_download(priv))
				return false;
			priv->dwlstate = WAITING_FOR_HEADERS;
			break;
		case WAITING_FOR_HEADERS:
			if (!read_and_validate_package(priv))
				return false;
			answer = priv->answer;
			if (answer->type == RANGE_HEADERS) {
				if (!parse_headers(priv)) {
					return false;
				}
			}
			if ((answer->type == RANGE_DATA)) {
				priv->dwlstate = WAITING_FOR_BOUNDARY;
			}
			break;
		case WAITING_FOR_BOUNDARY:
			/*
			 * Not needed to read data because package
			 * was already written as last step in WAITING_FOR_HEADERS
			 */
			if (!search_boundary_in_body(priv))
				return false;
			priv->parser = multipart_parser_init(priv->boundary,
							     &multipart_callbacks);
			multipart_parser_set_data(priv->parser, priv);
			priv->dwlstate = WAITING_FOR_FIRST_DATA;
			break;
		case WAITING_FOR_FIRST_DATA:
			if (priv->range_type == SINGLE_RANGE &&
				multipart_data_complete(priv->parser) != 0)
				return false;

			if (!fill_buffers_list(priv))
				return false;
			priv->dwlstate = WAITING_FOR_DATA;
			break;
		case WAITING_FOR_DATA:
			if (!read_and_validate_package(priv))
				return false;
			answer = priv->answer;
			if ((answer->type == RANGE_COMPLETED)) {
				priv->dwlstate = END_TRANSFER;
			} else if (!fill_buffers_list(priv))
				return false;
			break;
		case END_TRANSFER:
			if (priv->range_type == SINGLE_RANGE)
				multipart_data_end(priv->parser);
			dwl_cleanup(priv);
			priv->dwlstate = NOTRUNNING;
			*dstChunk = priv->chunk;
			return !priv->error_in_parser;
		}
	}

	return !priv->error_in_parser;
}

/*
 * This writes a chunk from an existing copy on the source path
 * The chunk to be copied is retrieved via zck_get_src_chunk()
 */
static bool copy_existing_chunks(zckChunk **dstChunk, struct hnd_priv *priv)
{
	unsigned long offset = 0;
	uint32_t checksum;
	int ret;
	unsigned char hash[SHA256_HASH_LENGTH];

	while (*dstChunk && zck_get_chunk_valid(*dstChunk)) {
		zckChunk *chunk	= zck_get_src_chunk(*dstChunk);
		size_t len = zck_get_chunk_size(chunk);
		size_t start = zck_get_chunk_start(chunk);
		char *sha = zck_get_chunk_digest_uncompressed(chunk);
		if (!len) {
			*dstChunk = zck_get_next_chunk(*dstChunk);
			continue;
		}
		if (!sha) {
			ERROR("Cannot get hash for chunk %ld", zck_get_chunk_number(chunk));
			return false;
		}
		if (lseek(priv->fdsrc, start, SEEK_SET) < 0) {
			ERROR("Seeking source file at %lu", start);
			free(sha);
			return false;
		}

		ascii_to_hash(hash, sha);

		if (priv->debugchunks)
			TRACE("Copying chunk %ld from SRC %ld, start %ld size %ld",
				zck_get_chunk_number(*dstChunk),
				zck_get_chunk_number(chunk),
				start,
				len);
		ret = copyfile(priv->fdsrc, &priv->fdout, len, &offset, 0, 0, COMPRESSED_FALSE,
				&checksum, hash, false, NULL, NULL);

		free(sha);
		if (ret)
			return false;

		*dstChunk = zck_get_next_chunk(*dstChunk);
	}
	return true;
}

#define PIPE_READ  0
#define PIPE_WRITE 1
/*
 * Handler entry point
 */
static int install_delta(struct img_type *img,
	void __attribute__ ((__unused__)) *data)
{
	struct hnd_priv *priv;
	int ret = -1;
	int dst_fd = -1, in_fd = -1, mem_fd = -1;
	zckChunk *iter;
	zckCtx *zckSrc = NULL, *zckDst = NULL;
	char *FIFO = NULL;
	pthread_t chain_handler_thread_id;
	int pipes[2];

	/*
	 * No streaming allowed
	 */
	if (img->install_directly) {
		ERROR("Do not set installed-directly with delta, the header cannot be streamed");
		return -EINVAL;
	}

	/*
	 * Initialize handler data
	 */
	priv = (struct hnd_priv *)calloc(1, sizeof(*priv));
	if (!priv) {
		ERROR("OOM when allocating handler data !");
		return -ENOMEM;
	}
	priv->answer = (range_answer_t *)malloc(sizeof(*priv->answer));
	if (!priv->answer) {
		ERROR("OOM when allocating buffer !");
		free(priv);
		return -ENOMEM;
	}

	/*
	 * Read setup from sw-description
	 */
	if (delta_retrieve_attributes(img, priv)) {
		ret = -EINVAL;
		goto cleanup;
	}


	priv->pipetodwl = pctl_getfd_from_type(SOURCE_CHUNKS_DOWNLOADER);

	if (priv->pipetodwl < 0) {
		ERROR("Chunks dowbnloader is not running, delta update not available !");
		ret = -EINVAL;
		goto cleanup;
	}

	if (pipe(pipes) < 0) {
		ERROR("Could not create pipes for chained handler, existing...");
		ret = -EFAULT;
		goto cleanup;
	}
	/*
	 * Open files
	 */
	dst_fd = open("/dev/null", O_TRUNC | O_WRONLY | O_CREAT, 0666);
	if (dst_fd < 0) {
		ERROR("/dev/null not present or cannot be opened, aborting...");
		goto cleanup;
	}

	if (priv->detectsrcsize) {
#if defined(CONFIG_DISKFORMAT)
		char *filesystem = diskformat_fs_detect(priv->srcdev);
		if (filesystem) {
			char* DATADST_DIR;
			if (asprintf(&DATADST_DIR, "%s%s", get_tmpdir(), DATADST_DIR_SUFFIX) != -1)  {
				if (!swupdate_mount(priv->srcdev, DATADST_DIR, filesystem)) {
					struct statvfs vfs;
					if (!statvfs(DATADST_DIR, &vfs)) {
						TRACE("Detected filesystem %s, block size : %lu, %lu blocks =  %lu size",
						       filesystem, vfs.f_frsize, vfs.f_blocks, vfs.f_frsize * vfs.f_blocks);
						priv->srcsize = vfs.f_frsize * vfs.f_blocks;
					}
					swupdate_umount(DATADST_DIR);
				}
				free(DATADST_DIR);
			}
			free(filesystem);
		}
#else
		WARN("SWUPdate not compiled with DISKFORMAT, skipping size detection..");
#endif
	}

	in_fd = open(priv->srcdev, O_RDONLY);
	if(in_fd < 0) {
		ERROR("Unable to open Source : %s for reading", priv->srcdev);
		goto cleanup;
	}

	/*
	 * Set ZCK log level
	 */
	zck_set_log_level(priv->zckloglevel >= 0 ?
				priv->zckloglevel : map_swupdate_to_zck_loglevel(loglevel));
	zck_set_log_callback(zck_log_toswupdate);

	/*
	 * Initialize zck context for source and destination
	 * source : device / file of current software
	 * dst : final software to be installed
	 */
	zckSrc = zck_create();
	if (!zckSrc) {
		ERROR("Cannot create ZCK Source %s",  zck_get_error(NULL));
		zck_clear_error(NULL);
		goto cleanup;
	}
	zckDst = zck_create();
	if (!zckDst) {
		ERROR("Cannot create ZCK Destination %s",  zck_get_error(NULL));
		zck_clear_error(NULL);
		goto cleanup;
	}

	/*
	 * Prepare zckSrc for writing: the ZCK header must be computed from
	 * the running source
	 */
	if(!zck_init_write(zckSrc, dst_fd)) {
		ERROR("Cannot initialize ZCK for writing (%s), aborting..",
			zck_get_error(zckSrc));
		goto cleanup;
	}

	mem_fd = memfd_create("zchunk header", 0);
	if (mem_fd == -1) {
		ERROR("Cannot create memory file: %s", strerror(errno));
		goto cleanup;
	}

	ret = copyfile(img->fdin,
		&mem_fd,
		img->size,
		(unsigned long *)&img->offset,
		img->seek,
		0,
		img->compressed,
		&img->checksum,
		img->sha256,
		img->is_encrypted,
		img->ivt_ascii,
		NULL);

	if (ret != 0) {
		ERROR("Error %d copying zchunk header, aborting.", ret);
		goto cleanup;
	}

	if (lseek(mem_fd, 0, SEEK_SET) < 0) {
		ERROR("Seeking start of memory file");
		goto cleanup;
	}

	if (!zck_init_read(zckDst, mem_fd)) {
		ERROR("Unable to read ZCK header from %s : %s",
			img->fname,
			zck_get_error(zckDst));
		goto cleanup;
	}

	TRACE("ZCK Header read successfully from SWU, creating header from %s",
		priv->srcdev);
	/*
	 * Now read completely source and generate the index file
	 * with hashes for the uncompressed data
	 */
	if (!zck_set_ioption(zckSrc, ZCK_UNCOMP_HEADER, 1)) {
		ERROR("%s\n", zck_get_error(zckSrc));
		goto cleanup;
	}
	if (!zck_set_ioption(zckSrc, ZCK_COMP_TYPE, ZCK_COMP_NONE)) {
		ERROR("Error setting ZCK_COMP_NONE %s\n", zck_get_error(zckSrc));
		goto cleanup;
	}
	if (!zck_set_ioption(zckSrc, ZCK_HASH_CHUNK_TYPE, ZCK_HASH_SHA256)) {
		ERROR("Error setting HASH Type %s\n", zck_get_error(zckSrc));
		goto cleanup;
	}
	if (!zck_set_ioption(zckSrc, ZCK_NO_WRITE, 1)) {
		WARN("ZCK does not support NO Write, use huge amount of RAM %s\n", zck_get_error(zckSrc));
	}

	if (!create_zckindex(zckSrc, in_fd, priv->srcsize)) {
		WARN("ZCK Header form %s cannot be created, fallback to full download",
			priv->srcdev);
	} else {
		zck_generate_hashdb(zckSrc);
		zck_find_matching_chunks(zckSrc, zckDst);
	}

	size_t uncompressed_size = get_total_size(zckDst, priv);
	INFO("Size of artifact to be installed : %lu", uncompressed_size);

	/*
	 * Everything checked: now starts to combine
	 * source data and ranges from server
	 */


	/* Overwrite some parameters for chained handler */
	struct chain_handler_data *priv_hnd;
	priv_hnd = &priv->chain_handler_data;
	memcpy(&priv_hnd->img, img, sizeof(*img));
	priv_hnd->img.compressed = COMPRESSED_FALSE;
	priv_hnd->img.size = uncompressed_size;
	memset(priv_hnd->img.sha256, 0, SHA256_HASH_LENGTH);
	strlcpy(priv_hnd->img.type, priv->chainhandler, sizeof(priv_hnd->img.type));
	priv_hnd->img.fdin = pipes[PIPE_READ];
	/* zchunk files are not encrypted, CBC is not suitable for range download */
	priv_hnd->img.is_encrypted = false;

	signal(SIGPIPE, SIG_IGN);

	chain_handler_thread_id = start_thread(chain_handler_thread, priv_hnd);
	wait_threads_ready();

	priv->fdout = pipes[PIPE_WRITE];

	ret = 0;

	iter = zck_get_first_chunk(zckDst);
	bool success;
	priv->tgt = zckDst;
	priv->fdsrc = in_fd;
	while (iter) {
		if (zck_get_chunk_valid(iter)) {
			success = copy_existing_chunks(&iter, priv);
		} else {
			success = copy_network_chunks(&iter, priv);
		}
		if (!success) {
			ERROR("Delta Update fails : aborting");
			ret = -1;
			goto cleanup;
		}
	}

	close(priv->fdout);

	INFO("Total downloaded data : %ld bytes", priv->totaldwlbytes);

	void *status;
	ret = pthread_join(chain_handler_thread_id, &status);
	if (ret) {
		ERROR("return code from pthread_join() is %d", ret);
	}
	ret = (unsigned long)status;
	TRACE("Chained handler returned %d", ret);

cleanup:
	if (zckSrc) zck_free(&zckSrc);
	if (zckDst) zck_free(&zckDst);
	if (dst_fd >= 0) close(dst_fd);
	if (in_fd >= 0) close(in_fd);
	if (mem_fd >= 0) close(mem_fd);
	if (FIFO) {
		unlink(FIFO);
		free(FIFO);
	}
	if (priv->answer) free(priv->answer);
	free(priv);
	return ret;
}

__attribute__((constructor))
void delta_handler(void)
{
	register_handler(handlername, install_delta,
				IMAGE_HANDLER | FILE_HANDLER, NULL);
}
