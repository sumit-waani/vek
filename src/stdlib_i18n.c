#include "vek_stdlib.h"
#include "vm.h"
#include "gc.h"

// Global i18n state
static ObjMap* i18n_translations = NULL;  // locale -> (key -> value)
static ObjString* i18n_current_locale = NULL;

static void ensure_i18n_state(void) {
    if (!i18n_translations) {
        i18n_translations = obj_map_new();
        vm_pin((ObjHeader*)i18n_translations);
    }
}

// i18n.load(locale, map) - load translations for a locale
// map is key -> translated_string
static Value native_i18n_load(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_STRING(args[0]) || !IS_MAP(args[1])) return VAL_NIL;
    ObjString* locale = AS_STRING(args[0]);
    ObjMap* translations = AS_MAP(args[1]);

    ensure_i18n_state();

    // Store the translations map for this locale
    obj_map_set(i18n_translations, locale, OBJ_VAL(translations));

    // If no current locale, set this as default
    if (!i18n_current_locale) {
        i18n_current_locale = locale;
        vm_pin((ObjHeader*)i18n_current_locale);
    }

    return VAL_TRUE;
}

// i18n.t(key) or i18n.t(key, locale) - translate a key
static Value native_i18n_t(int arg_count, Value* args) {
    if (arg_count < 1 || !IS_STRING(args[0])) return VAL_NIL;
    ObjString* key = AS_STRING(args[0]);

    ensure_i18n_state();

    // Determine locale
    ObjString* locale = NULL;
    if (arg_count >= 2 && IS_STRING(args[1])) {
        locale = AS_STRING(args[1]);
    } else {
        locale = i18n_current_locale;
    }

    if (!locale) return VAL_NIL;

    // Look up translations for the locale
    Value locale_map_val;
    if (!obj_map_get(i18n_translations, locale, &locale_map_val)) return VAL_NIL;
    if (!IS_MAP(locale_map_val)) return VAL_NIL;
    ObjMap* locale_map = AS_MAP(locale_map_val);

    // Look up key in locale translations
    Value result;
    if (obj_map_get(locale_map, key, &result)) {
        return result;
    }

    return VAL_NIL;
}

// i18n.set_locale(locale) - set the current locale
static Value native_i18n_set_locale(int arg_count, Value* args) {
    (void)arg_count;
    if (!IS_STRING(args[0])) return VAL_NIL;

    if (i18n_current_locale) {
        vm_unpin((ObjHeader*)i18n_current_locale);
    }
    i18n_current_locale = AS_STRING(args[0]);
    vm_pin((ObjHeader*)i18n_current_locale);

    return VAL_TRUE;
}

void stdlib_i18n_init(ObjMap* pkg) {
    stdlib_register(pkg, "load", native_i18n_load, 2);
    stdlib_register(pkg, "t", native_i18n_t, -1);
    stdlib_register(pkg, "set_locale", native_i18n_set_locale, 1);
}
