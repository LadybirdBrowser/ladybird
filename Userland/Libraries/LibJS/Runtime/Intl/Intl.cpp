/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Intl/AbstractOperations.h>
#include <LibJS/Runtime/Intl/CollatorConstructor.h>
#include <LibJS/Runtime/Intl/DateTimeFormatConstructor.h>
#include <LibJS/Runtime/Intl/DisplayNamesConstructor.h>
#include <LibJS/Runtime/Intl/DurationFormatConstructor.h>
#include <LibJS/Runtime/Intl/Intl.h>
#include <LibJS/Runtime/Intl/ListFormatConstructor.h>
#include <LibJS/Runtime/Intl/LocaleConstructor.h>
#include <LibJS/Runtime/Intl/NumberFormatConstructor.h>
#include <LibJS/Runtime/Intl/PluralRulesConstructor.h>
#include <LibJS/Runtime/Intl/RelativeTimeFormatConstructor.h>
#include <LibJS/Runtime/Intl/SegmenterConstructor.h>
#include <LibJS/Runtime/Temporal/TimeZone.h>
#include <LibLocale/DateTimeFormat.h>
#include <LibLocale/Locale.h>
#include <LibLocale/NumberFormat.h>

namespace JS::Intl {

JS_DEFINE_ALLOCATOR(Intl);

// 8 The Intl Object, https://tc39.es/ecma402/#intl-object
Intl::Intl(Realm& realm)
    : Object(ConstructWithPrototypeTag::Tag, realm.intrinsics().object_prototype())
{
}

void Intl::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 8.1.1 Intl[ @@toStringTag ], https://tc39.es/ecma402/#sec-Intl-toStringTag
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, "Intl"_string), Attribute::Configurable);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_intrinsic_accessor(vm.names.Collator, attr, [](auto& realm) -> Value { return realm.intrinsics().intl_collator_constructor(); });
    define_intrinsic_accessor(vm.names.DateTimeFormat, attr, [](auto& realm) -> Value { return realm.intrinsics().intl_date_time_format_constructor(); });
    define_intrinsic_accessor(vm.names.DisplayNames, attr, [](auto& realm) -> Value { return realm.intrinsics().intl_display_names_constructor(); });
    define_intrinsic_accessor(vm.names.DurationFormat, attr, [](auto& realm) -> Value { return realm.intrinsics().intl_duration_format_constructor(); });
    define_intrinsic_accessor(vm.names.ListFormat, attr, [](auto& realm) -> Value { return realm.intrinsics().intl_list_format_constructor(); });
    define_intrinsic_accessor(vm.names.Locale, attr, [](auto& realm) -> Value { return realm.intrinsics().intl_locale_constructor(); });
    define_intrinsic_accessor(vm.names.NumberFormat, attr, [](auto& realm) -> Value { return realm.intrinsics().intl_number_format_constructor(); });
    define_intrinsic_accessor(vm.names.PluralRules, attr, [](auto& realm) -> Value { return realm.intrinsics().intl_plural_rules_constructor(); });
    define_intrinsic_accessor(vm.names.RelativeTimeFormat, attr, [](auto& realm) -> Value { return realm.intrinsics().intl_relative_time_format_constructor(); });
    define_intrinsic_accessor(vm.names.Segmenter, attr, [](auto& realm) -> Value { return realm.intrinsics().intl_segmenter_constructor(); });

    define_native_function(realm, vm.names.getCanonicalLocales, get_canonical_locales, 1, attr);
    define_native_function(realm, vm.names.supportedValuesOf, supported_values_of, 1, attr);
}

// 8.3.1 Intl.getCanonicalLocales ( locales ), https://tc39.es/ecma402/#sec-intl.getcanonicallocales
JS_DEFINE_NATIVE_FUNCTION(Intl::get_canonical_locales)
{
    auto& realm = *vm.current_realm();

    auto locales = vm.argument(0);

    // 1. Let ll be ? CanonicalizeLocaleList(locales).
    auto locale_list = TRY(canonicalize_locale_list(vm, locales));

    MarkedVector<Value> marked_locale_list { vm.heap() };
    marked_locale_list.ensure_capacity(locale_list.size());

    for (auto& locale : locale_list)
        marked_locale_list.unchecked_append(PrimitiveString::create(vm, move(locale)));

    // 2. Return CreateArrayFromList(ll).
    return Array::create_from(realm, marked_locale_list);
}

// 6.5.4 AvailableCanonicalTimeZones ( ), https://tc39.es/ecma402/#sec-availablecanonicaltimezones
static Vector<StringView> available_canonical_time_zones()
{
    // 1. Let names be a List of all supported Zone and Link names in the IANA Time Zone Database.
    auto names = TimeZone::all_time_zones();

    // 2. Let result be a new empty List.
    Vector<StringView> result;

    // 3. For each element name of names, do
    for (auto const& name : names) {
        // a. Assert: IsValidTimeZoneName( name ) is true.
        // b. Let canonical be ! CanonicalizeTimeZoneName( name ).
        auto canonical = TimeZone::canonicalize_time_zone(name.name).value();

        // c. If result does not contain an element equal to canonical, then
        if (!result.contains_slow(canonical)) {
            // i. Append canonical to the end of result.
            result.append(canonical);
        }
    }

    // 4. Sort result in order as if an Array of the same values had been sorted using %Array.prototype.sort% using undefined as comparefn.
    quick_sort(result);

    // 5. Return result.
    return result;
}

// 8.3.2 Intl.supportedValuesOf ( key ), https://tc39.es/ecma402/#sec-intl.supportedvaluesof
JS_DEFINE_NATIVE_FUNCTION(Intl::supported_values_of)
{
    auto& realm = *vm.current_realm();

    // 1. Let key be ? ToString(key).
    auto key = TRY(vm.argument(0).to_string(vm));

    Optional<Variant<ReadonlySpan<StringView>, ReadonlySpan<String>>> list;

    // 2. If key is "calendar", then
    if (key == "calendar"sv) {
        // a. Let list be ! AvailableCanonicalCalendars( ).
        list = ::Locale::get_available_calendars();
    }
    // 3. Else if key is "collation", then
    else if (key == "collation"sv) {
        // a. Let list be ! AvailableCanonicalCollations( ).
        list = ::Locale::get_available_collation_types();
    }
    // 4. Else if key is "currency", then
    else if (key == "currency"sv) {
        // a. Let list be ! AvailableCanonicalCurrencies( ).
        static auto const currencies = ::Locale::available_currencies();
        list = currencies.span();
    }
    // 5. Else if key is "numberingSystem", then
    else if (key == "numberingSystem"sv) {
        // a. Let list be ! AvailableCanonicalNumberingSystems( ).
        list = ::Locale::get_available_number_systems();
    }
    // 6. Else if key is "timeZone", then
    else if (key == "timeZone"sv) {
        // a. Let list be ! AvailableCanonicalTimeZones( ).
        static auto const time_zones = available_canonical_time_zones();
        list = time_zones.span();
    }
    // 7. Else if key is "unit", then
    else if (key == "unit"sv) {
        // a. Let list be ! AvailableCanonicalUnits( ).
        static auto const units = sanctioned_single_unit_identifiers();
        list = units.span();
    }
    // 8. Else,
    else {
        // a. Throw a RangeError exception.
        return vm.throw_completion<RangeError>(ErrorType::IntlInvalidKey, key);
    }

    // 9. Return CreateArrayFromList( list ).
    return list->visit([&]<typename T>(ReadonlySpan<T> list) {
        return Array::create_from<T>(realm, list, [&](auto value) {
            return PrimitiveString::create(vm, value);
        });
    });
}

}
