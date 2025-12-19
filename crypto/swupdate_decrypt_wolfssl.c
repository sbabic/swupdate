/*
 * (C) Copyright 2024
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */


#include "swupdate.h"
#include "swupdate_wolfssl.h"

/*
 * Switch to WolfSSL in module
 */
#define NO_INCLUDE_OPENSSL
#define MODNAME	"WolfSSL"

#include "swupdate_decrypt_openssl.c"

