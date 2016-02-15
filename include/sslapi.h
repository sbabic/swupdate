/*
 * (C) Copyright 2016
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc.
 */

#ifndef _SWUPDATE_SSL_H
#define _SWUPDATE_SSL_H

#ifdef CONFIG_SIGNED_IMAGES

#include <openssl/bio.h>
#include <openssl/objects.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

struct swupdate_digest {
	EVP_PKEY *pkey;
	EVP_MD_CTX *ctx;
};

int swupdate_dgst_init(struct swupdate_cfg *sw, const char *keyfile);
void swupdate_dgst_cleanup(struct swupdate_digest *dgst);
struct swupdate_digest *swupdate_HASH_init(void);
int swupdate_HASH_update(struct swupdate_digest *dgst, unsigned char *buf,
				size_t len);
int swupdate_HASH_final(struct swupdate_digest *dgst, unsigned char *md_value,
	       			unsigned int *md_len);
int swupdate_verify_file(struct swupdate_digest *dgst, const char *sigfile,
	       	const char *file);
int swupdate_HASH_compare(unsigned char *hash1, unsigned char *hash2);
#else
#define swupdate_dgst_init(sw, keyfile) ( 0 )
#define swupdate_dgst_cleanup(sw)
#define swupdate_HASH_init(p) ( NULL )
#define swupdate_verify_file(dgst, sigfile, file) ( 0 )
#define swupdate_HASH_update(p, buf, len)
#define swupdate_HASH_final(p, result, len)
#define swupdate_HASH_compare(hash1,hash2)	(0)
#endif

#endif

