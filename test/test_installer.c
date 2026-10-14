// SPDX-FileCopyrightText: 2026 Stefano Babic <stefano.babic@swupdate.org>
//
// SPDX-License-Identifier: GPL-2.0-only

/*
 * Tests for core/installer.c: matching cpio entries with sw-description,
 * running the handlers and the scripts in the right order, updating the
 * bootloader environment and the list of installed versions.
 *
 * Handlers and bootloader are replaced by test implementations that
 * record what they are called with.
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
#include "bootloader.h"
#include "cpiohdr.h"
#include "handler.h"
#include "installer.h"
#include "swupdate.h"
#include "swupdate_crypto.h"
#include "swupdate_dict.h"
#include "swupdate_image.h"
#include "util.h"

static char tmpdir[] = "/tmp/swupdate-test-installer-XXXXXX";
static struct swupdate_cfg sw;
static struct swupdate_type_cfg update_type;

/* What the test handlers were called with */
static char calls[1024];
static char installed_data[256];
static const char *failing_image;	/* the image handler fails for this file */
static char bootenv[1024];		/* list passed to the bootloader */
static int bootenv_calls;

static void record(const char *fmt, const char *arg)
{
	size_t len = strlen(calls);

	snprintf(calls + len, sizeof(calls) - len, fmt, arg);
}

static int collect(void *out, const void *buf, size_t len)
{
	size_t cur = strlen((char *)out);

	if (cur + len >= sizeof(installed_data))
		return -1;
	memcpy((char *)out + cur, buf, len);
	((char *)out)[cur + len] = '\0';
	return 0;
}

/* image handler: reads the artifact and records the call */
static int test_image_handler(struct img_type *img, void __attribute__ ((__unused__)) *data)
{
	record("IMG:%s;", img->fname);
	if (copyimage(installed_data, img, collect) < 0)
		return -1;
	if (failing_image && !strcmp(img->fname, failing_image))
		return -EIO;
	return 0;
}

/* script handler: records when it is called */
static int test_script_handler(struct img_type *img, void *data)
{
	struct script_handler_data *script = data;
	const char *when = script->scriptfn == PREINSTALL ? "PRE" :
			   script->scriptfn == POSTINSTALL ? "POST" : "OTHER";
	char buf[64];

	snprintf(buf, sizeof(buf), "%s:%%s;", when);
	record(buf, img->fname);
	return 0;
}

static int test_apply_list(const char *script)
{
	unsigned char *buf = NULL;
	size_t len = 0;

	bootenv_calls++;
	if (read_file_into_buf(script, &buf, &len))
		return -1;
	snprintf(bootenv, sizeof(bootenv), "%s", (char *)buf);
	free(buf);
	return 0;
}

static char *test_env_get(const char __attribute__ ((__unused__)) *name) { return NULL; }
static int test_env_set(const char __attribute__ ((__unused__)) *name,
			const char __attribute__ ((__unused__)) *value) { return 0; }
static int test_env_unset(const char __attribute__ ((__unused__)) *name) { return 0; }

static bootloader test_bootloader = {
	.env_get = test_env_get,
	.env_set = test_env_set,
	.env_unset = test_env_unset,
	.apply_list = test_apply_list,
};

static void sw_init(void)
{
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
	calls[0] = '\0';
	installed_data[0] = '\0';
	bootenv[0] = '\0';
	bootenv_calls = 0;
	failing_image = NULL;
}

static void sw_free(void)
{
	struct sw_version *swver, *tmp;

	cleanup_files(&sw);
	LIST_FOREACH_SAFE(swver, &sw.installed_sw_list, next, tmp) {
		LIST_REMOVE(swver, next);
		free(swver);
	}
}

static char *tmp_path(const char *name)
{
	static char path[256];

	snprintf(path, sizeof(path), "%s/%s", tmpdir, name);
	return path;
}

static void write_tmp(const char *name, const char *content)
{
	int fd = open(tmp_path(name), O_CREAT | O_WRONLY | O_TRUNC, 0600);

	assert_true(fd >= 0);
	assert_int_equal(write(fd, content, strlen(content)), (ssize_t)strlen(content));
	close(fd);
}

static bool tmp_exists(const char *name)
{
	return access(tmp_path(name), F_OK) == 0;
}

/* entries are added at the end to keep the order of sw-description */
static struct img_type *add_entry(struct imglist *list, const char *fname,
				  const char *type)
{
	struct img_type *img = calloc(1, sizeof(*img)), *last, *it;

	assert_non_null(img);
	strlcpy(img->fname, fname, sizeof(img->fname));
	strlcpy(img->type, type, sizeof(img->type));
	img->provided = 1;
	last = NULL;
	LIST_FOREACH(it, list, next)
		last = it;
	if (last)
		LIST_INSERT_AFTER(last, img, next);
	else
		LIST_INSERT_HEAD(list, img, next);
	return img;
}

static struct img_type *add_image(const char *fname, const char *content)
{
	if (content)
		write_tmp(fname, content);
	return add_entry(&sw.images, fname, "testimg");
}

static struct img_type *add_script(const char *fname, const char *content)
{
	struct img_type *img;

	write_tmp(fname, content);
	img = add_entry(&sw.scripts, fname, "testscript");
	img->is_script = 1;
	img->size = strlen(content);
	return img;
}

/*
 * check_if_required()
 */
static void test_installer_check_if_required(void **state)
{
	(void)state;
	struct filehdr fdh = { 0 };
	struct img_type *img = NULL, *a, *b;

	sw_init();
	a = add_entry(&sw.images, "a.bin", "testimg");
	b = add_entry(&sw.images, "b.bin", "testimg");
	a->provided = b->provided = 0;
	b->install_directly = true;

	strlcpy(fdh.filename, "a.bin", sizeof(fdh.filename));
	fdh.size = 10;
	assert_int_equal(check_if_required(&sw.images, &fdh, "/dir/", &img), COPY_FILE);
	assert_ptr_equal(img, a);
	assert_true(a->provided);
	assert_int_equal(a->size, 10);
	assert_string_equal(a->extract_file, "/dir/a.bin");

	strlcpy(fdh.filename, "b.bin", sizeof(fdh.filename));
	assert_int_equal(check_if_required(&sw.images, &fdh, "/dir/", &img), INSTALL_FROM_STREAM);
	assert_ptr_equal(img, b);

	/* not in sw-description */
	strlcpy(fdh.filename, "other.bin", sizeof(fdh.filename));
	assert_int_equal(check_if_required(&sw.images, &fdh, "/dir/", &img), SKIP_FILE);

	/* the size in sw-description must match */
	strlcpy(fdh.filename, "a.bin", sizeof(fdh.filename));
	fdh.size = 11;
	assert_int_equal(check_if_required(&sw.images, &fdh, "/dir/", &img), -EINVAL);

	/* a file streamed to a handler cannot be used by another one */
	b->size = 0;
	add_entry(&sw.images, "b.bin", "testimg");
	strlcpy(fdh.filename, "b.bin", sizeof(fdh.filename));
	assert_int_equal(check_if_required(&sw.images, &fdh, "/dir/", &img), -EINVAL);

	/* path too long */
	char longdir[sizeof(a->extract_file)];
	memset(longdir, 'd', sizeof(longdir) - 1);
	longdir[sizeof(longdir) - 1] = '\0';
	a->size = 0;
	strlcpy(fdh.filename, "a.bin", sizeof(fdh.filename));
	assert_int_equal(check_if_required(&sw.images, &fdh, longdir, &img), -EBADF);
	sw_free();
}

/*
 * update_installed_image_version()
 */
static const char *installed_version(const char *name)
{
	struct sw_version *swver;

	LIST_FOREACH(swver, &sw.installed_sw_list, next)
		if (!strcmp(swver->name, name))
			return swver->version;
	return NULL;
}

static int count_installed(void)
{
	struct sw_version *swver;
	int n = 0;

	LIST_FOREACH(swver, &sw.installed_sw_list, next)
		n++;
	return n;
}

static void test_installer_versions(void **state)
{
	(void)state;
	struct img_type img = { 0 };

	sw_init();
	/* new component */
	strlcpy(img.id.name, "rootfs", sizeof(img.id.name));
	strlcpy(img.id.version, "1.0", sizeof(img.id.version));
	assert_true(update_installed_image_version(&sw.installed_sw_list, &img));
	assert_string_equal(installed_version("rootfs"), "1.0");

	/* the version of an installed component is updated */
	strlcpy(img.id.version, "2.0", sizeof(img.id.version));
	assert_true(update_installed_image_version(&sw.installed_sw_list, &img));
	assert_string_equal(installed_version("rootfs"), "2.0");
	assert_int_equal(count_installed(), 1);

	/* an image without version does not change the installed version */
	img.id.version[0] = '\0';
	assert_false(update_installed_image_version(&sw.installed_sw_list, &img));
	assert_string_equal(installed_version("rootfs"), "2.0");

	/* an image without name is not a component */
	img.id.name[0] = '\0';
	strlcpy(img.id.version, "3.0", sizeof(img.id.version));
	assert_false(update_installed_image_version(&sw.installed_sw_list, &img));
	assert_int_equal(count_installed(), 1);

	assert_false(update_installed_image_version(NULL, &img));
	sw_free();
}

/*
 * install_single_image()
 */
static void test_installer_single_image(void **state)
{
	(void)state;
	struct img_type *img;

	sw_init();
	img = add_image("single.bin", "single image data");
	img->fdin = open(tmp_path("single.bin"), O_RDONLY);
	img->size = strlen("single image data");
	assert_int_equal(install_single_image(img, false), 0);
	assert_string_equal(calls, "IMG:single.bin;");
	assert_string_equal(installed_data, "single image data");
	close(img->fdin);

	/* dry run: the dummy handler replaces the handler */
	calls[0] = '\0';
	img->fdin = open(tmp_path("single.bin"), O_RDONLY);
	assert_int_equal(install_single_image(img, true), 0);
	assert_string_equal(calls, "");
	close(img->fdin);

	/* unknown handler */
	strlcpy(img->type, "nohandler", sizeof(img->type));
	assert_int_not_equal(install_single_image(img, false), 0);

	/* the error of the handler is returned */
	strlcpy(img->type, "testimg", sizeof(img->type));
	img->fdin = open(tmp_path("single.bin"), O_RDONLY);
	failing_image = "single.bin";
	assert_int_equal(install_single_image(img, false), -EIO);
	close(img->fdin);
	sw_free();
}

/*
 * install_images()
 */
static void test_installer_install_images(void **state)
{
	(void)state;
	struct img_type *img;

	sw_init();
	add_script("script.sh", "#!/bin/sh\n");
	img = add_image("a.bin", "AAA");
	strlcpy(img->id.name, "comp-a", sizeof(img->id.name));
	strlcpy(img->id.version, "1.2", sizeof(img->id.version));
	add_image("b.bin", "BBB");
	/* already installed from the stream */
	img = add_image("streamed.bin", NULL);
	img->install_directly = true;
	dict_set_value(&sw.bootloader, "bootvar", "value");
	dict_set_value(&sw.bootloader, "dropvar", "");
	snprintf(sw.output_swversions, sizeof(sw.output_swversions), "%s/sw-versions", tmpdir);

	assert_int_equal(install_images(&sw), 0);
	/* preinstall scripts, images in order, postinstall scripts */
	assert_string_equal(calls, "PRE:script.sh;IMG:a.bin;IMG:b.bin;POST:script.sh;");
	assert_string_equal(installed_data, "AAABBB");
	/* the script is extracted to the scripts directory */
	assert_true(tmp_exists("scripts/script.sh"));
	/* bootloader environment */
	assert_int_equal(bootenv_calls, 1);
	assert_non_null(strstr(bootenv, "bootvar=value\n"));
	assert_non_null(strstr(bootenv, "dropvar=\n"));
	/* installed versions */
	assert_string_equal(installed_version("comp-a"), "1.2");
	unsigned char *buf = NULL;
	size_t len;
	assert_int_equal(read_file_into_buf(sw.output_swversions, &buf, &len), 0);
	assert_string_equal((char *)buf, "comp-a\t\t1.2\n");
	free(buf);
	unlink(sw.output_swversions);
	sw_free();
	/* cleanup_files() removes the temporary files */
	assert_false(tmp_exists("a.bin"));
	assert_false(tmp_exists("scripts/script.sh"));
}

static void test_installer_dry_run(void **state)
{
	(void)state;

	sw_init();
	sw.parms.dry_run = true;
	add_script("script.sh", "#!/bin/sh\n");
	add_image("a.bin", "AAA");
	dict_set_value(&sw.bootloader, "bootvar", "value");

	/* no scripts, no handlers, no bootloader */
	assert_int_equal(install_images(&sw), 0);
	assert_string_equal(calls, "");
	assert_int_equal(bootenv_calls, 0);
	sw_free();
}

static void test_installer_errors(void **state)
{
	(void)state;
	struct img_type *img;

	/* a handler fails: the next images and the postinstall scripts are not run */
	sw_init();
	add_script("script.sh", "#!/bin/sh\n");
	add_image("a.bin", "AAA");
	add_image("b.bin", "BBB");
	add_image("c.bin", "CCC");
	dict_set_value(&sw.bootloader, "bootvar", "value");
	failing_image = "b.bin";
	assert_int_not_equal(install_images(&sw), 0);
	assert_string_equal(calls, "PRE:script.sh;IMG:a.bin;IMG:b.bin;");
	assert_int_equal(bootenv_calls, 0);
	sw_free();

	/* the artifact is missing in TMPDIR */
	sw_init();
	add_image("missing.bin", NULL);
	unlink(tmp_path("missing.bin"));
	assert_int_not_equal(install_images(&sw), 0);
	assert_string_equal(calls, "");
	sw_free();

	/* a script in sw-description is not in the SWU */
	sw_init();
	img = add_script("script.sh", "#!/bin/sh\n");
	img->provided = 0;
	add_image("a.bin", "AAA");
	assert_int_not_equal(install_images(&sw), 0);
	assert_string_equal(calls, "");
	sw_free();

	/* the script does not match its hash: it is not left to be executed */
	sw_init();
	img = add_script("script.sh", "#!/bin/sh\n");
	memset(img->sha256, 0x11, sizeof(img->sha256));
	assert_int_not_equal(install_images(&sw), 0);
	assert_string_equal(calls, "");
	assert_false(tmp_exists("scripts/script.sh"));
	sw_free();
}

static void test_installer_same_location(void **state)
{
	(void)state;
	struct img_type *img;

	/* the file is already in its final location: no handler is called */
	sw_init();
	img = add_image("inplace.bin", "data");
	strlcpy(img->path, tmp_path("inplace.bin"), sizeof(img->path));
	strlcpy(img->extract_file, tmp_path("inplace.bin"), sizeof(img->extract_file));
	add_image("other.bin", "other");
	assert_int_equal(install_images(&sw), 0);
	assert_string_equal(calls, "IMG:other.bin;");
	sw_free();
	/* not in the list anymore, it is not removed by cleanup_files() */
	unlink(tmp_path("inplace.bin"));
}

static void test_installer_update_commands(void **state)
{
	(void)state;

	sw_init();
	snprintf(update_type.preupdatecmd, sizeof(update_type.preupdatecmd),
		 "touch %s", tmp_path("pre-done"));
	snprintf(update_type.postupdatecmd, sizeof(update_type.postupdatecmd),
		 "touch %s", tmp_path("post-done"));

	/* not executed in dry run */
	sw.parms.dry_run = true;
	assert_int_equal(preupdatecmd(&sw), 0);
	assert_int_equal(postupdate(&sw, NULL), 0);
	assert_false(tmp_exists("pre-done"));
	assert_false(tmp_exists("post-done"));

	sw.parms.dry_run = false;
	assert_int_equal(preupdatecmd(&sw), 0);
	assert_true(tmp_exists("pre-done"));
	assert_int_equal(postupdate(&sw, NULL), 0);
	assert_true(tmp_exists("post-done"));
	unlink(tmp_path("pre-done"));
	unlink(tmp_path("post-done"));

	/* the result of the command is returned */
	strlcpy(update_type.preupdatecmd, "exit 3", sizeof(update_type.preupdatecmd));
	assert_int_equal(preupdatecmd(&sw), 3);
	/* no command */
	update_type.preupdatecmd[0] = '\0';
	assert_int_equal(preupdatecmd(&sw), 0);
	assert_int_equal(preupdatecmd(NULL), 0);
	sw_free();
}

static int installer_group_setup(void **state)
{
	(void)state;
	if (!mkdtemp(tmpdir))
		return -1;
	setenv("TMPDIR", tmpdir, 1);
	/* created by SWUpdate at startup */
	if (mkdir(tmp_path("scripts"), 0700))
		return -1;
	if (register_handler("testimg", test_image_handler,
			     IMAGE_HANDLER | FILE_HANDLER, NULL) ||
	    register_handler("testscript", test_script_handler, SCRIPT_HANDLER, NULL))
		return -1;
	if (register_bootloader("testbl", &test_bootloader) || set_bootloader("testbl"))
		return -1;
	return 0;
}

static int installer_group_teardown(void **state)
{
	(void)state;
	unlink(tmp_path("single.bin"));
	rmdir(tmp_path("scripts"));
	return rmdir(tmpdir);
}

int main(void)
{
	static const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_installer_check_if_required),
		cmocka_unit_test(test_installer_versions),
		cmocka_unit_test(test_installer_single_image),
		cmocka_unit_test(test_installer_install_images),
		cmocka_unit_test(test_installer_dry_run),
		cmocka_unit_test(test_installer_errors),
		cmocka_unit_test(test_installer_same_location),
		cmocka_unit_test(test_installer_update_commands),
	};
	return cmocka_run_group_tests_name("installer", tests, installer_group_setup,
					   installer_group_teardown);
}
