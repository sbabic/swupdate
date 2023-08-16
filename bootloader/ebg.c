/*
 * Author: Christian Storm
 * Copyright (C) 2022, Siemens AG
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include <efibootguard/ebgenv.h>
#include "generated/autoconf.h"
#include "util.h"
#include "state.h"
#include "bootloader.h"

static struct {
	void (*beverbose)(ebgenv_t *e, bool v);
	int  (*env_create_new)(ebgenv_t *e);
	int  (*env_open_current)(ebgenv_t *e);
	int  (*env_get)(ebgenv_t *e, char *key, char* buffer);
	int  (*env_set)(ebgenv_t *e, char *key, char *value);
	int  (*env_set_ex)(ebgenv_t *e, char *key, uint64_t datatype,
			   uint8_t *value, uint32_t datalen);
	uint16_t (*env_getglobalstate)(ebgenv_t *e);
	int  (*env_setglobalstate)(ebgenv_t *e, uint16_t ustate);
	int  (*env_close)(ebgenv_t *e);
	int  (*env_finalize_update)(ebgenv_t *e);
} libebg;


/*
 * ----------------------------------------------------------------------------
 * |  Logics, Assumptions & Rationale                                         |
 * ----------------------------------------------------------------------------
 *
 * EFI Boot Guard boots the environment having `EBGENV_IN_PROGRESS == 0`
 * and the highest revision number. If multiple environments have the highest
 * revision number, environment probing order is decisive.
 * This environment is called the *current boot path*.
 * Sorted descending on revision numbers and arbitrated by probing order, the
 * other environments are termed *alternative boot paths*.
 *
 * Environment modifications ― except blessing a successful update ― must not
 * touch the current boot path. Instead, a new  boot path is created by
 * "upcycling" the least recent alternative boot path.
 * More specifically, environment modifications are captured in a *transaction*:
 * An in-memory working copy of the current boot path environment is created
 * which has a by one incremented higher revision than the current boot path.
 * Modifications are performed on this working copy environment.
 * When committing the transaction, i.e., writing it to disk, the new current
 * boot path is persisted and booted next.
 *
 * A transaction is started by setting
 *  `EBGENV_USTATE = STATE_IN_PROGRESS` or
 *  `BOOTVAR_TRANSACTION = STATE_IN_PROGRESS`
 * which is idempotent. Then, `libebgenv` sets
 * - `EBGENV_IN_PROGRESS = 1` and
 * - `EBGENV_REVISION` to the current boot path's revision plus one, and
 * - the transaction `inflight` marker is set to `true`.
 *
 * A transaction is committed when setting `EBGENV_USTATE = STATE_INSTALLED`.
 * Then, `libebgenv` sets
 * - `EBGENV_IN_PROGRESS = 0` and
 * - `EBGENV_USTATE = USTATE_INSTALLED`,
 * - the new current boot path is persisted to disk, and
 * - the transaction `inflight` marker is reset to `false`.
 * With this, the current boot path becomes the most recent alternative boot
 * path serving as rollback boot path if the new current boot path fails to
 * boot. In this case, the failed boot path is marked with
 * - `EBGENV_USTATE = USTATE_FAILED` and
 * - `EBGENV_REVISION = 0`
 * by which the rollback boot path becomes the current boot path (again).
 * If the new current boot path boots successfully, the /current/ boot path
 * needs to be written to acknowledge the successful update via setting
 * `EBGENV_USTATE = USTATE_OK`.
 *
 * Note: The modification of EFI Boot Guard's environment variable
 * `EBGENV_IN_PROGRESS` cannot be disabled as it's hard-wired in EFI
 * Boot Guard with particular semantics.
 * Note: Successive calls to libebgenv's `env_open_current()` after the first
 * invocation are no-ops and select the in-memory working copy's current
 * environment.
 * Note: libebgenv's `env_close()` first *writes* to the current environment and
 * then closes it. There's currently no way to just close it, e.g., for re-
 * loading the environment from disk.
 *
 */


/* EFI Boot Guard hard-coded environment variable names. */
#define EBGENV_IN_PROGRESS (char *)"in_progress"
#define EBGENV_REVISION (char *)"revision"
#define EBGENV_USTATE (char *)"ustate"

static ebgenv_t ebgenv = { 0 };
static bool inflight = false;

static inline bool is(const char *s1, const char *s2)
{
	return strcmp(s1, s2) == 0;
}

static char *_env_get(const char *name)
{
	/*
	 * libebgenv's env_get() is two-staged: The first call yields the
	 * value's size in bytes, the second call, with an accordingly
	 * sized buffer, yields the actual value.
	 */
	int size = libebg.env_get(&ebgenv, (char *)name, NULL);
	if (size <= 0) {
		WARN("Cannot find key %s", name);
		return NULL;
	}

	char *value = malloc(size);
	if (value == NULL) {
		ERROR("Error allocating %d bytes of memory to get '%s'", size, name);
		return NULL;
	}

	int result = libebg.env_get(&ebgenv, (char *)name, value);
	if (result != 0) {
		ERROR("Cannot get %s: %s", name, strerror(-result));
		free(value);
		return NULL;
	}
	/* ensure value is null-terminated (string) */
	if (value[size - 1] != '\0') {
		ERROR("Cannot handle value of key %s", name);
		free(value);
		return NULL;
	}
	return value;
}

/* Note: EFI Boot Guard Environment integers are at most uint32_t. */
static inline uint32_t _env_to_uint32(char *value)
{
	if (!value) {
		return UINT_MAX;
	}
	errno = 0;
	uint32_t result = strtoul(value, NULL, 10);
	free(value);
	return errno != 0 ? UINT_MAX : result;
}

static inline uint8_t ascii_to_uint8(unsigned char value)
{
	return value - '0';
}

static inline unsigned char uint8_to_ascii(uint8_t value)
{
	return value + '0';
}

static char *do_env_get(const char *name)
{
	errno = 0;
	libebg.beverbose(&ebgenv, loglevel > INFOLEVEL ? true : false);

	int result = libebg.env_open_current(&ebgenv);
	if (result != 0) {
		ERROR("Cannot open bootloader environment: %s", strerror(result));
		return NULL;
	}

	if (!inflight && is(name, EBGENV_USTATE)) {
		/*
		 * When not in an in-flight transaction, get the "global significant"
		 * EBGENV_USTATE value:
		 * If rolled-back and successfully booted, there's an alternative
		 * boot path that has
		 *   EBGENV_REVISION == 0 and
		 *   EBGENV_USTATE == STATE_FAILED
		 * which is how EFI Boot Guard encodes a rolled-back condition.
		 * To act on this condition, e.g., report and clear it, STATE_FAILED
		 * as the global significant EBGENV_USTATE value is returned.
		 *
		 * If not rolled-back, the current boot path's EBGENV_USTATE value
		 * is returned.
		 */
		char *value = NULL;
		if (asprintf(&value, "%u", libebg.env_getglobalstate(&ebgenv)) == -1) {
			ERROR("Error allocating memory");
			return NULL;
		}
		return value;
	}

	return _env_get(name);
}

static int create_new_environment(void)
{
	if (inflight) {
		DEBUG("Reusing already created new environment.");
		return 0;
	}
	uint32_t revision = _env_to_uint32(_env_get(EBGENV_REVISION));
	uint8_t in_progress = (uint8_t)_env_to_uint32(_env_get(EBGENV_IN_PROGRESS));
	if ((revision == UINT_MAX) || (in_progress == UINT8_MAX)) {
		ERROR("Cannot get environment revision or in-progress marker");
		return -EIO;
	}
	if (in_progress == 1) {
		return 0;
	}
	int result = libebg.env_create_new(&ebgenv);
	if (result != 0) {
		ERROR("Cannot create new environment revision: %s", strerror(result));
		return -result;
	}
	/*
	 * libebgenv has now set:
	 *   EBG_ENVDATA->in_progress = 1
	 *   EBG_ENVDATA->revision = <current boot path's revision>++
	 */
	uint32_t new_revision = _env_to_uint32(_env_get(EBGENV_REVISION));
	if (new_revision == UINT_MAX) {
		return -EIO;
	}
	if (++revision != new_revision) {
		ERROR("No new environment revision was created!");
		return -ENOENT;
	}
	inflight = true;
	DEBUG("Created new environment revision %d, starting transaction",
	      new_revision);
	return 0;
}

static int do_env_set(const char *name, const char *value)
{
	errno = 0;
	libebg.beverbose(&ebgenv, loglevel > INFOLEVEL ? true : false);

	if (!inflight) {
		/*
		 * Without an in-flight transaction, only allow
		 * (1) starting a transaction or
		 * (2) acknowledging an update.
		 */
		if (!is(name, BOOTVAR_TRANSACTION) && !is(name, EBGENV_USTATE)) {
			ERROR("Not setting %s=%s w/o in-flight transaction",
			      name, value);
			return -EINVAL;
		}
		if (is(name, BOOTVAR_TRANSACTION) &&
		    !is(value, get_state_string(STATE_IN_PROGRESS))) {
			ERROR("Not setting %s=%s w/o in-flight transaction",
			      name, value);
			return -EINVAL;
		}

		if (is(name, EBGENV_USTATE)) {
			switch (*value) {
			case STATE_OK:		/* Acknowledging an update. */
			case STATE_IN_PROGRESS:	/* Starting a transaction.  */
				break;
			default:
				ERROR("Not setting %s=%s w/o in-flight transaction",
				      name, value);
				return -EINVAL;
			}
		}
	}

	int result = libebg.env_open_current(&ebgenv);
	if (result != 0) {
		ERROR("Cannot open bootloader environment: %s", strerror(result));
		return -result;
	}

	if (is(name, BOOTVAR_TRANSACTION)) {
		/*
		 * Note: This gets called by core/stream_interface.c's
		 *       update_transaction_state() with the update
		 *       state's string representation.
		 */
		if (is(value, get_state_string(STATE_IN_PROGRESS))) {
			return create_new_environment();
		}

		if (is(value, get_state_string(STATE_FAILED)) ||
		    is(value, get_state_string(STATE_INSTALLED)) ||
		    is(value, get_state_string(STATE_OK))) {
			/*
			 * Irrespective of the value, set EBGENV_IN_PROGRESS = 0,
			 * else EFI Boot Guard will *NOT* consider this environment
			 * for booting at all.
			 */
			if ((result = libebg.env_set(&ebgenv, EBGENV_IN_PROGRESS,
						     (char *)"0")) != 0) {
				ERROR("Error setting %s=0: %s", EBGENV_IN_PROGRESS,
				      strerror(-result));
				return result;
			}
			return 0;
		}

		/* Fall-through for invalid EBGENV_IN_PROGRESS values. */
		ERROR("Unsupported setting %s=%s", EBGENV_IN_PROGRESS, value);
		return -EINVAL;
	}

	if (!is(name, EBGENV_USTATE)) {
		if ((result = libebg.env_set(&ebgenv, (char *)name, (char *)value)) != 0) {
			ERROR("Error setting %s=%s: %s", name, value, strerror(-result));
			return result;
		}
		return 0;
	}

	switch (*value) {
	case STATE_IN_PROGRESS:
		return create_new_environment();
	case STATE_OK:
		if (inflight) {
			/*
			 * Environment modification within the in-flight transaction,
			 * i.e., the in-memory working copy, just set it.
			 */
			if ((result = libebg.env_set(&ebgenv, EBGENV_USTATE,
						     (char *)value)) != 0) {
				ERROR("Error setting %s=%s: %s", EBGENV_USTATE,
				      get_state_string(STATE_OK),
				      strerror(-result));
				return result;
			}
			return 0;
		}

		unsigned char global_ustate = uint8_to_ascii((uint8_t)
					libebg.env_getglobalstate(&ebgenv));
		if (global_ustate == STATE_NOT_AVAILABLE) {
			ERROR("Cannot read global %s", EBGENV_USTATE);
			return -EIO;
		}
		unsigned char current_ustate = uint8_to_ascii((uint8_t)
						_env_to_uint32(
							_env_get(EBGENV_USTATE)));
		if (!is_valid_state(current_ustate)) {
			ERROR("Cannot read current %s", EBGENV_USTATE);
			return -EIO;
		}

		if (global_ustate == STATE_FAILED) {
			TRACE("Found rolled-back condition, clearing marker");
			/*
			 * Clear rolled-back condition by setting
			 * EBGENV_USTATE = STATE_OK on all alternative
			 * boot paths having EBGENV_USTATE != STATE_OK
			 * and write them to disk.
			 * Note: Does not write to the current boot path (to
			 * which was rolled-back to).
			 */
			if ((result = libebg.env_setglobalstate(&ebgenv,
					ascii_to_uint8(STATE_OK))) != 0) {
				ERROR("Error resetting failure condition: %s",
				      strerror(-result));
				return result;
			}
			/*
			 * Restore prior current boot path's EBGENV_USTATE value
			 * (as there's no way to reload it from disk).
			 * Should be STATE_OK anyway but better play it safe...
			 */
			if (current_ustate != STATE_OK) {
				if ((result = libebg.env_set(&ebgenv, EBGENV_USTATE,
					(char[2]){ current_ustate, '\0' })) != 0) {
					ERROR("Error restoring %s: %s", EBGENV_USTATE,
					      strerror(-result));
					return result;
				}
			}
			return 0;
		}

		if (current_ustate == STATE_TESTING) {
			TRACE("Found successful update, blessing it");
			/*
			 * Acknowledge, update the /current/ boot path on disk.
			 */
			if ((result = libebg.env_set(&ebgenv, EBGENV_USTATE,
						     (char *)value)) != 0) {
				ERROR("Error setting %s=%s: %s", EBGENV_USTATE,
				      value, strerror(-result));
				return result;
			}

			if ((result = libebg.env_close(&ebgenv)) != 0) {
				ERROR("Error persisting environment: %s",
				      strerror(result));
				return -result;
			}
			return 0;
		}

		WARN("Unsupported state for setting %s=%s", EBGENV_USTATE,
		     get_state_string(*value));
		return -EINVAL;
	case STATE_INSTALLED:
		if ((result = libebg.env_finalize_update(&ebgenv)) != 0) {
			ERROR("Error finalizing environment: %s", strerror(result));
			return -result;
		}
		/*
		 * libebgenv has now set:
		 *   EBG_ENVDATA->in_progress = 0
		 *   EBG_ENVDATA->ustate = USTATE_INSTALLED.
		 * Now, persist the in-memory working copy environment as new
		 * current boot path and terminate the transaction.
		 * Note: Does write to an alternative boot path "upcycled" to
		 * the new current boot path; Does *NOT* write to the current
		 * boot path.
		 */
		if ((result = libebg.env_close(&ebgenv)) != 0) {
			ERROR("Error persisting environment: %s", strerror(result));
			return -result;
		}
		inflight = false;
		return 0;
	case STATE_FAILED:
		/*
		 * SWUpdate sets this if the installation has failed. In this case,
		 * the transaction is simply not committed, so nothing to do.
		 */
		return 0;
	default:
		break;
	}

	/*
	 * Fall-through for invalid or EBGENV_USTATE values handled
	 * by EFI Boot Guard internally.
	 */
	WARN("Unsupported setting %s=%s", EBGENV_USTATE, get_state_string(*value));
	return -EINVAL;
}

static int do_env_unset(const char *name)
{
	errno = 0;
	libebg.beverbose(&ebgenv, loglevel > INFOLEVEL ? true : false);

	int result = libebg.env_open_current(&ebgenv);
	if (result != 0) {
		ERROR("Cannot open bootloader environment: %s", strerror(result));
		return -result;
	}

	if (is(name, EBGENV_USTATE)) {
		/*
		 * Unsetting EBGENV_USTATE is semantically equivalent to
		 * setting EBGENV_USTATE = STATE_OK.
		 */
		return do_env_set(EBGENV_USTATE, (char[2]){ STATE_OK, '\0' });
	}

	if (is(name, BOOTVAR_TRANSACTION)) {
		/*
		 * Unsetting BOOTVAR_TRANSACTION is semantically equivalent to
		 * setting EBGENV_IN_PROGRESS = 0.
		 */
		return do_env_set(EBGENV_IN_PROGRESS, (char *)"0");
	}

	if ((result = libebg.env_set_ex(&ebgenv, (char *)name, USERVAR_TYPE_DELETED,
					(uint8_t *)"", 1)) != 0) {
		ERROR("Error unsetting %s: %s", name, strerror(-result));
		return result;
	}
	return 0;
}

static int do_apply_list(const char *filename)
{
	errno = 0;
	libebg.beverbose(&ebgenv, loglevel > INFOLEVEL ? true : false);

	FILE *file = fopen(filename, "rb");
	if (!file) {
		ERROR("Cannot open bootloader environment source file %s: %s",
		      filename, strerror(errno));
		return -EIO;
	}

	char *line = NULL;
	size_t length = 0;
	int result = 0;
	while ((getline(&line, &length, file)) != -1) {
		char *key = strtok(line, "=");
		char *value = strtok(NULL, "\t\n");
		if (key != NULL && value != NULL) {
			if ((result = do_env_set(key, value)) != 0) {
				break;
			}
		}
	}

	fclose(file);
	free(line);
	return result;
}

static bootloader ebg = {
	.env_get = &do_env_get,
	.env_set = &do_env_set,
	.env_unset = &do_env_unset,
	.apply_list = &do_apply_list
};

static bootloader *probe(void)
{
	if (!is(STATE_KEY, EBGENV_USTATE)) {
		/* Note: Logging system is not yet initialized, hence use printf(). */
		printf("[ERROR] : CONFIG_UPDATE_STATE_BOOTLOADER=%s is required "
		       "for EFI Boot Guard support\n", EBGENV_USTATE);
		return NULL;
	}

#if defined(BOOTLOADER_STATIC_LINKED)
	libebg.beverbose = ebg_beverbose;
	libebg.env_create_new  = ebg_env_create_new;
	libebg.env_open_current  = ebg_env_open_current;
	libebg.env_get  = ebg_env_get;
	libebg.env_set  =  ebg_env_set;
	libebg.env_set_ex  = ebg_env_set_ex;
	libebg.env_getglobalstate = ebg_env_getglobalstate;
	libebg.env_setglobalstate  = ebg_env_setglobalstate;
	libebg.env_close  = ebg_env_close;
	libebg.env_finalize_update  = ebg_env_finalize_update;
#else
	void *handle = dlopen("libebgenv.so.0", RTLD_NOW | RTLD_GLOBAL);
	if (!handle) {
		return NULL;
	}

	(void)dlerror();
	load_symbol(handle, &libebg.beverbose, "ebg_beverbose");
	load_symbol(handle, &libebg.env_create_new, "ebg_env_create_new");
	load_symbol(handle, &libebg.env_open_current, "ebg_env_open_current");
	load_symbol(handle, &libebg.env_get, "ebg_env_get");
	load_symbol(handle, &libebg.env_set, "ebg_env_set");
	load_symbol(handle, &libebg.env_set_ex, "ebg_env_set_ex");
	load_symbol(handle, &libebg.env_getglobalstate, "ebg_env_getglobalstate");
	load_symbol(handle, &libebg.env_setglobalstate, "ebg_env_setglobalstate");
	load_symbol(handle, &libebg.env_close, "ebg_env_close");
	load_symbol(handle, &libebg.env_finalize_update, "ebg_env_finalize_update");
#endif
	return &ebg;
}

__attribute__((constructor))
static void ebg_probe(void)
{
	(void)register_bootloader(BOOTLOADER_EBG, probe());
}
