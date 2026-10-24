// SPDX-FileCopyrightText: 2026 Stefano Babic <stefano.babic@swupdate.org>
//
// SPDX-License-Identifier: GPL-2.0-only

/*
 * Tests for corelib/mender_artifact.c. The artifacts are built here,
 * following the Mender Artifact v3 format, with every combination of
 * compression that is built in.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "generated/autoconf.h"
#include "swupdate_crypto.h"
#include "util.h"
#include "mender_artifact.h"

#ifdef CONFIG_GUNZIP
#include <zlib.h>
#endif
#ifdef CONFIG_XZ
#include <lzma.h>
#endif
#ifdef CONFIG_ZSTD
#include <zstd.h>
#endif

#define PAYLOAD_SIZE	(200 * 1024 + 11)	/* more than one decompression buffer */

enum comp { C_NONE, C_GZ, C_XZ, C_ZSTD };

static const struct {
	enum comp c;
	const char *suffix;
} comps[] = {
	{ C_NONE, "" },
#ifdef CONFIG_GUNZIP
	{ C_GZ, ".gz" },
#endif
#ifdef CONFIG_XZ
	{ C_XZ, ".xz" },
#endif
#ifdef CONFIG_ZSTD
	{ C_ZSTD, ".zst" },
#endif
};

static uint8_t *payload;

struct mbuf {
	uint8_t *p;
	size_t len;
	size_t cap;
};

static void mb_add(struct mbuf *b, const void *d, size_t n)
{
	if (b->len + n > b->cap) {
		b->cap = (b->len + n) * 2 + 1024;
		b->p = realloc(b->p, b->cap);
		assert_non_null(b->p);
	}
	if (n)
		memcpy(b->p + b->len, d, n);
	b->len += n;
}

static void mb_free(struct mbuf *b)
{
	free(b->p);
	memset(b, 0, sizeof(*b));
}

/* tar writer */
enum size_fmt { SIZE_OCTAL, SIZE_PAX, SIZE_BASE256 };

static void tar_hdr_fmt(struct mbuf *b, const char *name, char type, size_t size,
			enum size_fmt fmt)
{
	uint8_t h[512] = { 0 };
	unsigned int sum = 0;
	int i;

	strncpy((char *)h, name, 100);
	memcpy(h + 100, "0000644", 8);
	memcpy(h + 108, "0000000", 8);
	memcpy(h + 116, "0000000", 8);
	if (fmt == SIZE_OCTAL) {
		snprintf((char *)h + 124, 12, "%011zo", size);
	} else if (fmt == SIZE_PAX) {
		memcpy(h + 124, "00000000000", 12);	/* as written when it does not fit */
	} else {
		uint64_t v = size;

		h[124] = 0x80;
		for (i = 135; i > 124; i--, v >>= 8)
			h[i] = (uint8_t)v;
	}
	memcpy(h + 136, "00000000000", 12);
	h[156] = (uint8_t)type;
	memcpy(h + 257, "ustar", 6);
	memcpy(h + 263, "00", 2);
	memset(h + 148, ' ', 8);
	for (i = 0; i < 512; i++)
		sum += h[i];
	snprintf((char *)h + 148, 8, "%06o", sum);
	mb_add(b, h, sizeof(h));
}

static void tar_hdr(struct mbuf *b, const char *name, char type, size_t size)
{
	tar_hdr_fmt(b, name, type, size, SIZE_OCTAL);
}

static void tar_pad(struct mbuf *b, size_t size)
{
	static const uint8_t zero[512];

	mb_add(b, zero, (512 - size % 512) % 512);
}

static void pax_add(struct mbuf *b, const char *key, const char *value)
{
	char rec[4200];
	size_t body = strlen(key) + strlen(value) + 3;	/* ' ', '=', '\n' */
	size_t n = body + 1;

	while (snprintf(NULL, 0, "%zu", n) + body != n)
		n++;
	snprintf(rec, sizeof(rec), "%zu %s=%s\n", n, key, value);
	tar_hdr(b, "PaxHeaders/entry", 'x', n);
	mb_add(b, rec, n);
	tar_pad(b, n);
}

static void tar_add_fmt(struct mbuf *b, const char *name, char type,
			const void *data, size_t len, enum size_fmt fmt)
{
	if (fmt == SIZE_PAX) {
		char size[32];

		snprintf(size, sizeof(size), "%zu", len);
		pax_add(b, "size", size);
	}
	tar_hdr_fmt(b, name, type, len, fmt);
	mb_add(b, data, len);
	tar_pad(b, len);
}

static void tar_add(struct mbuf *b, const char *name, char type,
		    const void *data, size_t len)
{
	if (strlen(name) >= 100)
		pax_add(b, "path", name);
	tar_hdr(b, name, type, len);
	mb_add(b, data, len);
	tar_pad(b, len);
}

static void tar_end(struct mbuf *b)
{
	static const uint8_t zero[1024];

	mb_add(b, zero, sizeof(zero));
}

static void compress_buf(enum comp c, const struct mbuf *in, struct mbuf *out)
{
	size_t cap = in->len + in->len / 2 + 1024;

	memset(out, 0, sizeof(*out));
	out->p = malloc(cap);
	assert_non_null(out->p);
	out->cap = cap;

	switch (c) {
	case C_NONE:
		memcpy(out->p, in->p, in->len);
		out->len = in->len;
		break;
#ifdef CONFIG_GUNZIP
	case C_GZ: {
		z_stream z = { 0 };

		assert_int_equal(deflateInit2(&z, 6, Z_DEFLATED, 16 + MAX_WBITS, 8,
					      Z_DEFAULT_STRATEGY), Z_OK);
		z.next_in = in->p;
		z.avail_in = (uInt)in->len;
		z.next_out = out->p;
		z.avail_out = (uInt)cap;
		assert_int_equal(deflate(&z, Z_FINISH), Z_STREAM_END);
		out->len = z.total_out;
		deflateEnd(&z);
		break;
	}
#endif
#ifdef CONFIG_XZ
	case C_XZ:
		assert_int_equal(lzma_easy_buffer_encode(1, LZMA_CHECK_CRC64, NULL,
							 in->p, in->len, out->p,
							 &out->len, cap), LZMA_OK);
		break;
#endif
#ifdef CONFIG_ZSTD
	case C_ZSTD:
		out->len = ZSTD_compress(out->p, cap, in->p, in->len, 3);
		assert_false(ZSTD_isError(out->len));
		break;
#endif
	default:
		fail();
	}
}

static void sha256_hex(const void *data, size_t len, char hex[65])
{
	unsigned char md[32];
	unsigned int mdlen = 0;
	void *dgst = swupdate_HASH_init(SHA_DEFAULT);
	int i;

	assert_non_null(dgst);
	assert_int_equal(swupdate_HASH_update(dgst, data, len), 0);
	assert_int_equal(swupdate_HASH_final(dgst, md, &mdlen), 0);
	assert_int_equal(mdlen, 32);
	swupdate_HASH_cleanup(dgst);
	for (i = 0; i < 32; i++)
		sprintf(hex + 2 * i, "%02x", md[i]);
}

/* Variations of the artifact */
struct opts {
	enum comp hcomp, dcomp;
	const char *suffix_h, *suffix_d;
	const char *type;		/* header-info payload type, "null" for null */
	const char *type_info;		/* type-info type, default: type */
	const char *version;		/* version file */
	const char *payload_name;
	size_t payload_len;
	bool bad_version_sum;
	bool bad_header_sum;
	bool bad_payload_sum;
	bool payload_not_listed;
	bool extra_listed;
	bool two_files;
	bool signature;
	bool scripts;
	bool augment;
	bool data_first;
	bool no_manifest;
	bool type_info_first;
	bool second_bucket;
	bool data_dir;
	bool no_data;
	enum size_fmt size_fmt;		/* of the payload file */
	size_t trim_data;		/* bytes cut from the compressed data */
};

static void flip_hex(char *hex)
{
	hex[0] = hex[0] == '0' ? '1' : '0';
}

static void build(const struct opts *o, struct mbuf *art)
{
	const char *type = o->type ? o->type : "swu";
	const char *tinfo = o->type_info ? o->type_info : type;
	const char *pname = o->payload_name ? o->payload_name : "update.swu";
	const char *version = o->version ? o->version :
		"{\n  \"format\": \"mender\",\n  \"version\": 3\n}\n";
	bool null_type = !strcmp(type, "null");
	struct mbuf htar = { 0 }, dtar = { 0 }, hz, dz, manifest = { 0 };
	char hname[64], dname[64], hex[65], line[4300], json[1024];
	size_t plen = o->payload_len ? o->payload_len : PAYLOAD_SIZE;

	memset(art, 0, sizeof(*art));

	/* header */
	snprintf(json, sizeof(json),
		 "{\"payloads\":[{\"type\":%s%s%s}],"
		 "\"artifact_provides\":{\"artifact_name\":\"release-1\"},"
		 "\"artifact_depends\":{\"device_type\":[\"qemu\",\"board\"]}}",
		 null_type ? "" : "\"", type, null_type ? "" : "\"");
	if (!strcmp(tinfo, "null"))
		strcpy(line, "{\"type\":null}");
	else
		snprintf(line, sizeof(line), "{\"type\":\"%s\"}", tinfo);
	if (o->type_info_first)
		tar_add(&htar, "headers/0000/type-info", '0', line, strlen(line));
	tar_add(&htar, "header-info", '0', json, strlen(json));
	if (o->scripts)
		tar_add(&htar, "scripts/ArtifactInstall_Enter_00", '0', "#!/bin/sh\n", 10);
	tar_add(&htar, "headers/0000/", '5', NULL, 0);
	if (!o->type_info_first && !null_type)
		tar_add(&htar, "headers/0000/type-info", '0', line, strlen(line));
	tar_add(&htar, "headers/0000/meta-data", '0', "{}", 2);
	if (o->second_bucket)
		tar_add(&htar, "headers/0001/type-info", '0', line, strlen(line));
	tar_end(&htar);
	compress_buf(o->hcomp, &htar, &hz);
	snprintf(hname, sizeof(hname), "header.tar%s", o->suffix_h ? o->suffix_h : "");

	/* data */
	if (!null_type) {
		if (o->size_fmt != SIZE_OCTAL)
			tar_add_fmt(&dtar, pname, '0', payload, plen, o->size_fmt);
		else
			tar_add(&dtar, pname, '0', payload, plen);
		if (o->two_files)
			tar_add(&dtar, "second.swu", '0', payload, 100);
	}
	tar_end(&dtar);
	compress_buf(o->dcomp, &dtar, &dz);
	assert_true(o->trim_data < dz.len);
	dz.len -= o->trim_data;
	snprintf(dname, sizeof(dname), "data/0000.tar%s", o->suffix_d ? o->suffix_d : "");

	/* manifest */
	sha256_hex(version, strlen(version), hex);
	if (o->bad_version_sum)
		flip_hex(hex);
	snprintf(line, sizeof(line), "%s  version\n", hex);
	mb_add(&manifest, line, strlen(line));
	sha256_hex(hz.p, hz.len, hex);
	if (o->bad_header_sum)
		flip_hex(hex);
	snprintf(line, sizeof(line), "%s  %s\n", hex, hname);
	mb_add(&manifest, line, strlen(line));
	if (!null_type && !o->payload_not_listed) {
		sha256_hex(payload, plen, hex);
		if (o->bad_payload_sum)
			flip_hex(hex);
		snprintf(line, sizeof(line), "%s  data/0000/%s\n", hex, pname);
		mb_add(&manifest, line, strlen(line));
	}
	if (o->two_files) {
		sha256_hex(payload, 100, hex);
		snprintf(line, sizeof(line), "%s  data/0000/second.swu\n", hex);
		mb_add(&manifest, line, strlen(line));
	}
	if (o->extra_listed) {
		sha256_hex("x", 1, hex);
		snprintf(line, sizeof(line), "%s  data/0000/missing\n", hex);
		mb_add(&manifest, line, strlen(line));
	}

	/* artifact */
	tar_add(art, "version", '0', version, strlen(version));
	if (!o->no_manifest)
		tar_add(art, "manifest", '0', manifest.p, manifest.len);
	if (o->signature)
		tar_add(art, "manifest.sig", '0', "c2lnbmF0dXJl\n", 13);
	if (o->augment)
		tar_add(art, "manifest-augment", '0', "", 0);
	if (o->data_first)
		tar_add(art, dname, '0', dz.p, dz.len);
	tar_add(art, hname, '0', hz.p, hz.len);
	if (o->data_dir)
		tar_add(art, "data/", '5', NULL, 0);
	if (!o->data_first && !o->no_data)
		tar_add(art, dname, '0', dz.p, dz.len);
	tar_end(art);

	mb_free(&htar);
	mb_free(&dtar);
	mb_free(&hz);
	mb_free(&dz);
	mb_free(&manifest);
}

/* Payload sink */
struct sink {
	struct mbuf out;
	int fail_with;
	bool name_known;	/* artifact name was available at the first byte */
	struct mender_artifact *ma;
};

static int sink_cb(void *priv, const void *buf, size_t len)
{
	struct sink *s = priv;

	if (!s->out.len && s->ma)
		s->name_known = mender_artifact_name(s->ma) != NULL;
	if (s->fail_with)
		return s->fail_with;
	mb_add(&s->out, buf, len);
	return 0;
}

static int unwrap(const uint8_t *buf, size_t len, size_t chunk,
		  const char *type, const char *device, struct sink *s,
		  struct mender_artifact **keep)
{
	struct mender_artifact_cfg cfg = {
		.payload_type = type,
		.device_type = device,
		.payload = sink_cb,
		.priv = s,
	};
	struct mender_artifact *ma = mender_artifact_new(&cfg);
	size_t off = 0;
	int ret = 0;

	assert_non_null(ma);
	s->ma = ma;
	while (off < len && !ret) {
		size_t n = min(chunk, len - off);

		ret = mender_artifact_feed(ma, buf + off, n);
		off += n;
	}
	if (!ret)
		ret = mender_artifact_finish(ma);
	/* errors stay */
	if (ret) {
		assert_int_equal(mender_artifact_feed(ma, buf, 1), ret);
		assert_int_equal(mender_artifact_finish(ma), ret);
	}
	if (keep)
		*keep = ma;
	else
		mender_artifact_free(ma);
	s->ma = NULL;

	return ret;
}

static int run(const struct opts *o, const char *type, const char *device,
	       struct sink *s)
{
	struct mbuf art;
	int ret;

	build(o, &art);
	ret = unwrap(art.p, art.len, 4096, type, device, s, NULL);
	mb_free(&art);

	return ret;
}

static void assert_payload(const struct sink *s, size_t len)
{
	assert_int_equal(s->out.len, len);
	assert_memory_equal(s->out.p, payload, len);
}

static void test_plain(void **state)
{
	static const size_t chunks[] = { 1, 7, 511, 512, 513, 4096, 65536, SIZE_MAX };
	struct opts o = { 0 };
	struct mbuf art;
	size_t i;

	(void)state;
	build(&o, &art);
	for (i = 0; i < ARRAY_SIZE(chunks); i++) {
		struct sink s = { 0 };
		struct mender_artifact *ma;

		assert_int_equal(unwrap(art.p, art.len, chunks[i], "swu", "board", &s, &ma), 0);
		assert_payload(&s, PAYLOAD_SIZE);
		assert_true(s.name_known);
		assert_string_equal(mender_artifact_name(ma), "release-1");
		assert_string_equal(mender_artifact_payload_type(ma), "swu");
		assert_string_equal(mender_artifact_payload_name(ma), "update.swu");
		assert_int_equal(mender_artifact_payload_size(ma), PAYLOAD_SIZE);
		assert_true(mender_artifact_device_compatible(ma, "qemu"));
		assert_false(mender_artifact_device_compatible(ma, "other"));
		mender_artifact_free(ma);
		mb_free(&s.out);
	}
	mb_free(&art);
}

static void test_compressed(void **state)
{
	static const size_t chunks[] = { 1, 1000, SIZE_MAX };
	size_t h, d, i;

	(void)state;
	for (h = 0; h < ARRAY_SIZE(comps); h++) {
		for (d = 0; d < ARRAY_SIZE(comps); d++) {
			struct opts o = {
				.hcomp = comps[h].c, .suffix_h = comps[h].suffix,
				.dcomp = comps[d].c, .suffix_d = comps[d].suffix,
			};
			struct mbuf art;

			build(&o, &art);
			for (i = 0; i < ARRAY_SIZE(chunks); i++) {
				struct sink s = { 0 };

				assert_int_equal(unwrap(art.p, art.len, chunks[i], "swu",
							NULL, &s, NULL), 0);
				assert_payload(&s, PAYLOAD_SIZE);
				mb_free(&s.out);
			}
			mb_free(&art);
		}
	}
}

static void test_small_payload(void **state)
{
	static const size_t sizes[] = { 1, 511, 512, 513 };
	size_t i, c;

	(void)state;
	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		for (c = 0; c < ARRAY_SIZE(comps); c++) {
			struct opts o = { .payload_len = sizes[i],
					  .dcomp = comps[c].c, .suffix_d = comps[c].suffix };
			struct sink s = { 0 };

			assert_int_equal(run(&o, "swu", NULL, &s), 0);
			assert_payload(&s, sizes[i]);
			mb_free(&s.out);
		}
	}
}

static void test_checksums(void **state)
{
	struct sink s = { 0 };
	struct opts o = { .bad_payload_sum = true };

	(void)state;
	/* the end of the payload is held back, it never arrives complete */
	assert_int_equal(run(&o, "swu", NULL, &s), -EBADMSG);
	assert_payload(&s, PAYLOAD_SIZE - 64 * 1024);
	mb_free(&s.out);
	o.payload_len = 3000;
	assert_int_equal(run(&o, "swu", NULL, &s), -EBADMSG);
	assert_int_equal(s.out.len, 0);

	/* header and version are verified before any payload byte */
	o = (struct opts){ .bad_header_sum = true };
	assert_int_equal(run(&o, "swu", NULL, &s), -EBADMSG);
	assert_int_equal(s.out.len, 0);

	o = (struct opts){ .bad_version_sum = true };
	assert_int_equal(run(&o, "swu", NULL, &s), -EBADMSG);
	assert_int_equal(s.out.len, 0);

	o = (struct opts){ .payload_not_listed = true };
	assert_int_equal(run(&o, "swu", NULL, &s), -EBADMSG);
	assert_int_equal(s.out.len, 0);

	/* a verified payload is still held back if the artifact is not valid */
	o = (struct opts){ .extra_listed = true };
	assert_int_equal(run(&o, "swu", NULL, &s), -EBADMSG);
	assert_payload(&s, PAYLOAD_SIZE - 64 * 1024);
	mb_free(&s.out);
}

static void test_compatibility(void **state)
{
	struct sink s = { 0 };
	struct opts o = { 0 };

	(void)state;
	assert_int_equal(run(&o, "swu", "qemu", &s), 0);
	mb_free(&s.out);
	assert_int_equal(run(&o, NULL, NULL, &s), 0);
	mb_free(&s.out);

	assert_int_equal(run(&o, "swu", "other", &s), -ENOEXEC);
	assert_int_equal(s.out.len, 0);
	assert_int_equal(run(&o, "rootfs-image", NULL, &s), -ENOEXEC);
	assert_int_equal(s.out.len, 0);

	o.type = "rootfs-image";
	assert_int_equal(run(&o, "swu", NULL, &s), -ENOEXEC);
	assert_int_equal(s.out.len, 0);

	o = (struct opts){ .type_info = "other" };
	assert_int_equal(run(&o, NULL, NULL, &s), -EINVAL);
	assert_int_equal(s.out.len, 0);
}

static void test_version(void **state)
{
	struct sink s = { 0 };
	struct opts o = { .version = "{\"format\":\"mender\",\"version\":2}" };

	(void)state;
	assert_int_equal(run(&o, "swu", NULL, &s), -ENOTSUP);
	o.version = "{\"format\":\"other\",\"version\":3}";
	assert_int_equal(run(&o, "swu", NULL, &s), -EINVAL);
	o.version = "{\"format\":\"mender\",\"version\":\"3\"}";
	assert_int_equal(run(&o, "swu", NULL, &s), -EINVAL);
	o.version = "not json";
	assert_int_equal(run(&o, "swu", NULL, &s), -EINVAL);
	assert_int_equal(s.out.len, 0);
}

static void test_layout(void **state)
{
	struct sink s = { 0 };
	struct opts o;

	(void)state;
	o = (struct opts){ .data_first = true };
	assert_int_equal(run(&o, "swu", NULL, &s), -EINVAL);
	o = (struct opts){ .no_manifest = true };
	assert_int_equal(run(&o, "swu", NULL, &s), -EINVAL);
	o = (struct opts){ .type_info_first = true };
	assert_int_equal(run(&o, "swu", NULL, &s), -EINVAL);
	o = (struct opts){ .no_data = true };
	assert_int_equal(run(&o, "swu", NULL, &s), -EINVAL);
	assert_int_equal(s.out.len, 0);

	/* optional parts */
	o = (struct opts){ .signature = true, .scripts = true, .data_dir = true };
	assert_int_equal(run(&o, "swu", NULL, &s), 0);
	assert_payload(&s, PAYLOAD_SIZE);
	mb_free(&s.out);
}

static void test_unsupported(void **state)
{
	struct sink s = { 0 };
	struct opts o;

	(void)state;
	o = (struct opts){ .augment = true };
	assert_int_equal(run(&o, "swu", NULL, &s), -ENOTSUP);
	o = (struct opts){ .second_bucket = true };
	assert_int_equal(run(&o, "swu", NULL, &s), -ENOTSUP);
	assert_int_equal(s.out.len, 0);

	o = (struct opts){ .two_files = true };
	assert_int_equal(run(&o, "swu", NULL, &s), -ENOTSUP);
	assert_payload(&s, PAYLOAD_SIZE - 64 * 1024);
	mb_free(&s.out);

	/* unknown compression */
	o = (struct opts){ .suffix_d = ".bz2" };
	assert_int_equal(run(&o, "swu", NULL, &s), -ENOTSUP);
	assert_int_equal(s.out.len, 0);
}

static void test_long_name(void **state)
{
	char name[200];
	struct sink s = { 0 };
	struct mbuf art;
	struct mender_artifact *ma;
	struct opts o = { .payload_name = name };

	(void)state;
	memset(name, 'n', sizeof(name) - 5);
	strcpy(name + sizeof(name) - 5, ".swu");
	build(&o, &art);
	assert_int_equal(unwrap(art.p, art.len, 300, "swu", NULL, &s, &ma), 0);
	assert_payload(&s, PAYLOAD_SIZE);
	assert_string_equal(mender_artifact_payload_name(ma), name);
	mender_artifact_free(ma);
	mb_free(&s.out);
	mb_free(&art);
}

static void test_size_formats(void **state)
{
	static const enum size_fmt fmts[] = { SIZE_PAX, SIZE_BASE256 };
	size_t i;

	(void)state;
	for (i = 0; i < ARRAY_SIZE(fmts); i++) {
		struct opts o = { .size_fmt = fmts[i] };
		struct sink s = { 0 };
		struct mender_artifact *ma;
		struct mbuf art;

		build(&o, &art);
		assert_int_equal(unwrap(art.p, art.len, 4096, "swu", NULL, &s, &ma), 0);
		assert_payload(&s, PAYLOAD_SIZE);
		assert_int_equal(mender_artifact_payload_size(ma), PAYLOAD_SIZE);
		mender_artifact_free(ma);
		mb_free(&s.out);
		mb_free(&art);
	}
}

/* A compressed stream that ends early fails even if the tar inside is complete */
static void test_trimmed_stream(void **state)
{
	size_t c;

	(void)state;
	for (c = 1; c < ARRAY_SIZE(comps); c++) {
		struct opts o = { .dcomp = comps[c].c, .suffix_d = comps[c].suffix,
				  .trim_data = 4 };
		struct sink s = { 0 };

		assert_int_equal(run(&o, "swu", NULL, &s), -EINVAL);
		mb_free(&s.out);
	}
}

static void test_null_payload(void **state)
{
	struct sink s = { 0 };
	struct opts o = { .type = "null" };

	(void)state;
	assert_int_equal(run(&o, NULL, NULL, &s), 0);
	assert_int_equal(s.out.len, 0);
	assert_int_equal(run(&o, "swu", NULL, &s), -ENOEXEC);
}

static void test_callback_error(void **state)
{
	struct sink s = { .fail_with = -EIO };
	struct opts o = { 0 };

	(void)state;
	assert_int_equal(run(&o, "swu", NULL, &s), -EIO);

	/* a small payload goes out only from finish() */
	o.payload_len = 3000;
	assert_int_equal(run(&o, "swu", NULL, &s), -EIO);
}

static void test_empty(void **state)
{
	struct mender_artifact *ma = mender_artifact_new(NULL);

	(void)state;
	assert_non_null(ma);
	assert_null(mender_artifact_name(ma));
	assert_null(mender_artifact_payload_name(ma));
	assert_int_equal(mender_artifact_feed(ma, NULL, 0), 0);
	assert_int_equal(mender_artifact_finish(ma), -EINVAL);
	mender_artifact_free(ma);
	mender_artifact_free(NULL);
}

/* Every truncation must fail, never pass on more than the payload */
static void test_truncated(void **state)
{
	size_t c, cut;

	(void)state;
	for (c = 0; c < ARRAY_SIZE(comps); c++) {
		struct opts o = { .payload_len = 3000,
				  .hcomp = comps[c].c, .suffix_h = comps[c].suffix,
				  .dcomp = comps[c].c, .suffix_d = comps[c].suffix };
		struct mbuf art;
		size_t complete;

		build(&o, &art);
		/* the archive is complete with the first end-of-archive block */
		complete = art.len - 512;
		for (cut = 0; cut < complete; cut += (c ? 13 : 61)) {
			struct sink s = { 0 };

			assert_int_not_equal(unwrap(art.p, cut, 509, "swu", NULL, &s, NULL), 0);
			assert_true(s.out.len <= 3000);
			if (s.out.len)
				assert_memory_equal(s.out.p, payload, s.out.len);
			mb_free(&s.out);
		}
		mb_free(&art);
	}
}

/* A changed byte either fails or does not change the payload */
static void test_corrupted(void **state)
{
	size_t c, pos;

	(void)state;
	for (c = 0; c < ARRAY_SIZE(comps); c++) {
		struct opts o = { .payload_len = 3000,
				  .hcomp = comps[c].c, .suffix_h = comps[c].suffix,
				  .dcomp = comps[c].c, .suffix_d = comps[c].suffix };
		struct mbuf art;

		build(&o, &art);
		for (pos = 0; pos < art.len; pos += 7) {
			struct sink s = { 0 };
			uint8_t saved = art.p[pos];
			int ret;

			art.p[pos] ^= 0x5a;
			ret = unwrap(art.p, art.len, 4096, "swu", NULL, &s, NULL);
			art.p[pos] = saved;
			if (!ret)
				assert_payload(&s, 3000);
			mb_free(&s.out);
		}
		mb_free(&art);
	}
}

static void test_garbage(void **state)
{
	uint8_t buf[8192];
	struct sink s = { 0 };
	size_t i;

	(void)state;
	for (i = 0; i < sizeof(buf); i++)
		buf[i] = payload[i];
	assert_int_not_equal(unwrap(buf, sizeof(buf), 100, NULL, NULL, &s, NULL), 0);
	memset(buf, 0, sizeof(buf));
	assert_int_not_equal(unwrap(buf, sizeof(buf), 100, NULL, NULL, &s, NULL), 0);
	assert_int_equal(s.out.len, 0);
}

static int setup(void **state)
{
	uint32_t x = 0x12345678;
	size_t i;

	(void)state;
	payload = malloc(PAYLOAD_SIZE);
	if (!payload)
		return -1;
	/* half random, half text: something to compress, nothing trivial */
	for (i = 0; i < PAYLOAD_SIZE; i++) {
		x = x * 1103515245 + 12345;
		payload[i] = (i / 4096) % 2 ? (uint8_t)(x >> 24) : (uint8_t)("swupdate"[i % 8]);
	}

	return 0;
}

static int teardown(void **state)
{
	(void)state;
	free(payload);
	return 0;
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_plain),
		cmocka_unit_test(test_compressed),
		cmocka_unit_test(test_small_payload),
		cmocka_unit_test(test_checksums),
		cmocka_unit_test(test_compatibility),
		cmocka_unit_test(test_version),
		cmocka_unit_test(test_layout),
		cmocka_unit_test(test_unsupported),
		cmocka_unit_test(test_long_name),
		cmocka_unit_test(test_size_formats),
		cmocka_unit_test(test_trimmed_stream),
		cmocka_unit_test(test_null_payload),
		cmocka_unit_test(test_callback_error),
		cmocka_unit_test(test_empty),
		cmocka_unit_test(test_truncated),
		cmocka_unit_test(test_corrupted),
		cmocka_unit_test(test_garbage),
	};

	return cmocka_run_group_tests_name("mender_artifact", tests, setup, teardown);
}
