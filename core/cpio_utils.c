/*
 * (C) Copyright 2012
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * SPDX-License-Identifier:     GPL-2.0-or-later
 */

#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#ifdef CONFIG_GUNZIP
#include <zlib.h>
#endif

#include "generated/autoconf.h"
#include "cpiohdr.h"
#include "util.h"
#include "sslapi.h"
#include "progress.h"

#define MODULE_NAME "cpio"

#define BUFF_SIZE	 16384

#define NPAD_BYTES(o) ((4 - (o % 4)) % 4)

static int get_cpiohdr(unsigned char *buf, unsigned long *size,
			unsigned long *namesize, unsigned long *chksum)
{
	struct new_ascii_header *cpiohdr;

	cpiohdr = (struct new_ascii_header *)buf;
	if (strncmp(cpiohdr->c_magic, "070702", 6) != 0) {
		ERROR("CPIO Format not recognized: magic not found");
			return -EINVAL;
	}
	*size = FROM_HEX(cpiohdr->c_filesize);
	*namesize = FROM_HEX(cpiohdr->c_namesize);
	*chksum =  FROM_HEX(cpiohdr->c_chksum);

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
 * Export this to be used in other modules
 * It just copy a buffer to a file
 */
int copy_write(void *out, const void *buf, unsigned int len)
{
	int ret;
	int fd = (out != NULL) ? *(int *)out : -1;

	while (len) {
		errno = 0;
		ret = write(fd, buf, len);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			ERROR("cannot write %d bytes: %s", len, strerror(errno));
			return -1;
		}

		if (ret == 0) {
			ERROR("cannot write %d bytes: %s", len, strerror(errno));
			return -1;
		}

		len -= ret;
		buf += ret;
	}

	return 0;
}

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
	unsigned int nbytes;
	unsigned long *offs;
	void *dgst;	/* use a private context for HASH */
	uint32_t checksum;
};

static int input_step(void *state, void *buffer, size_t size)
{
	struct InputState *s = (struct InputState *)state;
	if (size >= s->nbytes) {
		size = s->nbytes;
	}
	int ret = fill_buffer(s->fdin, buffer, size, s->offs, &s->checksum, s->dgst);
	if (ret < 0) {
		return ret;
	}
	s->nbytes -= size;
	return ret;
}

struct DecryptState
{
	PipelineStep upstream_step;
	void *upstream_state;

	void *dcrypt;	/* use a private context for decryption */
	uint8_t input[BUFF_SIZE];
	uint8_t output[BUFF_SIZE + AES_BLOCK_SIZE];
	int outlen;
	bool eof;
};

static int decrypt_step(void *state, void *buffer, size_t size)
{
	struct DecryptState *s = (struct DecryptState *)state;
	int ret;
	int inlen;

	if (s->outlen != 0) {
		if (size > s->outlen) {
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
		} else {
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
		if (size > s->outlen) {
			size = s->outlen;
		}
		memcpy(buffer, s->output, size);
		s->outlen -= size;
		memmove(s->output, s->output + size, s->outlen);
		return size;
	}

	return 0;
}

#ifdef CONFIG_GUNZIP

struct GunzipState
{
	PipelineStep upstream_step;
	void *upstream_state;

	z_stream strm;
	bool initialized;
	uint8_t input[BUFF_SIZE];
	bool eof;
};

static int gunzip_step(void *state, void *buffer, size_t size)
{
	struct GunzipState *s = (struct GunzipState *)state;
	int ret;
	int outlen = 0;

	s->strm.next_out = buffer;
	s->strm.avail_out = size;
	while (outlen == 0) {
		if (s->strm.avail_in == 0) {
			ret = s->upstream_step(s->upstream_state, s->input, sizeof s->input);
			if (ret < 0) {
				return ret;
			}
			s->strm.avail_in = ret;
			s->strm.next_in = s->input;
		}
		if (s->eof) {
			break;
		}

		ret = inflate(&s->strm, Z_NO_FLUSH);
		outlen = size - s->strm.avail_out;
		if (ret == Z_STREAM_END) {
			s->eof = true;
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

int copyfile(int fdin, void *out, unsigned int nbytes, unsigned long *offs, unsigned long long seek,
	int skip_file, int __attribute__ ((__unused__)) compressed,
	uint32_t *checksum, unsigned char *hash, int encrypted, writeimage callback)
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
	unsigned char *salt;

	struct InputState input_state = {
		.fdin = fdin,
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

#ifdef CONFIG_GUNZIP
	struct GunzipState gunzip_state = {
		.upstream_step = NULL, .upstream_state = NULL,
		.strm = {
			.zalloc = Z_NULL, .zfree = Z_NULL, .opaque = Z_NULL,
			.avail_in = 0, .next_in = Z_NULL,
			.avail_out = 0, .next_out = Z_NULL
		},
		.initialized = false,
		.eof = false
	};
#endif

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
		ivt = get_aes_ivt();
		salt = get_aes_salt();
		decrypt_state.dcrypt = swupdate_DECRYPT_init(aes_key, ivt, salt);
		if (!decrypt_state.dcrypt) {
			ERROR("decrypt initialization failure, aborting");
			ret = -EFAULT;
			goto copyfile_exit;
		}
	}

	if (compressed) {
#ifdef CONFIG_GUNZIP
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
#else
		TRACE("Request decompressing, but CONFIG_GUNZIP not set !");
		ret = -EINVAL;
		goto copyfile_exit;
#endif
	}

	if (seek) {
		int fdout = (out != NULL) ? *(int *)out : -1;
		TRACE("offset has been defined: %llu bytes", seek);
		if (lseek(fdout, seek, SEEK_SET) < 0) {
			ERROR("offset argument: seek failed");
			ret = -EFAULT;
			goto copyfile_exit;
		}
	}

#ifdef CONFIG_GUNZIP
	if (compressed) {
		if (encrypted) {
			decrypt_state.upstream_step = &input_step;
			decrypt_state.upstream_state = &input_state;
			gunzip_state.upstream_step = &decrypt_step;
			gunzip_state.upstream_state = &decrypt_state;
		} else {
			gunzip_state.upstream_step = &input_step;
			gunzip_state.upstream_state = &input_state;
		}
		step = &gunzip_step;
		state = &gunzip_state;
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
#ifdef CONFIG_GUNZIP
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

			ERROR("HASH mismatch : %s <--> %s",
				hashstring, newhashstring);
			ret = -EFAULT;
			goto copyfile_exit;
		}
	}

	fill_buffer(fdin, buffer, NPAD_BYTES(*offs), offs, checksum, NULL);

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

	return ret;
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
			callback);
}

int extract_cpio_header(int fd, struct filehdr *fhdr, unsigned long *offset)
{
	unsigned char buf[256];
	if (fill_buffer(fd, buf, sizeof(struct new_ascii_header), offset, NULL, NULL) < 0)
		return -EINVAL;
	if (get_cpiohdr(buf, &fhdr->size, &fhdr->namesize, &fhdr->chksum) < 0) {
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
	strncpy(fhdr->filename, (char *)buf, sizeof(fhdr->filename));

	/* Skip filename padding, if any */
	if (fill_buffer(fd, buf, (4 - (*offset % 4)) % 4, offset, NULL, NULL) < 0)
		return -EINVAL;

	return 0;
}

int extract_sw_description(int fd, const char *descfile, off_t *offs)
{
	struct filehdr fdh;
	unsigned long offset = *offs;
	char output_file[64];
	uint32_t checksum;
	int fdout;
	const char* TMPDIR = get_tmpdir();

	if (extract_cpio_header(fd, &fdh, &offset)) {
		ERROR("CPIO Header wrong");
		return -1;
	}

	if (strcmp(fdh.filename, descfile)) {
		ERROR("Expected %s but found %s.",
			descfile,
			fdh.filename);
		return -1;
	}
	if ((strlen(TMPDIR) + strlen(fdh.filename)) > sizeof(output_file)) {
		ERROR("File Name too long : %s", fdh.filename);
		return -1;
	}
	strncpy(output_file, TMPDIR, sizeof(output_file));
	strcat(output_file, fdh.filename);
	fdout = openfileoutput(output_file);

	if (lseek(fd, offset, SEEK_SET) < 0) {
		ERROR("CPIO file corrupted : %s", strerror(errno));
		close(fdout);
		return -1;
	}
	if (copyfile(fd, &fdout, fdh.size, &offset, 0, 0, 0, &checksum, NULL, 0, NULL) < 0) {
		ERROR("%s corrupted or not valid", descfile);
		close(fdout);
		return -1;
	}

	close(fdout);

	TRACE("Found file:\n\tfilename %s\n\tsize %lu\n\tchecksum 0x%lx %s",
		fdh.filename,
		(unsigned long)fdh.size,
		(unsigned long)checksum,
		(checksum == fdh.chksum) ? "VERIFIED" : "WRONG");

	if (checksum != fdh.chksum) {
		ERROR("Checksum WRONG ! Computed 0x%lx, it should be 0x%lx",
			(unsigned long)checksum, fdh.chksum);
		return -1;
	}

	*offs = offset;

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

off_t extract_next_file(int fd, int fdout, off_t start, int compressed,
		int encrypted, unsigned char *hash)
{
	int ret;
	struct filehdr fdh;
	uint32_t checksum = 0;
	unsigned long offset = start;

	ret = lseek(fd, offset, SEEK_SET);
	if (ret < 0) {
		ERROR("CPIO file corrupted : %s",
		strerror(errno));
		return ret;
	}

	ret = extract_cpio_header(fd, &fdh, &offset);
	if (ret) {
		ERROR("CPIO Header wrong");
		return ret;
	}

	ret = lseek(fd, offset, SEEK_SET);
	if (ret < 0) {
		ERROR("CPIO file corrupted : %s", strerror(errno));
		return ret;
	}

	ret = copyfile(fd, &fdout, fdh.size, &offset, 0, 0, compressed, &checksum, hash, encrypted, NULL);
	if (ret < 0) {
		ERROR("Error copying extracted file");
		return ret;
	}

	TRACE("Copied file:\n\tfilename %s\n\tsize %u\n\tchecksum 0x%lx %s",
		fdh.filename,
		(unsigned int)fdh.size,
		(unsigned long)checksum,
		(checksum == fdh.chksum) ? "VERIFIED" : "WRONG");

	if (checksum != fdh.chksum) {
		ERROR("Checksum WRONG ! Computed 0x%lx, it should be 0x%lx",
			(unsigned long)checksum, fdh.chksum);
		return -EINVAL;
	}

	return offset;
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
				0, NULL) != 0) {
			ERROR("invalid archive");
			return -1;
		}

		if ((uint32_t)(fdh.chksum) != checksum) {
			ERROR("Checksum verification failed for %s: %x != %x",
			fdh.filename, (uint32_t)fdh.chksum, checksum);
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
