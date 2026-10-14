// SPDX-FileCopyrightText: 2026 Stefano Babic <stefano.babic@swupdate.org>
//
// SPDX-License-Identifier: GPL-2.0-only

/*
 * Tests for core/stream_interface.c: reading a SWU (cpio archive),
 * checking it against sw-description, extracting or streaming the
 * artifacts, and saving the incoming SWU.
 *
 * The functions under test are static: the source file is included,
 * its global symbols are renamed so that they do not clash with the
 * ones linked from SWUpdate.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#define network_initializer		test_network_initializer
#define get_install_info		test_get_install_info
#define get_install_running_mode	test_get_install_running_mode
#define get_install_source		test_get_install_source
#define get_install_swset		test_get_install_swset
#define is_dryrun_install		test_is_dryrun_install
#define set_version_range		test_set_version_range
#define stream_cond			test_stream_cond
#define stream_mutex			test_stream_mutex
#define stream_wkup			test_stream_wkup

#include "../core/stream_interface.c"

#include "swupdate_dict.h"

#if !defined(CONFIG_SIGNED_IMAGES) && !defined(CONFIG_ENCRYPTED_SW_DESCRIPTION)
#define TEST_STREAM
#endif

#ifdef TEST_STREAM

static char tmpdir[] = "/tmp/swupdate-test-stream-XXXXXX";
static struct swupdate_cfg sw;

/*
 * Building a SWU
 */
struct swu {
	unsigned char *buf;
	size_t len;
	size_t cap;
};

static void swu_put(struct swu *s, const void *data, size_t len)
{
	if (s->len + len > s->cap) {
		s->cap = (s->len + len) * 2 + 1024;
		s->buf = realloc(s->buf, s->cap);
		assert_non_null(s->buf);
	}
	memcpy(s->buf + s->len, data, len);
	s->len += len;
}

static void swu_pad(struct swu *s)
{
	static const unsigned char zero[4];

	if (s->len % 4)
		swu_put(s, zero, 4 - s->len % 4);
}

/* add an entry in the crc format, wrong_chksum alters the checksum */
static void swu_add(struct swu *s, const char *name, const void *data, size_t len,
		    bool wrong_chksum)
{
	char hdr[111];
	uint32_t chksum = 0;

	for (size_t i = 0; i < len; i++)
		chksum += ((const unsigned char *)data)[i];
	if (wrong_chksum)
		chksum++;
	snprintf(hdr, sizeof(hdr),
		 "070702%08X%08X%08X%08X%08X%08X%08zX%08X%08X%08X%08X%08zX%08X",
		 1, 0100644, 0, 0, 1, 0, len, 0, 0, 0, 0, strlen(name) + 1, chksum);
	swu_put(s, hdr, 110);
	swu_put(s, name, strlen(name) + 1);
	swu_pad(s);
	swu_put(s, data, len);
	swu_pad(s);
}

static void swu_trailer(struct swu *s)
{
	static const unsigned char zero[512];

	swu_add(s, "TRAILER!!!", "", 0, false);
	/* cpio pads the archive to 512 bytes */
	if (s->len % 512)
		swu_put(s, zero, 512 - s->len % 512);
}

static void swu_free(struct swu *s)
{
	free(s->buf);
	memset(s, 0, sizeof(*s));
}

static int swu_fd(const struct swu *s)
{
	char path[128];
	int fd;

	snprintf(path, sizeof(path), "%s/test.swu", tmpdir);
	fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600);
	assert_true(fd >= 0);
	assert_int_equal(write(fd, s->buf, s->len), (ssize_t)s->len);
	assert_int_equal(lseek(fd, 0, SEEK_SET), 0);
	unlink(path);
	return fd;
}

static void hash_ascii(const void *data, size_t len, char *out)
{
	unsigned char md[64];
	unsigned int mdlen = 0;
	void *dgst = swupdate_HASH_init(SHA_DEFAULT);

	assert_non_null(dgst);
	assert_int_equal(swupdate_HASH_update(dgst, (const unsigned char *)data, len), 0);
	assert_int_equal(swupdate_HASH_final(dgst, md, &mdlen), 0);
	swupdate_HASH_cleanup(dgst);
	hash_to_ascii(md, out);
}

/*
 * Configuration
 */
static void sw_init(void)
{
	static struct swupdate_type_cfg update_type;

	memset(&sw, 0, sizeof(sw));
	LIST_INIT(&sw.images);
	LIST_INIT(&sw.hardware);
	LIST_INIT(&sw.scripts);
	LIST_INIT(&sw.bootloader);
	LIST_INIT(&sw.vars);
	LIST_INIT(&sw.extprocs);
	LIST_INIT(&sw.swupdate_types);
	LIST_INIT(&sw.external_urls);
	LIST_INIT(&sw.installed_sw_list);
	memset(&update_type, 0, sizeof(update_type));
	strlcpy(update_type.type_name, "default", sizeof(update_type.type_name));
	LIST_INSERT_HEAD(&sw.swupdate_types, &update_type, next);
	sw.update_type = &update_type;
	strlcpy(sw.hw.boardname, "board", sizeof(sw.hw.boardname));
	strlcpy(sw.hw.revision, "1.0", sizeof(sw.hw.revision));
	/* no bootloader in the tests */
	sw.no_state_marker = true;
	sw.no_transaction_marker = true;
}

static void sw_free(void)
{
	cleanup_files(&sw);
}

static char *path_in_tmp(const char *name)
{
	static char path[256];

	snprintf(path, sizeof(path), "%s/%s", tmpdir, name);
	return path;
}

static bool file_equal(const char *path, const void *data, size_t len)
{
	unsigned char *buf = NULL;
	size_t flen = 0;
	bool ret;

	if (read_file_into_buf(path, &buf, &flen))
		return false;
	ret = flen == len && !memcmp(buf, data, len);
	free(buf);
	return ret;
}

/*
 * A SWU with an image extracted to TMPDIR and one streamed to its
 * destination by the rawfile handler.
 */
static const char copied[] = "this artifact is copied into TMPDIR";
static const char streamed[] = "this artifact is streamed to the handler";

static void make_desc(char *desc, size_t size, const char *copied_hash,
		      const char *streamed_hash, const char *extra)
{
	snprintf(desc, size,
		 "software = { version = \"1.0\"; hardware-compatibility = [ \"1.0\" ];\n"
		 " files: (\n"
		 "  { filename = \"copied.bin\"; path = \"%s/copied.out\"; sha256 = \"%s\"; },\n"
		 "  { filename = \"streamed.bin\"; path = \"%s/streamed.out\"; sha256 = \"%s\";"
		 " installed-directly = true; }\n"
		 " );\n"
		 " %s\n"
		 "};\n",
		 tmpdir, copied_hash, tmpdir, streamed_hash, extra ? extra : "");
}

struct swu_options {
	const char *extra_desc;		/* added to sw-description */
	bool desc_not_first;
	bool skip_copied;		/* required file missing */
	bool wrong_hash;
	bool wrong_chksum;
	const char *unknown_file;	/* file not in sw-description */
	bool no_trailer;
};

static void make_swu(struct swu *s, const struct swu_options *o)
{
	char desc[2048], hcopied[65], hstreamed[65];

	hash_ascii(copied, sizeof(copied), hcopied);
	hash_ascii(streamed, sizeof(streamed), hstreamed);
	if (o->wrong_hash)
		hcopied[0] = hcopied[0] == '0' ? '1' : '0';
	make_desc(desc, sizeof(desc), hcopied, hstreamed, o->extra_desc);

	if (o->desc_not_first)
		swu_add(s, "copied.bin", copied, sizeof(copied), false);
	swu_add(s, "sw-description", desc, strlen(desc), false);
	if (o->unknown_file)
		swu_add(s, o->unknown_file, "unknown", 7, o->wrong_chksum);
	if (!o->skip_copied && !o->desc_not_first)
		swu_add(s, "copied.bin", copied, sizeof(copied),
			o->wrong_chksum && !o->unknown_file);
	swu_add(s, "streamed.bin", streamed, sizeof(streamed), false);
	if (!o->no_trailer)
		swu_trailer(s);
}

static int run_extract(const struct swu_options *o)
{
	struct swu s = { 0 };
	int fd, ret;

	unlink(path_in_tmp("streamed.out"));
	make_swu(&s, o);
	fd = swu_fd(&s);
	ret = extract_files(fd, &sw);
	close(fd);
	swu_free(&s);
	return ret;
}

static void test_stream_extract(void **state)
{
	(void)state;
	const struct swu_options o = { 0 };
	struct img_type *img;

	sw_init();
	assert_int_equal(run_extract(&o), 0);
	assert_string_equal(sw.version, "1.0");
	/* copied: extracted to TMPDIR to be installed later */
	assert_true(file_equal(path_in_tmp("copied.bin"), copied, sizeof(copied)));
	/* streamed: already installed */
	assert_true(file_equal(path_in_tmp("streamed.out"), streamed, sizeof(streamed)));
	LIST_FOREACH(img, &sw.images, next) {
		assert_true(img->provided);
		assert_true(img->size > 0);
	}
	sw_free();
	/* cleanup_files() removes the extracted files */
	assert_int_not_equal(access(path_in_tmp("copied.bin"), F_OK), 0);
}

static void test_stream_unknown_file(void **state)
{
	(void)state;
	const struct swu_options o = { .unknown_file = "notused.bin" };

	/* a file not in sw-description is skipped */
	sw_init();
	assert_int_equal(run_extract(&o), 0);
	assert_int_not_equal(access(path_in_tmp("notused.bin"), F_OK), 0);
	sw_free();
}

static void test_stream_errors(void **state)
{
	(void)state;
	static const struct swu_options cases[] = {
		{ .desc_not_first = true },
		{ .skip_copied = true },
		{ .wrong_hash = true },
		{ .wrong_chksum = true },
		/* also a skipped file must have the right checksum */
		{ .wrong_chksum = true, .unknown_file = "notused.bin" },
		{ .unknown_file = "../escape.bin" },
		{ .unknown_file = "/abs.bin" },
		{ .no_trailer = true },
		/* the archive does not match sw-description */
		{ .extra_desc = "images: ( { filename = \"missing.img\"; device = \"/dev/null\"; } );" },
	};

	for (unsigned int i = 0; i < ARRAY_SIZE(cases); i++) {
		sw_init();
		if (run_extract(&cases[i]) == 0)
			fail_msg("case %u: invalid SWU accepted", i);
		sw_free();
	}
}

static void test_stream_size_mismatch(void **state)
{
	(void)state;
	char desc[2048], h1[65], h2[65];
	struct swu s = { 0 };
	int fd;

	/* the size in sw-description must match the size in the archive */
	hash_ascii(copied, sizeof(copied), h1);
	hash_ascii(streamed, sizeof(streamed), h2);
	make_desc(desc, sizeof(desc), h1, h2, NULL);
	char *p = strstr(desc, "sha256");
	assert_non_null(p);
	memmove(p + 12, p, strlen(p) + 1);
	memcpy(p, "size = 1234;", 12);

	sw_init();
	swu_add(&s, "sw-description", desc, strlen(desc), false);
	swu_add(&s, "copied.bin", copied, sizeof(copied), false);
	swu_add(&s, "streamed.bin", streamed, sizeof(streamed), false);
	swu_trailer(&s);
	fd = swu_fd(&s);
	assert_int_not_equal(extract_files(fd, &sw), 0);
	close(fd);
	swu_free(&s);
	sw_free();
}

#ifdef CONFIG_HW_COMPATIBILITY
static void test_stream_hw_incompatible(void **state)
{
	(void)state;
	const struct swu_options o = { 0 };

	sw_init();
	strlcpy(sw.hw.revision, "2.0", sizeof(sw.hw.revision));
	assert_int_not_equal(run_extract(&o), 0);
	/* nothing was installed */
	assert_int_not_equal(access(path_in_tmp("streamed.out"), F_OK), 0);
	sw_free();
}
#endif

static void test_stream_max_size(void **state)
{
	(void)state;
	const struct swu_options o = { 0 };

	sw_init();
	sw.swdesc_max_size = 100;
	assert_int_not_equal(run_extract(&o), 0);
	sw_free();

	sw_init();
	sw.swdesc_max_size = 64 * 1024;
	assert_int_equal(run_extract(&o), 0);
	sw_free();
}

static void test_stream_dry_run(void **state)
{
	(void)state;
	const struct swu_options o = { 0 };

	/* in dry run, the streamed artifact is checked but not installed */
	sw_init();
	sw.parms.dry_run = true;
	assert_int_equal(run_extract(&o), 0);
	assert_int_not_equal(access(path_in_tmp("streamed.out"), F_OK), 0);
	sw_free();
}

static void test_stream_cpfiles(void **state)
{
	(void)state;
	static unsigned char data[40000];
	char out[128];
	int fdin, fdout;

	for (size_t i = 0; i < sizeof(data); i++)
		data[i] = (unsigned char)(i * 13);
	struct swu s = { 0 };
	swu_put(&s, data, sizeof(data));
	snprintf(out, sizeof(out), "%s/cpfiles.out", tmpdir);

	/* with a limit */
	fdin = swu_fd(&s);
	fdout = openfileoutput(out);
	assert_true(fdout >= 0);
	assert_int_equal(cpfiles(fdin, fdout, 20000), 0);
	close(fdout);
	assert_true(file_equal(out, data, 20000));
	/* the rest is still in the input */
	assert_int_equal(lseek(fdin, 0, SEEK_CUR), 20000);
	close(fdin);

	/* everything */
	fdin = swu_fd(&s);
	fdout = openfileoutput(out);
	assert_int_equal(cpfiles(fdin, fdout, 0), 0);
	close(fdout);
	assert_true(file_equal(out, data, sizeof(data)));
	close(fdin);

	/* write error */
	fdin = swu_fd(&s);
	assert_int_equal(cpfiles(fdin, -1, 0), -EIO);
	close(fdin);
	unlink(out);
	swu_free(&s);
}

static void test_stream_save(void **state)
{
	(void)state;
	const struct swu_options o = { 0 };
	struct swu s = { 0 };
	char out[256];
	int fd;

	make_swu(&s, &o);

	/* the incoming SWU is saved as it is */
	sw_init();
	snprintf(sw.output, sizeof(sw.output), "%s/saved.swu", tmpdir);
	strlcpy(out, sw.output, sizeof(out));
	fd = swu_fd(&s);
	assert_int_equal(save_stream(fd, &sw), 0);
	close(fd);
	assert_true(file_equal(out, s.buf, s.len));
	unlink(out);

	/* the directory of the output is created if missing */
	sw_init();
	snprintf(sw.output, sizeof(sw.output), "%s/newdir/sub/saved.swu", tmpdir);
	strlcpy(out, sw.output, sizeof(out));
	fd = swu_fd(&s);
	assert_int_equal(save_stream(fd, &sw), 0);
	close(fd);
	assert_true(file_equal(out, s.buf, s.len));
	unlink(out);
	rmdir(path_in_tmp("newdir/sub"));
	rmdir(path_in_tmp("newdir"));

	/* sw-description too large */
	sw_init();
	sw.swdesc_max_size = 100;
	snprintf(sw.output, sizeof(sw.output), "%s/saved.swu", tmpdir);
	fd = swu_fd(&s);
	assert_int_not_equal(save_stream(fd, &sw), 0);
	close(fd);
	assert_int_not_equal(access(path_in_tmp("saved.swu"), F_OK), 0);

	/* not a cpio archive */
	sw_init();
	memset(s.buf, 'x', s.len);
	fd = swu_fd(&s);
	assert_int_not_equal(save_stream(fd, &sw), 0);
	close(fd);
	assert_int_equal(save_stream(-1, &sw), -EINVAL);
	swu_free(&s);
}

static int stream_group_setup(void **state)
{
	(void)state;
	if (!mkdtemp(tmpdir))
		return -1;
	/* artifacts are extracted to TMPDIR */
	setenv("TMPDIR", tmpdir, 1);
	return 0;
}

static int stream_group_teardown(void **state)
{
	(void)state;
	unlink(path_in_tmp("streamed.out"));
	unlink(path_in_tmp("copied.out"));
	return rmdir(tmpdir);
}

int main(void)
{
	static const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_stream_extract),
		cmocka_unit_test(test_stream_unknown_file),
		cmocka_unit_test(test_stream_errors),
		cmocka_unit_test(test_stream_size_mismatch),
#ifdef CONFIG_HW_COMPATIBILITY
		cmocka_unit_test(test_stream_hw_incompatible),
#endif
		cmocka_unit_test(test_stream_max_size),
		cmocka_unit_test(test_stream_dry_run),
		cmocka_unit_test(test_stream_cpfiles),
		cmocka_unit_test(test_stream_save),
	};
	return cmocka_run_group_tests_name("stream_interface", tests, stream_group_setup,
					   stream_group_teardown);
}

#else

int main(void)
{
	/* signed or encrypted sw-description are not covered yet */
	return 0;
}

#endif
