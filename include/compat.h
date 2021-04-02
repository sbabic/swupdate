/*
 * Author: Christian Storm
 * Copyright (C) 2018, Siemens AG
 *
 * SPDX-License-Identifier:     GPL-2.0-only
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
#define __u64 uint64_t
#endif
