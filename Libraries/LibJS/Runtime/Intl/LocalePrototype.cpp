/*
 * Copyright (c) 2021-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Intl/Locale.h>
#include <LibJS/Runtime/Intl/LocalePrototype.h>
#include <LibUnicode/Locale.h>

namespace JS::Intl {

GC_DEFINE_ALLOCATOR(LocalePrototype);

// 15.3 Properties of the Intl.Locale Prototype Object, https://tc39.es/ecma402/#sec-properties-of-intl-locale-prototype-object
LocalePrototype::LocalePrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().object_prototype())
{
}

void LocalePrototype::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.maximize, maximize, 0, attr);
    define_native_function(realm, vm.names.minimize, minimize, 0, attr);
    define_native_function(realm, vm.names.toString, to_string, 0, attr);
    define_native_function(realm, vm.names.getCalendars, get_calendars, 0, attr);
    define_native_function(realm, vm.names.getCollations, get_collations, 0, attr);
    define_native_function(realm, vm.names.getHourCycles, get_hour_cycles, 0, attr);
    define_native_function(realm, vm.names.getNumberingSystems, get_numbering_systems, 0, attr);
    define_native_function(realm, vm.names.getTimeZones, get_time_zones, 0, attr);
    define_native_function(realm, vm.names.getTextInfo, get_text_info, 0, attr);
    define_native_function(realm, vm.names.getWeekInfo, get_week_info, 0, attr);

    // 15.3.15 Intl.Locale.prototype [ %Symbol.toStringTag% ], https://tc39.es/ecma402/#sec-intl.locale.prototype-%symbol.tostringtag%
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, "Intl.Locale"_string), Attribute::Configurable);

    define_native_accessor(realm, vm.names.baseName, base_name, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.calendar, calendar, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.caseFirst, case_first, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.collation, collation, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.firstDayOfWeek, first_day_of_week, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.hourCycle, hour_cycle, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.language, language, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.numberingSystem, numbering_system, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.numeric, numeric, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.region, region, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.script, script, {}, Attribute::Configurable);
}

// 15.3.2 get Intl.Locale.prototype.baseName, https://tc39.es/ecma402/#sec-Intl.Locale.prototype.baseName
JS_DEFINE_NATIVE_FUNCTION(LocalePrototype::base_name)
{
    // 1. Let loc be the this value.
    // 2. Perform ? RequireInternalSlot(loc, [[InitializedLocale]]).
    auto locale_object = TRY(typed_this_object(vm));

    // 3. Return GetLocaleBaseName(loc.[[Locale]]).
    return PrimitiveString::create(vm, locale_object->locale_id().language_id.to_string());
}

#define JS_ENUMERATE_LOCALE_KEYWORD_PROPERTIES \
    __JS_ENUMERATE(calendar)                   \
    __JS_ENUMERATE(case_first)                 \
    __JS_ENUMERATE(collation)                  \
    __JS_ENUMERATE(first_day_of_week)          \
    __JS_ENUMERATE(hour_cycle)                 \
    __JS_ENUMERATE(numbering_system)

// 15.3.3 get Intl.Locale.prototype.calendar, https://tc39.es/ecma402/#sec-Intl.Locale.prototype.calendar
// 15.3.4 get Intl.Locale.prototype.caseFirst, https://tc39.es/ecma402/#sec-Intl.Locale.prototype.caseFirst
// 15.3.5 get Intl.Locale.prototype.collation, https://tc39.es/ecma402/#sec-Intl.Locale.prototype.collation
// 1.4.1 get Intl.Locale.prototype.firstDayOfWeek, https://tc39.es/proposal-intl-locale-info/#sec-Intl.Locale.prototype.firstDayOfWeek
// 15.3.6 get Intl.Locale.prototype.hourCycle, https://tc39.es/ecma402/#sec-Intl.Locale.prototype.hourCycle
// 15.3.10 get Intl.Locale.prototype.numberingSystem, https://tc39.es/ecma402/#sec-Intl.Locale.prototype.numberingSystem
#define __JS_ENUMERATE(keyword)                                       \
    JS_DEFINE_NATIVE_FUNCTION(LocalePrototype::keyword)               \
    {                                                                 \
        auto locale_object = TRY(typed_this_object(vm));              \
        if (!locale_object->has_##keyword())                          \
            return js_undefined();                                    \
        return PrimitiveString::create(vm, locale_object->keyword()); \
    }
JS_ENUMERATE_LOCALE_KEYWORD_PROPERTIES
#undef __JS_ENUMERATE

// 15.3.7 get Intl.Locale.prototype.language, https://tc39.es/ecma402/#sec-Intl.Locale.prototype.language
JS_DEFINE_NATIVE_FUNCTION(LocalePrototype::language)
{
    // 1. Let loc be the this value.
    // 2. Perform ? RequireInternalSlot(loc, [[InitializedLocale]]).
    auto locale_object = TRY(typed_this_object(vm));

    // 3. Return GetLocaleLanguage(loc.[[Locale]]).
    return PrimitiveString::create(vm, *locale_object->locale_id().language_id.language);
}

// 15.3.8 Intl.Locale.prototype.maximize ( ), https://tc39.es/ecma402/#sec-Intl.Locale.prototype.maximize
JS_DEFINE_NATIVE_FUNCTION(LocalePrototype::maximize)
{
    auto& realm = *vm.current_realm();

    // 1. Let loc be the this value.
    // 2. Perform ? RequireInternalSlot(loc, [[InitializedLocale]]).
    auto locale_object = TRY(typed_this_object(vm));

    // 3. Let maximal be the result of the Add Likely Subtags algorithm applied to loc.[[Locale]]. If an error is signaled, set maximal to loc.[[Locale]].
    auto maximal = Unicode::add_likely_subtags(locale_object->locale()).value_or(locale_object->locale());

    // 4. Return ! Construct(%Intl.Locale%, maximal).
    return Locale::create(realm, locale_object, move(maximal));
}

// 15.3.9 Intl.Locale.prototype.minimize ( ), https://tc39.es/ecma402/#sec-Intl.Locale.prototype.minimize
JS_DEFINE_NATIVE_FUNCTION(LocalePrototype::minimize)
{
    auto& realm = *vm.current_realm();

    // 1. Let loc be the this value.
    // 2. Perform ? RequireInternalSlot(loc, [[InitializedLocale]]).
    auto locale_object = TRY(typed_this_object(vm));

    // 3. Let minimal be the result of the Remove Likely Subtags algorithm applied to loc.[[Locale]]. If an error is signaled, set minimal to loc.[[Locale]].
    auto minimal = Unicode::remove_likely_subtags(locale_object->locale()).value_or(locale_object->locale());

    // 4. Return ! Construct(%Intl.Locale%, minimal).
    return Locale::create(realm, locale_object, move(minimal));
}

// 15.3.11 get Intl.Locale.prototype.numeric, https://tc39.es/ecma402/#sec-Intl.Locale.prototype.numeric
JS_DEFINE_NATIVE_FUNCTION(LocalePrototype::numeric)
{
    // 1. Let loc be the this value.
    // 2. Perform ? RequireInternalSlot(loc, [[InitializedLocale]]).
    auto locale_object = TRY(typed_this_object(vm));

    // 3. Return loc.[[Numeric]].
    return Value { locale_object->numeric() };
}

// 15.3.12 get Intl.Locale.prototype.region, https://tc39.es/ecma402/#sec-Intl.Locale.prototype.region
JS_DEFINE_NATIVE_FUNCTION(LocalePrototype::region)
{
    // 1. Let loc be the this value.
    // 2. Perform ? RequireInternalSlot(loc, [[InitializedLocale]]).
    auto locale_object = TRY(typed_this_object(vm));

    // 3. Return GetLocaleRegion(loc.[[Locale]]).
    if (auto const& region = locale_object->locale_id().language_id.region; region.has_value())
        return PrimitiveString::create(vm, *region);
    return js_undefined();
}

// 15.3.13 get Intl.Locale.prototype.script, https://tc39.es/ecma402/#sec-Intl.Locale.prototype.script
JS_DEFINE_NATIVE_FUNCTION(LocalePrototype::script)
{
    // 1. Let loc be the this value.
    // 2. Perform ? RequireInternalSlot(loc, [[InitializedLocale]]).
    auto locale_object = TRY(typed_this_object(vm));

    // 3. Return GetLocaleScript(loc.[[Locale]]).
    if (auto const& script = locale_object->locale_id().language_id.script; script.has_value())
        return PrimitiveString::create(vm, *script);
    return js_undefined();
}

// 15.3.14 Intl.Locale.prototype.toString ( ), https://tc39.es/ecma402/#sec-Intl.Locale.prototype.toString
JS_DEFINE_NATIVE_FUNCTION(LocalePrototype::to_string)
{
    // 1. Let loc be the this value.
    // 2. Perform ? RequireInternalSlot(loc, [[InitializedLocale]]).
    auto locale_object = TRY(typed_this_object(vm));

    // 3. Return loc.[[Locale]].
    return PrimitiveString::create(vm, locale_object->locale());
}

#define JS_ENUMERATE_LOCALE_INFO_PROPERTIES \
    __JS_ENUMERATE(calendars)               \
    __JS_ENUMERATE(collations)              \
    __JS_ENUMERATE(hour_cycles)             \
    __JS_ENUMERATE(numbering_systems)

// 1.4.2 Intl.Locale.prototype.getCalendars, https://tc39.es/proposal-intl-locale-info/#sec-Intl.Locale.prototype.getCalendars
// 1.4.3 Intl.Locale.prototype.getCollations, https://tc39.es/proposal-intl-locale-info/#sec-Intl.Locale.prototype.getCollations
// 1.4.4 Intl.Locale.prototype.getHourCycles, https://tc39.es/proposal-intl-locale-info/#sec-Intl.Locale.prototype.getHourCycles
// 1.4.5 Intl.Locale.prototype.getNumberingSystems, https://tc39.es/proposal-intl-locale-info/#sec-Intl.Locale.prototype.getNumberingSystems
#define __JS_ENUMERATE(keyword)                               \
    JS_DEFINE_NATIVE_FUNCTION(LocalePrototype::get_##keyword) \
    {                                                         \
        auto locale_object = TRY(typed_this_object(vm));      \
        return keyword##_of_locale(vm, locale_object);        \
    }
JS_ENUMERATE_LOCALE_INFO_PROPERTIES
#undef __JS_ENUMERATE

// 1.4.6 Intl.Locale.prototype.getTimeZones, https://tc39.es/proposal-intl-locale-info/#sec-Intl.Locale.prototype.getTimeZones
JS_DEFINE_NATIVE_FUNCTION(LocalePrototype::get_time_zones)
{
    // 1. Let loc be the this value.
    // 2. Perform ? RequireInternalSlot(loc, [[InitializedLocale]]).
    auto locale_object = TRY(typed_this_object(vm));

    // 3. Let region be GetLocaleRegion(loc.[[Locale]]).
    auto const& region = locale_object->locale_id().language_id.region;

    // 4. If region is undefined, return undefined.
    if (!region.has_value())
        return js_undefined();

    // 5. Return TimeZonesOfLocale(loc).
    return time_zones_of_locale(vm, locale_object);
}

// 1.4.7 Intl.Locale.prototype.getTextInfo, https://tc39.es/proposal-intl-locale-info/#sec-Intl.Locale.prototype.getTextInfo
JS_DEFINE_NATIVE_FUNCTION(LocalePrototype::get_text_info)
{
    auto& realm = *vm.current_realm();

    // 1. Let loc be the this value.
    // 2. Perform ? RequireInternalSlot(loc, [[InitializedLocale]]).
    auto locale_object = TRY(typed_this_object(vm));

    // 3. Let info be OrdinaryObjectCreate(%Object.prototype%).
    auto info = Object::create(realm, realm.intrinsics().object_prototype());

    // 4. Let dir be "ltr".
    auto direction = "ltr"sv;

    // 5. If LocaleIsRightToLeft(loc) is true, then
    if (Unicode::is_locale_character_ordering_right_to_left(locale_object->locale())) {
        // a. Set dir to "rtl".
        direction = "rtl"sv;
    }

    // 6. Perform ! CreateDataPropertyOrThrow(info, "direction", dir).
    MUST(info->create_data_property_or_throw(vm.names.direction, PrimitiveString::create(vm, direction)));

    // 7. Return info.
    return info;
}

// 1.4.8 Intl.Locale.prototype.getWeekInfo, https://tc39.es/proposal-intl-locale-info/#sec-Intl.Locale.prototype.getWeekInfo
JS_DEFINE_NATIVE_FUNCTION(LocalePrototype::get_week_info)
{
    auto& realm = *vm.current_realm();

    // 1. Let loc be the this value.
    // 2. Perform ? RequireInternalSlot(loc, [[InitializedLocale]]).
    auto locale_object = TRY(typed_this_object(vm));

    // 3. Let info be OrdinaryObjectCreate(%Object.prototype%).
    auto info = Object::create(realm, realm.intrinsics().object_prototype());

    // 4. Let wi be WeekInfoOfLocale(loc).
    auto week_info = week_info_of_locale(locale_object);

    // 5. Let we be CreateArrayFromList(wi.[[Weekend]]).
    auto weekend = Array::create_from<u8>(realm, week_info.weekend, [](auto day) { return Value { day }; });

    // 6. Perform ! CreateDataPropertyOrThrow(info, "firstDay", wi.[[FirstDay]]).
    MUST(info->create_data_property_or_throw(vm.names.firstDay, Value { week_info.first_day }));

    // 7. Perform ! CreateDataPropertyOrThrow(info, "weekend", we).
    MUST(info->create_data_property_or_throw(vm.names.weekend, weekend));

    // 8. Perform ! CreateDataPropertyOrThrow(info, "minimalDays", wi.[[MinimalDays]]).
    MUST(info->create_data_property_or_throw(vm.names.minimalDays, Value { week_info.minimal_days }));

    // 9. Return info.
    return info;
}

}
