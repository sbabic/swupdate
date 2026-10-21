// SPDX-FileCopyrightText: 2026 Stefano Babic <stefano.babic@swupdate.org>
//
// SPDX-License-Identifier: GPL-2.0-only

/*
 * Tests for the sw-description parser.
 *
 * Each test describes sw-description both in libconfig and in JSON
 * syntax: the same checks run on the result of both parsers.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "generated/autoconf.h"
#include "swupdate.h"
#include "swupdate_image.h"
#include "swupdate_dict.h"
#include "parsers.h"
#include "installer.h"
#include "hw-compatibility.h"
#include "swupdate_crypto.h"
#include "util.h"

/* sha256 used for images, the value is not checked */
#define HASH "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20"

enum fmt {
	FMT_CFG,
	FMT_JSON,
	FMT_LAST
};

/*
 * cfg and json are the content of "software", without version and
 * hardware-compatibility that are added, unless raw is set: then they
 * are the whole sw-description. Both can contain one %s, replaced by
 * the directory for temporary files.
 */
struct swd {
	const char *cfg;
	const char *json;
	bool raw;
};

static struct swupdate_cfg sw;
static char tmpdir[] = "/tmp/swupdate-test-parser-XXXXXX";
static char *parse_error;

static const char *fmt_name[] = { "libconfig", "JSON" };

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
	/* set, so that the parser does not read /etc/hwrevision */
	strlcpy(sw.hw.boardname, "board", sizeof(sw.hw.boardname));
	strlcpy(sw.hw.revision, "1.0", sizeof(sw.hw.revision));
	parse_error = NULL;
}

static void sw_free(void)
{
	struct sw_version *swver, *tmp;

	cleanup_files(&sw);
	LIST_FOREACH_SAFE(swver, &sw.installed_sw_list, next, tmp) {
		LIST_REMOVE(swver, next);
		free(swver);
	}
	free(parse_error);
	parse_error = NULL;
}

static void add_installed(const char *name, const char *version)
{
	struct sw_version *swver = calloc(1, sizeof(*swver));

	assert_non_null(swver);
	strlcpy(swver->name, name, sizeof(swver->name));
	strlcpy(swver->version, version, sizeof(swver->version));
	LIST_INSERT_HEAD(&sw.installed_sw_list, swver, next);
}

static int parse_swd(const struct swd *t, enum fmt f)
{
	char path[128], body[16384], text[16384];
	const char *src = (f == FMT_CFG) ? t->cfg : t->json;
	int fd, len;

	snprintf(body, sizeof(body), src, tmpdir);
	if (t->raw)
		len = snprintf(text, sizeof(text), "%s", body);
	else if (f == FMT_CFG)
		len = snprintf(text, sizeof(text),
			       "software = {\n"
			       "  version = \"1.0\";\n"
			       "  hardware-compatibility = [ \"1.0\" ];\n"
			       "  %s\n"
			       "};\n", body);
	else
		len = snprintf(text, sizeof(text),
			       "{ \"software\": {\n"
			       "  \"version\": \"1.0\",\n"
			       "  \"hardware-compatibility\": [ \"1.0\" ],\n"
			       "  %s\n"
			       "} }\n", body);
	assert_true(len > 0 && len < (int)sizeof(text));

	snprintf(path, sizeof(path), "%s/test-sw-description", tmpdir);
	fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
	assert_true(fd >= 0);
	assert_int_equal(write(fd, text, len), len);
	close(fd);

	return f == FMT_CFG ? parse_cfg(&sw, path, &parse_error)
			    : parse_json(&sw, path, &parse_error);
}

static enum fmt first_fmt(void)
{
#ifdef CONFIG_LIBCONFIG
	return FMT_CFG;
#else
	return FMT_JSON;
#endif
}

/* run body for each format, with a clean configuration */
#define FOR_EACH_FORMAT(f) \
	for (enum fmt f = first_fmt(); f < FMT_LAST; f++)

static int count_images(struct imglist *list)
{
	struct img_type *img;
	int n = 0;

	LIST_FOREACH(img, list, next)
		n++;
	return n;
}

static struct img_type *find_image(struct imglist *list, const char *fname)
{
	struct img_type *img;

	LIST_FOREACH(img, list, next)
		if (!strcmp(img->fname, fname))
			return img;
	return NULL;
}

/* a missing value fails the test instead of crashing it */
#define assert_dict_value(dict, key, expected) do { \
	const char *_v = dict_get_value(dict, key); \
	assert_non_null(_v); \
	assert_string_equal(_v, expected); \
} while (0)

static bool list_has(struct dict_list *list, const char *value)
{
	struct dict_list_elem *elem;

	if (!list)
		return false;
	LIST_FOREACH(elem, list, next)
		if (!strcmp(elem->value, value))
			return true;
	return false;
}

static int count_list(struct dict_list *list)
{
	struct dict_list_elem *elem;
	int n = 0;

	if (!list)
		return 0;
	LIST_FOREACH(elem, list, next)
		n++;
	return n;
}

static void test_parser_minimal(void **state)
{
	(void)state;
	static const struct swd t = {
		.cfg = "images: ( { filename = \"rootfs.ext4\"; device = \"/dev/mmcblk0p2\";"
		       " sha256 = \"" HASH "\"; } );",
		.json = "\"images\": [ { \"filename\": \"rootfs.ext4\", \"device\": \"/dev/mmcblk0p2\","
			" \"sha256\": \"" HASH "\" } ]",
	};

	FOR_EACH_FORMAT(f) {
		sw_init();
		assert_int_equal(parse_swd(&t, f), 0);
		assert_string_equal(sw.version, "1.0");
		assert_int_equal(count_images(&sw.images), 1);
		struct img_type *img = LIST_FIRST(&sw.images);
		assert_string_equal(img->fname, "rootfs.ext4");
		assert_string_equal(img->device, "/dev/mmcblk0p2");
		/* default handler for an image with device */
		assert_string_equal(img->type, "raw");
		assert_true(IsValidHash(img->sha256));
		assert_int_equal(img->sha256[0], 0x01);
		assert_int_equal(img->sha256[31], 0x20);
		assert_int_equal(img->compressed, COMPRESSED_FALSE);
		assert_false(img->is_encrypted);
		assert_false(img->install_directly);
		assert_int_equal(img->skip, SKIP_NONE);
		assert_int_equal(sw.reboot_enabled, REBOOT_UNSET);
		assert_true(sw.bootloader_state_marker);
		assert_true(sw.bootloader_transaction_marker);
		sw_free();
	}
}

static void test_parser_errors(void **state)
{
	(void)state;
	static const struct swd missing_version = {
		.raw = true,
		.cfg = "software = { hardware-compatibility = [ \"1.0\" ];"
		       " images: ( { filename = \"a\"; device = \"/dev/a\"; } ); };",
		.json = "{ \"software\": { \"hardware-compatibility\": [ \"1.0\" ],"
			" \"images\": [ { \"filename\": \"a\", \"device\": \"/dev/a\" } ] } }",
	};
	static const struct swd nothing = { .cfg = "", .json = "\"description\": \"empty\"" };
	static const struct swd syntax = {
		.raw = true,
		.cfg = "software = { version = ; };",
		.json = "{ \"software\": { \"version\": ",
	};
	static const struct swd compressed = {
		.cfg = "images: ( { filename = \"a\"; device = \"/dev/a\"; compressed = \"rar\"; } );",
		.json = "\"images\": [ { \"filename\": \"a\", \"device\": \"/dev/a\", \"compressed\": \"rar\" } ]",
	};
	static const struct swd offset = {
		.cfg = "images: ( { filename = \"a\"; device = \"/dev/a\"; offset = \"abc\"; } );",
		.json = "\"images\": [ { \"filename\": \"a\", \"device\": \"/dev/a\", \"offset\": \"abc\" } ]",
	};

	FOR_EACH_FORMAT(f) {
		sw_init();
		assert_int_not_equal(parse_swd(&missing_version, f), 0);
		sw_free();

		sw_init();
		assert_int_not_equal(parse_swd(&nothing, f), 0);
		sw_free();

		sw_init();
		assert_int_not_equal(parse_swd(&syntax, f), 0);
		/* the error is reported to the caller */
		assert_non_null(parse_error);
		sw_free();

		sw_init();
		assert_int_not_equal(parse_swd(&compressed, f), 0);
		sw_free();

		sw_init();
		assert_int_not_equal(parse_swd(&offset, f), 0);
		sw_free();
	}
}

static void test_parser_common_fields(void **state)
{
	(void)state;
	static const struct swd t = {
		.cfg = "description = \"test update\"; update-type = \"OS\"; reboot = false;"
		       " bootloader_transaction_marker = false; namespace-for-vars = \"ns\";"
		       " images: ( { filename = \"a\"; device = \"/dev/a\"; } );",
		.json = "\"description\": \"test update\", \"update-type\": \"OS\", \"reboot\": false,"
			" \"bootloader_transaction_marker\": false, \"namespace-for-vars\": \"ns\","
			" \"images\": [ { \"filename\": \"a\", \"device\": \"/dev/a\" } ]",
	};
	static const struct swd reboot_str = {
		.cfg = "reboot = \"enabled\"; images: ( { filename = \"a\"; device = \"/dev/a\"; } );",
		.json = "\"reboot\": \"enabled\", \"images\": [ { \"filename\": \"a\", \"device\": \"/dev/a\" } ]",
	};

	FOR_EACH_FORMAT(f) {
		sw_init();
		assert_int_equal(parse_swd(&t, f), 0);
		assert_string_equal(sw.description, "test update");
		assert_string_equal(sw.update_type_name, "OS");
		assert_int_equal(sw.reboot_enabled, REBOOT_DISABLED);
		assert_false(sw.bootloader_transaction_marker);
		assert_true(sw.bootloader_state_marker);
		assert_string_equal(sw.namespace_for_vars, "ns");
		sw_free();

		sw_init();
		assert_int_equal(parse_swd(&reboot_str, f), 0);
		assert_int_equal(sw.reboot_enabled, REBOOT_ENABLED);
		sw_free();

		/* markers can be disabled from the configuration */
		sw_init();
		sw.no_transaction_marker = true;
		assert_int_equal(parse_swd(&reboot_str, f), 0);
		assert_false(sw.bootloader_transaction_marker);
		sw_free();
	}
}

static void test_parser_default_types(void **state)
{
	(void)state;
	static const struct swd t = {
		.cfg = "images: ( { filename = \"raw.img\"; device = \"/dev/a\"; },"
		       "          { filename = \"ubi.img\"; volume = \"rootfs\"; },"
		       "          { filename = \"arch.tar\"; type = \"archive\"; path = \"/opt\"; } );"
		       " files: ( { filename = \"file.txt\"; path = \"/etc/file.txt\"; } );"
		       " scripts: ( { filename = \"script.lua\"; },"
		       "            { filename = \"script.sh\"; type = \"shellscript\"; } );",
		.json = "\"images\": [ { \"filename\": \"raw.img\", \"device\": \"/dev/a\" },"
			"            { \"filename\": \"ubi.img\", \"volume\": \"rootfs\" },"
			"            { \"filename\": \"arch.tar\", \"type\": \"archive\", \"path\": \"/opt\" } ],"
			" \"files\": [ { \"filename\": \"file.txt\", \"path\": \"/etc/file.txt\" } ],"
			" \"scripts\": [ { \"filename\": \"script.lua\" },"
			"             { \"filename\": \"script.sh\", \"type\": \"shellscript\" } ]",
	};

	FOR_EACH_FORMAT(f) {
		sw_init();
		assert_int_equal(parse_swd(&t, f), 0);
		/* files are in the same list as images */
		assert_int_equal(count_images(&sw.images), 4);
		assert_string_equal(find_image(&sw.images, "raw.img")->type, "raw");
		assert_string_equal(find_image(&sw.images, "ubi.img")->type, "ubivol");
		assert_string_equal(find_image(&sw.images, "ubi.img")->volname, "rootfs");
		assert_string_equal(find_image(&sw.images, "arch.tar")->type, "archive");
		assert_string_equal(find_image(&sw.images, "arch.tar")->path, "/opt");
		assert_string_equal(find_image(&sw.images, "file.txt")->type, "rawfile");
		assert_int_equal(count_images(&sw.scripts), 2);
		assert_string_equal(find_image(&sw.scripts, "script.lua")->type, "lua");
		assert_true(find_image(&sw.scripts, "script.lua")->is_script);
		assert_string_equal(find_image(&sw.scripts, "script.sh")->type, "shellscript");
		/* the order in sw-description is kept */
		assert_string_equal(LIST_FIRST(&sw.scripts)->fname, "script.lua");
		sw_free();
	}
}

static void test_parser_attributes(void **state)
{
	(void)state;
	static const struct swd t = {
		.cfg = "images: ("
		       " { filename = \"z\"; device = \"/dev/a\"; compressed = \"zlib\"; offset = \"2M\";"
		       "   installed-directly = true; name = \"rootfs\"; version = \"2.1\"; },"
		       " { filename = \"s\"; device = \"/dev/a\"; compressed = \"zstd\"; offset = 4096; },"
		       " { filename = \"b\"; device = \"/dev/a\"; compressed = true; preserve-attributes = true; }"
		       " );",
		.json = "\"images\": ["
			" { \"filename\": \"z\", \"device\": \"/dev/a\", \"compressed\": \"zlib\", \"offset\": \"2M\","
			"   \"installed-directly\": true, \"name\": \"rootfs\", \"version\": \"2.1\" },"
			" { \"filename\": \"s\", \"device\": \"/dev/a\", \"compressed\": \"zstd\", \"offset\": 4096 },"
			" { \"filename\": \"b\", \"device\": \"/dev/a\", \"compressed\": true, \"preserve-attributes\": true }"
			" ]",
	};

	FOR_EACH_FORMAT(f) {
		sw_init();
		assert_int_equal(parse_swd(&t, f), 0);
		struct img_type *z = find_image(&sw.images, "z");
		struct img_type *s = find_image(&sw.images, "s");
		struct img_type *b = find_image(&sw.images, "b");
		assert_non_null(z);
		assert_non_null(s);
		assert_non_null(b);
		assert_int_equal(z->compressed, COMPRESSED_ZLIB);
		assert_int_equal(z->seek, 2 * 1024 * 1024);
		assert_true(z->install_directly);
		assert_string_equal(z->id.name, "rootfs");
		assert_string_equal(z->id.version, "2.1");
		assert_int_equal(s->compressed, COMPRESSED_ZSTD);
		assert_int_equal(s->seek, 4096);
		assert_int_equal(b->compressed, COMPRESSED_TRUE);
		assert_true(b->preserve_attributes);
		assert_false(z->preserve_attributes);
		sw_free();
	}
}

static void test_parser_encryption(void **state)
{
	(void)state;
#define IVT "0102030405060708090a0b0c0d0e0f10"
#define KEY128 "00112233445566778899aabbccddeeff"
#define KEY192 KEY128 "0011223344556677"
#define KEY256 KEY128 KEY128
	static const struct swd t = {
		.cfg = "images: ("
		       " { filename = \"k128\"; device = \"/dev/a\"; encrypted = true; ivt = \"" IVT "\"; aes-key = \"" KEY128 "\"; },"
		       " { filename = \"k192\"; device = \"/dev/a\"; encrypted = true; aes-key = \"" KEY192 "\"; },"
		       " { filename = \"k256\"; device = \"/dev/a\"; encrypted = true; aes-key = \"" KEY256 "\"; },"
		       " { filename = \"bad\"; device = \"/dev/a\"; encrypted = true; ivt = \"0102\"; aes-key = \"0011\"; },"
		       " { filename = \"plain\"; device = \"/dev/a\"; }"
		       " );",
		.json = "\"images\": ["
			" { \"filename\": \"k128\", \"device\": \"/dev/a\", \"encrypted\": true, \"ivt\": \"" IVT "\", \"aes-key\": \"" KEY128 "\" },"
			" { \"filename\": \"k192\", \"device\": \"/dev/a\", \"encrypted\": true, \"aes-key\": \"" KEY192 "\" },"
			" { \"filename\": \"k256\", \"device\": \"/dev/a\", \"encrypted\": true, \"aes-key\": \"" KEY256 "\" },"
			" { \"filename\": \"bad\", \"device\": \"/dev/a\", \"encrypted\": true, \"ivt\": \"0102\", \"aes-key\": \"0011\" },"
			" { \"filename\": \"plain\", \"device\": \"/dev/a\" }"
			" ]",
	};

	FOR_EACH_FORMAT(f) {
		sw_init();
		assert_int_equal(parse_swd(&t, f), 0);
		struct img_type *img = find_image(&sw.images, "k128");
		assert_true(img->is_encrypted);
		assert_string_equal(img->ivt_ascii, IVT);
		assert_string_equal(img->aes_ascii, KEY128);
		assert_string_equal(find_image(&sw.images, "k192")->aes_ascii, KEY192);
		assert_string_equal(find_image(&sw.images, "k256")->aes_ascii, KEY256);
		assert_string_equal(find_image(&sw.images, "k256")->ivt_ascii, "");
		/* values with a wrong length are dropped */
		img = find_image(&sw.images, "bad");
		assert_true(img->is_encrypted);
		assert_string_equal(img->ivt_ascii, "");
		assert_string_equal(img->aes_ascii, "");
		assert_false(find_image(&sw.images, "plain")->is_encrypted);
		sw_free();
	}
}

static void test_parser_properties(void **state)
{
	(void)state;
	static const struct swd t = {
		.cfg = "images: ( { filename = \"a\"; device = \"/dev/a\";"
		       " properties: { single = \"one\"; list = [ \"x\", \"y\", \"z\" ]; }; } );",
		.json = "\"images\": [ { \"filename\": \"a\", \"device\": \"/dev/a\","
			" \"properties\": { \"single\": \"one\", \"list\": [ \"x\", \"y\", \"z\" ] } } ]",
	};

	FOR_EACH_FORMAT(f) {
		sw_init();
		assert_int_equal(parse_swd(&t, f), 0);
		struct img_type *img = LIST_FIRST(&sw.images);
		assert_dict_value(&img->properties, "single", "one");
		/* all values of a list are stored (the order is not checked) */
		assert_int_equal(count_list(dict_get_list(&img->properties, "list")), 3);
		assert_true(list_has(dict_get_list(&img->properties, "list"), "x"));
		assert_true(list_has(dict_get_list(&img->properties, "list"), "y"));
		assert_true(list_has(dict_get_list(&img->properties, "list"), "z"));
		assert_null(dict_get_value(&img->properties, "missing"));
		sw_free();
	}
}

static void test_parser_versions(void **state)
{
	(void)state;
	static const struct swd t = {
		.cfg = "images: ("
		       " { filename = \"same\"; device = \"/dev/a\"; name = \"rootfs\"; version = \"1.0\"; install-if-different = true; },"
		       " { filename = \"diff\"; device = \"/dev/a\"; name = \"rootfs\"; version = \"2.0\"; install-if-different = true; },"
		       " { filename = \"lower\"; device = \"/dev/a\"; name = \"rootfs\"; version = \"0.9\"; install-if-higher = true; },"
		       " { filename = \"higher\"; device = \"/dev/a\"; name = \"rootfs\"; version = \"1.1\"; install-if-higher = true; },"
		       " { filename = \"always\"; device = \"/dev/a\"; name = \"rootfs\"; version = \"1.0\"; }"
		       " );",
		.json = "\"images\": ["
			" { \"filename\": \"same\", \"device\": \"/dev/a\", \"name\": \"rootfs\", \"version\": \"1.0\", \"install-if-different\": true },"
			" { \"filename\": \"diff\", \"device\": \"/dev/a\", \"name\": \"rootfs\", \"version\": \"2.0\", \"install-if-different\": true },"
			" { \"filename\": \"lower\", \"device\": \"/dev/a\", \"name\": \"rootfs\", \"version\": \"0.9\", \"install-if-higher\": true },"
			" { \"filename\": \"higher\", \"device\": \"/dev/a\", \"name\": \"rootfs\", \"version\": \"1.1\", \"install-if-higher\": true },"
			" { \"filename\": \"always\", \"device\": \"/dev/a\", \"name\": \"rootfs\", \"version\": \"1.0\" }"
			" ]",
	};

	FOR_EACH_FORMAT(f) {
		sw_init();
		add_installed("rootfs", "1.0");
		assert_int_equal(parse_swd(&t, f), 0);
		assert_int_equal(count_images(&sw.images), 3);
		assert_null(find_image(&sw.images, "same"));
		assert_non_null(find_image(&sw.images, "diff"));
		assert_null(find_image(&sw.images, "lower"));
		assert_non_null(find_image(&sw.images, "higher"));
		assert_non_null(find_image(&sw.images, "always"));
		sw_free();
	}
}

static void test_parser_selection(void **state)
{
	(void)state;
	static const struct swd t = {
		.cfg = "stable = {"
		       "  main = { images: ( { filename = \"main.img\"; device = \"/dev/a\"; } ); };"
		       "  alt = { images: ( { filename = \"alt.img\"; device = \"/dev/b\"; } ); };"
		       "};"
		       " myboard = {"
		       "  images: ( { filename = \"board.img\"; device = \"/dev/c\"; } );"
		       "  stable = { main = { images: ( { filename = \"board-main.img\"; device = \"/dev/d\"; } ); }; };"
		       "};"
		       " images: ( { filename = \"default.img\"; device = \"/dev/e\"; } );",
		.json = "\"stable\": {"
			"  \"main\": { \"images\": [ { \"filename\": \"main.img\", \"device\": \"/dev/a\" } ] },"
			"  \"alt\": { \"images\": [ { \"filename\": \"alt.img\", \"device\": \"/dev/b\" } ] }"
			"},"
			" \"myboard\": {"
			"  \"images\": [ { \"filename\": \"board.img\", \"device\": \"/dev/c\" } ],"
			"  \"stable\": { \"main\": { \"images\": [ { \"filename\": \"board-main.img\", \"device\": \"/dev/d\" } ] } }"
			"},"
			" \"images\": [ { \"filename\": \"default.img\", \"device\": \"/dev/e\" } ]",
	};
	static const struct {
		const char *board;
		const char *set;
		const char *mode;
		const char *expected;
	} cases[] = {
		{ "board",   "",       "",     "default.img" },
		{ "board",   "stable", "main", "main.img" },
		{ "board",   "stable", "alt",  "alt.img" },
		{ "myboard", "",       "",     "board.img" },
		{ "myboard", "stable", "main", "board-main.img" },
		/* not found for the board: the generic selection is used */
		{ "myboard", "stable", "alt",  "alt.img" },
	};

	FOR_EACH_FORMAT(f) {
		for (unsigned int i = 0; i < ARRAY_SIZE(cases); i++) {
			sw_init();
			strlcpy(sw.hw.boardname, cases[i].board, sizeof(sw.hw.boardname));
			strlcpy(sw.parms.software_set, cases[i].set, sizeof(sw.parms.software_set));
			strlcpy(sw.parms.running_mode, cases[i].mode, sizeof(sw.parms.running_mode));
			assert_int_equal(parse_swd(&t, f), 0);
			assert_int_equal(count_images(&sw.images), 1);
			if (strcmp(LIST_FIRST(&sw.images)->fname, cases[i].expected))
				fail_msg("%s: %s/%s/%s selected %s instead of %s", fmt_name[f],
					 cases[i].board, cases[i].set, cases[i].mode,
					 LIST_FIRST(&sw.images)->fname, cases[i].expected);
			sw_free();
		}
	}
}

static void test_parser_links(void **state)
{
	(void)state;
	static const struct swd t = {
		.cfg = "stable = { main = { ref = \"#./../shared\"; }; };"
		       " shared = { images: ( { ref = \"#./../common\"; },"
		       "                      { filename = \"own.img\"; device = \"/dev/a\"; } ); };"
		       " common: ( { filename = \"common.img\"; device = \"/dev/b\"; } );",
		.json = "\"stable\": { \"main\": { \"ref\": \"#./../shared\" } },"
			" \"shared\": { \"images\": [ { \"ref\": \"#./../common\" },"
			"                        { \"filename\": \"own.img\", \"device\": \"/dev/a\" } ] },"
			" \"common\": [ { \"filename\": \"common.img\", \"device\": \"/dev/b\" } ]",
	};
	static const struct swd loop = {
		.cfg = "images: ( { ref = \"#./images\"; } );",
		.json = "\"images\": [ { \"ref\": \"#./images\" } ]",
	};
	static const struct swd deep = {
		.cfg = "images: ( { ref = \"#./a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p/q/r/s/t/u/v/w/x/y/z\"; } );",
		.json = "\"images\": [ { \"ref\": \"#./a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p/q/r/s/t/u/v/w/x/y/z\" } ]",
	};

	FOR_EACH_FORMAT(f) {
		sw_init();
		strlcpy(sw.parms.software_set, "stable", sizeof(sw.parms.software_set));
		strlcpy(sw.parms.running_mode, "main", sizeof(sw.parms.running_mode));
		assert_int_equal(parse_swd(&t, f), 0);
		assert_int_equal(count_images(&sw.images), 2);
		assert_non_null(find_image(&sw.images, "own.img"));
		assert_non_null(find_image(&sw.images, "common.img"));
		sw_free();

		/* a link to itself must fail, not recurse forever */
		sw_init();
		assert_int_not_equal(parse_swd(&loop, f), 0);
		sw_free();

		/* too many path elements */
		sw_init();
		assert_int_not_equal(parse_swd(&deep, f), 0);
		sw_free();
	}
}

static void test_parser_bootenv_vars(void **state)
{
	(void)state;
	static const struct swd t = {
		.cfg = "bootenv: ( { name = \"a\"; value = \"1\"; }, { name = \"b\"; } );"
		       " vars: ( { name = \"v\"; value = \"x\"; }, { ref = \"#./morevars\"; } );"
		       " morevars: ( { name = \"w\"; value = \"y\"; } );",
		.json = "\"bootenv\": [ { \"name\": \"a\", \"value\": \"1\" }, { \"name\": \"b\" } ],"
			" \"vars\": [ { \"name\": \"v\", \"value\": \"x\" }, { \"ref\": \"#./morevars\" } ],"
			" \"morevars\": [ { \"name\": \"w\", \"value\": \"y\" } ]",
	};
	static const struct swd uboot = {
		.cfg = "uboot: ( { name = \"u\"; value = \"2\"; } );",
		.json = "\"uboot\": [ { \"name\": \"u\", \"value\": \"2\" } ]",
	};
	static const struct swd noname = {
		.cfg = "bootenv: ( { value = \"1\"; } );",
		.json = "\"bootenv\": [ { \"value\": \"1\" } ]",
	};

	FOR_EACH_FORMAT(f) {
		sw_init();
		/* bootenv alone is something to install */
		assert_int_equal(parse_swd(&t, f), 0);
		assert_dict_value(&sw.bootloader, "a", "1");
		/* without value the variable is removed */
		assert_dict_value(&sw.bootloader, "b", "");
		assert_dict_value(&sw.vars, "v", "x");
		/* a link in vars adds vars, not bootloader variables */
		assert_dict_value(&sw.vars, "w", "y");
		assert_null(dict_get_value(&sw.bootloader, "w"));
		sw_free();

		sw_init();
		assert_int_equal(parse_swd(&uboot, f), 0);
		assert_dict_value(&sw.bootloader, "u", "2");
		sw_free();

		sw_init();
		assert_int_not_equal(parse_swd(&noname, f), 0);
		sw_free();
	}
}

static void test_parser_partitions(void **state)
{
	(void)state;
	static const struct swd t = {
		.cfg = "images: ( { filename = \"rootfs.ubifs\"; volume = \"rootfs\"; } );"
		       " partitions: ( { name = \"rootfs\"; device = \"mtd4\"; size = 1048576; } );",
		.json = "\"images\": [ { \"filename\": \"rootfs.ubifs\", \"volume\": \"rootfs\" } ],"
			" \"partitions\": [ { \"name\": \"rootfs\", \"device\": \"mtd4\", \"size\": 1048576 } ]",
	};
	static const struct swd incomplete = {
		.cfg = "partitions: ( { name = \"rootfs\"; size = 1048576; } );",
		.json = "\"partitions\": [ { \"name\": \"rootfs\", \"size\": 1048576 } ]",
	};

	FOR_EACH_FORMAT(f) {
		sw_init();
		assert_int_equal(parse_swd(&t, f), 0);
		assert_int_equal(count_images(&sw.images), 2);
		/* partitions are processed before the images */
		struct img_type *part = LIST_FIRST(&sw.images);
		assert_true(part->is_partitioner);
		assert_string_equal(part->type, "ubipartition");
		assert_string_equal(part->volname, "rootfs");
		assert_string_equal(part->device, "mtd4");
		assert_int_equal(part->partsize, 1048576);
		sw_free();

		sw_init();
		assert_int_not_equal(parse_swd(&incomplete, f), 0);
		sw_free();
	}
}

static void test_parser_large_numbers(void **state)
{
	(void)state;
	/* values above 4 GiB need 64 bit, also in JSON */
	static const struct swd t = {
		.cfg = "images: ( { filename = \"big.img\"; device = \"/dev/a\";"
		       "            offset = 6442450944L; size = 5000000000L; } );"
		       " partitions: ( { name = \"data\"; device = \"mtd4\"; size = 3221225472L; } );",
		.json = "\"images\": [ { \"filename\": \"big.img\", \"device\": \"/dev/a\","
			"              \"offset\": 6442450944, \"size\": 5000000000 } ],"
			" \"partitions\": [ { \"name\": \"data\", \"device\": \"mtd4\", \"size\": 3221225472 } ]",
	};

	FOR_EACH_FORMAT(f) {
		sw_init();
		assert_int_equal(parse_swd(&t, f), 0);
		struct img_type *img = find_image(&sw.images, "big.img");
		assert_non_null(img);
		assert_true(img->seek == 6442450944LL);
		assert_true(img->size == 5000000000LL);
		struct img_type *part = LIST_FIRST(&sw.images);
		assert_true(part->is_partitioner);
		assert_true(part->partsize == 3221225472LL);
		sw_free();
	}
}

static void test_parser_string_size(void **state)
{
	(void)state;
	char name256[257], name255[256];
	char cfg[1024], json[1024];

	memset(name256, 'a', 256);
	name256[256] = '\0';
	memset(name255, 'b', 255);
	name255[255] = '\0';
	snprintf(cfg, sizeof(cfg),
		 "images: ( { filename = \"%s\"; device = \"/dev/a\"; },"
		 "          { filename = \"%s\"; device = \"/dev/b\"; } );", name256, name255);
	snprintf(json, sizeof(json),
		 "\"images\": [ { \"filename\": \"%s\", \"device\": \"/dev/a\" },"
		 "            { \"filename\": \"%s\", \"device\": \"/dev/b\" } ]", name256, name255);
	const struct swd t = { .cfg = cfg, .json = json };

	FOR_EACH_FORMAT(f) {
		sw_init();
		assert_int_equal(parse_swd(&t, f), 0);
		assert_int_equal(count_images(&sw.images), 2);
		/* a string that does not fit is cropped and terminated */
		name256[255] = '\0';
		assert_non_null(find_image(&sw.images, name256));
		name256[255] = 'a';
		assert_string_equal(find_image(&sw.images, name256 + 1)->device, "/dev/a");
		assert_non_null(find_image(&sw.images, name255));
		sw_free();
	}
}

#ifdef CONFIG_HW_COMPATIBILITY
static void test_parser_hw_compatibility(void **state)
{
	(void)state;
	static const struct swd t = {
		.raw = true,
		.cfg = "software = { version = \"1.0\";"
		       " hardware-compatibility = [ \"1.0\", \"#RE:^2\\\\.[0-9]$\" ];"
		       " images: ( { filename = \"a\"; device = \"/dev/a\"; } ); };",
		.json = "{ \"software\": { \"version\": \"1.0\","
			" \"hardware-compatibility\": [ \"1.0\", \"#RE:^2\\\\.[0-9]$\" ],"
			" \"images\": [ { \"filename\": \"a\", \"device\": \"/dev/a\" } ] } }",
	};
	static const struct swd missing = {
		.raw = true,
		.cfg = "software = { version = \"1.0\"; images: ( { filename = \"a\"; device = \"/dev/a\"; } ); };",
		.json = "{ \"software\": { \"version\": \"1.0\","
			" \"images\": [ { \"filename\": \"a\", \"device\": \"/dev/a\" } ] } }",
	};
	static const struct {
		const char *revision;
		int expected;
	} cases[] = {
		{ "1.0", 0 }, { "2.5", 0 }, { "1.1", -1 }, { "2.10", -1 },
	};

	FOR_EACH_FORMAT(f) {
		for (unsigned int i = 0; i < ARRAY_SIZE(cases); i++) {
			sw_init();
			assert_int_equal(parse_swd(&t, f), 0);
			strlcpy(sw.hw.revision, cases[i].revision, sizeof(sw.hw.revision));
			assert_int_equal(check_hw_compatibility(&sw.hw, &sw.hardware),
					 cases[i].expected);
			sw_free();
		}

		/* hardware-compatibility is mandatory */
		sw_init();
		assert_int_not_equal(parse_swd(&missing, f), 0);
		sw_free();
	}
}
#endif

#ifdef CONFIG_LUA
static void test_parser_hooks(void **state)
{
	(void)state;
#define HOOKS "function skip(image) return true, nil end" \
	      " function setdev(image) image.device = '/dev/changed' return true, image end" \
	      " function fail(image) return false, image end"
	static const struct swd t = {
		.cfg = "embedded-script = \"" HOOKS "\";"
		       " images: ( { filename = \"skipped\"; device = \"/dev/a\"; hook = \"skip\"; },"
		       "           { filename = \"changed\"; device = \"/dev/b\"; hook = \"setdev\"; } );",
		.json = "\"embedded-script\": \"" HOOKS "\","
			" \"images\": [ { \"filename\": \"skipped\", \"device\": \"/dev/a\", \"hook\": \"skip\" },"
			"             { \"filename\": \"changed\", \"device\": \"/dev/b\", \"hook\": \"setdev\" } ]",
	};
	static const struct swd failing = {
		.cfg = "embedded-script = \"" HOOKS "\";"
		       " images: ( { filename = \"a\"; device = \"/dev/a\"; hook = \"fail\"; } );",
		.json = "\"embedded-script\": \"" HOOKS "\","
			" \"images\": [ { \"filename\": \"a\", \"device\": \"/dev/a\", \"hook\": \"fail\" } ]",
	};

	FOR_EACH_FORMAT(f) {
		sw_init();
		assert_int_equal(parse_swd(&t, f), 0);
		assert_int_equal(count_images(&sw.images), 1);
		assert_null(find_image(&sw.images, "skipped"));
		assert_string_equal(find_image(&sw.images, "changed")->device, "/dev/changed");
		sw_free();

		sw_init();
		assert_int_not_equal(parse_swd(&failing, f), 0);
		sw_free();
	}
}
#endif

#ifdef CONFIG_HASH_VERIFY
static void test_parser_install_if_hash_different(void **state)
{
	(void)state;
	static const unsigned char content[] = "content already on the device";
	char hash[2 * SHA256_HASH_LENGTH + 1];
	unsigned char md[64];
	unsigned int mdlen;
	char cfg[1024], json[1024], path[128];
	void *dgst;
	int fd;

	/* the device has the image */
	snprintf(path, sizeof(path), "%s/device.img", tmpdir);
	fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
	assert_true(fd >= 0);
	assert_int_equal(write(fd, content, sizeof(content)), sizeof(content));
	close(fd);
	dgst = swupdate_HASH_init(SHA_DEFAULT);
	assert_non_null(dgst);
	assert_int_equal(swupdate_HASH_update(dgst, content, sizeof(content)), 0);
	assert_int_equal(swupdate_HASH_final(dgst, md, &mdlen), 0);
	swupdate_HASH_cleanup(dgst);
	hash_to_ascii(md, hash);

	/* same: skipped; larger than the device: installed, without hanging */
	snprintf(cfg, sizeof(cfg),
		 "images: ( { filename = \"same\"; device = \"%s\"; type = \"raw\"; size = %zu;"
		 " sha256 = \"%s\"; install-if-hash-different = true; },"
		 " { filename = \"larger\"; device = \"%s\"; type = \"raw\"; size = 100000;"
		 " sha256 = \"%s\"; install-if-hash-different = true; } );",
		 path, sizeof(content), hash, path, hash);
	snprintf(json, sizeof(json),
		 "\"images\": [ { \"filename\": \"same\", \"device\": \"%s\", \"type\": \"raw\", \"size\": %zu,"
		 " \"sha256\": \"%s\", \"install-if-hash-different\": true },"
		 " { \"filename\": \"larger\", \"device\": \"%s\", \"type\": \"raw\", \"size\": 100000,"
		 " \"sha256\": \"%s\", \"install-if-hash-different\": true } ]",
		 path, sizeof(content), hash, path, hash);
	const struct swd t = { .cfg = cfg, .json = json };

	/* reading a device smaller than size used to loop forever */
	alarm(30);
	FOR_EACH_FORMAT(f) {
		sw_init();
		assert_int_equal(parse_swd(&t, f), 0);
		assert_null(find_image(&sw.images, "same"));
		assert_non_null(find_image(&sw.images, "larger"));
		sw_free();
	}
	alarm(0);
	unlink(path);
}
#endif

static int parser_group_setup(void **state)
{
	(void)state;
	if (!mkdtemp(tmpdir))
		return -1;
	/* cleanup_files() removes files in TMPDIR */
	setenv("TMPDIR", tmpdir, 1);
	return 0;
}

static int parser_group_teardown(void **state)
{
	char path[128];

	(void)state;
	snprintf(path, sizeof(path), "%s/test-sw-description", tmpdir);
	unlink(path);
	return rmdir(tmpdir);
}

int main(void)
{
	static const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_parser_minimal),
		cmocka_unit_test(test_parser_errors),
		cmocka_unit_test(test_parser_common_fields),
		cmocka_unit_test(test_parser_default_types),
		cmocka_unit_test(test_parser_attributes),
		cmocka_unit_test(test_parser_encryption),
		cmocka_unit_test(test_parser_properties),
		cmocka_unit_test(test_parser_versions),
		cmocka_unit_test(test_parser_selection),
		cmocka_unit_test(test_parser_links),
		cmocka_unit_test(test_parser_bootenv_vars),
		cmocka_unit_test(test_parser_partitions),
		cmocka_unit_test(test_parser_large_numbers),
		cmocka_unit_test(test_parser_string_size),
#ifdef CONFIG_HW_COMPATIBILITY
		cmocka_unit_test(test_parser_hw_compatibility),
#endif
#ifdef CONFIG_LUA
		cmocka_unit_test(test_parser_hooks),
#endif
#ifdef CONFIG_HASH_VERIFY
		cmocka_unit_test(test_parser_install_if_hash_different),
#endif
	};
	return cmocka_run_group_tests_name("parser", tests, parser_group_setup,
					   parser_group_teardown);
}
