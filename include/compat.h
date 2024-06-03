/*
 * Author: Christian Storm
 * Copyright (C) 2018, Siemens AG
 *
 * SPDX-License-Identifier:     LGPL-2.1-or-later
 */

#pragma once

#ifndef strndupa
/*
 * Define char *strndupa(const char *s, size_t n)
 * for, e.g., musl (https://www.musl-libc.org/)
 * which does not bother to implement this function.
 */
#define strndupa(s, n)                          \
	(__extension__({                            \
		const char *__in = (s);                 \
		size_t __len = strnlen(__in, (n)) + 1;  \
		char *__out = (char *)alloca(__len);    \
		__out[__len - 1] = '\0';                \
		(char *)memcpy(__out, __in, __len - 1); \
	}))
#endif

#ifndef strdupa
#define strdupa(s) strndupa(s, strlen(s))
#endif

#if defined(__FreeBSD__)
/*
 * Define ENODATA (61 - No data available) to
 * ENOATTR (87 - Attribute not found) on FreeBSD
 * since that's closest to Linux's ENODATA, and
 * 61 on FreeBSD is ECONNREFUSED.
 */
#define ENODATA ENOATTR

/*
 * Define ENOKEY (required key not available) as
 * on Linux since FreeBSD has no such definition.
 */
#define	ENOKEY 126

/*
 * The BSDs don't define this while Linux does.
 */
#include <sys/types.h>
typedef int8_t   __s8;
typedef uint8_t  __u8;
typedef int16_t  __s16;
typedef uint16_t __u16;
typedef int32_t  __s32;
typedef uint32_t __u32;
typedef int64_t  __s64;
typedef uint64_t __u64;
#endif
