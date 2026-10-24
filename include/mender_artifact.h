/*
 * (C) Copyright 2026
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Streaming unwrapper for Mender Artifacts (format version 3).
 *
 * A Mender Artifact is a tar archive that carries one payload file
 * (for SWUpdate, the SWU) inside data/0000.tar[.gz|.xz|.zst]. The
 * unwrapper is fed the artifact in chunks of any size, as they arrive
 * from the network, and passes the payload file to a callback without
 * storing the artifact.
 *
 * The checksums from the manifest are verified:
 *  - version and header are verified before any payload byte is passed on.
 *  - the payload is verified when it ends. Its last 64 KiB are held back
 *    and passed on by mender_artifact_finish(), once the whole artifact
 *    is verified, so a payload or an artifact that does not match never
 *    reaches the callback complete.
 *
 * manifest.sig is not verified. Augmented artifacts, more than one
 * payload and Mender state scripts are not supported; state scripts are
 * ignored.
 *
 * Errors (negative errno):
 *  -EINVAL     malformed artifact, or truncated stream at finish
 *  -EBADMSG    checksum mismatch, or file missing from the manifest
 *  -ENOTSUP    unsupported version, compression or feature
 *  -ENOEXEC    payload type or device type does not match
 *  -ENOMEM     out of memory
 * or the error returned by the payload callback.
 * After an error, every further call returns the same error.
 */

struct mender_artifact;

/*
 * Receives the payload file. Return 0 to go on, a negative errno to stop.
 */
typedef int (*mender_payload_fn)(void *priv, const void *buf, size_t len);

struct mender_artifact_cfg {
	const char *payload_type;	/* required payload type, NULL: any */
	const char *device_type;	/* must be in artifact_depends, NULL: no check */
	mender_payload_fn payload;	/* may be NULL */
	void *priv;
};

struct mender_artifact *mender_artifact_new(const struct mender_artifact_cfg *cfg);
int mender_artifact_feed(struct mender_artifact *ma, const void *buf, size_t len);
int mender_artifact_finish(struct mender_artifact *ma);
void mender_artifact_free(struct mender_artifact *ma);

/*
 * Values from the header. They are available once the header has been
 * verified, that is before the first payload byte; NULL before.
 */
const char *mender_artifact_name(const struct mender_artifact *ma);
const char *mender_artifact_payload_type(const struct mender_artifact *ma);
bool mender_artifact_device_compatible(const struct mender_artifact *ma,
				       const char *device_type);

/* Name and size of the payload file, once it has started; NULL / 0 before. */
const char *mender_artifact_payload_name(const struct mender_artifact *ma);
uint64_t mender_artifact_payload_size(const struct mender_artifact *ma);
