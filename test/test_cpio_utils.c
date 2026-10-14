// SPDX-FileCopyrightText: 2026 Stefano Babic <stefano.babic@swupdate.org>
//
// SPDX-License-Identifier: GPL-2.0-only

/*
 * Tests for core/cpio_utils.c: cpio headers and copyfile(), that
 * reads an artifact and verifies, decrypts and decompresses it.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "generated/autoconf.h"
#include "cpiohdr.h"
#include "swupdate_crypto.h"
#include "swupdate_image.h"
#include "util.h"

#ifdef CONFIG_GUNZIP
#include <zlib.h>
#endif
#ifdef CONFIG_XZ
#include <lzma.h>
#endif
#ifdef CONFIG_ZSTD
#include <zstd.h>
#endif
#ifdef CONFIG_LZ4
#include <lz4frame.h>
#endif
#if defined(CONFIG_ENCRYPTED_IMAGES) && defined(CONFIG_SSL_IMPL_OPENSSL)
#include <openssl/evp.h>
#define TEST_ENCRYPTION
#endif

#define DATA_SIZE	(100 * 1024 + 3)	/* more than one internal buffer, not aligned */

static char tmpdir[] = "/tmp/swupdate-test-cpio-XXXXXX";
static unsigned char *data;	/* compressible test data */

/* Output collected by the copyfile() callback */
struct sink {
	unsigned char *buf;
	size_t len;
	size_t cap;
	int calls;
	int fail_after;		/* fail the callback after this many calls, 0 = never */
};

static int sink_write(void *out, const void *buf, size_t len)
{
	struct sink *s = (struct sink *)out;

	s->calls++;
	if (s->fail_after && s->calls > s->fail_after)
		return -1;
	if (s->len + len > s->cap) {
		s->cap = (s->len + len) * 2;
		s->buf = realloc(s->buf, s->cap);
		assert_non_null(s->buf);
	}
	memcpy(s->buf + s->len, buf, len);
	s->len += len;
	return 0;
}

static void sink_free(struct sink *s)
{
	free(s->buf);
	memset(s, 0, sizeof(*s));
}

/* Write buf into a temporary file and return it opened for reading */
static int input_fd(const void *buf, size_t len)
{
	char path[128];
	int fd;

	snprintf(path, sizeof(path), "%s/input", tmpdir);
	fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600);
	assert_true(fd >= 0);
	assert_int_equal(write(fd, buf, len), (ssize_t)len);
	assert_int_equal(lseek(fd, 0, SEEK_SET), 0);
	unlink(path);
	return fd;
}

static void sha256(const void *buf, size_t len, unsigned char *hash)
{
	unsigned char md[64];
	unsigned int mdlen = 0;
	void *dgst = swupdate_HASH_init(SHA_DEFAULT);

	assert_non_null(dgst);
	assert_int_equal(swupdate_HASH_update(dgst, buf, len), 0);
	assert_int_equal(swupdate_HASH_final(dgst, md, &mdlen), 0);
	assert_int_equal(mdlen, SHA256_HASH_LENGTH);
	swupdate_HASH_cleanup(dgst);
	memcpy(hash, md, SHA256_HASH_LENGTH);
}

static uint32_t sum(const unsigned char *buf, size_t len)
{
	uint32_t s = 0;

	for (size_t i = 0; i < len; i++)
		s += buf[i];
	return s;
}

/*
 * Run copyfile() on input with the output collected in sink.
 * hash may be NULL (no verification).
 */
static int copy(const unsigned char *in, size_t inlen, size_t nbytes,
		enum compression_type compressed, unsigned char *hash,
		struct sink *s, uint32_t *checksum)
{
	unsigned long offs = 0;
	int fd = input_fd(in, inlen);
	struct swupdate_copy args = {
		.fdin = fd,
		.out = s,
		.callback = sink_write,
		.nbytes = nbytes,
		.offs = &offs,
		.compressed = compressed,
		.hash = hash,
		.checksum = checksum,
	};
	int ret = copyfile(&args);

	close(fd);
	return ret;
}

/*
 * cpio headers
 */
static size_t make_cpio_entry(unsigned char *buf, const char *magic, const char *name,
			      size_t filesize, uint32_t chksum)
{
	size_t namesize = strlen(name) + 1;
	int len;

	len = sprintf((char *)buf,
		      "%s%08X%08X%08X%08X%08X%08X%08zX%08X%08X%08X%08X%08zX%08X",
		      magic, 1, 0100644, 0, 0, 1, 0, filesize, 0, 0, 0, 0,
		      namesize, chksum);
	memcpy(buf + len, name, namesize);
	len += namesize;
	/* header + name are padded to 4 bytes */
	while (len % 4)
		buf[len++] = 0;
	return len;
}

static void test_cpio_header(void **state)
{
	(void)state;
	unsigned char buf[512];
	struct filehdr fhdr;

	make_cpio_entry(buf, "070701", "sw-description", 0x1234, 0);
	assert_int_equal(get_cpiohdr(buf, &fhdr), 0);
	assert_int_equal(fhdr.format, CPIO_NEWASCII);
	assert_int_equal(fhdr.size, 0x1234);
	assert_int_equal(fhdr.namesize, strlen("sw-description") + 1);

	make_cpio_entry(buf, "070702", "image", 10, 0xabcdef);
	assert_int_equal(get_cpiohdr(buf, &fhdr), 0);
	assert_int_equal(fhdr.format, CPIO_CRCASCII);
	assert_int_equal(fhdr.chksum, 0xabcdef);

	/* old binary and odc formats are not supported */
	make_cpio_entry(buf, "070707", "image", 10, 0);
	assert_int_equal(get_cpiohdr(buf, &fhdr), -EINVAL);
	assert_int_equal(get_cpiohdr(NULL, &fhdr), -EINVAL);
}

static void test_cpio_extract_header(void **state)
{
	(void)state;
	unsigned char buf[1024];
	struct filehdr fhdr;
	unsigned long offset;
	size_t len;
	int fd;

	/* the file data follows the padded header */
	len = make_cpio_entry(buf, "070702", "rootfs.ext4", 5, 0);
	memcpy(buf + len, "DATA!", 5);
	fd = input_fd(buf, len + 5);
	offset = 0;
	assert_int_equal(extract_cpio_header(fd, &fhdr, &offset), 0);
	assert_string_equal(fhdr.filename, "rootfs.ext4");
	assert_int_equal(fhdr.size, 5);
	assert_int_equal(offset, len);
	assert_int_equal(offset % 4, 0);
	assert_int_equal(read(fd, buf, 5), 5);
	assert_memory_equal(buf, "DATA!", 5);

	/* extract_img_from_cpio() seeks to the header and after it */
	assert_int_equal(extract_img_from_cpio(fd, 0, &fhdr), 0);
	assert_string_equal(fhdr.filename, "rootfs.ext4");
	assert_int_equal(lseek(fd, 0, SEEK_CUR), (off_t)len);
	close(fd);

	/* truncated header */
	fd = input_fd(buf, 50);
	offset = 0;
	assert_int_not_equal(extract_cpio_header(fd, &fhdr, &offset), 0);
	close(fd);

	/* truncated name */
	len = make_cpio_entry(buf, "070702", "a-long-file-name", 0, 0);
	fd = input_fd(buf, 110 + 4);
	offset = 0;
	assert_int_not_equal(extract_cpio_header(fd, &fhdr, &offset), 0);
	close(fd);

	/* name larger than the buffer in struct filehdr */
	char longname[sizeof(fhdr.filename) + 10];
	memset(longname, 'n', sizeof(longname) - 1);
	longname[sizeof(longname) - 1] = '\0';
	unsigned char *big = malloc(sizeof(longname) + 256);
	assert_non_null(big);
	len = make_cpio_entry(big, "070702", longname, 0, 0);
	fd = input_fd(big, len);
	offset = 0;
	assert_int_not_equal(extract_cpio_header(fd, &fhdr, &offset), 0);
	close(fd);
	free(big);

	/* garbage instead of a header */
	memset(buf, 'x', sizeof(buf));
	fd = input_fd(buf, sizeof(buf));
	offset = 0;
	assert_int_not_equal(extract_cpio_header(fd, &fhdr, &offset), 0);
	close(fd);
}

static void test_cpio_verify_chksum(void **state)
{
	(void)state;
	struct filehdr fhdr = { .format = CPIO_CRCASCII, .chksum = 1234 };

	assert_true(swupdate_verify_chksum(1234, &fhdr));
	assert_false(swupdate_verify_chksum(1235, &fhdr));
	/* the newc format has no checksum */
	fhdr.format = CPIO_NEWASCII;
	assert_true(swupdate_verify_chksum(1235, &fhdr));
}

/*
 * copyfile() without compression and encryption
 */
static void test_copyfile_plain(void **state)
{
	(void)state;
	unsigned char hash[SHA256_HASH_LENGTH];
	struct sink s = { 0 };
	uint32_t checksum = 0;

	sha256(data, DATA_SIZE, hash);
	assert_int_equal(copy(data, DATA_SIZE, DATA_SIZE, COMPRESSED_FALSE, hash, &s, &checksum), 0);
	assert_int_equal(s.len, DATA_SIZE);
	assert_memory_equal(s.buf, data, DATA_SIZE);
	/* checksum of the cpio crc format: sum of the input bytes */
	assert_int_equal(checksum, sum(data, DATA_SIZE));
	sink_free(&s);

	/* without hash, only the data is copied */
	assert_int_equal(copy(data, DATA_SIZE, DATA_SIZE, COMPRESSED_FALSE, NULL, &s, NULL), 0);
	assert_int_equal(s.len, DATA_SIZE);
	sink_free(&s);

	/* only nbytes are read */
	assert_int_equal(copy(data, DATA_SIZE, 1000, COMPRESSED_FALSE, NULL, &s, NULL), 0);
	assert_int_equal(s.len, 1000);
	assert_memory_equal(s.buf, data, 1000);
	sink_free(&s);

	/* empty file */
	assert_int_equal(copy(data, 0, 0, COMPRESSED_FALSE, NULL, &s, NULL), 0);
	assert_int_equal(s.len, 0);
	sink_free(&s);
}

static void test_copyfile_errors(void **state)
{
	(void)state;
	unsigned char hash[SHA256_HASH_LENGTH];
	struct sink s = { 0 };

	/* wrong hash */
	sha256(data, DATA_SIZE, hash);
	hash[0] ^= 1;
	assert_int_equal(copy(data, DATA_SIZE, DATA_SIZE, COMPRESSED_FALSE, hash, &s, NULL), -EFAULT);
	sink_free(&s);

	/* the stream ends before nbytes */
	assert_true(copy(data, 1000, 2000, COMPRESSED_FALSE, NULL, &s, NULL) < 0);
	sink_free(&s);

	/* the consumer fails */
	s.fail_after = 1;
	assert_int_equal(copy(data, DATA_SIZE, DATA_SIZE, COMPRESSED_FALSE, NULL, &s, NULL), -ENOSPC);
	sink_free(&s);

	/* compression not supported */
	assert_int_equal(copy(data, DATA_SIZE, DATA_SIZE, 99, NULL, &s, NULL), -EINVAL);
	sink_free(&s);
#ifndef CONFIG_LZ4
	assert_int_equal(copy(data, DATA_SIZE, DATA_SIZE, COMPRESSED_LZ4, NULL, &s, NULL), -EINVAL);
	sink_free(&s);
#endif
}

static void test_copyfile_padding(void **state)
{
	(void)state;
	unsigned char in[16] = "12345678901234";
	unsigned long offs = 0;
	struct sink s = { 0 };
	int fd = input_fd(in, sizeof(in));
	struct swupdate_copy args = {
		.fdin = fd, .out = &s, .callback = sink_write,
		.nbytes = 5, .offs = &offs,
	};

	/* after the data, the padding to 4 bytes of the cpio entry is read */
	assert_int_equal(copyfile(&args), 0);
	assert_int_equal(s.len, 5);
	assert_int_equal(offs, 8);
	assert_int_equal(lseek(fd, 0, SEEK_CUR), 8);
	close(fd);
	sink_free(&s);
}

static void test_copyfile_memory(void **state)
{
	(void)state;
	unsigned char hash[SHA256_HASH_LENGTH];
	unsigned long offs = 0;
	struct sink s = { 0 };
	struct swupdate_copy args = {
		.fdin = -1, .inbuf = data, .out = &s, .callback = sink_write,
		.nbytes = DATA_SIZE, .offs = &offs, .hash = hash,
	};

	sha256(data, DATA_SIZE, hash);
	assert_int_equal(copyfile(&args), 0);
	assert_int_equal(s.len, DATA_SIZE);
	assert_memory_equal(s.buf, data, DATA_SIZE);
	sink_free(&s);
}

static void test_copyfile_skip(void **state)
{
	(void)state;
	unsigned char hash[SHA256_HASH_LENGTH];
	unsigned long offs = 0;
	struct sink s = { 0 };
	int fd = input_fd(data, DATA_SIZE);
	struct swupdate_copy args = {
		.fdin = fd, .out = &s, .callback = sink_write,
		.nbytes = DATA_SIZE, .offs = &offs, .hash = hash, .skip_file = 1,
	};

	/* skip_file: the input is verified, but not passed to the callback */
	sha256(data, DATA_SIZE, hash);
	assert_int_equal(copyfile(&args), 0);
	assert_int_equal(s.calls, 0);
	close(fd);

	fd = input_fd(data, DATA_SIZE);
	args.fdin = fd;
	offs = 0;
	hash[1] ^= 1;
	assert_int_equal(copyfile(&args), -EFAULT);
	close(fd);
}

static void test_copyfile_seek(void **state)
{
	(void)state;
	char path[128];
	unsigned char out[64];
	unsigned long offs = 0;
	int fdin = input_fd("payload", 7);
	int fdout;

	snprintf(path, sizeof(path), "%s/output", tmpdir);
	fdout = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600);
	assert_true(fdout >= 0);
	assert_int_equal(write(fdout, "0123456789", 10), 10);

	/* the default callback writes to the fd in out, after seeking */
	struct swupdate_copy args = {
		.fdin = fdin, .out = &fdout, .nbytes = 7, .offs = &offs, .seek = 2,
	};
	assert_int_equal(copyfile(&args), 0);
	assert_int_equal(pread(fdout, out, sizeof(out), 0), 10);
	assert_memory_equal(out, "01payload9", 10);

	/* seek requires an fd as output */
	assert_int_equal(lseek(fdin, 0, SEEK_SET), 0);
	offs = 0;
	args.out = NULL;
	assert_int_equal(copyfile(&args), -EFAULT);

	close(fdin);
	close(fdout);
	unlink(path);
}

static void test_copy_write(void **state)
{
	(void)state;
	int fds[2];
	char buf[8];

	assert_int_equal(pipe(fds), 0);
	assert_int_equal(copy_write(&fds[1], "abc", 3), 0);
	assert_int_equal(read(fds[0], buf, sizeof(buf)), 3);
	assert_memory_equal(buf, "abc", 3);
	close(fds[0]);
	close(fds[1]);
	/* invalid fd */
	assert_int_equal(copy_write(&fds[1], "abc", 3), -1);
	assert_int_equal(copy_write(NULL, "abc", 3), -1);
}

static void test_copyimage(void **state)
{
	(void)state;
	struct img_type img = { 0 };
	struct sink s = { 0 };
	unsigned char *in = calloc(1, DATA_SIZE + 4);

	/* the entry in the cpio archive is followed by its padding */
	assert_non_null(in);
	memcpy(in, data, DATA_SIZE);
	img.fdin = input_fd(in, DATA_SIZE + NPAD_BYTES(DATA_SIZE));
	free(in);
	img.size = DATA_SIZE;
	sha256(data, DATA_SIZE, img.sha256);
	assert_int_equal(copyimage(&s, &img, sink_write), 0);
	assert_int_equal(s.len, DATA_SIZE);
	assert_memory_equal(s.buf, data, DATA_SIZE);
	/* the checksum and the offset are updated in the image */
	assert_int_equal(img.checksum, sum(data, DATA_SIZE));
	assert_int_equal(img.offset, DATA_SIZE + NPAD_BYTES(DATA_SIZE));
	close(img.fdin);
	sink_free(&s);
}

/*
 * Decompression: for each format, the artifact is decompressed, the
 * hash and the checksum are computed on the compressed data, corrupted
 * and truncated streams are errors.
 */
static void check_decompression(enum compression_type type,
				const unsigned char *c, size_t clen)
{
	unsigned char hash[SHA256_HASH_LENGTH];
	struct sink s = { 0 };
	uint32_t checksum = 0;
	unsigned char *bad;

	sha256(c, clen, hash);
	assert_int_equal(copy(c, clen, clen, type, hash, &s, &checksum), 0);
	assert_int_equal(s.len, DATA_SIZE);
	assert_memory_equal(s.buf, data, DATA_SIZE);
	assert_int_equal(checksum, sum(c, clen));
	sink_free(&s);

	/* corrupted in the middle */
	bad = malloc(clen);
	assert_non_null(bad);
	memcpy(bad, c, clen);
	for (size_t i = clen / 2; i < clen / 2 + 16 && i < clen; i++)
		bad[i] ^= 0x55;
	assert_true(copy(bad, clen, clen, type, NULL, &s, NULL) < 0);
	sink_free(&s);
	free(bad);

	/* the compressed stream is incomplete */
	assert_true(copy(c, clen / 2, clen / 2, type, NULL, &s, NULL) < 0);
	sink_free(&s);
	assert_true(copy(c, clen - 1, clen - 1, type, NULL, &s, NULL) < 0);
	sink_free(&s);
}

#ifdef CONFIG_GUNZIP
static void test_copyfile_gzip(void **state)
{
	(void)state;
	z_stream strm = { 0 };
	size_t cap = compressBound(DATA_SIZE) + 64;
	unsigned char *c = malloc(cap);

	assert_non_null(c);
	/* gzip format, as created by gzip */
	assert_int_equal(deflateInit2(&strm, 9, Z_DEFLATED, 16 + MAX_WBITS, 8,
				      Z_DEFAULT_STRATEGY), Z_OK);
	strm.next_in = data;
	strm.avail_in = DATA_SIZE;
	strm.next_out = c;
	strm.avail_out = cap;
	assert_int_equal(deflate(&strm, Z_FINISH), Z_STREAM_END);
	size_t clen = cap - strm.avail_out;
	deflateEnd(&strm);

	check_decompression(COMPRESSED_ZLIB, c, clen);

	/* compressed = true is the deprecated form for zlib */
	struct sink s = { 0 };
	assert_int_equal(copy(c, clen, clen, COMPRESSED_TRUE, NULL, &s, NULL), 0);
	assert_int_equal(s.len, DATA_SIZE);
	sink_free(&s);
	free(c);
}
#endif

#ifdef CONFIG_XZ
static void test_copyfile_xz(void **state)
{
	(void)state;
	size_t cap = lzma_stream_buffer_bound(DATA_SIZE);
	unsigned char *c = malloc(cap);
	size_t clen = 0;

	assert_non_null(c);
	assert_int_equal(lzma_easy_buffer_encode(6, LZMA_CHECK_CRC64, NULL, data, DATA_SIZE,
						 c, &clen, cap), LZMA_OK);
	check_decompression(COMPRESSED_XZ, c, clen);
	free(c);
}
#endif

#ifdef CONFIG_ZSTD
static void test_copyfile_zstd(void **state)
{
	(void)state;
	size_t cap = ZSTD_compressBound(DATA_SIZE);
	unsigned char *c = malloc(cap);
	size_t clen;

	ZSTD_CCtx *cctx = ZSTD_createCCtx();

	assert_non_null(c);
	assert_non_null(cctx);
	/* with checksum as the zstd tool: a corrupted frame is detected */
	assert_false(ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, 3)));
	assert_false(ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 1)));
	clen = ZSTD_compress2(cctx, c, cap, data, DATA_SIZE);
	assert_false(ZSTD_isError(clen));
	ZSTD_freeCCtx(cctx);
	check_decompression(COMPRESSED_ZSTD, c, clen);
	free(c);
}
#endif

#ifdef CONFIG_LZ4
static void test_copyfile_lz4(void **state)
{
	(void)state;
	/* with checksum as the lz4 tool: a corrupted frame is detected */
	LZ4F_preferences_t prefs = { .frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled };
	size_t cap = LZ4F_compressFrameBound(DATA_SIZE, &prefs);
	unsigned char *c = malloc(cap);
	size_t clen;

	assert_non_null(c);
	clen = LZ4F_compressFrame(c, cap, data, DATA_SIZE, &prefs);
	assert_false(LZ4F_isError(clen));
	check_decompression(COMPRESSED_LZ4, c, clen);
	free(c);
}
#endif

#ifdef TEST_ENCRYPTION
#define IVT "0102030405060708090a0b0c0d0e0f10"

static size_t encrypt(const unsigned char *in, size_t len, const char *key_ascii,
		      unsigned char *out)
{
	unsigned char key[32], iv[16];
	size_t keylen = strlen(key_ascii) / 2;
	const EVP_CIPHER *cipher = keylen == 16 ? EVP_aes_128_cbc() :
				   keylen == 24 ? EVP_aes_192_cbc() : EVP_aes_256_cbc();
	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	int l1, l2;

	assert_int_equal(ascii_to_bin(key, keylen, key_ascii), 0);
	assert_int_equal(ascii_to_bin(iv, sizeof(iv), IVT), 0);
	assert_non_null(ctx);
	assert_int_equal(EVP_EncryptInit_ex(ctx, cipher, NULL, key, iv), 1);
	assert_int_equal(EVP_EncryptUpdate(ctx, out, &l1, in, len), 1);
	assert_int_equal(EVP_EncryptFinal_ex(ctx, out + l1, &l2), 1);
	EVP_CIPHER_CTX_free(ctx);
	return l1 + l2;
}

static int copy_encrypted(const unsigned char *in, size_t len, const char *key,
			  const char *ivt, enum compression_type compressed,
			  unsigned char *hash, struct sink *s)
{
	unsigned long offs = 0;
	int fd = input_fd(in, len);
	struct swupdate_copy args = {
		.fdin = fd, .out = s, .callback = sink_write,
		.nbytes = len, .offs = &offs, .hash = hash,
		.compressed = compressed,
		.encrypted = true, .imgaes = key, .imgivt = ivt, .cipher = AES_CBC,
	};
	int ret = copyfile(&args);

	close(fd);
	return ret;
}

static void test_copyfile_encrypted(void **state)
{
	(void)state;
	static const char *keys[] = {
		"00112233445566778899aabbccddeeff",
		"00112233445566778899aabbccddeeff0011223344556677",
		"00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff",
	};
	unsigned char *c = malloc(DATA_SIZE + 32);
	unsigned char hash[SHA256_HASH_LENGTH];
	struct sink s = { 0 };
	size_t clen;

	assert_non_null(c);
	for (unsigned int i = 0; i < ARRAY_SIZE(keys); i++) {
		clen = encrypt(data, DATA_SIZE, keys[i], c);
		/* the hash is computed on the encrypted artifact */
		sha256(c, clen, hash);
		assert_int_equal(copy_encrypted(c, clen, keys[i], IVT, COMPRESSED_FALSE, hash, &s), 0);
		assert_int_equal(s.len, DATA_SIZE);
		assert_memory_equal(s.buf, data, DATA_SIZE);
		sink_free(&s);
	}

	/* wrong key: the padding is wrong */
	clen = encrypt(data, DATA_SIZE, keys[2], c);
	assert_true(copy_encrypted(c, clen, keys[0], IVT, COMPRESSED_FALSE, NULL, &s) < 0);
	sink_free(&s);

	/* invalid key and ivt */
	assert_int_equal(copy_encrypted(c, clen, "0011", IVT, COMPRESSED_FALSE, NULL, &s), -EINVAL);
	assert_int_equal(copy_encrypted(c, clen, "zz112233445566778899aabbccddeeff", IVT,
					COMPRESSED_FALSE, NULL, &s), -EINVAL);
	assert_int_equal(copy_encrypted(c, clen, keys[2], "0102", COMPRESSED_FALSE, NULL, &s), -EINVAL);
	sink_free(&s);

	/* no key at all */
	assert_true(copy_encrypted(c, clen, NULL, IVT, COMPRESSED_FALSE, NULL, &s) < 0);
	sink_free(&s);

	/* the global key is used if the image has none */
	assert_int_equal(set_aes_key(keys[2], IVT), 0);
	assert_int_equal(copy_encrypted(c, clen, NULL, NULL, COMPRESSED_FALSE, NULL, &s), 0);
	assert_int_equal(s.len, DATA_SIZE);
	assert_memory_equal(s.buf, data, DATA_SIZE);
	sink_free(&s);
	free(c);
}

#ifdef CONFIG_ZSTD
static void test_copyfile_encrypted_compressed(void **state)
{
	(void)state;
	static const char *key =
		"00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
	size_t cap = ZSTD_compressBound(DATA_SIZE);
	unsigned char *z = malloc(cap);
	unsigned char *c = malloc(cap + 32);
	unsigned char hash[SHA256_HASH_LENGTH];
	struct sink s = { 0 };
	size_t zlen, clen;

	assert_non_null(z);
	assert_non_null(c);
	/* compressed first, then encrypted */
	zlen = ZSTD_compress(z, cap, data, DATA_SIZE, 3);
	assert_false(ZSTD_isError(zlen));
	clen = encrypt(z, zlen, key, c);
	sha256(c, clen, hash);
	assert_int_equal(copy_encrypted(c, clen, key, IVT, COMPRESSED_ZSTD, hash, &s), 0);
	assert_int_equal(s.len, DATA_SIZE);
	assert_memory_equal(s.buf, data, DATA_SIZE);
	sink_free(&s);
	free(z);
	free(c);
}
#endif
#endif

static int cpio_group_setup(void **state)
{
	(void)state;
	if (!mkdtemp(tmpdir))
		return -1;
	data = malloc(DATA_SIZE);
	if (!data)
		return -1;
	/* compressible, but not trivial */
	for (size_t i = 0; i < DATA_SIZE; i++)
		data[i] = (unsigned char)((i * 7) ^ (i >> 5) ^ (i % 251 == 0 ? 0x5a : 0));
	return 0;
}

static int cpio_group_teardown(void **state)
{
	(void)state;
	free(data);
	return rmdir(tmpdir);
}

int main(void)
{
	static const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_cpio_header),
		cmocka_unit_test(test_cpio_extract_header),
		cmocka_unit_test(test_cpio_verify_chksum),
		cmocka_unit_test(test_copyfile_plain),
		cmocka_unit_test(test_copyfile_errors),
		cmocka_unit_test(test_copyfile_padding),
		cmocka_unit_test(test_copyfile_memory),
		cmocka_unit_test(test_copyfile_skip),
		cmocka_unit_test(test_copyfile_seek),
		cmocka_unit_test(test_copy_write),
		cmocka_unit_test(test_copyimage),
#ifdef CONFIG_GUNZIP
		cmocka_unit_test(test_copyfile_gzip),
#endif
#ifdef CONFIG_XZ
		cmocka_unit_test(test_copyfile_xz),
#endif
#ifdef CONFIG_ZSTD
		cmocka_unit_test(test_copyfile_zstd),
#endif
#ifdef CONFIG_LZ4
		cmocka_unit_test(test_copyfile_lz4),
#endif
#ifdef TEST_ENCRYPTION
		cmocka_unit_test(test_copyfile_encrypted),
#ifdef CONFIG_ZSTD
		cmocka_unit_test(test_copyfile_encrypted_compressed),
#endif
#endif
	};
	return cmocka_run_group_tests_name("cpio_utils", tests, cpio_group_setup,
					   cpio_group_teardown);
}
