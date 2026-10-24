/*
 * (C) Copyright 2026
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 *
 * Streaming unwrapper for Mender Artifacts, format version 3:
 *
 *   version
 *   manifest
 *   manifest.sig                         (optional, not verified)
 *   header.tar[.gz|.xz|.zst]
 *     header-info
 *     scripts/...                        (ignored)
 *     headers/0000/type-info
 *     headers/0000/meta-data
 *   data/0000.tar[.gz|.xz|.zst]
 *     <payload file>
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <json-c/json.h>

#include "generated/autoconf.h"
#include "util.h"
#include "swupdate_crypto.h"
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

#define TAR_BLOCK	512
#define TAR_NAME_MAX	4096
#define TAR_META_MAX	(64 * 1024)	/* pax records, GNU long names */
#define VERSION_MAX	4096
#define MANIFEST_MAX	(1024 * 1024)
#define JSON_MAX	(64 * 1024)	/* header-info, type-info */
#define DECOMP_BUF	(64 * 1024)
#define PAYLOAD_TAIL	(64 * 1024)	/* held back until the artifact is verified */
#define SHA256_HEX	(2 * SHA256_HASH_LENGTH)

/*
 * Push parser for tar streams: ustar, pax (path, size) and GNU long names.
 */
typedef int (*tar_begin_fn)(void *priv, const char *name, char type, uint64_t size);
typedef int (*tar_data_fn)(void *priv, const uint8_t *buf, size_t len);
typedef int (*tar_end_fn)(void *priv);

enum tar_state { TAR_HDR, TAR_DATA, TAR_PAD, TAR_EOF };
enum tar_meta { META_NONE, META_PAX, META_LONGNAME, META_SKIP };

struct tar {
	enum tar_state state;
	uint8_t blk[TAR_BLOCK];
	size_t fill;
	uint64_t size;		/* size of the current entry */
	uint64_t remain;
	size_t pad;
	enum tar_meta meta;
	char *metabuf;
	size_t metalen;
	char name[TAR_NAME_MAX];
	char next_name[TAR_NAME_MAX];
	bool has_next_name;
	uint64_t next_size;
	bool has_next_size;
	tar_begin_fn begin;
	tar_data_fn data;
	tar_end_fn end;
	void *priv;
};

static void tar_init(struct tar *t, tar_begin_fn begin, tar_data_fn data,
		     tar_end_fn end, void *priv)
{
	free(t->metabuf);
	memset(t, 0, sizeof(*t));
	t->begin = begin;
	t->data = data;
	t->end = end;
	t->priv = priv;
}

static void tar_free(struct tar *t)
{
	free(t->metabuf);
	t->metabuf = NULL;
}

static int tar_octal(const uint8_t *p, size_t len, uint64_t *val)
{
	uint64_t v = 0;
	size_t i = 0;

	if (p[0] & 0x80) {
		/* base-256, GNU extension for big sizes */
		if (p[0] != 0x80)
			return -EINVAL;	/* negative or more than 64 bit */
		for (i = 1; i < len; i++) {
			if (v >> 56)
				return -EINVAL;
			v = (v << 8) | p[i];
		}
		*val = v;
		return 0;
	}

	while (i < len && p[i] == ' ')
		i++;
	for (; i < len && p[i] >= '0' && p[i] <= '7'; i++) {
		if (v >> 61)
			return -EINVAL;
		v = (v << 3) | (uint64_t)(p[i] - '0');
	}
	for (; i < len; i++)
		if (p[i] != ' ' && p[i] != '\0')
			return -EINVAL;
	*val = v;
	return 0;
}

static int tar_checksum(const uint8_t *blk)
{
	uint64_t stored;
	unsigned long sum = 0;
	int i;

	if (tar_octal(blk + 148, 8, &stored))
		return -EINVAL;
	for (i = 0; i < TAR_BLOCK; i++)
		sum += (i >= 148 && i < 156) ? ' ' : blk[i];

	return sum == stored ? 0 : -EINVAL;
}

static void tar_copy_field(char *dst, const uint8_t *src, size_t len)
{
	size_t n = strnlen((const char *)src, len);

	memcpy(dst, src, n);
	dst[n] = '\0';
}

/* Records are "<len> <key>=<value>\n" */
static int tar_parse_pax(struct tar *t)
{
	const char *p = t->metabuf;
	const char *end = t->metabuf + t->metalen;

	while (p < end) {
		const char *rec = p, *key, *eq, *recend;
		unsigned long len = 0;

		while (p < end && *p >= '0' && *p <= '9') {
			len = len * 10 + (unsigned long)(*p - '0');
			if (len > t->metalen)
				return -EINVAL;
			p++;
		}
		if (p == rec || p >= end || *p != ' ' ||
		    len > (unsigned long)(end - rec))
			return -EINVAL;
		recend = rec + len;
		if (recend[-1] != '\n')
			return -EINVAL;
		key = p + 1;
		eq = memchr(key, '=', (size_t)(recend - key));
		if (!eq)
			return -EINVAL;

		if ((size_t)(eq - key) == 4 && !strncmp(key, "path", 4)) {
			size_t n = (size_t)(recend - 1 - (eq + 1));

			if (!n || n >= sizeof(t->next_name))
				return -EINVAL;
			memcpy(t->next_name, eq + 1, n);
			t->next_name[n] = '\0';
			t->has_next_name = true;
		} else if ((size_t)(eq - key) == 4 && !strncmp(key, "size", 4)) {
			uint64_t v = 0;
			const char *q;

			for (q = eq + 1; q < recend - 1; q++) {
				if (*q < '0' || *q > '9' || v > (UINT64_MAX - 9) / 10)
					return -EINVAL;
				v = v * 10 + (uint64_t)(*q - '0');
			}
			if (q == eq + 1)
				return -EINVAL;
			t->next_size = v;
			t->has_next_size = true;
		}
		p = recend;
	}

	return 0;
}

static int tar_entry_done(struct tar *t)
{
	int ret = 0;

	switch (t->meta) {
	case META_PAX:
		ret = tar_parse_pax(t);
		break;
	case META_LONGNAME:
		if (!t->metalen || t->metalen >= sizeof(t->next_name)) {
			ret = -EINVAL;
			break;
		}
		memcpy(t->next_name, t->metabuf, t->metalen);
		t->next_name[t->metalen] = '\0';	/* stops at the first NUL */
		if (!t->next_name[0])
			ret = -EINVAL;
		t->has_next_name = true;
		break;
	case META_SKIP:
		break;
	case META_NONE:
		ret = t->end(t->priv);
		break;
	}
	if (ret == -EINVAL && t->meta != META_NONE)
		ERROR("tar: invalid extended header");
	t->meta = META_NONE;
	t->metalen = 0;
	if (ret)
		return ret;

	t->pad = (size_t)((TAR_BLOCK - t->size % TAR_BLOCK) % TAR_BLOCK);
	t->state = t->pad ? TAR_PAD : TAR_HDR;

	return 0;
}

static int tar_header(struct tar *t)
{
	const uint8_t *b = t->blk;
	char type = (char)b[156];
	uint64_t size;
	int i, ret;

	for (i = 0; i < TAR_BLOCK && !b[i]; i++)
		;
	if (i == TAR_BLOCK) {
		/* end of archive, anything after it is ignored */
		if (t->has_next_name || t->has_next_size)
			return -EINVAL;
		t->state = TAR_EOF;
		return 0;
	}

	if (tar_checksum(b) || tar_octal(b + 124, 12, &size)) {
		ERROR("tar: invalid header");
		return -EINVAL;
	}

	if (t->has_next_name) {
		strlcpy(t->name, t->next_name, sizeof(t->name));
	} else if (!memcmp(b + 257, "ustar", 5) && b[345]) {
		char prefix[156], name[101];

		tar_copy_field(prefix, b + 345, 155);
		tar_copy_field(name, b, 100);
		snprintf(t->name, sizeof(t->name), "%s/%s", prefix, name);
	} else {
		tar_copy_field(t->name, b, 100);
	}

	t->size = size;
	t->remain = size;
	t->meta = META_NONE;
	t->metalen = 0;

	switch (type) {
	case 'x':
	case 'L':
		if (size > TAR_META_MAX) {
			ERROR("tar: extended header too large");
			return -EINVAL;
		}
		if (!t->metabuf) {
			t->metabuf = malloc(TAR_META_MAX);
			if (!t->metabuf)
				return -ENOMEM;
		}
		t->meta = type == 'x' ? META_PAX : META_LONGNAME;
		break;
	case 'g':
	case 'K':
		t->meta = META_SKIP;
		break;
	default:
		if (t->has_next_size) {
			t->size = t->next_size;
			t->remain = t->next_size;
		}
		t->has_next_name = false;
		t->has_next_size = false;
		/* only regular files carry data */
		if (!t->name[0] ||
		    (type != '0' && type != '\0' && type != '7' && t->size)) {
			ERROR("tar: invalid entry");
			return -EINVAL;
		}
		ret = t->begin(t->priv, t->name, type == '\0' || type == '7' ? '0' : type,
			       t->size);
		if (ret)
			return ret;
		break;
	}

	if (!t->remain)
		return tar_entry_done(t);
	t->state = TAR_DATA;

	return 0;
}

static int tar_feed(struct tar *t, const uint8_t *buf, size_t len)
{
	int ret;

	while (len) {
		size_t n;

		switch (t->state) {
		case TAR_EOF:
			return 0;
		case TAR_HDR:
			n = min(len, TAR_BLOCK - t->fill);
			memcpy(t->blk + t->fill, buf, n);
			t->fill += n;
			buf += n;
			len -= n;
			if (t->fill < TAR_BLOCK)
				break;
			t->fill = 0;
			ret = tar_header(t);
			if (ret)
				return ret;
			break;
		case TAR_DATA:
			n = (size_t)min((uint64_t)len, t->remain);
			if (t->meta == META_PAX || t->meta == META_LONGNAME) {
				memcpy(t->metabuf + t->metalen, buf, n);
				t->metalen += n;
			} else if (t->meta == META_NONE) {
				ret = t->data(t->priv, buf, n);
				if (ret)
					return ret;
			}
			buf += n;
			len -= n;
			t->remain -= n;
			if (!t->remain) {
				ret = tar_entry_done(t);
				if (ret)
					return ret;
			}
			break;
		case TAR_PAD:
			n = min(len, t->pad);
			buf += n;
			len -= n;
			t->pad -= n;
			if (!t->pad)
				t->state = TAR_HDR;
			break;
		}
	}

	return 0;
}

/* The archive must be complete, with its end-of-archive marker */
static int tar_finish(struct tar *t)
{
	return t->state == TAR_EOF ? 0 : -EINVAL;
}

static int tar_feed_cb(void *priv, const uint8_t *buf, size_t len)
{
	return tar_feed(priv, buf, len);
}

/*
 * Push decompressors
 */
enum comp { COMP_NONE, COMP_GZ, COMP_XZ, COMP_ZSTD };

typedef int (*sink_fn)(void *priv, const uint8_t *buf, size_t len);

struct decomp {
	enum comp type;
	bool active;
	bool done;
	uint8_t *buf;
#ifdef CONFIG_GUNZIP
	z_stream z;
#endif
#ifdef CONFIG_XZ
	lzma_stream x;
#endif
#ifdef CONFIG_ZSTD
	ZSTD_DStream *zs;
#endif
};

static const struct {
	const char *suffix;
	enum comp type;
	bool available;
} compressors[] = {
	{ "", COMP_NONE, true },
#ifdef CONFIG_GUNZIP
	{ ".gz", COMP_GZ, true },
#else
	{ ".gz", COMP_GZ, false },
#endif
#ifdef CONFIG_XZ
	{ ".xz", COMP_XZ, true },
#else
	{ ".xz", COMP_XZ, false },
#endif
#ifdef CONFIG_ZSTD
	{ ".zst", COMP_ZSTD, true },
#else
	{ ".zst", COMP_ZSTD, false },
#endif
};

/*
 * Match "<base>.tar<suffix>". Returns 0, -ENOENT if the name does not
 * match, -ENOTSUP if the compression is unknown or not built in.
 */
static int match_tar_name(const char *name, const char *base, enum comp *type)
{
	size_t blen = strlen(base);
	size_t i;

	if (strncmp(name, base, blen) || strncmp(name + blen, ".tar", 4))
		return -ENOENT;
	name += blen + 4;
	for (i = 0; i < ARRAY_SIZE(compressors); i++) {
		if (strcmp(name, compressors[i].suffix))
			continue;
		if (!compressors[i].available)
			return -ENOTSUP;
		*type = compressors[i].type;
		return 0;
	}

	return *name == '.' ? -ENOTSUP : -ENOENT;
}

static void decomp_end_stream(struct decomp *d)
{
	if (!d->active)
		return;
	switch (d->type) {
#ifdef CONFIG_GUNZIP
	case COMP_GZ:
		inflateEnd(&d->z);
		break;
#endif
#ifdef CONFIG_XZ
	case COMP_XZ:
		lzma_end(&d->x);
		break;
#endif
#ifdef CONFIG_ZSTD
	case COMP_ZSTD:
		ZSTD_freeDStream(d->zs);
		d->zs = NULL;
		break;
#endif
	default:
		break;
	}
	d->active = false;
}

static int decomp_init(struct decomp *d, enum comp type)
{
	decomp_end_stream(d);
	d->type = type;
	d->done = type == COMP_NONE;

	if (type != COMP_NONE && !d->buf) {
		d->buf = malloc(DECOMP_BUF);
		if (!d->buf)
			return -ENOMEM;
	}

	switch (type) {
#ifdef CONFIG_GUNZIP
	case COMP_GZ:
		memset(&d->z, 0, sizeof(d->z));
		if (inflateInit2(&d->z, 16 + MAX_WBITS) != Z_OK)
			return -ENOMEM;
		break;
#endif
#ifdef CONFIG_XZ
	case COMP_XZ:
		d->x = (lzma_stream)LZMA_STREAM_INIT;
		if (lzma_stream_decoder(&d->x, UINT64_MAX, LZMA_CONCATENATED) != LZMA_OK)
			return -ENOMEM;
		break;
#endif
#ifdef CONFIG_ZSTD
	case COMP_ZSTD:
		d->zs = ZSTD_createDStream();
		if (!d->zs || ZSTD_isError(ZSTD_initDStream(d->zs))) {
			ZSTD_freeDStream(d->zs);
			d->zs = NULL;
			return -ENOMEM;
		}
		break;
#endif
	default:
		break;
	}
	d->active = true;

	return 0;
}

#ifdef CONFIG_GUNZIP
static int decomp_gz(struct decomp *d, const uint8_t *in, size_t len,
		     sink_fn sink, void *priv)
{
	int r, ret;
	size_t produced;

	d->z.next_in = (Bytef *)in;
	d->z.avail_in = (uInt)len;

	for (;;) {
		if (d->done) {
			if (!d->z.avail_in)
				return 0;
			/* another gzip member follows */
			if (inflateReset(&d->z) != Z_OK)
				return -EINVAL;
			d->done = false;
		}
		d->z.next_out = d->buf;
		d->z.avail_out = DECOMP_BUF;
		r = inflate(&d->z, Z_NO_FLUSH);
		if (r != Z_OK && r != Z_STREAM_END && r != Z_BUF_ERROR) {
			ERROR("gzip: %s", d->z.msg ? d->z.msg : "decompression error");
			return -EINVAL;
		}
		produced = DECOMP_BUF - d->z.avail_out;
		if (produced) {
			ret = sink(priv, d->buf, produced);
			if (ret)
				return ret;
		}
		if (r == Z_STREAM_END) {
			d->done = true;
			continue;
		}
		if (r == Z_BUF_ERROR && !produced)
			return 0;	/* needs more input */
		if (!d->z.avail_in && d->z.avail_out)
			return 0;
	}
}
#endif

#ifdef CONFIG_XZ
static int decomp_xz(struct decomp *d, const uint8_t *in, size_t len,
		     lzma_action action, sink_fn sink, void *priv)
{
	lzma_ret r;
	int ret;
	size_t produced;

	d->x.next_in = in;
	d->x.avail_in = len;

	for (;;) {
		d->x.next_out = d->buf;
		d->x.avail_out = DECOMP_BUF;
		r = lzma_code(&d->x, action);
		produced = DECOMP_BUF - d->x.avail_out;
		if (produced) {
			ret = sink(priv, d->buf, produced);
			if (ret)
				return ret;
		}
		if (r == LZMA_STREAM_END) {
			d->done = true;
			return d->x.avail_in ? -EINVAL : 0;
		}
		if (r == LZMA_BUF_ERROR && !produced)
			return action == LZMA_FINISH ? -EINVAL : 0;
		if (r != LZMA_OK) {
			ERROR("xz: decompression error %d", r);
			return -EINVAL;
		}
		if (!d->x.avail_in && d->x.avail_out && action == LZMA_RUN)
			return 0;
	}
}
#endif

#ifdef CONFIG_ZSTD
static int decomp_zstd(struct decomp *d, const uint8_t *in, size_t len,
		       sink_fn sink, void *priv)
{
	ZSTD_inBuffer ib = { in, len, 0 };
	size_t r;
	int ret;

	for (;;) {
		ZSTD_outBuffer ob = { d->buf, DECOMP_BUF, 0 };

		r = ZSTD_decompressStream(d->zs, &ob, &ib);
		if (ZSTD_isError(r)) {
			ERROR("zstd: %s", ZSTD_getErrorName(r));
			return -EINVAL;
		}
		if (ob.pos) {
			ret = sink(priv, d->buf, ob.pos);
			if (ret)
				return ret;
		}
		d->done = r == 0;
		if (ib.pos == ib.size && ob.pos < ob.size)
			return 0;
	}
}
#endif

static int decomp_feed(struct decomp *d, const uint8_t *in, size_t len,
		       sink_fn sink, void *priv)
{
	switch (d->type) {
	case COMP_NONE:
		return sink(priv, in, len);
#ifdef CONFIG_GUNZIP
	case COMP_GZ:
		return decomp_gz(d, in, len, sink, priv);
#endif
#ifdef CONFIG_XZ
	case COMP_XZ:
		return decomp_xz(d, in, len, LZMA_RUN, sink, priv);
#endif
#ifdef CONFIG_ZSTD
	case COMP_ZSTD:
		return decomp_zstd(d, in, len, sink, priv);
#endif
	default:
		return -ENOTSUP;
	}
}

/* The compressed stream must be complete */
static int decomp_finish(struct decomp *d, sink_fn sink, void *priv)
{
	int ret = 0;

#ifdef CONFIG_XZ
	if (d->type == COMP_XZ)
		ret = decomp_xz(d, NULL, 0, LZMA_FINISH, sink, priv);
#else
	(void)sink;
	(void)priv;
#endif
	if (!ret && !d->done)
		ret = -EINVAL;
	decomp_end_stream(d);

	return ret;
}

/*
 * Collected small files: version, manifest, header-info, type-info
 */
struct dbuf {
	char *p;
	size_t len;
	size_t max;
};

static void dbuf_reset(struct dbuf *b, size_t max)
{
	free(b->p);
	b->p = NULL;
	b->len = 0;
	b->max = max;
}

static int dbuf_add(struct dbuf *b, const uint8_t *buf, size_t len)
{
	char *p;

	if (len > b->max - b->len)
		return -EINVAL;
	p = realloc(b->p, b->len + len + 1);
	if (!p)
		return -ENOMEM;
	memcpy(p + b->len, buf, len);
	b->p = p;
	b->len += len;
	b->p[b->len] = '\0';

	return 0;
}

/*
 * Artifact
 */
struct manifest_entry {
	char *name;
	unsigned char digest[SHA256_HASH_LENGTH];
	bool seen;
};

enum outer_entry {
	OUTER_NONE,
	OUTER_VERSION,
	OUTER_MANIFEST,
	OUTER_SIGNATURE,
	OUTER_HEADER,
	OUTER_DATA,
	OUTER_DIR,
};

/* What may come next in the outer archive */
enum stage {
	STAGE_VERSION,
	STAGE_MANIFEST,
	STAGE_HEADER,		/* manifest.sig or header */
	STAGE_DATA,
	STAGE_DONE,
};

enum inner_entry {
	INNER_NONE,
	INNER_HEADER_INFO,
	INNER_TYPE_INFO,
	INNER_IGNORE,
	INNER_PAYLOAD,
};

struct mender_artifact {
	struct mender_artifact_cfg cfg;
	int err;

	struct tar outer;
	enum outer_entry cur;
	enum stage stage;
	char curname[TAR_NAME_MAX];
	void *dgst;
	unsigned char version_digest[SHA256_HASH_LENGTH];

	struct manifest_entry *manifest;
	size_t manifest_count;

	struct decomp dc;
	struct tar inner;
	enum inner_entry icur;
	bool header_info_seen;
	bool type_info_seen;
	bool data_seen;
	void *idgst;
	char iname[TAR_NAME_MAX + 16];

	struct dbuf collect;	/* outer small files */
	struct dbuf icollect;	/* header small files */

	json_object *header_info;
	const char *artifact_name;
	const char *payload_type;
	char *payload_name;
	uint64_t payload_size;
	uint64_t payload_done;
	unsigned int payload_count;
	uint8_t *tail;
	size_t tail_cap;
	size_t tail_len;
};

static void *hash_start(void)
{
	void *dgst = swupdate_HASH_init(SHA_DEFAULT);

	if (!dgst)
		ERROR("Mender Artifact: cannot initialize sha256");

	return dgst;
}

static int hash_end(void **dgst, unsigned char *digest)
{
	unsigned int len = 0;
	int ret;

	ret = swupdate_HASH_final(*dgst, digest, &len);
	swupdate_HASH_cleanup(*dgst);
	*dgst = NULL;
	if (ret || len != SHA256_HASH_LENGTH)
		return -EFAULT;

	return 0;
}

static void hash_drop(void **dgst)
{
	if (*dgst) {
		swupdate_HASH_cleanup(*dgst);
		*dgst = NULL;
	}
}

static struct manifest_entry *manifest_find(struct mender_artifact *ma,
					    const char *name)
{
	size_t i;

	for (i = 0; i < ma->manifest_count; i++)
		if (!strcmp(ma->manifest[i].name, name))
			return &ma->manifest[i];

	return NULL;
}

static int manifest_check(struct mender_artifact *ma, const char *name,
			  const unsigned char *digest)
{
	struct manifest_entry *e = manifest_find(ma, name);

	if (!e) {
		ERROR("Mender Artifact: %s is not in the manifest", name);
		return -EBADMSG;
	}
	if (e->seen) {
		ERROR("Mender Artifact: %s found twice", name);
		return -EINVAL;
	}
	if (swupdate_HASH_compare(e->digest, digest)) {
		ERROR("Mender Artifact: checksum mismatch for %s", name);
		return -EBADMSG;
	}
	e->seen = true;

	return 0;
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* Lines as written by sha256sum: "<64 hex>  <name>\n" */
static int manifest_parse(struct mender_artifact *ma)
{
	char *p = ma->collect.p;
	char *end = p + ma->collect.len;

	if (!p || memchr(p, '\0', ma->collect.len))
		return -EINVAL;

	while (p < end) {
		struct manifest_entry *entries, *e;
		char *eol = memchr(p, '\n', (size_t)(end - p));
		char *name;
		int i;

		if (!eol)
			eol = end;
		if (eol - p < SHA256_HEX + 3 || p[SHA256_HEX] != ' ' ||
		    p[SHA256_HEX + 1] != ' ')
			return -EINVAL;
		name = p + SHA256_HEX + 2;
		*eol = '\0';

		if (manifest_find(ma, name))
			return -EINVAL;
		entries = realloc(ma->manifest,
				  (ma->manifest_count + 1) * sizeof(*entries));
		if (!entries)
			return -ENOMEM;
		ma->manifest = entries;
		e = &entries[ma->manifest_count];
		memset(e, 0, sizeof(*e));
		for (i = 0; i < SHA256_HASH_LENGTH; i++) {
			int hi = hexval(p[2 * i]), lo = hexval(p[2 * i + 1]);

			if (hi < 0 || lo < 0)
				return -EINVAL;
			e->digest[i] = (unsigned char)(hi << 4 | lo);
		}
		e->name = strdup(name);
		if (!e->name)
			return -ENOMEM;
		ma->manifest_count++;
		p = eol + 1;
	}

	return ma->manifest_count ? 0 : -EINVAL;
}

static int version_parse(struct mender_artifact *ma)
{
	json_object *root, *fmt, *ver;
	int ret = 0;

	root = ma->collect.p ? json_tokener_parse(ma->collect.p) : NULL;
	if (!root || !json_object_object_get_ex(root, "format", &fmt) ||
	    !json_object_is_type(fmt, json_type_string) ||
	    strcmp(json_object_get_string(fmt), "mender") ||
	    !json_object_object_get_ex(root, "version", &ver) ||
	    !json_object_is_type(ver, json_type_int)) {
		ERROR("Mender Artifact: invalid version file");
		ret = -EINVAL;
	} else if (json_object_get_int64(ver) != 3) {
		ERROR("Mender Artifact: version %lld is not supported",
		      (long long)json_object_get_int64(ver));
		ret = -ENOTSUP;
	}
	json_object_put(root);

	return ret;
}

static const char *json_string(json_object *obj, const char *key)
{
	json_object *val;

	if (!json_object_object_get_ex(obj, key, &val) ||
	    !json_object_is_type(val, json_type_string))
		return NULL;

	return json_object_get_string(val);
}

static int header_info_parse(struct mender_artifact *ma)
{
	json_object *root, *payloads, *provides, *first;

	root = ma->icollect.p ? json_tokener_parse(ma->icollect.p) : NULL;
	if (!root || !json_object_is_type(root, json_type_object)) {
		json_object_put(root);
		ERROR("Mender Artifact: invalid header-info");
		return -EINVAL;
	}
	ma->header_info = root;

	if (!json_object_object_get_ex(root, "payloads", &payloads) ||
	    !json_object_is_type(payloads, json_type_array) ||
	    !json_object_array_length(payloads)) {
		ERROR("Mender Artifact: header-info has no payloads");
		return -EINVAL;
	}
	if (json_object_array_length(payloads) > 1) {
		ERROR("Mender Artifact: more than one payload is not supported");
		return -ENOTSUP;
	}
	first = json_object_array_get_idx(payloads, 0);
	if (json_object_is_type(first, json_type_object) &&
	    json_object_object_get_ex(first, "type", &first) &&
	    json_object_is_type(first, json_type_null))
		ma->payload_type = "null";
	else
		ma->payload_type = json_string(json_object_array_get_idx(payloads, 0),
					       "type");
	if (!ma->payload_type || !ma->payload_type[0]) {
		ERROR("Mender Artifact: payload without type");
		return -EINVAL;
	}

	if (!json_object_object_get_ex(root, "artifact_provides", &provides) ||
	    !(ma->artifact_name = json_string(provides, "artifact_name")) ||
	    !ma->artifact_name[0]) {
		ERROR("Mender Artifact: header-info has no artifact_name");
		return -EINVAL;
	}

	return 0;
}

static int type_info_parse(struct mender_artifact *ma)
{
	json_object *root, *type;
	const char *t = NULL;
	int ret = 0;

	root = ma->icollect.p ? json_tokener_parse(ma->icollect.p) : NULL;
	if (root && json_object_object_get_ex(root, "type", &type)) {
		if (json_object_is_type(type, json_type_null))
			t = "null";
		else if (json_object_is_type(type, json_type_string))
			t = json_object_get_string(type);
	}
	if (!t || strcmp(t, ma->payload_type)) {
		ERROR("Mender Artifact: type-info does not match header-info");
		ret = -EINVAL;
	}
	json_object_put(root);

	return ret;
}

static bool device_compatible(const struct mender_artifact *ma,
			      const char *device_type)
{
	json_object *depends, *types;
	size_t i;

	if (!device_type)
		return false;
	if (!json_object_object_get_ex(ma->header_info, "artifact_depends", &depends) ||
	    !json_object_object_get_ex(depends, "device_type", &types) ||
	    !json_object_is_type(types, json_type_array))
		return false;

	for (i = 0; i < json_object_array_length(types); i++) {
		json_object *t = json_object_array_get_idx(types, i);

		if (json_object_is_type(t, json_type_string) &&
		    !strcmp(json_object_get_string(t), device_type))
			return true;
	}

	return false;
}

bool mender_artifact_device_compatible(const struct mender_artifact *ma,
				       const char *device_type)
{
	return mender_artifact_name(ma) && device_compatible(ma, device_type);
}

/* Header archive */
static int header_begin(void *priv, const char *name, char type, uint64_t size)
{
	struct mender_artifact *ma = priv;

	(void)size;
	ma->icur = INNER_IGNORE;
	if (type == '5')
		return 0;
	if (type != '0') {
		ERROR("Mender Artifact: unexpected entry %s in header", name);
		return -EINVAL;
	}

	if (!ma->header_info_seen && strcmp(name, "header-info")) {
		ERROR("Mender Artifact: header-info must be first in header");
		return -EINVAL;
	}

	if (!strcmp(name, "header-info")) {
		if (ma->header_info_seen)
			return -EINVAL;
		ma->icur = INNER_HEADER_INFO;
	} else if (!strcmp(name, "headers/0000/type-info")) {
		if (ma->type_info_seen)
			return -EINVAL;
		ma->icur = INNER_TYPE_INFO;
	} else if (!strcmp(name, "headers/0000/meta-data")) {
		ma->icur = INNER_IGNORE;
	} else if (!strncmp(name, "scripts/", 8)) {
		WARN("Mender Artifact: state script %s is ignored", name + 8);
	} else if (!strncmp(name, "headers/", 8)) {
		ERROR("Mender Artifact: %s: more than one payload is not supported",
		      name);
		return -ENOTSUP;
	} else {
		ERROR("Mender Artifact: unexpected entry %s in header", name);
		return -EINVAL;
	}
	if (ma->icur != INNER_IGNORE)
		dbuf_reset(&ma->icollect, JSON_MAX);

	return 0;
}

static int header_data(void *priv, const uint8_t *buf, size_t len)
{
	struct mender_artifact *ma = priv;

	if (ma->icur == INNER_HEADER_INFO || ma->icur == INNER_TYPE_INFO)
		return dbuf_add(&ma->icollect, buf, len);

	return 0;
}

static int header_end(void *priv)
{
	struct mender_artifact *ma = priv;
	int ret = 0;

	switch (ma->icur) {
	case INNER_HEADER_INFO:
		ret = header_info_parse(ma);
		ma->header_info_seen = true;
		break;
	case INNER_TYPE_INFO:
		ret = type_info_parse(ma);
		ma->type_info_seen = true;
		break;
	default:
		break;
	}
	ma->icur = INNER_NONE;

	return ret;
}

/* Header is complete and verified */
static int header_done(struct mender_artifact *ma)
{
	if (!ma->header_info_seen) {
		ERROR("Mender Artifact: header without header-info");
		return -EINVAL;
	}
	if (strcmp(ma->payload_type, "null") && !ma->type_info_seen) {
		ERROR("Mender Artifact: header without type-info");
		return -EINVAL;
	}
	if (ma->cfg.payload_type && strcmp(ma->cfg.payload_type, ma->payload_type)) {
		ERROR("Mender Artifact: payload type is %s, expected %s",
		      ma->payload_type, ma->cfg.payload_type);
		return -ENOEXEC;
	}
	if (ma->cfg.device_type && !device_compatible(ma, ma->cfg.device_type)) {
		ERROR("Mender Artifact %s is not compatible with device type %s",
		      ma->artifact_name, ma->cfg.device_type);
		return -ENOEXEC;
	}

	return 0;
}

/* Payload archive */
static int payload_begin(void *priv, const char *name, char type, uint64_t size)
{
	struct mender_artifact *ma = priv;

	ma->icur = INNER_IGNORE;
	if (type == '5')
		return 0;
	if (type != '0' || strchr(name, '/')) {
		ERROR("Mender Artifact: unexpected entry %s in payload", name);
		return -EINVAL;
	}
	if (!strcmp(ma->payload_type, "null")) {
		ERROR("Mender Artifact: empty payload contains %s", name);
		return -EINVAL;
	}
	if (ma->payload_count) {
		ERROR("Mender Artifact: more than one file in payload is not supported");
		return -ENOTSUP;
	}

	snprintf(ma->iname, sizeof(ma->iname), "data/0000/%s", name);
	/* refuse it before passing any byte on */
	if (!manifest_find(ma, ma->iname)) {
		ERROR("Mender Artifact: %s is not in the manifest", ma->iname);
		return -EBADMSG;
	}
	ma->payload_name = strdup(name);
	if (!ma->payload_name)
		return -ENOMEM;
	ma->payload_size = size;
	ma->payload_done = 0;
	ma->payload_count++;
	ma->tail_cap = (size_t)min(size, (uint64_t)PAYLOAD_TAIL);
	ma->tail_len = 0;
	if (ma->tail_cap) {
		ma->tail = malloc(ma->tail_cap);
		if (!ma->tail)
			return -ENOMEM;
	}
	ma->idgst = hash_start();
	if (!ma->idgst)
		return -EFAULT;
	ma->icur = INNER_PAYLOAD;

	return 0;
}

/*
 * The end of the payload is held back until the whole artifact is
 * verified, see mender_artifact_finish(): a payload or an artifact that
 * does not match never reaches the caller complete, and an installer
 * reading it fails.
 */
static int payload_data(void *priv, const uint8_t *buf, size_t len)
{
	struct mender_artifact *ma = priv;
	uint64_t held_from = ma->payload_size - ma->tail_cap;
	size_t direct = 0;
	int ret;

	if (ma->icur != INNER_PAYLOAD)
		return 0;
	if (swupdate_HASH_update(ma->idgst, buf, len))
		return -EFAULT;

	if (ma->payload_done < held_from)
		direct = (size_t)min((uint64_t)len, held_from - ma->payload_done);
	if (direct && ma->cfg.payload) {
		ret = ma->cfg.payload(ma->cfg.priv, buf, direct);
		if (ret)
			return ret;
	}
	memcpy(ma->tail + ma->tail_len, buf + direct, len - direct);
	ma->tail_len += len - direct;
	ma->payload_done += len;

	return 0;
}

static int payload_end(void *priv)
{
	struct mender_artifact *ma = priv;
	unsigned char digest[SHA256_HASH_LENGTH];
	int ret;

	if (ma->icur != INNER_PAYLOAD)
		return 0;
	ma->icur = INNER_NONE;
	ret = hash_end(&ma->idgst, digest);
	if (ret)
		return ret;

	return manifest_check(ma, ma->iname, digest);
}

/* Outer archive */
static int outer_begin(void *priv, const char *name, char type, uint64_t size)
{
	struct mender_artifact *ma = priv;
	enum comp comp = COMP_NONE;
	int ret;

	(void)size;
	strlcpy(ma->curname, name, sizeof(ma->curname));

	if (type == '5') {
		if (strcmp(name, "data") && strcmp(name, "data/")) {
			ERROR("Mender Artifact: unexpected directory %s", name);
			return -EINVAL;
		}
		ma->cur = OUTER_DIR;
		return 0;
	}
	if (type != '0') {
		ERROR("Mender Artifact: unexpected entry %s", name);
		return -EINVAL;
	}

	if (!strcmp(name, "version")) {
		if (ma->stage != STAGE_VERSION)
			goto order;
		ma->cur = OUTER_VERSION;
		dbuf_reset(&ma->collect, VERSION_MAX);
	} else if (!strcmp(name, "manifest")) {
		if (ma->stage != STAGE_MANIFEST)
			goto order;
		ma->cur = OUTER_MANIFEST;
		dbuf_reset(&ma->collect, MANIFEST_MAX);
		return 0;
	} else if (!strcmp(name, "manifest.sig")) {
		if (ma->stage != STAGE_HEADER)
			goto order;
		INFO("Mender Artifact: manifest.sig is not verified");
		ma->cur = OUTER_SIGNATURE;
		return 0;
	} else if (!strcmp(name, "manifest-augment") ||
		   match_tar_name(name, "header-augment", &comp) != -ENOENT) {
		ERROR("Mender Artifact: augmented artifacts are not supported");
		return -ENOTSUP;
	} else if ((ret = match_tar_name(name, "header", &comp)) != -ENOENT) {
		if (ret)
			goto nocomp;
		if (ma->stage != STAGE_HEADER)
			goto order;
		ma->cur = OUTER_HEADER;
		tar_init(&ma->inner, header_begin, header_data, header_end, ma);
		ret = decomp_init(&ma->dc, comp);
		if (ret)
			return ret;
	} else if ((ret = match_tar_name(name, "data/0000", &comp)) != -ENOENT) {
		if (ret)
			goto nocomp;
		if (ma->stage != STAGE_DATA)
			goto order;
		ma->cur = OUTER_DATA;
		ma->data_seen = true;
		tar_init(&ma->inner, payload_begin, payload_data, payload_end, ma);
		ret = decomp_init(&ma->dc, comp);
		if (ret)
			return ret;
		/* the data file itself is not in the manifest, its content is */
		return 0;
	} else if (!strncmp(name, "data/", 5)) {
		ERROR("Mender Artifact: %s: more than one payload is not supported",
		      name);
		return -ENOTSUP;
	} else {
		ERROR("Mender Artifact: unexpected entry %s", name);
		return -EINVAL;
	}

	ma->dgst = hash_start();
	if (!ma->dgst)
		return -EFAULT;

	return 0;

nocomp:
	ERROR("Mender Artifact: %s: compression not supported by this build", name);
	return -ENOTSUP;
order:
	ERROR("Mender Artifact: %s is not in the expected place", name);
	return -EINVAL;
}

static int outer_data(void *priv, const uint8_t *buf, size_t len)
{
	struct mender_artifact *ma = priv;
	int ret;

	if (ma->dgst && swupdate_HASH_update(ma->dgst, buf, len))
		return -EFAULT;

	switch (ma->cur) {
	case OUTER_VERSION:
	case OUTER_MANIFEST:
		return dbuf_add(&ma->collect, buf, len);
	case OUTER_HEADER:
	case OUTER_DATA:
		ret = decomp_feed(&ma->dc, buf, len, tar_feed_cb, &ma->inner);
		if (ret == -EINVAL)
			ERROR("Mender Artifact: %s is corrupted", ma->curname);
		return ret;
	default:
		return 0;
	}
}

static int inner_finish(struct mender_artifact *ma)
{
	int ret = decomp_finish(&ma->dc, tar_feed_cb, &ma->inner);

	if (!ret)
		ret = tar_finish(&ma->inner);
	if (ret == -EINVAL)
		ERROR("Mender Artifact: %s is truncated or corrupted", ma->curname);

	return ret;
}

static int outer_end(void *priv)
{
	struct mender_artifact *ma = priv;
	unsigned char digest[SHA256_HASH_LENGTH];
	enum outer_entry cur = ma->cur;
	int ret = 0;

	ma->cur = OUTER_NONE;
	if (ma->dgst) {
		ret = hash_end(&ma->dgst, digest);
		if (ret)
			return ret;
	}

	switch (cur) {
	case OUTER_VERSION:
		memcpy(ma->version_digest, digest, sizeof(digest));
		ret = version_parse(ma);
		ma->stage = STAGE_MANIFEST;
		break;
	case OUTER_MANIFEST:
		ret = manifest_parse(ma);
		if (ret) {
			if (ret == -EINVAL)
				ERROR("Mender Artifact: invalid manifest");
			break;
		}
		ret = manifest_check(ma, "version", ma->version_digest);
		ma->stage = STAGE_HEADER;
		break;
	case OUTER_HEADER:
		ret = inner_finish(ma);
		if (!ret)
			ret = manifest_check(ma, ma->curname, digest);
		if (!ret)
			ret = header_done(ma);
		ma->stage = STAGE_DATA;
		break;
	case OUTER_DATA:
		ret = inner_finish(ma);
		ma->stage = STAGE_DONE;
		break;
	default:
		break;
	}
	dbuf_reset(&ma->collect, 0);

	return ret;
}

struct mender_artifact *mender_artifact_new(const struct mender_artifact_cfg *cfg)
{
	struct mender_artifact *ma = calloc(1, sizeof(*ma));

	if (!ma)
		return NULL;
	if (cfg)
		ma->cfg = *cfg;
	if ((cfg && cfg->payload_type && !(ma->cfg.payload_type = strdup(cfg->payload_type))) ||
	    (cfg && cfg->device_type && !(ma->cfg.device_type = strdup(cfg->device_type)))) {
		free((char *)ma->cfg.payload_type);
		free(ma);
		return NULL;
	}
	tar_init(&ma->outer, outer_begin, outer_data, outer_end, ma);

	return ma;
}

int mender_artifact_feed(struct mender_artifact *ma, const void *buf, size_t len)
{
	int ret;

	if (!ma)
		return -EINVAL;
	if (ma->err)
		return ma->err;
	if (!len)
		return 0;
	if (!buf)
		return ma->err = -EINVAL;

	ret = tar_feed(&ma->outer, buf, len);
	if (ret)
		ma->err = ret;

	return ret;
}

int mender_artifact_finish(struct mender_artifact *ma)
{
	size_t i;

	if (!ma)
		return -EINVAL;
	if (ma->err)
		return ma->err;

	if (tar_finish(&ma->outer) || ma->stage < STAGE_DATA) {
		ERROR("Mender Artifact: truncated");
		return ma->err = -EINVAL;
	}
	if (strcmp(ma->payload_type, "null") && !ma->payload_count) {
		ERROR("Mender Artifact: no payload");
		return ma->err = -EINVAL;
	}
	for (i = 0; i < ma->manifest_count; i++) {
		if (!ma->manifest[i].seen) {
			ERROR("Mender Artifact: %s is in the manifest but missing",
			      ma->manifest[i].name);
			return ma->err = -EBADMSG;
		}
	}

	/* everything verified, pass on the end of the payload */
	if (ma->tail_len && ma->cfg.payload) {
		int ret = ma->cfg.payload(ma->cfg.priv, ma->tail, ma->tail_len);

		if (ret)
			return ma->err = ret;
	}
	ma->tail_len = 0;

	return 0;
}

void mender_artifact_free(struct mender_artifact *ma)
{
	size_t i;

	if (!ma)
		return;
	hash_drop(&ma->dgst);
	hash_drop(&ma->idgst);
	decomp_end_stream(&ma->dc);
	free(ma->dc.buf);
	tar_free(&ma->outer);
	tar_free(&ma->inner);
	dbuf_reset(&ma->collect, 0);
	dbuf_reset(&ma->icollect, 0);
	for (i = 0; i < ma->manifest_count; i++)
		free(ma->manifest[i].name);
	free(ma->manifest);
	json_object_put(ma->header_info);
	free(ma->payload_name);
	free(ma->tail);
	free((char *)ma->cfg.payload_type);
	free((char *)ma->cfg.device_type);
	free(ma);
}

const char *mender_artifact_name(const struct mender_artifact *ma)
{
	return ma && ma->stage >= STAGE_DATA && !ma->err ? ma->artifact_name : NULL;
}

const char *mender_artifact_payload_type(const struct mender_artifact *ma)
{
	return ma && ma->stage >= STAGE_DATA && !ma->err ? ma->payload_type : NULL;
}

const char *mender_artifact_payload_name(const struct mender_artifact *ma)
{
	return ma ? ma->payload_name : NULL;
}

uint64_t mender_artifact_payload_size(const struct mender_artifact *ma)
{
	return ma ? ma->payload_size : 0;
}
