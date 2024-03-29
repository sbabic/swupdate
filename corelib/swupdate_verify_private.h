/*
 * (C) Copyright 2019
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#ifndef _SWUPDATE_VERIFY_H
#define _SWUPDATE_VERIFY_H

struct swupdate_digest;
int dgst_init(struct swupdate_digest *dgst, const EVP_MD *md);

#if defined(CONFIG_SIGALG_RAWRSA) || defined(CONFIG_SIGALG_RSAPSS)
EVP_PKEY *load_pubkey(const char *file);
#endif

#ifdef CONFIG_SIGALG_CMS
#ifndef CONFIG_CMS_IGNORE_CERTIFICATE_PURPOSE
int check_code_sign(const X509_PURPOSE *xp, const X509 *crt, int ca);
#endif
X509_STORE *load_cert_chain(const char *file);
#endif

#endif
