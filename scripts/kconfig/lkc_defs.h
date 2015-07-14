#include <stdarg.h>

/* confdata.c */
#define conf_parse (*conf_parse_p)
#define conf_read (*conf_read_p)
#define conf_read_simple (*conf_read_simple_p)
#define conf_write_defconfig (*conf_write_defconfig_p)
#define conf_write (*conf_write_p)
#define conf_write_autoconf (*conf_write_autoconf_p)
#define conf_get_changed (*conf_get_changed_p)
#define conf_set_changed_callback (*conf_set_changed_callback_p)
#define conf_set_message_callback (*conf_set_message_callback_p)

/* menu.c */
#define rootmenu (*rootmenu_p)

#define menu_is_visible (*menu_is_visible_p)
#define menu_has_prompt (*menu_has_prompt_p)
#define menu_get_prompt (*menu_get_prompt_p)
#define menu_get_root_menu (*menu_get_root_menu_p)
#define menu_get_parent_menu (*menu_get_parent_menu_p)
#define menu_has_help (*menu_has_help_p)
#define menu_get_help (*menu_get_help_p)
#define get_symbol_str (*get_symbol_str_p)
#define get_relations_str (*get_relations_str_p)
#define menu_get_ext_help (*menu_get_ext_help_p)

/* symbol.c */
#define symbol_hash (*symbol_hash_p)

#define sym_lookup (*sym_lookup_p)
#define sym_find (*sym_find_p)
#define sym_expand_string_value (*sym_expand_string_value_p)
#define sym_re_search (*sym_re_search_p)
#define sym_type_name (*sym_type_name_p)
#define sym_calc_value (*sym_calc_value_p)
#define sym_get_type (*sym_get_type_p)
#define sym_tristate_within_range (*sym_tristate_within_range_p)
#define sym_set_tristate_value (*sym_set_tristate_value_p)
#define sym_toggle_tristate_value (*sym_toggle_tristate_value_p)
#define sym_string_valid (*sym_string_valid_p)
#define sym_string_within_range (*sym_string_within_range_p)
#define sym_set_string_value (*sym_set_string_value_p)
#define sym_is_changable (*sym_is_changable_p)
#define sym_get_choice_prop (*sym_get_choice_prop_p)
#define sym_get_default_prop (*sym_get_default_prop_p)
#define sym_get_string_value (*sym_get_string_value_p)

#define prop_get_type_name (*prop_get_type_name_p)

/* expr.c */
#define expr_compare_type (*expr_compare_type_p)
#define expr_print (*expr_print_p)
