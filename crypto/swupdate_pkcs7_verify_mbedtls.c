// SPDX-FileCopyrightText: 2026 Bastian Germann
//
// SPDX-License-Identifier: GPL-2.0-only

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "swupdate.h"
#include "swupdate_crypto.h"
#include "swupdate_mbedtls.h"
#include "util.h"

static swupdate_dgst_lib libs;

/* messageDigest OID: 1.2.840.113549.1.9.4 */
#define OID_MSG_DIGEST  MBEDTLS_OID_PKCS9 "\x04"

static void trace_mbedtls_error(const char *label, int error)
{
	char buf[256];

	mbedtls_strerror(error, buf, sizeof(buf));
	ERROR("%s: %s (%d)", label, buf, error);
}

#ifndef CONFIG_CMS_IGNORE_CERTIFICATE_PURPOSE
static int check_cert_purpose(const mbedtls_x509_crt *crt, unsigned int purpose)
{
	int error;

	error = mbedtls_x509_crt_check_key_usage(crt,
			MBEDTLS_X509_KU_DIGITAL_SIGNATURE);
	if (error) {
		trace_mbedtls_error("signer certificate key usage check failed", error);
		return error;
	}

	switch (purpose) {
	case CERT_PURPOSE_CODE_SIGN:
		error = mbedtls_x509_crt_check_extended_key_usage(crt,
				MBEDTLS_OID_CODE_SIGNING,
				MBEDTLS_OID_SIZE(MBEDTLS_OID_CODE_SIGNING));
		break;
	case CERT_PURPOSE_EMAIL_PROT:
	default:
		error = mbedtls_x509_crt_check_extended_key_usage(crt,
				MBEDTLS_OID_EMAIL_PROTECTION,
				MBEDTLS_OID_SIZE(MBEDTLS_OID_EMAIL_PROTECTION));
		break;
	}

	if (error) {
		trace_mbedtls_error("signer certificate extended key usage check failed",
			error);
		return error;
	}

	return 0;
}
#endif

static int certificate_verify_callback(void *ctx, mbedtls_x509_crt *crt,
		int depth, uint32_t *flags)
{
	(void)ctx;
	(void)crt;
	(void)depth;
	(void)flags;

#if defined(CONFIG_CMS_IGNORE_EXPIRED_CERTIFICATE)
	*flags &= ~(MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE);
#endif

	return 0;
}

static int verify_signer_cert(const struct mbedtls_digest *dgst,
		const mbedtls_x509_crt *crt, const char *signer_name)
{
	uint32_t flags = 0;
	int error;
	char info[NOTIFY_BUF_SIZE];

	if (!crt || !crt->raw.p) {
		return -EINVAL;
	}

	if (signer_name != NULL && signer_name[0] == '\0')
		signer_name = NULL;

	error = mbedtls_x509_crt_verify((mbedtls_x509_crt *)crt,
			(mbedtls_x509_crt *)&dgst->trusted_certs, NULL, signer_name, &flags,
			certificate_verify_callback, NULL);
	if (error) {
		mbedtls_x509_crt_verify_info(info, sizeof(info), "", flags);
		ERROR("signer certificate verification failed: %s", info);
		trace_mbedtls_error("mbedtls_x509_crt_verify", error);
	}

	return error;
}

/*
 * Parse a DER AlgorithmIdentifier SEQUENCE, return the MD type.
 * Advances *p past the entire SEQUENCE on both success and failure.
 */
static int pkcs7_get_md_alg(unsigned char **p, const unsigned char *end,
		mbedtls_md_type_t *md_type)
{
	int ret;
	size_t len;
	mbedtls_asn1_buf oid;

	if ((ret = mbedtls_asn1_get_tag(p, end, &len,
				MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE)) != 0)
		return ret;
	const unsigned char *seq_end = *p + len;

	oid.tag = MBEDTLS_ASN1_OID;
	if ((ret = mbedtls_asn1_get_tag(p, seq_end, &oid.len,
				MBEDTLS_ASN1_OID)) != 0) {
		*p = (unsigned char *)seq_end;
		return ret;
	}
	oid.p = *p;
	ret = mbedtls_oid_get_md_alg(&oid, md_type);
	*p = (unsigned char *)seq_end;
	return ret;
}

/*
 * Find an attribute value by OID inside a raw DER SET OF Attribute block
 * (the value region of a signedAttrs or unsignedAttrs field).
 * Sets *val_p / *val_len to the content bytes of the first matching SET value.
 */
static int pkcs7_find_attr(const unsigned char *attrs_p, size_t attrs_len,
		const char *oid_bytes, size_t oid_bytes_len,
		const unsigned char **val_p, size_t *val_len)
{
	unsigned char *p = (unsigned char *)attrs_p;
	const unsigned char *end = attrs_p + attrs_len;
	int ret;
	size_t seq_len;

	while (p < end) {
		if ((ret = mbedtls_asn1_get_tag(&p, end, &seq_len,
					MBEDTLS_ASN1_CONSTRUCTED |
					MBEDTLS_ASN1_SEQUENCE)) != 0)
			return ret;
		const unsigned char *attr_end = p + seq_len;

		mbedtls_asn1_buf oid;
		oid.tag = MBEDTLS_ASN1_OID;
		if ((ret = mbedtls_asn1_get_tag(&p, attr_end, &oid.len,
					MBEDTLS_ASN1_OID)) != 0)
			return ret;
		oid.p = p;
		p += oid.len;

		if (oid.len == oid_bytes_len &&
				memcmp(oid.p, oid_bytes, oid_bytes_len) == 0) {
			size_t set_len;
			if ((ret = mbedtls_asn1_get_tag(&p, attr_end, &set_len,
						MBEDTLS_ASN1_CONSTRUCTED |
						MBEDTLS_ASN1_SET)) != 0)
				return ret;
			*val_p = p;
			*val_len = set_len;
			return 0;
		}
		p = (unsigned char *)attr_end;
	}
	return MBEDTLS_ERR_ASN1_UNEXPECTED_TAG;
}

/*
 * Verify CMS SignedData with authenticated attributes (signedAttrs).
 *
 * The mbedTLS PKCS#7 parser does not support signedAttrs.  This function
 * implements RFC 5652, 5.4 verification manually using the mbedTLS ASN.1
 * and PK primitives:
 *
 *   1. Walk ContentInfo -> SignedData -> signerInfos.
 *   2. For each SignerInfo that contains signedAttrs (tag 0xA0):
 *      a. Find the matching signer certificate by issuer + serial.
 *      b. Verify the certificate chain against the trust store.
 *      c. Check certificate purpose and CN (honouring Kconfig options).
 *      d. Compute hash(content) and compare with the messageDigest attribute.
 *      e. Re-encode signedAttrs with UNIVERSAL SET tag 0x31 (patched in-place
 *         since sigbuf is our private mutable copy), hash the result, and
 *         verify the signature over that hash with the signer's public key.
 *
 * Returns 0 when at least one SignerInfo verifies successfully.
 */
static int pkcs7_verify_with_signed_attrs(unsigned char *sigbuf, size_t sigbuf_len,
		const unsigned char *content, size_t content_len,
		const struct mbedtls_digest *dgst,
		const char *signer_name)
{
	unsigned char *p = sigbuf;
	unsigned char *end = sigbuf + sigbuf_len;
	int ret;
	size_t len;
	int any_verified = 0;

	/* ContentInfo SEQUENCE */
	if (mbedtls_asn1_get_tag(&p, end, &len,
				MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) != 0)
		return -EINVAL;
	end = p + len;

	/* contentType OID (skip) */
	if (mbedtls_asn1_get_tag(&p, end, &len, MBEDTLS_ASN1_OID) != 0)
		return -EINVAL;
	p += len;

	/* [0] EXPLICIT SignedData */
	if (mbedtls_asn1_get_tag(&p, end, &len,
				MBEDTLS_ASN1_CONTEXT_SPECIFIC |
				MBEDTLS_ASN1_CONSTRUCTED) != 0)
		return -EINVAL;
	end = p + len;

	/* SignedData SEQUENCE */
	if (mbedtls_asn1_get_tag(&p, end, &len,
				MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) != 0)
		return -EINVAL;
	end = p + len;

	/* version INTEGER (skip) */
	int ver;
	if (mbedtls_asn1_get_int(&p, end, &ver) != 0)
		return -EINVAL;

	/* digestAlgorithms SET (skip) */
	if (mbedtls_asn1_get_tag(&p, end, &len,
				MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SET) != 0)
		return -EINVAL;
	p += len;

	/* encapContentInfo SEQUENCE (skip; detached signature) */
	if (mbedtls_asn1_get_tag(&p, end, &len,
				MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) != 0)
		return -EINVAL;
	p += len;

	/* [0] IMPLICIT certificates (tag 0xA0, optional) */
	mbedtls_x509_crt embedded;
	mbedtls_x509_crt_init(&embedded);
	if (p < end &&
			*p == (MBEDTLS_ASN1_CONTEXT_SPECIFIC | MBEDTLS_ASN1_CONSTRUCTED)) {
		unsigned char *cp = p;
		cp++;
		size_t certs_len;
		if (mbedtls_asn1_get_len(&cp, end, &certs_len) == 0) {
			const unsigned char *certs_end = cp + certs_len;
			while (cp < certs_end) {
				const unsigned char *cert_hdr = cp;
				cp++;   /* tag byte */
				size_t body_len;
				unsigned char *body = cp;
				if (mbedtls_asn1_get_len(&body, certs_end,
							&body_len) == 0) {
					size_t full = (cert_hdr == body) ?
						body_len :
						(size_t)(body - cert_hdr) + body_len;
					mbedtls_x509_crt_parse_der(&embedded,
							cert_hdr, full);
					cp = body + body_len;
				} else {
					break;
				}
			}
			p = (unsigned char *)certs_end;
		}
	}

	/* [1] IMPLICIT crls (tag 0xA1, optional, skip) */
	if (p < end &&
			*p == (MBEDTLS_ASN1_CONTEXT_SPECIFIC |
				MBEDTLS_ASN1_CONSTRUCTED | 0x01)) {
		p++;
		if (mbedtls_asn1_get_len(&p, end, &len) == 0)
			p += len;
	}

	/* signerInfos SET */
	if (mbedtls_asn1_get_tag(&p, end, &len,
				MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SET) != 0) {
		mbedtls_x509_crt_free(&embedded);
		return -EINVAL;
	}
	const unsigned char *signers_end = p + len;

	while (p < signers_end && !any_verified) {
		/* SignerInfo SEQUENCE */
		size_t si_len;
		if (mbedtls_asn1_get_tag(&p, signers_end, &si_len,
					MBEDTLS_ASN1_CONSTRUCTED |
					MBEDTLS_ASN1_SEQUENCE) != 0)
			break;
		const unsigned char *si_end = p + si_len;

		/* version (skip) */
		int si_ver;
		if (mbedtls_asn1_get_int(&p, si_end, &si_ver) != 0) {
			p = (unsigned char *)si_end; continue;
		}

		/* sid -- only IssuerAndSerialNumber (version 1) */
		if (*p != (MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE)) {
			p = (unsigned char *)si_end; continue;
		}
		mbedtls_x509_buf si_issuer = { 0, 0, NULL };
		mbedtls_x509_buf si_serial = { 0, 0, NULL };
		{
			size_t sid_len;
			if (mbedtls_asn1_get_tag(&p, si_end, &sid_len,
						MBEDTLS_ASN1_CONSTRUCTED |
						MBEDTLS_ASN1_SEQUENCE) != 0) {
				p = (unsigned char *)si_end; continue;
			}
			const unsigned char *sid_end = p + sid_len;

			/* issuer Name SEQUENCE -- capture full TLV bytes */
			si_issuer.p = p;
			si_issuer.tag = *p;
			size_t iss_len;
			if (mbedtls_asn1_get_tag(&p, sid_end, &iss_len,
						MBEDTLS_ASN1_CONSTRUCTED |
						MBEDTLS_ASN1_SEQUENCE) != 0) {
				p = (unsigned char *)si_end; continue;
			}
			si_issuer.len = (size_t)(p + iss_len - si_issuer.p);
			p += iss_len;

			/* serial INTEGER -- capture value bytes only */
			size_t ser_len;
			if (mbedtls_asn1_get_tag(&p, sid_end, &ser_len,
						MBEDTLS_ASN1_INTEGER) != 0) {
				p = (unsigned char *)si_end; continue;
			}
			si_serial.tag = MBEDTLS_ASN1_INTEGER;
			si_serial.p = p;
			si_serial.len = ser_len;
			p = (unsigned char *)sid_end;
		}

		/* digestAlgorithm */
		mbedtls_md_type_t md_type = MBEDTLS_MD_NONE;
		if (pkcs7_get_md_alg(&p, si_end, &md_type) != 0 ||
				md_type == MBEDTLS_MD_NONE) {
			p = (unsigned char *)si_end; continue;
		}

		/* signedAttrs [0] IMPLICIT (tag 0xA0, required here) */
		unsigned char *sa_tag_ptr = NULL;
		const unsigned char *sa_value = NULL;
		size_t sa_value_len = 0;
		if (p < si_end &&
				*p == (MBEDTLS_ASN1_CONTEXT_SPECIFIC |
					MBEDTLS_ASN1_CONSTRUCTED)) {
			sa_tag_ptr = p;
			p++;
			if (mbedtls_asn1_get_len(&p, si_end,
						&sa_value_len) != 0) {
				p = (unsigned char *)si_end; continue;
			}
			sa_value = p;
			p += sa_value_len;
		}

		/* signatureAlgorithm SEQUENCE (skip) */
		{
			size_t sa_seq_len;
			if (mbedtls_asn1_get_tag(&p, si_end, &sa_seq_len,
						MBEDTLS_ASN1_CONSTRUCTED |
						MBEDTLS_ASN1_SEQUENCE) != 0) {
				p = (unsigned char *)si_end; continue;
			}
			p += sa_seq_len;
		}

		/* signature OCTET STRING */
		const unsigned char *sig_p;
		size_t sig_len;
		if (mbedtls_asn1_get_tag(&p, si_end, &sig_len,
					MBEDTLS_ASN1_OCTET_STRING) != 0) {
			p = (unsigned char *)si_end; continue;
		}
		sig_p = p;
		p += sig_len;

		/* Only handle the signedAttrs path */
		if (!sa_tag_ptr || !sa_value) {
			p = (unsigned char *)si_end; continue;
		}

		/* Find matching certificate (embedded first, then trust store) */
		const mbedtls_x509_crt *signer_cert = NULL;
		const mbedtls_x509_crt *chains[2] = {
			&embedded, &dgst->trusted_certs
		};
		for (int ci = 0; ci < 2 && !signer_cert; ci++) {
			const mbedtls_x509_crt *c;
			for (c = chains[ci]; c && c->raw.p; c = c->next) {
				if (c->serial.len == si_serial.len &&
						memcmp(c->serial.p, si_serial.p,
							si_serial.len) == 0 &&
						c->issuer_raw.len == si_issuer.len &&
						memcmp(c->issuer_raw.p, si_issuer.p,
							si_issuer.len) == 0) {
					signer_cert = c;
					break;
				}
			}
		}
		if (!signer_cert) {
			TRACE("no cert matching SignerInfo issuer/serial");
			p = (unsigned char *)si_end; continue;
		}

		/* Trust-chain, purpose, CN */
		if (verify_signer_cert(dgst, signer_cert, signer_name) != 0) {
			p = (unsigned char *)si_end; continue;
		}
#ifndef CONFIG_CMS_IGNORE_CERTIFICATE_PURPOSE
		if (check_cert_purpose(signer_cert, dgst->cert_purpose) != 0) {
			p = (unsigned char *)si_end; continue;
		}
#endif

		/* Cryptographic verification */
		const mbedtls_md_info_t *md_info =
			mbedtls_md_info_from_type(md_type);
		if (!md_info) {
			p = (unsigned char *)si_end; continue;
		}
		size_t hash_len = mbedtls_md_get_size(md_info);

		/* Step 1: Hash detached content */
		unsigned char content_hash[MBEDTLS_MD_MAX_SIZE];
		if (mbedtls_md(md_info, content, content_len, content_hash) != 0) {
			p = (unsigned char *)si_end; continue;
		}

		/* Step 2: messageDigest attribute must equal hash(content) */
		const unsigned char *md_attr_val = NULL;
		size_t md_attr_len = 0;
		if (pkcs7_find_attr(sa_value, sa_value_len,
					OID_MSG_DIGEST,
					sizeof(OID_MSG_DIGEST) - 1,
					&md_attr_val, &md_attr_len) != 0) {
			ERROR("messageDigest attribute not found in signedAttrs");
			p = (unsigned char *)si_end; continue;
		}
		{
			unsigned char *md_p = (unsigned char *)md_attr_val;
			const unsigned char *md_end_p = md_attr_val + md_attr_len;
			size_t md_os_len;
			if (mbedtls_asn1_get_tag(&md_p, md_end_p, &md_os_len,
						MBEDTLS_ASN1_OCTET_STRING) != 0 ||
					md_os_len != hash_len ||
					memcmp(md_p, content_hash, hash_len) != 0) {
				ERROR("messageDigest does not match content hash");
				p = (unsigned char *)si_end; continue;
			}
		}

		/*
		 * Step 3: Re-encode signedAttrs with UNIVERSAL SET tag 0x31
		 * (RFC 5652, 5.4: the signature is computed over the full DER
		 *  encoding of the signedAttrs SET, not the implicit [0] form).
		 * sigbuf is our private mutable copy so we patch in-place.
		 */
		size_t sa_hdr_len = (size_t)(sa_value - sa_tag_ptr);
		size_t sa_total_len = sa_hdr_len + sa_value_len;
		unsigned char saved_tag = *sa_tag_ptr;
		*sa_tag_ptr = MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SET;
		unsigned char sa_hash[MBEDTLS_MD_MAX_SIZE];
		ret = mbedtls_md(md_info, sa_tag_ptr, sa_total_len, sa_hash);
		*sa_tag_ptr = saved_tag;
		if (ret != 0) {
			p = (unsigned char *)si_end; continue;
		}

		/* Step 4: Verify signature */
		ret = mbedtls_pk_verify(
				(mbedtls_pk_context *)&signer_cert->pk,
				md_type, sa_hash, hash_len,
				sig_p, sig_len);
		if (ret != 0) {
			trace_mbedtls_error("Signature verification", ret);
			p = (unsigned char *)si_end; continue;
		}

		any_verified = 1;
	}

	mbedtls_x509_crt_free(&embedded);
	return any_verified ? 0 : -EBADMSG;
}

static int pkcs7_verify_with_cert(struct mbedtls_digest *dgst, mbedtls_pkcs7 *pkcs7,
		const mbedtls_x509_crt *crt, const unsigned char *content,
		size_t content_len, const char *signer_name, unsigned int purpose)
{
	int error;

	if (!crt || !crt->raw.p) {
		return -EINVAL;
	}

	error = verify_signer_cert(dgst, crt, signer_name);
	if (error) {
		return error;
	}

#ifndef CONFIG_CMS_IGNORE_CERTIFICATE_PURPOSE
	error = check_cert_purpose(crt, purpose);
	if (error) {
		return error;
	}
#endif

	error = mbedtls_pkcs7_signed_data_verify(pkcs7, crt, content, content_len);
	if (error) {
		trace_mbedtls_error("mbedtls_pkcs7_signed_data_verify", error);
		return error;
	}

	return 0;
}

static bool signer_info_matches_cert(const mbedtls_pkcs7_signer_info *signer,
		const mbedtls_x509_crt *crt)
{
	const mbedtls_x509_buf *serial;
	const mbedtls_x509_buf *issuer;

	if (!signer || !crt || !crt->raw.p) {
		return false;
	}

	serial = &signer->MBEDTLS_PRIVATE(serial);
	issuer = &signer->MBEDTLS_PRIVATE(issuer_raw);

	return serial->tag == crt->serial.tag &&
		serial->len == crt->serial.len &&
		memcmp(serial->p, crt->serial.p, serial->len) == 0 &&
		issuer->tag == crt->issuer_raw.tag &&
		issuer->len == crt->issuer_raw.len &&
		memcmp(issuer->p, crt->issuer_raw.p, issuer->len) == 0;
}

static bool cert_matches_pkcs7_signer(const mbedtls_pkcs7 *pkcs7,
		const mbedtls_x509_crt *crt)
{
	const mbedtls_pkcs7_signer_info *signer;
	int i;

	if (!pkcs7 || !crt) {
		return false;
	}

	signer = &pkcs7->MBEDTLS_PRIVATE(signed_data).MBEDTLS_PRIVATE(signers);
	for (i = 0; i < pkcs7->MBEDTLS_PRIVATE(signed_data).MBEDTLS_PRIVATE(no_of_signers) && signer;
		     ++i, signer = signer->MBEDTLS_PRIVATE(next)) {
		return signer_info_matches_cert(signer, crt);
	}

	return false;
}

static int mbedtls_pkcs7_dgst_init(struct swupdate_cfg *sw, const char *keyfile)
{
	struct mbedtls_digest *dgst;
	int error;

	if (sw->dgst) {
		return -EBUSY;
	}

	dgst = calloc(1, sizeof(*dgst));
	if (!dgst) {
		return -ENOMEM;
	}

#if defined(MBEDTLS_USE_PSA_CRYPTO)
	if (psa_crypto_init() != PSA_SUCCESS) {
		free(dgst);
		return -EFAULT;
	}
#endif
	mbedtls_x509_crt_init(&dgst->trusted_certs);
	dgst->cert_purpose = sw->cert_purpose;
	error = mbedtls_x509_crt_parse_file(&dgst->trusted_certs, keyfile);
	if (error != 0) {
		if (error < 0)
			trace_mbedtls_error("mbedtls_x509_crt_parse_file", error);
		else
			ERROR("mbedtls_x509_crt_parse_file: %d certificate(s) failed to parse",
				error);
		mbedtls_x509_crt_free(&dgst->trusted_certs);
		free(dgst);
		return -EINVAL;
	}

	sw->dgst = dgst;
	return 0;
}

static int mbedtls_pkcs7_verify_file(void *ctx, const char *sigfile,
		const char *file, const char *signer_name)
{
	struct mbedtls_digest *dgst = (struct mbedtls_digest *)ctx;
	mbedtls_pkcs7 pkcs7;
	unsigned char *sigbuf = NULL;
	unsigned char *content = NULL;
	size_t sigbuf_len = 0;
	size_t content_len = 0;
	int error;
	int status = -EFAULT;
	const mbedtls_x509_crt *crt;
	unsigned int purpose;

	if (!dgst) {
		return -EINVAL;
	}
	purpose = dgst->cert_purpose;

	mbedtls_pkcs7_init(&pkcs7);

	error = read_file_into_buf(sigfile, &sigbuf, &sigbuf_len);
	if (error) {
		status = error;
		goto out;
	}

	int parse_error = mbedtls_pkcs7_parse_der(&pkcs7, sigbuf, sigbuf_len);
	if (parse_error == MBEDTLS_ERR_PKCS7_INVALID_SIGNER_INFO) {
		/*
		 * The mbedTLS PKCS#7 parser does not support signedAttrs.
		 * Read the content file and verify manually.
		 */
		error = read_file_into_buf(file, &content, &content_len);
		if (error) {
			status = error;
			goto out;
		}
		TRACE("SignerInfo contains authenticatedAttributes; "
			"using manual verification path");
		error = pkcs7_verify_with_signed_attrs(
				sigbuf, sigbuf_len,
				content, content_len,
				dgst, signer_name);
		if (error == 0) {
			TRACE("Verified OK");
			status = 0;
		} else {
			ERROR("Signature verification failed");
			status = -EBADMSG;
		}
		goto out;
	}
	else if (parse_error < 0) {
		ERROR("%s cannot be parsed as DER-encoded PKCS#7 signature blob", sigfile);
		trace_mbedtls_error("mbedtls_pkcs7_parse_der", parse_error);
		status = -EFAULT;
		goto out;
	}
	else if (parse_error != MBEDTLS_PKCS7_SIGNED_DATA) {
		ERROR("%s is not a detached PKCS#7 signed-data blob", sigfile);
		status = -EBADMSG;
		goto out;
	}

	if (pkcs7.MBEDTLS_PRIVATE(signed_data).MBEDTLS_PRIVATE(no_of_signers) <= 0) {
		ERROR("PKCS#7 blob does not contain signer information");
		status = -EBADMSG;
		goto out;
	}

	/*
	 * No signedAttributes available.
	 * The signature was built, e.g., with openssl cms -noattr.
	 */

	error = read_file_into_buf(file, &content, &content_len);
	if (error) {
		status = error;
		goto out;
	}

	for (crt = pkcs7.MBEDTLS_PRIVATE(signed_data).MBEDTLS_PRIVATE(no_of_certs) > 0 ?
			&pkcs7.MBEDTLS_PRIVATE(signed_data).MBEDTLS_PRIVATE(certs) : NULL;
			crt && crt->raw.p; crt = crt->next) {
		if (!cert_matches_pkcs7_signer(&pkcs7, crt))
			continue;
		error = pkcs7_verify_with_cert(dgst, &pkcs7, crt, content,
				content_len, signer_name, purpose);
		if (!error) {
			TRACE("Verified OK");
			status = 0;
			goto out;
		}
	}

	for (crt = &dgst->trusted_certs; crt && crt->raw.p; crt = crt->next) {
		if (!cert_matches_pkcs7_signer(&pkcs7, crt))
			continue;

		error = pkcs7_verify_with_cert(dgst, &pkcs7, crt, content, content_len,
				signer_name, purpose);
		if (!error) {
			TRACE("Verified OK");
			status = 0;
			goto out;
		}
	}

	ERROR("Signature verification failed");
	status = -EBADMSG;

out:
	mbedtls_pkcs7_free(&pkcs7);
	free(content);
	free(sigbuf);
	return status;
}

__attribute__((constructor))
static void mbedtls_pkcs7_dgst(void)
{
	libs.dgst_init = mbedtls_pkcs7_dgst_init;
	libs.verify_file = mbedtls_pkcs7_verify_file;
	(void)register_dgstlib("pkcs#7mbedtls", &libs);
}
