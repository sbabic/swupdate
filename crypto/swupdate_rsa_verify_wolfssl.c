/*
 * (C) Copyright 2024
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "swupdate.h"
#include "swupdate_wolfssl.h"

/*
 * Switch to WolfSSL in module
 */
#define NO_INCLUDE_OPENSSL
#define MODNAME	"WolfSSLRSA"
#define MODNAME_PSS	"WolfSSLRSAPSS"

#include "swupdate_rsa_verify_openssl.c"

