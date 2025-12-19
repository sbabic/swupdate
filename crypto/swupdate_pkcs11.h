/*
 * (C) Copyright 2025
 * Bastian Germann
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#pragma once

#include <p11-kit/uri.h>
#include <p11-kit/p11-kit.h>

#include "util.h"

struct pkcs11_digest {
	P11KitUri *uri;
	CK_FUNCTION_LIST_PTR module;
	CK_SESSION_HANDLE session;
	CK_MECHANISM mechanism;
	CK_BYTE iv[AES_BLK_SIZE];
	CK_BYTE last[AES_BLK_SIZE + 1];
};
