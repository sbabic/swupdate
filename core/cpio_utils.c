/*
 * (C) Copyright 2012
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#include <stdbool.h>
#include <stdlib.h>
#include <inttypes.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#ifdef CONFIG_GUNZIP
#include <zlib.h>
#endif
#ifdef CONFIG_ZSTD
#include <zstd.h>
#endif

#include "generated/autoconf.h"
#include "cpiohdr.h"
#include "swupdate.h"
#include "util.h"
#include "sslapi.h"
#include "progress.h"

#define MODULE_NAME "cpio"

#define BUFF_SIZE	 16384

#define NPAD_BYTES(o) ((4 - (o % 4)) % 4)

typedef enum {
	INPUT_FROM_FD,
	INPUT_FROM_MEMORY
} input_type_t;

int get_cpiohdr(unsigned char *buf, struct filehdr *fhdr)
{
	struct new_ascii_header *cpiohdr;

	if (!buf || !fhdr)
		return -EINVAL;

	cpiohdr = (struct new_ascii_header *)buf;
	if (!strncmp(cpiohdr->c_magic, "070701", 6))
		fhdr->format = CPIO_NEWASCII;
	else if (!strncmp(cpiohdr->c_magic, "070702", 6))
		fhdr->format = CPIO_CRCASCII;
	else {
		ERROR("CPIO Format not recognized: magic not found");
		return -EINVAL;
	}
	fhdr->size = FROM_HEX(cpiohdr->c_filesize);
	fhdr->namesize = FROM_HEX(cpiohdr->c_namesize);
	fhdr->chksum = FROM_HEX(cpiohdr->c_chksum);

	return 0;
}

static int fill_buffer(int fd, unsigned char *buf, unsigned int nbytes, unsigned long *offs,
	uint32_t *checksum, void *dgst)
{
	ssize_t len;
	unsigned long count = 0;
	int i;

	while (nbytes > 0) {
		len = read(fd, buf, nbytes);
		if (len < 0) {
			ERROR("Failure in stream %d: %s", fd, strerror(errno));
			return -EFAULT;
		}
		if (len == 0) {
			return 0;
		}
		if (checksum)
			for (i = 0; i < len; i++)
				*checksum += buf[i];

		if (dgst) {
			if (swupdate_HASH_update(dgst, buf, len) < 0)
				return -EFAULT;
		}
		buf += len;
		count += len;
		nbytes -= len;
		*offs += len;
	}

	return count;
}

/*
 * Read padding that could exists between the cpio trailer and the end-of-file.
 * cpio aligns the file to 512 bytes
 */
void extract_padding(int fd)
{
    int padding;
    ssize_t len;
	unsigned char buf[512];
    int old_flags;
    struct pollfd pfd;
    int retval;

    if (fd < 0)
        return;

    old_flags = fcntl(fd, F_GETFL);
    if (old_flags < 0)
        return;
    fcntl(fd, F_SETFL, old_flags | O_NONBLOCK);

    pfd.fd = fd;
    pfd.events = POLLIN;

    padding = 512;

    TRACE("Expecting up to 512 padding bytes at end-of-file");
    do {
        retval = poll(&pfd, 1, 1000);
        if (retval < 0) {
            DEBUG("Failure while waiting on fd %d: %s", fd, strerror(errno));
            fcntl(fd, F_SETFL, old_flags);
            return;
        }
        len = read(fd, buf, padding);
        if (len < 0) {
            DEBUG("Failure while reading padding %d: %s", fd, strerror(errno));
            fcntl(fd, F_SETFL, old_flags);
            return;
        }
        padding -= len;
    } while (len > 0 && padding > 0);

    fcntl(fd, F_SETFL, old_flags);
    return;
}

/*
 * Export the copy_write{,_*} functions to be used in other modules
 * for copying a buffer to a file.
 */
int copy_write(void *out, const void *buf, size_t len)
{
	ssize_t ret;
	int fd;

	if (!out) {
		ERROR("Output file descriptor invalid !");
		return -1;
	}

	fd = *(int *)out;

	while (len) {
		errno = 0;
		ret = write(fd, buf, len);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			ERROR("cannot write %" PRIuPTR " bytes: %s", len, strerror(errno));
			return -1;
		}

		if (ret == 0) {
			ERROR("cannot write %" PRIuPTR " bytes: %s", len, strerror(errno));
			return -1;
		}

		len -= ret;
		buf += ret;
	}

	return 0;
}

#if defined(__FreeBSD__)
/*
 * FreeBSD likes to have multiples of 512 bytes written
 * to a device node, hence slice the buffer in palatable
 * chunks assuming that only the last written buffer's
 * length is smaller than cpio_utils.c's CPIO_BUFFER_SIZE and
 * doesn't satisfy length % 512 == 0.
 */
int copy_write_padded(void *out, const void *buf, size_t len)
{
	if (len % 512 == 0) {
		return copy_write(out, buf, len);
	}

	uint8_t buffer[512] = { 0 };
	int chunklen = len - (len % 512);
	int res = copy_write(out, buf, chunklen);
	if (res != 0) {
		return res;
	}
	memcpy(&buffer, buf+chunklen, len-chunklen);
	return copy_write(out, buffer, 512);
}
#endif

/*
 * Pipeline description
 *
 * Any given step has an input buffer and an output buffer. If output data is
 * pending, it is immediately returned to the downstream step. If the output
 * buffer is empty, more input data is processed. If the input buffer is empty,
 * data is pulled from the upstream step. When no more data can be produced,
 * zero is returned.
 */

typedef int (*PipelineStep)(void *state, void *buffer, size_t size);

struct InputState
{
	int fdin;
	input_type_t source;
	unsigned char *inbuf;
	size_t pos;
	size_t nbytes;
	unsigned long *offs;
	void *dgst;	/* use a private context for HASH */
	uint32_t checksum;
};

static int input_step(void *state, void *buffer, size_t size)
{
	struct InputState *s = (struct InputState *)state;
	int ret = 0;
	if (size >= s->nbytes) {
		size = s->nbytes;
	}
	switch (s->source) {
	case INPUT_FROM_FD:
		ret = fill_buffer(s->fdin, buffer, size, s->offs, &s->checksum, s->dgst);
		if (ret < 0) {
			return ret;
		}
		break;
	case INPUT_FROM_MEMORY:
		memcpy(buffer, &s->inbuf[s->pos], size);
		if (s->dgst) {
			if (swupdate_HASH_update(s->dgst, &s->inbuf[s->pos], size) < 0)
				return -EFAULT;
		}
		ret = size;
		s->pos += size;
		break;
	}
	s->nbytes -= ret;
	return ret;
}

struct DecryptState
{
	PipelineStep upstream_step;
	void *upstream_state;

	void *dcrypt;	/* use a private context for decryption */
	uint8_t input[BUFF_SIZE];
	uint8_t output[BUFF_SIZE + AES_BLK_SIZE];
	int outlen;
	bool eof;
};

static int decrypt_step(void *state, void *buffer, size_t size)
{
	struct DecryptState *s = (struct DecryptState *)state;
	int ret;
	int inlen;

	if (s->outlen != 0) {
		if ((int)size > s->outlen) {
			size = s->outlen;
		}
		memcpy(buffer, s->output, size);
		s->outlen -= size;
		memmove(s->output, s->output + size, s->outlen);
		return size;
	}

	ret = s->upstream_step(s->upstream_state, s->input, sizeof s->input);
	if (ret < 0) {
		return ret;
	}

	inlen = ret;

	if (!s->eof) {
		if (inlen != 0) {
			ret = swupdate_DECRYPT_update(s->dcrypt,
				s->output, &s->outlen, s->input, inlen);
		}
		if (inlen == 0 || s->outlen == 0) {
			/*
			 * Finalise the decryption. Further plaintext bytes may
			 * be written at this stage.
			 */
			ret = swupdate_DECRYPT_final(s->dcrypt,
				s->output, &s->outlen);
			s->eof = true;
		}
		if (ret < 0) {
			return ret;
		}
	}

	if (s->outlen != 0) {
		if ((int)size > s->outlen) {
			size = s->outlen;
		}
		memcpy(buffer, s->output, size);
		s->outlen -= size;
		memmove(s->output, s->output + size, s->outlen);
		return size;
	}

	return 0;
}

#if defined(CONFIG_GUNZIP) || defined(CONFIG_ZSTD)
typedef int (*DecompressStep)(void *state, void *buffer, size_t size);

struct DecompressState {
	PipelineStep upstream_step;
	void *upstream_state;
	void *impl_state;
	uint8_t input[BUFF_SIZE];
	bool eof;
};
#endif

#ifdef CONFIG_GUNZIP

struct GunzipState {
	z_stream strm;
	bool initialized;
};

static int gunzip_step(void *state, void *buffer, size_t size)
{
	struct DecompressState *ds = (struct DecompressState *)state;
	struct GunzipState *s = (struct GunzipState *)ds->impl_state;
	int ret;
	int outlen = 0;

	s->strm.next_out = buffer;
	s->strm.avail_out = size;
	while (outlen == 0) {
		if (s->strm.avail_in == 0) {
			ret = ds->upstream_step(ds->upstream_state, ds->input, sizeof ds->input);
			if (ret < 0) {
				return ret;
			} else if (ret == 0) {
				ds->eof = true;
			}
			s->strm.avail_in = ret;
			s->strm.next_in = ds->input;
		}
		if (ds->eof) {
			break;
		}

		ret = inflate(&s->strm, Z_NO_FLUSH);
		outlen = size - s->strm.avail_out;
		if (ret == Z_STREAM_END) {
			ds->eof = true;
			break;
		}
		if (ret != Z_OK && ret != Z_BUF_ERROR) {
			ERROR("inflate failed (returned %d)", ret);
			return -1;
		}
	}
	return outlen;
}

#endif

#ifdef CONFIG_ZSTD

struct ZstdState {
	ZSTD_DStream* dctx;
	ZSTD_inBuffer input_view;
};

static int zstd_step(void* state, void* buffer, size_t size)
{
	struct DecompressState *ds = (struct DecompressState *)state;
	struct ZstdState *s = (struct ZstdState *)ds->impl_state;
	size_t decompress_ret;
	int ret;
	ZSTD_outBuffer output = { buffer, size, 0 };

	do {
		if (s->input_view.pos == s->input_view.size) {
			ret = ds->upstream_step(ds->upstream_state, ds->input, sizeof ds->input);
			if (ret < 0) {
				return ret;
			} else if (ret == 0) {
				ds->eof = true;
			}
			s->input_view.size = ret;
			s->input_view.pos = 0;
		}

		do {
			decompress_ret = ZSTD_decompressStream(s->dctx, &output, &s->input_view);

			if (ZSTD_isError(decompress_ret)) {
				ERROR("ZSTD_decompressStream failed: %s",
				      ZSTD_getErrorName(decompress_ret));
				return -1;
			}

			if (output.pos == output.size) {
				break;
			}
		} while (s->input_view.pos < s->input_view.size);
	} while (output.pos == 0 && !ds->eof);

	return output.pos;
}

#endif

static int __swupdate_copy(int fdin, unsigned char *inbuf, void *out, size_t nbytes, unsigned long *offs, unsigned long long seek,
	int skip_file, int __attribute__ ((__unused__)) compressed,
	uint32_t *checksum, unsigned char *hash, bool encrypted, const char *imgivt, writeimage callback)
{
	unsigned int percent, prevpercent = 0;
	int ret = 0;
	int len;
	unsigned char md_value[64]; /*
				     *  Maximum hash is 64 bytes for SHA512
				     *  and we use sha256 in swupdate
				     */
	unsigned int md_len = 0;
	unsigned char *aes_key = NULL;
	unsigned char *ivt = NULL;
	unsigned char ivtbuf[AES_BLK_SIZE];

	struct InputState input_state = {
		.fdin = fdin,
		.source = INPUT_FROM_FD,
		.inbuf = NULL,
		.pos = 0,
		.nbytes = nbytes,
		.offs = offs,
		.dgst = NULL,
		.checksum = 0
	};

	struct DecryptState decrypt_state = {
		.upstream_step = NULL, .upstream_state = NULL,
		.dcrypt = NULL,
		.outlen = 0, .eof = false
	};

#if defined(CONFIG_GUNZIP) || defined(CONFIG_ZSTD)
	struct DecompressState decompress_state = {
		.upstream_step = NULL, .upstream_state = NULL,
		.impl_state = NULL
	};

	DecompressStep decompress_step = NULL;
#ifdef CONFIG_GUNZIP
	struct GunzipState gunzip_state = {
		.strm = {
			.zalloc = Z_NULL, .zfree = Z_NULL, .opaque = Z_NULL,
			.avail_in = 0, .next_in = Z_NULL,
			.avail_out = 0, .next_out = Z_NULL
		},
		.initialized = false,
	};
#endif
#ifdef CONFIG_ZSTD
	struct ZstdState zstd_state = {
		.dctx = NULL,
		.input_view = { NULL, 0, 0 },
	};
#endif
#endif

	/*
	 * If inbuf is set, read from buffer instead of from file
	 */
	if (inbuf) {
		input_state.inbuf = inbuf;
		input_state.source = INPUT_FROM_MEMORY;
	}

	PipelineStep step = NULL;
	void *state = NULL;
	uint8_t buffer[BUFF_SIZE];

	if (!callback) {
		callback = copy_write;
	}

	if (checksum)
		*checksum = 0;

	if (IsValidHash(hash)) {
		input_state.dgst = swupdate_HASH_init(SHA_DEFAULT);
		if (!input_state.dgst)
			return -EFAULT;
	}

	if (encrypted) {
		aes_key = get_aes_key();
		if (imgivt && strlen(imgivt)) {
			if (!is_hex_str(imgivt) || ascii_to_bin(ivtbuf, sizeof(ivtbuf), imgivt)) {
				ERROR("Invalid image ivt");
				return -EINVAL;
			}
			ivt = ivtbuf;
		} else
			ivt = get_aes_ivt();
		decrypt_state.dcrypt = swupdate_DECRYPT_init(aes_key, get_aes_keylen(), ivt);
		if (!decrypt_state.dcrypt) {
			ERROR("decrypt initialization failure, aborting");
			ret = -EFAULT;
			goto copyfile_exit;
		}
	}

	if (compressed) {
		if (compressed == COMPRESSED_TRUE) {
			WARN("compressed argument: boolean form is deprecated, use compressed = \"zlib\";");
		}
#ifdef CONFIG_GUNZIP
		if (compressed == COMPRESSED_ZLIB || compressed == COMPRESSED_TRUE) {
			/*
			 * 16 + MAX_WBITS means that Zlib should expect and decode a
			 * gzip header.
			 */
			if (inflateInit2(&gunzip_state.strm, 16 + MAX_WBITS) != Z_OK) {
				ERROR("inflateInit2 failed");
				ret = -EFAULT;
				goto copyfile_exit;
			}
			gunzip_state.initialized = true;
			decompress_step = &gunzip_step;
			decompress_state.impl_state = &gunzip_state;
		} else
#endif
#ifdef CONFIG_ZSTD
		if (compressed == COMPRESSED_ZSTD) {
			if ((zstd_state.dctx = ZSTD_createDStream()) == NULL) {
				ERROR("ZSTD_createDStream failed");
				ret = -EFAULT;
				goto copyfile_exit;
			}
			zstd_state.input_view.src = decompress_state.input;
			decompress_step = &zstd_step;
			decompress_state.impl_state = &zstd_state;
		} else
#endif
		{
			TRACE("Requested decompression method (%d) is not configured!", compressed);
			ret = -EINVAL;
			goto copyfile_exit;
		}
	}

	if (seek) {
		int fdout = (out != NULL) ? *(int *)out : -1;
		if (fdout < 0) {
			ERROR("out argument: invalid fd or pointer");
			ret = -EFAULT;
			goto copyfile_exit;
		}

		TRACE("offset has been defined: %llu bytes", seek);
		if (lseek(fdout, seek, SEEK_SET) < 0) {
			ERROR("offset argument: seek failed");
			ret = -EFAULT;
			goto copyfile_exit;
		}
	}

#if defined(CONFIG_GUNZIP) || defined(CONFIG_ZSTD)
	if (compressed) {
		if (encrypted) {
			decrypt_state.upstream_step = &input_step;
			decrypt_state.upstream_state = &input_state;
			decompress_state.upstream_step = &decrypt_step;
			decompress_state.upstream_state = &decrypt_state;
		} else {
			decompress_state.upstream_step = &input_step;
			decompress_state.upstream_state = &input_state;
		}
		step = decompress_step;
		state = &decompress_state;
	} else {
#endif
		if (encrypted) {
			decrypt_state.upstream_step = &input_step;
			decrypt_state.upstream_state = &input_state;
			step = &decrypt_step;
			state = &decrypt_state;
		} else {
			step = &input_step;
			state = &input_state;
		}
#if defined(CONFIG_GUNZIP) || defined(CONFIG_ZSTD)
	}
#endif

	for (;;) {
		ret = step(state, buffer, sizeof buffer);
		if (ret < 0) {
			goto copyfile_exit;
		}
		if (ret == 0) {
			break;
		}
		if (skip_file) {
			continue;
		}
		len = ret;
		/*
		 * If there is no enough place,
		 * returns an error and close the output file that
		 * results corrupted. This lets the cleanup routine
		 * to remove it
		 */
		if (callback(out, buffer, len) < 0) {
			ret = -ENOSPC;
			goto copyfile_exit;
		}

		percent = (unsigned)(100ULL * (nbytes - input_state.nbytes) / nbytes);
		if (percent != prevpercent) {
			prevpercent = percent;
			swupdate_progress_update(percent);
		}
	}

	if (IsValidHash(hash)) {
		if (swupdate_HASH_final(input_state.dgst, md_value, &md_len) < 0) {
			ret = -EFAULT;
			goto copyfile_exit;
		}


		/*
		 * Now check if the computed hash is equal
		 * to the value retrieved from sw-descritpion
		 */
		if (md_len != SHA256_HASH_LENGTH || swupdate_HASH_compare(hash, md_value)) {
			char hashstring[2 * SHA256_HASH_LENGTH + 1];
			char newhashstring[2 * SHA256_HASH_LENGTH + 1];

			hash_to_ascii(hash, hashstring);
			hash_to_ascii(md_value, newhashstring);

#ifndef CONFIG_ENCRYPTED_IMAGES_HARDEN_LOGGING
			ERROR("HASH mismatch : %s <--> %s",
				hashstring, newhashstring);
#endif
			ret = -EFAULT;
			goto copyfile_exit;
		}
	}

	if (!inbuf) {
		ret = fill_buffer(fdin, buffer, NPAD_BYTES(*offs), offs, checksum, NULL);
		if (ret < 0)
			DEBUG("Padding bytes are not read, ignoring");
	}

	if (checksum != NULL) {
		*checksum = input_state.checksum;
	}

	ret = 0;

copyfile_exit:
	if (decrypt_state.dcrypt) {
		swupdate_DECRYPT_cleanup(decrypt_state.dcrypt);
	}
	if (input_state.dgst) {
		swupdate_HASH_cleanup(input_state.dgst);
	}
#ifdef CONFIG_GUNZIP
	if (gunzip_state.initialized) {
		inflateEnd(&gunzip_state.strm);
	}
#endif
#ifdef CONFIG_ZSTD
	if (zstd_state.dctx != NULL) {
		ZSTD_freeDStream(zstd_state.dctx);
	}
#endif

	return ret;
}

int copyfile(int fdin, void *out, size_t nbytes, unsigned long *offs, unsigned long long seek,
	int skip_file, int __attribute__ ((__unused__)) compressed,
	uint32_t *checksum, unsigned char *hash, bool encrypted, const char *imgivt, writeimage callback)
{
	return __swupdate_copy(fdin,
				NULL,
				out,
				nbytes,
				offs,
				seek,
				skip_file,
				compressed,
				checksum,
				hash,
				encrypted,
				imgivt,
				callback);
}

int copybuffer(unsigned char *inbuf, void *out, size_t nbytes, int __attribute__ ((__unused__)) compressed,
	unsigned char *hash, bool encrypted, const char *imgivt, writeimage callback)
{
	return __swupdate_copy(-1,
				inbuf,
				out,
				nbytes,
				NULL,
				0,
				0,
				compressed,
				NULL,
				hash,
				encrypted,
				imgivt,
				callback);
}

int copyimage(void *out, struct img_type *img, writeimage callback)
{
	return copyfile(img->fdin,
			out,
			img->size,
			(unsigned long *)&img->offset,
			img->seek,
			0, /* no skip */
			img->compressed,
			&img->checksum,
			img->sha256,
			img->is_encrypted,
			img->ivt_ascii,
			callback);
}

int extract_cpio_header(int fd, struct filehdr *fhdr, unsigned long *offset)
{
	unsigned char buf[sizeof(fhdr->filename)];
	if (fill_buffer(fd, buf, sizeof(struct new_ascii_header), offset, NULL, NULL) < 0)
		return -EINVAL;
	if (get_cpiohdr(buf, fhdr) < 0) {
		ERROR("CPIO Header corrupted, cannot be parsed");
		return -EINVAL;
	}
	if (fhdr->namesize >= sizeof(fhdr->filename))
	{
		ERROR("CPIO Header filelength too big %u >= %u (max)",
			(unsigned int)fhdr->namesize,
			(unsigned int)sizeof(fhdr->filename));
		return -EINVAL;
	}

	if (fill_buffer(fd, buf, fhdr->namesize , offset, NULL, NULL) < 0)
		return -EINVAL;
	buf[fhdr->namesize] = '\0';
	strlcpy(fhdr->filename, (char *)buf, sizeof(fhdr->filename));

	/* Skip filename padding, if any */
	if (fill_buffer(fd, buf, (4 - (*offset % 4)) % 4, offset, NULL, NULL) < 0)
		return -EINVAL;

	return 0;
}

int extract_img_from_cpio(int fd, unsigned long offset, struct filehdr *fdh)
{

	if (lseek(fd, offset, SEEK_SET) < 0) {
		ERROR("CPIO file corrupted : %s",
		strerror(errno));
		return -EBADF;
	}
	if (extract_cpio_header(fd, fdh, &offset)) {
		ERROR("CPIO Header wrong");
		return -1;
	}
	if (lseek(fd, offset, SEEK_SET) < 0) {
		ERROR("CPIO file corrupted : %s", strerror(errno));
		return -1;
	}

	return 0;
}

int cpio_scan(int fd, struct swupdate_cfg *cfg, off_t start)
{
	struct filehdr fdh;
	unsigned long offset = start;
	int file_listed;
	uint32_t checksum;

	while (1) {
		file_listed = 0;
		start = offset;
		if (extract_cpio_header(fd, &fdh, &offset)) {
			return -1;
		}
		if (strcmp("TRAILER!!!", fdh.filename) == 0) {
			return 0;
		}

		struct img_type *img = NULL;
		SEARCH_FILE(img, cfg->images, file_listed, start);
		SEARCH_FILE(img, cfg->scripts, file_listed, start);
		SEARCH_FILE(img, cfg->bootscripts, file_listed, start);

		TRACE("Found file:\n\tfilename %s\n\tsize %lu\n\t%s",
			fdh.filename,
			fdh.size,
			file_listed ? "REQUIRED" : "not required");

		/*
		 * use copyfile for checksum and hash verification, as we skip file
		 * we do not have to provide fdout
		 */
		if (copyfile(fd, NULL, fdh.size, &offset, 0, 1, 0, &checksum, img ? img->sha256 : NULL,
				false, NULL, NULL) != 0) {
			ERROR("invalid archive");
			return -1;
		}

		if (!swupdate_verify_chksum(checksum, &fdh)) {
			return -1;
		}

		/* Next header must be 4-bytes aligned */
		offset += NPAD_BYTES(offset);
		if (lseek(fd, offset, SEEK_SET) < 0) {
			ERROR("CPIO file corrupted : %s", strerror(errno));
			return -1;
		}
	}

	return 0;
}

bool swupdate_verify_chksum(const uint32_t chk1, struct filehdr *fhdr) {
	bool ret = (chk1 == fhdr->chksum);
	if (fhdr->format == CPIO_NEWASCII)
		return true;
	if (!ret) {
		ERROR("Checksum WRONG ! Computed 0x%x, it should be 0x%x",
			chk1, (uint32_t)fhdr->chksum);
	}
	return ret;
}
