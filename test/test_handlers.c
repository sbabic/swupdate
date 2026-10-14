// SPDX-FileCopyrightText: 2026 Stefano Babic <stefano.babic@swupdate.org>
//
// SPDX-License-Identifier: GPL-2.0-only

/*
 * Tests for handlers that work on files and directories: they can run
 * without special hardware. Devices are replaced by regular files.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "generated/autoconf.h"
#include "handler.h"
#include "installer.h"
#include "swupdate_crypto.h"
#include "swupdate_dict.h"
#include "swupdate_image.h"
#include "util.h"

#ifdef CONFIG_ARCHIVE
#include <archive.h>
#include <archive_entry.h>
#endif

static char tmpdir[] = "/tmp/swupdate-test-handlers-XXXXXX";

static char *path(const char *name)
{
	static char buf[4][256];
	static int i;

	i = (i + 1) % 4;
	snprintf(buf[i], sizeof(buf[i]), "%s/%s", tmpdir, name);
	return buf[i];
}

static void write_file(const char *p, const void *data, size_t len)
{
	int fd = open(p, O_CREAT | O_WRONLY | O_TRUNC, 0644);

	assert_true(fd >= 0);
	assert_int_equal(write(fd, data, len), (ssize_t)len);
	close(fd);
}

static bool file_equal(const char *p, const void *data, size_t len)
{
	unsigned char *buf = NULL;
	size_t flen = 0;
	bool ret;

	if (read_file_into_buf(p, &buf, &flen))
		return false;
	ret = flen == len && !memcmp(buf, data, len);
	free(buf);
	return ret;
}

static bool exists(const char *p)
{
	return access(p, F_OK) == 0;
}

static void sha256(const void *buf, size_t len, unsigned char *hash)
{
	unsigned char md[64];
	unsigned int mdlen = 0;
	void *dgst = swupdate_HASH_init(SHA_DEFAULT);

	assert_non_null(dgst);
	assert_int_equal(swupdate_HASH_update(dgst, buf, len), 0);
	assert_int_equal(swupdate_HASH_final(dgst, md, &mdlen), 0);
	swupdate_HASH_cleanup(dgst);
	memcpy(hash, md, SHA256_HASH_LENGTH);
}

/*
 * An image with its artifact, as prepared by the installer:
 * fdin is the artifact, size and sha256 are set.
 */
static struct img_type *new_image(const char *type, const void *data, size_t len)
{
	struct img_type *img = calloc(1, sizeof(*img));
	char *artifact = path("artifact");

	assert_non_null(img);
	strlcpy(img->type, type, sizeof(img->type));
	strlcpy(img->fname, "artifact", sizeof(img->fname));
	img->fdin = -1;
	if (data) {
		write_file(artifact, data, len);
		img->fdin = open(artifact, O_RDONLY);
		assert_true(img->fdin >= 0);
		unlink(artifact);
		img->size = len;
		sha256(data, len, img->sha256);
	}
	return img;
}

static void free_img(struct img_type *img)
{
	if (img->fdin >= 0)
		close(img->fdin);
	dict_drop_db(&img->properties);
	free(img);
}

static void set_prop(struct img_type *img, const char *key, const char *value)
{
	assert_int_equal(dict_insert_value(&img->properties, key, value), 0);
}

static int run_image_handler(struct img_type *img)
{
	return install_single_image(img, false);
}

static int run_script_handler(struct img_type *img, script_fn fn)
{
	struct installer_handler *hnd = find_handler(img);
	struct script_handler_data data = { .scriptfn = fn };

	assert_non_null(hnd);
	data.data = hnd->data;
	return hnd->installer(img, &data);
}

static const char payload[] = "payload of the artifact, not aligned to anything";

/*
 * raw: write to a device, here a regular file
 */
#ifdef CONFIG_RAW
static void test_handler_raw(void **state)
{
	(void)state;
	struct img_type *img;
	char expected[64 + sizeof(payload)];

	/* the device is overwritten from the start, the rest is kept */
	memset(expected, '-', sizeof(expected));
	write_file(path("device"), expected, sizeof(expected));
	img = new_image("raw", payload, sizeof(payload));
	strlcpy(img->device, path("device"), sizeof(img->device));
	assert_int_equal(run_image_handler(img), 0);
	memcpy(expected, payload, sizeof(payload));
	assert_true(file_equal(path("device"), expected, sizeof(expected)));
	free_img(img);

	/* offset in the device */
	memset(expected, '-', sizeof(expected));
	write_file(path("device"), expected, sizeof(expected));
	img = new_image("raw", payload, sizeof(payload));
	strlcpy(img->device, path("device"), sizeof(img->device));
	img->seek = 64;
	assert_int_equal(run_image_handler(img), 0);
	memcpy(expected + 64, payload, sizeof(payload));
	assert_true(file_equal(path("device"), expected, sizeof(expected)));
	free_img(img);

	/* wrong hash */
	img = new_image("raw", payload, sizeof(payload));
	strlcpy(img->device, path("device"), sizeof(img->device));
	img->sha256[0] ^= 1;
	assert_int_not_equal(run_image_handler(img), 0);
	free_img(img);

	/* the device does not exist */
	img = new_image("raw", payload, sizeof(payload));
	strlcpy(img->device, path("nodevice"), sizeof(img->device));
	assert_int_equal(run_image_handler(img), -ENODEV);
	free_img(img);
	unlink(path("device"));
}

static void test_handler_rawfile(void **state)
{
	(void)state;
	struct img_type *img;

	img = new_image("rawfile", payload, sizeof(payload));
	strlcpy(img->path, path("file.out"), sizeof(img->path));
	assert_int_equal(run_image_handler(img), 0);
	assert_true(file_equal(path("file.out"), payload, sizeof(payload)));
	free_img(img);

	/* an existing file is replaced, not only overwritten */
	write_file(path("file.out"), "a much longer content that must disappear completely",
		   strlen("a much longer content that must disappear completely"));
	img = new_image("rawfile", "short", 5);
	strlcpy(img->path, path("file.out"), sizeof(img->path));
	assert_int_equal(run_image_handler(img), 0);
	assert_true(file_equal(path("file.out"), "short", 5));
	free_img(img);
	unlink(path("file.out"));

	/* path is mandatory */
	img = new_image("rawfile", payload, sizeof(payload));
	assert_int_not_equal(run_image_handler(img), 0);
	free_img(img);

	/* the destination directory is created only if requested */
	img = new_image("rawfile", payload, sizeof(payload));
	strlcpy(img->path, path("newdir/sub/file.out"), sizeof(img->path));
	assert_int_not_equal(run_image_handler(img), 0);
	free_img(img);
	img = new_image("rawfile", payload, sizeof(payload));
	strlcpy(img->path, path("newdir/sub/file.out"), sizeof(img->path));
	set_prop(img, "create-destination", "true");
	assert_int_equal(run_image_handler(img), 0);
	assert_true(file_equal(path("newdir/sub/file.out"), payload, sizeof(payload)));
	free_img(img);
	unlink(path("newdir/sub/file.out"));
	rmdir(path("newdir/sub"));
	rmdir(path("newdir"));
}

static void test_handler_rawfile_atomic(void **state)
{
	(void)state;
	struct img_type *img;

	/* written to a temporary file and renamed */
	write_file(path("atomic.out"), "old", 3);
	img = new_image("rawfile", payload, sizeof(payload));
	strlcpy(img->path, path("atomic.out"), sizeof(img->path));
	set_prop(img, "atomic-install", "true");
	assert_int_equal(run_image_handler(img), 0);
	assert_true(file_equal(path("atomic.out"), payload, sizeof(payload)));
	assert_false(exists(path("atomic.out.tmp")));
	free_img(img);

	/* on error, the old file is kept and the temporary file removed */
	write_file(path("atomic.out"), "old", 3);
	img = new_image("rawfile", payload, sizeof(payload));
	strlcpy(img->path, path("atomic.out"), sizeof(img->path));
	set_prop(img, "atomic-install", "true");
	img->sha256[0] ^= 1;
	assert_int_not_equal(run_image_handler(img), 0);
	assert_true(file_equal(path("atomic.out"), "old", 3));
	assert_false(exists(path("atomic.out.tmp")));
	free_img(img);
	unlink(path("atomic.out"));
}
#endif

/*
 * dummy: reads and verifies the artifact, installs nothing
 */
static void test_handler_dummy(void **state)
{
	(void)state;
	struct img_type *img;

	img = new_image("dummy", payload, sizeof(payload));
	assert_int_equal(run_image_handler(img), 0);
	free_img(img);

	img = new_image("dummy", payload, sizeof(payload));
	img->sha256[0] ^= 1;
	assert_int_not_equal(run_image_handler(img), 0);
	free_img(img);
}

/*
 * executor: the artifact is the input of a command
 */
static void test_handler_executor(void **state)
{
	(void)state;
	struct img_type *img;
	char parms[512];

	snprintf(parms, sizeof(parms), "-c 'cat > %s'", path("exec.out"));
	img = new_image("executor", payload, sizeof(payload));
	set_prop(img, "cmd", "/bin/sh");
	set_prop(img, "parms", parms);
	assert_int_equal(run_image_handler(img), 0);
	assert_true(file_equal(path("exec.out"), payload, sizeof(payload)));
	free_img(img);

	/* wrong hash: the command gets the data, but the install fails */
	img = new_image("executor", payload, sizeof(payload));
	set_prop(img, "cmd", "/bin/sh");
	set_prop(img, "parms", parms);
	img->sha256[0] ^= 1;
	assert_int_not_equal(run_image_handler(img), 0);
	free_img(img);
	unlink(path("exec.out"));

	/* the result of the command */
	img = new_image("executor", payload, sizeof(payload));
	set_prop(img, "cmd", "/bin/sh");
	set_prop(img, "parms", "-c 'cat > /dev/null; exit 4'");
	assert_int_equal(run_image_handler(img), 4);
	free_img(img);

	/*
	 * A command that does not read the data: small data fits into the
	 * pipe, larger data cannot be transferred, but it does not block
	 */
	img = new_image("executor", payload, sizeof(payload));
	set_prop(img, "cmd", "/bin/true");
	assert_int_equal(run_image_handler(img), 0);
	free_img(img);
	size_t biglen = 1024 * 1024;
	char *big = calloc(1, biglen);
	assert_non_null(big);
	img = new_image("executor", big, biglen);
	free(big);
	set_prop(img, "cmd", "/bin/true");
	assert_int_not_equal(run_image_handler(img), 0);
	free_img(img);

	/* cmd is mandatory and must exist */
	img = new_image("executor", payload, sizeof(payload));
	assert_int_equal(run_image_handler(img), -EINVAL);
	free_img(img);
	img = new_image("executor", payload, sizeof(payload));
	set_prop(img, "cmd", path("nocommand"));
	assert_int_equal(run_image_handler(img), -EINVAL);
	free_img(img);
}

/*
 * readback: verifies the hash of a device after the installation
 */
#ifdef CONFIG_READBACKHANDLER
static void test_handler_readback(void **state)
{
	(void)state;
	struct img_type *img;
	char ascii[2 * SHA256_HASH_LENGTH + 1], size[32];
	unsigned char hash[SHA256_HASH_LENGTH];
	char device[128 + sizeof(payload)];

	memset(device, 'x', 128);
	memcpy(device + 128, payload, sizeof(payload));
	write_file(path("rbdevice"), device, sizeof(device));
	sha256(payload, sizeof(payload), hash);
	hash_to_ascii(hash, ascii);
	snprintf(size, sizeof(size), "%zu", sizeof(payload));

	img = new_image("readback", NULL, 0);
	strlcpy(img->device, path("rbdevice"), sizeof(img->device));
	set_prop(img, "sha256", ascii);
	set_prop(img, "size", size);
	set_prop(img, "offset", "128");
	/* only after the installation */
	assert_int_equal(run_script_handler(img, PREINSTALL), 0);
	assert_int_equal(run_script_handler(img, POSTINSTALL), 0);
	free_img(img);

	/* the device does not contain the expected data */
	img = new_image("readback", NULL, 0);
	strlcpy(img->device, path("rbdevice"), sizeof(img->device));
	set_prop(img, "sha256", ascii);
	set_prop(img, "size", size);
	set_prop(img, "offset", "127");
	assert_int_not_equal(run_script_handler(img, POSTINSTALL), 0);
	free_img(img);

	/* sha256 is mandatory */
	img = new_image("readback", NULL, 0);
	strlcpy(img->device, path("rbdevice"), sizeof(img->device));
	set_prop(img, "size", size);
	assert_int_equal(run_script_handler(img, POSTINSTALL), -EINVAL);
	free_img(img);
	unlink(path("rbdevice"));
}
#endif

/*
 * shellscript: the script is called with the phase and with data
 */
#ifdef CONFIG_SHELLSCRIPTHANDLER
static struct img_type *new_script(const char *type, const char *script, const char *data)
{
	char p[256];
	struct img_type *img = new_image(type, NULL, 0);

	snprintf(p, sizeof(p), "%s%s", get_tmpdirscripts(), "test.sh");
	write_file(p, script, strlen(script));
	strlcpy(img->fname, "test.sh", sizeof(img->fname));
	if (data)
		strlcpy(img->type_data, data, sizeof(img->type_data));
	return img;
}

static void test_handler_shellscript(void **state)
{
	(void)state;
	struct img_type *img;
	char script[512], data[256];

	/* the script records its arguments */
	snprintf(script, sizeof(script), "#!/bin/sh\necho \"$@\" >> %s\n", path("args"));
	img = new_script("shellscript", script, "one two");
	assert_int_equal(run_script_handler(img, PREINSTALL), 0);
	assert_int_equal(run_script_handler(img, POSTINSTALL), 0);
	assert_int_equal(run_script_handler(img, POSTFAILURE), 0);
	assert_true(file_equal(path("args"), "preinst one two\npostinst one two\npostfailure one two\n",
			       strlen("preinst one two\npostinst one two\npostfailure one two\n")));
	free_img(img);
	unlink(path("args"));

	/* preinstall and postinstall run only in their phase */
	img = new_script("preinstall", script, NULL);
	assert_int_equal(run_script_handler(img, PREINSTALL), 0);
	assert_int_equal(run_script_handler(img, POSTINSTALL), 0);
	free_img(img);
	img = new_script("postinstall", script, NULL);
	assert_int_equal(run_script_handler(img, PREINSTALL), 0);
	assert_int_equal(run_script_handler(img, POSTINSTALL), 0);
	free_img(img);
	assert_true(file_equal(path("args"), "\n\n", 2));
	unlink(path("args"));

	/* data is passed completely, also if long */
	memset(data, 'd', sizeof(data) - 1);
	data[sizeof(data) - 1] = '\0';
	img = new_script("shellscript", script, data);
	assert_int_equal(run_script_handler(img, POSTINSTALL), 0);
	char expected[300];
	snprintf(expected, sizeof(expected), "postinst %s\n", data);
	assert_true(file_equal(path("args"), expected, strlen(expected)));
	free_img(img);
	unlink(path("args"));

	/* the result of the script */
	img = new_script("shellscript", "#!/bin/sh\nexit 3\n", NULL);
	assert_int_equal(run_script_handler(img, POSTINSTALL), 3);
	free_img(img);
	unlink(path("scripts/test.sh"));
}
#endif

/*
 * copy: copies a file or a directory, the data is installed by a
 * chained handler
 */
#if defined(CONFIG_COPY) && defined(CONFIG_RAW)
static void test_handler_copy(void **state)
{
	(void)state;
	struct img_type *img;

	write_file(path("source"), payload, sizeof(payload));

	/* single file, by default with the raw handler */
	write_file(path("copy.dev"), "", 0);
	img = new_image("copy", NULL, 0);
	strlcpy(img->device, path("copy.dev"), sizeof(img->device));
	set_prop(img, "copyfrom", path("source"));
	assert_int_equal(run_script_handler(img, POSTINSTALL), 0);
	assert_true(file_equal(path("copy.dev"), payload, sizeof(payload)));
	free_img(img);

	/* part of the source with the rawfile handler */
	img = new_image("copy", NULL, 0);
	strlcpy(img->path, path("copy.out"), sizeof(img->path));
	set_prop(img, "copyfrom", path("source"));
	set_prop(img, "chain", "rawfile");
	set_prop(img, "offset", "8");
	set_prop(img, "size", "10");
	assert_int_equal(run_script_handler(img, POSTINSTALL), 0);
	assert_true(file_equal(path("copy.out"), payload + 8, 10));
	free_img(img);
	unlink(path("copy.out"));

	/* type selects the phase */
	img = new_image("copy", NULL, 0);
	strlcpy(img->path, path("copy.out"), sizeof(img->path));
	set_prop(img, "copyfrom", path("source"));
	set_prop(img, "chain", "rawfile");
	set_prop(img, "type", "preinstall");
	assert_int_equal(run_script_handler(img, POSTINSTALL), 0);
	assert_false(exists(path("copy.out")));
	assert_int_equal(run_script_handler(img, PREINSTALL), 0);
	assert_true(exists(path("copy.out")));
	free_img(img);
	unlink(path("copy.out"));

	/* copyfrom is mandatory and must exist */
	img = new_image("copy", NULL, 0);
	strlcpy(img->path, path("copy.out"), sizeof(img->path));
	assert_int_equal(run_script_handler(img, POSTINSTALL), -EINVAL);
	set_prop(img, "copyfrom", path("nosource"));
	assert_int_equal(run_script_handler(img, POSTINSTALL), -EINVAL);
	free_img(img);
	unlink(path("source"));
	unlink(path("copy.dev"));
}

static void test_handler_copy_recursive(void **state)
{
	(void)state;
	struct img_type *img;

	mkdir(path("srcdir"), 0755);
	mkdir(path("srcdir/sub"), 0755);
	write_file(path("srcdir/a"), "A", 1);
	write_file(path("srcdir/sub/b"), "BB", 2);
	/* special files are skipped */
	assert_int_equal(mkfifo(path("srcdir/fifo"), 0600), 0);

	img = new_image("copy", NULL, 0);
	strlcpy(img->path, path("dstdir"), sizeof(img->path));
	set_prop(img, "copyfrom", path("srcdir"));
	set_prop(img, "chain", "rawfile");
	set_prop(img, "recursive", "true");
	set_prop(img, "create-destination", "true");
	assert_int_equal(run_script_handler(img, POSTINSTALL), 0);
	assert_true(file_equal(path("dstdir/a"), "A", 1));
	assert_true(file_equal(path("dstdir/sub/b"), "BB", 2));
	assert_false(exists(path("dstdir/fifo")));
	free_img(img);

	unlink(path("dstdir/sub/b"));
	unlink(path("dstdir/a"));
	rmdir(path("dstdir/sub"));
	rmdir(path("dstdir"));
	unlink(path("srcdir/fifo"));
	unlink(path("srcdir/sub/b"));
	unlink(path("srcdir/a"));
	rmdir(path("srcdir/sub"));
	rmdir(path("srcdir"));
}
#endif

/*
 * archive: extracts below path, nothing outside
 */
#ifdef CONFIG_ARCHIVE
struct tar_entry {
	const char *name;
	const char *data;	/* NULL: directory */
	const char *link;	/* symlink target */
};

/* create a tar in memory */
static size_t make_tar(const struct tar_entry *entries, unsigned int n,
		       unsigned char *buf, size_t size)
{
	struct archive *a = archive_write_new();
	size_t used = 0;

	assert_non_null(a);
	assert_int_equal(archive_write_set_format_pax_restricted(a), ARCHIVE_OK);
	assert_int_equal(archive_write_open_memory(a, buf, size, &used), ARCHIVE_OK);
	for (unsigned int i = 0; i < n; i++) {
		struct archive_entry *e = archive_entry_new();

		archive_entry_set_pathname(e, entries[i].name);
		if (entries[i].link) {
			archive_entry_set_filetype(e, AE_IFLNK);
			archive_entry_set_symlink(e, entries[i].link);
			archive_entry_set_perm(e, 0777);
		} else if (!entries[i].data) {
			archive_entry_set_filetype(e, AE_IFDIR);
			archive_entry_set_perm(e, 0755);
		} else {
			archive_entry_set_filetype(e, AE_IFREG);
			archive_entry_set_perm(e, 0644);
			archive_entry_set_size(e, strlen(entries[i].data));
		}
		assert_int_equal(archive_write_header(a, e), ARCHIVE_OK);
		if (entries[i].data && !entries[i].link)
			assert_int_equal(archive_write_data(a, entries[i].data, strlen(entries[i].data)),
					 (la_ssize_t)strlen(entries[i].data));
		archive_entry_free(e);
	}
	assert_int_equal(archive_write_close(a), ARCHIVE_OK);
	archive_write_free(a);
	return used;
}

static int extract_tar(const struct tar_entry *entries, unsigned int n, const char *dest,
		       const char *prop, const char *value)
{
	static unsigned char buf[64 * 1024];
	size_t len = make_tar(entries, n, buf, sizeof(buf));
	struct img_type *img = new_image("archive", buf, len);
	int ret;

	if (dest)
		strlcpy(img->path, dest, sizeof(img->path));
	if (prop)
		set_prop(img, prop, value);
	ret = run_image_handler(img);
	free_img(img);
	return ret;
}

static void test_handler_archive(void **state)
{
	(void)state;
	static const struct tar_entry ok[] = {
		{ "dir", NULL, NULL },
		{ "dir/file", "content", NULL },
		{ "top", "top level", NULL },
		{ "link", NULL, "dir/file" },
	};
	char target[64];
	ssize_t l;

	mkdir(path("extract"), 0755);
	assert_int_equal(extract_tar(ok, ARRAY_SIZE(ok), path("extract"), NULL, NULL), 0);
	assert_true(file_equal(path("extract/dir/file"), "content", 7));
	assert_true(file_equal(path("extract/top"), "top level", 9));
	l = readlink(path("extract/link"), target, sizeof(target) - 1);
	assert_true(l > 0);
	target[l] = '\0';
	assert_string_equal(target, "dir/file");

	/* path is mandatory */
	assert_int_not_equal(extract_tar(ok, ARRAY_SIZE(ok), NULL, NULL, NULL), 0);

	/* the destination is created only if requested */
	assert_int_not_equal(extract_tar(ok, 1, path("newextract"), NULL, NULL), 0);
	assert_int_equal(extract_tar(ok, 1, path("newextract"), "create-destination", "true"), 0);
	assert_true(exists(path("newextract/dir")));
	rmdir(path("newextract/dir"));
	rmdir(path("newextract"));

	unlink(path("extract/link"));
	unlink(path("extract/top"));
	unlink(path("extract/dir/file"));
	rmdir(path("extract/dir"));
	rmdir(path("extract"));
}

static void test_handler_archive_outside(void **state)
{
	(void)state;
	char abs[256];
	const struct tar_entry absolute[] = { { abs, "escaped", NULL } };
	static const struct tar_entry dotdot[] = { { "../escaped", "escaped", NULL } };
	char outside[256];
	const struct tar_entry through_link[] = {
		{ "out", NULL, outside },
		{ "out/escaped", "escaped", NULL },
	};

	mkdir(path("extract"), 0755);
	mkdir(path("outside"), 0755);
	snprintf(abs, sizeof(abs), "%s", path("outside/escaped"));
	strlcpy(outside, path("outside"), sizeof(outside));

	/* nothing is written outside of path */
	assert_int_not_equal(extract_tar(absolute, 1, path("extract"), NULL, NULL), 0);
	assert_false(exists(path("outside/escaped")));
	assert_int_not_equal(extract_tar(dotdot, 1, path("extract"), NULL, NULL), 0);
	assert_false(exists(path("escaped")));
	assert_int_not_equal(extract_tar(through_link, 2, path("extract"), NULL, NULL), 0);
	assert_false(exists(path("outside/escaped")));
	unlink(path("extract/out"));

	/* unless symlinks may be followed */
	assert_int_equal(extract_tar(through_link, 2, path("extract"), "follow-symlinks", "true"), 0);
	assert_true(file_equal(path("outside/escaped"), "escaped", 7));
	/* absolute paths and ".." are always rejected */
	unlink(path("outside/escaped"));
	assert_int_not_equal(extract_tar(absolute, 1, path("extract"), "follow-symlinks", "true"), 0);
	assert_false(exists(path("outside/escaped")));

	unlink(path("extract/out"));
	rmdir(path("extract"));
	rmdir(path("outside"));
}
#endif

static int handlers_group_setup(void **state)
{
	(void)state;
	if (!mkdtemp(tmpdir))
		return -1;
	/* the archive handler uses a FIFO in TMPDIR */
	setenv("TMPDIR", tmpdir, 1);
	if (mkdir(path("scripts"), 0700))
		return -1;
	return 0;
}

static int handlers_group_teardown(void **state)
{
	(void)state;
	rmdir(path("scripts"));
	return rmdir(tmpdir);
}

int main(void)
{
	static const struct CMUnitTest tests[] = {
#ifdef CONFIG_RAW
		cmocka_unit_test(test_handler_raw),
		cmocka_unit_test(test_handler_rawfile),
		cmocka_unit_test(test_handler_rawfile_atomic),
#endif
		cmocka_unit_test(test_handler_dummy),
		cmocka_unit_test(test_handler_executor),
#ifdef CONFIG_READBACKHANDLER
		cmocka_unit_test(test_handler_readback),
#endif
#ifdef CONFIG_SHELLSCRIPTHANDLER
		cmocka_unit_test(test_handler_shellscript),
#endif
#if defined(CONFIG_COPY) && defined(CONFIG_RAW)
		cmocka_unit_test(test_handler_copy),
		cmocka_unit_test(test_handler_copy_recursive),
#endif
#ifdef CONFIG_ARCHIVE
		cmocka_unit_test(test_handler_archive),
		cmocka_unit_test(test_handler_archive_outside),
#endif
	};
	return cmocka_run_group_tests_name("handlers", tests, handlers_group_setup,
					   handlers_group_teardown);
}
