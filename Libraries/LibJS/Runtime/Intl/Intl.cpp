/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Date.h>
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
#include <LibJS/Runtime/Intl/SingleUnitIdentifiers.h>
#include <LibUnicode/DateTimeFormat.h>
#include <LibUnicode/Locale.h>
#include <LibUnicode/NumberFormat.h>
#include <LibUnicode/UnicodeKeywords.h>

namespace JS::Intl {

GC_DEFINE_ALLOCATOR(Intl);

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

    GC::MarkedVector<Value> marked_locale_list { vm.heap() };
    marked_locale_list.ensure_capacity(locale_list.size());

    for (auto& locale : locale_list)
        marked_locale_list.unchecked_append(PrimitiveString::create(vm, move(locale)));

    // 2. Return CreateArrayFromList(ll).
    return Array::create_from(realm, marked_locale_list);
}

// 6.5.4 AvailablePrimaryTimeZoneIdentifiers ( ), https://tc39.es/ecma402/#sec-availableprimarytimezoneidentifiers
static Vector<String> available_primary_time_zone_identifiers()
{
    // 1. Let records be AvailableNamedTimeZoneIdentifiers().
    auto const& records = available_named_time_zone_identifiers();

    // 2. Let result be a new empty List.
    Vector<String> result;

    // 3. For each element timeZoneIdentifierRecord of records, do
    for (auto const& time_zone_identifier_record : records) {
        // a. If timeZoneIdentifierRecord.[[Identifier]] is timeZoneIdentifierRecord.[[PrimaryIdentifier]], then
        if (time_zone_identifier_record.identifier == time_zone_identifier_record.primary_identifier) {
            // i. Append timeZoneIdentifierRecord.[[Identifier]] to result.
            result.append(time_zone_identifier_record.identifier);
        }
    }

    // 4. Return result.
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
        list = Unicode::available_calendars().span();
    }
    // 3. Else if key is "collation", then
    else if (key == "collation"sv) {
        // a. Let list be ! AvailableCanonicalCollations( ).
        list = Unicode::available_collations().span();
    }
    // 4. Else if key is "currency", then
    else if (key == "currency"sv) {
        // a. Let list be ! AvailableCanonicalCurrencies( ).
        list = Unicode::available_currencies().span();
    }
    // 5. Else if key is "numberingSystem", then
    else if (key == "numberingSystem"sv) {
        // a. Let list be ! AvailableCanonicalNumberingSystems( ).
        list = Unicode::available_number_systems().span();
    }
    // 6. Else if key is "timeZone", then
    else if (key == "timeZone"sv) {
        // a. Let list be ! AvailablePrimaryTimeZoneIdentifiers( ).
        static auto const time_zones = available_primary_time_zone_identifiers();
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
