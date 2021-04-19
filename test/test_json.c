
/*
 * Author: Christian Storm
 * Copyright (C) 2016, Siemens AG
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdlib.h>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <json-c/json.h>
#include "suricatta/suricatta.h"

#define JSON_OBJECT_FREED 1
#define JSONQUOTE(...) #__VA_ARGS__

extern json_object *json_get_key(json_object *json_root, const char *key);
extern json_object *json_get_path_key(json_object *json_root,
				      const char **json_path);

static int json_setup(void **state)
{
	/* clang-format off */
	const char *json_string = JSONQUOTE(
	{
		"name": "hawkBit",
		"id": 5,
		"artifacts" : {
			"count": 3
		},
		"config" : {
			"polling" : {
				"sleep" : "00:01:00"
			}
		}
	});
	/* clang-format on */
	return (*state = json_tokener_parse(json_string)) == NULL ? 1 : 0;
}

static int json_teardown(void **state)
{
	return (json_object_put(*state) != JSON_OBJECT_FREED) ? 1 : 0;
}

extern json_object *__real_json_get_key(json_object *json_root,
					const char *key);
json_object *__wrap_json_get_key(json_object *json_root, const char *key);
json_object *__wrap_json_get_key(json_object *json_root, const char *key)
{
	return (struct json_object *)__real_json_get_key(json_root, key);
}

extern json_object *__real_json_get_path_key(json_object *json_root,
					     const char **json_path);
json_object *__wrap_json_get_path_key(json_object *json_root,
				      const char **json_path);
json_object *__wrap_json_get_path_key(json_object *json_root,
				      const char **json_path)
{
	return __real_json_get_path_key(json_root, json_path);
}

extern json_bool __real_json_object_object_get_ex(struct json_object *obj,
						  const char *key,
						  struct json_object **value);
json_bool __wrap_json_object_object_get_ex(struct json_object *obj,
					   const char *key,
					   struct json_object **value);
json_bool __wrap_json_object_object_get_ex(struct json_object *obj,
					   const char *key,
					   struct json_object **value)
{
	return __real_json_object_object_get_ex(obj, key, value);
}

static void test_json_get_path_key(void **state)
{
	json_object *json_data = json_get_path_key(
	    *state, (const char *[]){"artifacts", "count", NULL});
	assert_non_null(json_data);
	assert_int_equal(json_object_get_type(json_data), json_type_int);
	assert_int_equal(json_object_get_int(json_data), 3);
}

static void test_json_get_key(void **state)
{
	/* parse it via json_get_key(...) function ... */
	json_object *json_child = NULL;
	json_child = json_get_key(*state, "name");
	assert_non_null(json_child);
	assert_int_equal(json_object_get_type(json_child), json_type_string);
	assert_string_equal(json_object_get_string(json_child), "hawkBit");

	/* ... and check it by manual parsing */
	assert_true(json_object_object_get_ex(*state, "name", &json_child));
	assert_int_equal(json_object_get_type(json_child), json_type_string);
	assert_string_equal(json_object_get_string(json_child), "hawkBit");

	/* check not found keys return NULL */
	assert_null((json_child = json_get_key(*state, "wrongname")));
}

int main(void)
{
	int error_count = 0;
	const struct CMUnitTest json_tests[] = {
	    cmocka_unit_test(test_json_get_key),
	    cmocka_unit_test(test_json_get_path_key),
	};
	error_count += cmocka_run_group_tests_name("json", json_tests,
						   json_setup, json_teardown);
	return error_count;
}
