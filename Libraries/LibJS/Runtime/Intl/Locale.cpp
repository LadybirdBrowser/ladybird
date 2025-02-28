/*
 * Copyright (c) 2021-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Intl/Locale.h>
#include <LibUnicode/DateTimeFormat.h>
#include <LibUnicode/Locale.h>
#include <LibUnicode/TimeZone.h>
#include <LibUnicode/UnicodeKeywords.h>

namespace JS::Intl {

GC_DEFINE_ALLOCATOR(Locale);

GC::Ref<Locale> Locale::create(Realm& realm, GC::Ref<Locale> source_locale, String locale_tag)
{
    auto locale = realm.create<Locale>(realm.intrinsics().intl_locale_prototype());

    locale->set_locale(move(locale_tag));
    locale->m_calendar = source_locale->m_calendar;
    locale->m_case_first = source_locale->m_case_first;
    locale->m_collation = source_locale->m_collation;
    locale->m_hour_cycle = source_locale->m_hour_cycle;
    locale->m_numbering_system = source_locale->m_numbering_system;
    locale->m_numeric = source_locale->m_numeric;

    return locale;
}

// 15 Locale Objects, https://tc39.es/ecma402/#locale-objects
Locale::Locale(Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
{
}

// 1.1.1 CreateArrayFromListOrRestricted ( list , restricted )
static GC::Ref<Array> create_array_from_list_or_restricted(VM& vm, Vector<String> list, Optional<String> restricted)
{
    auto& realm = *vm.current_realm();

    // 1. If restricted is not undefined, then
    if (restricted.has_value()) {
        // a. Set list to « restricted ».
        list = { restricted.release_value() };
    }

    // 2. Return ! CreateArrayFromList( list ).
    return Array::create_from<String>(realm, list, [&vm](auto value) {
        return PrimitiveString::create(vm, move(value));
    });
}

// 1.1.2 CalendarsOfLocale ( loc ), https://tc39.es/proposal-intl-locale-info/#sec-calendars-of-locale
GC::Ref<Array> calendars_of_locale(VM& vm, Locale const& locale_object)
{
    // 1. Let restricted be loc.[[Calendar]].
    Optional<String> restricted = locale_object.has_calendar() ? locale_object.calendar() : Optional<String> {};

    // 2. Let locale be loc.[[Locale]].
    auto const& locale = locale_object.locale();

    // 3. Assert: locale matches the unicode_locale_id production.
    VERIFY(Unicode::parse_unicode_locale_id(locale).has_value());

    // 4. Let list be a List of 1 or more unique canonical calendar identifiers, which must be lower case String values conforming to the type sequence from UTS 35 Unicode Locale Identifier, section 3.2, sorted in descending preference of those in common use for date and time formatting in locale.
    auto list = Unicode::available_calendars(locale);

    // 5. Return ! CreateArrayFromListOrRestricted( list, restricted ).
    return create_array_from_list_or_restricted(vm, move(list), move(restricted));
}

// 1.1.3 CollationsOfLocale ( loc ), https://tc39.es/proposal-intl-locale-info/#sec-collations-of-locale
GC::Ref<Array> collations_of_locale(VM& vm, Locale const& locale_object)
{
    // 1. Let restricted be loc.[[Collation]].
    Optional<String> restricted = locale_object.has_collation() ? locale_object.collation() : Optional<String> {};

    // 2. Let locale be loc.[[Locale]].
    auto const& locale = locale_object.locale();

    // 3. Assert: locale matches the unicode_locale_id production.
    VERIFY(Unicode::parse_unicode_locale_id(locale).has_value());

    // 4. Let list be a List of 1 or more unique canonical collation identifiers, which must be lower case String values conforming to the type sequence from UTS 35 Unicode Locale Identifier, section 3.2, ordered as if an Array of the same values had been sorted, using %Array.prototype.sort% using undefined as comparefn, of those in common use for string comparison in locale. The values "standard" and "search" must be excluded from list.
    auto list = Unicode::available_collations(locale);

    // 5. Return ! CreateArrayFromListOrRestricted( list, restricted ).
    return create_array_from_list_or_restricted(vm, move(list), move(restricted));
}

// 1.1.4 HourCyclesOfLocale ( loc ), https://tc39.es/proposal-intl-locale-info/#sec-hour-cycles-of-locale
GC::Ref<Array> hour_cycles_of_locale(VM& vm, Locale const& locale_object)
{
    // 1. Let restricted be loc.[[HourCycle]].
    Optional<String> restricted = locale_object.has_hour_cycle() ? locale_object.hour_cycle() : Optional<String> {};

    // 2. Let locale be loc.[[Locale]].
    auto const& locale = locale_object.locale();

    // 3. Assert: locale matches the unicode_locale_id production.
    VERIFY(Unicode::parse_unicode_locale_id(locale).has_value());

    // 4. Let list be a List of 1 or more unique hour cycle identifiers, which must be lower case String values indicating either the 12-hour format ("h11", "h12") or the 24-hour format ("h23", "h24"), sorted in descending preference of those in common use for date and time formatting in locale.
    auto list = Unicode::available_hour_cycles(locale);

    // 5. Return ! CreateArrayFromListOrRestricted( list, restricted ).
    return create_array_from_list_or_restricted(vm, move(list), move(restricted));
}

// 1.1.5 NumberingSystemsOfLocale ( loc ), https://tc39.es/proposal-intl-locale-info/#sec-numbering-systems-of-locale
GC::Ref<Array> numbering_systems_of_locale(VM& vm, Locale const& locale_object)
{
    // 1. Let restricted be loc.[[NumberingSystem]].
    Optional<String> restricted = locale_object.has_numbering_system() ? locale_object.numbering_system() : Optional<String> {};

    // 2. Let locale be loc.[[Locale]].
    auto const& locale = locale_object.locale();

    // 3. Assert: locale matches the unicode_locale_id production.
    VERIFY(Unicode::parse_unicode_locale_id(locale).has_value());

    // 4. Let list be a List of 1 or more unique canonical numbering system identifiers, which must be lower case String values conforming to the type sequence from UTS 35 Unicode Locale Identifier, section 3.2, sorted in descending preference of those in common use for formatting numeric values in locale.
    auto list = Unicode::available_number_systems(locale);

    // 5. Return ! CreateArrayFromListOrRestricted( list, restricted ).
    return create_array_from_list_or_restricted(vm, move(list), move(restricted));
}

// 1.1.6 TimeZonesOfLocale ( loc ), https://tc39.es/proposal-intl-locale-info/#sec-time-zones-of-locale
// NOTE: Our implementation takes a region rather than a Locale object to avoid needlessly parsing the locale twice.
GC::Ref<Array> time_zones_of_locale(VM& vm, StringView region)
{
    auto& realm = *vm.current_realm();

    // 1. Let locale be loc.[[Locale]].
    // 2. Assert: locale matches the unicode_locale_id production.
    // 3. Let region be the substring of locale corresponding to the unicode_region_subtag production of the unicode_language_id.

    // 4. Let list be a List of unique canonical time zone identifiers, which must be String values indicating a canonical Zone name of the IANA Time Zone Database, ordered as if an Array of the same values had been sorted using %Array.prototype.sort% using undefined as comparefn, of those in common use in region. If no time zones are commonly used in region, let list be a new empty List.
    auto list = Unicode::available_time_zones_in_region(region);

    // 5. Return ! CreateArrayFromList( list ).
    return Array::create_from<String>(realm, list, [&vm](auto value) {
        return PrimitiveString::create(vm, value);
    });
}

// 1.1.7 CharacterDirectionOfLocale ( loc ), https://tc39.es/proposal-intl-locale-info/#sec-character-direction-of-locale
StringView character_direction_of_locale(Locale const& locale_object)
{
    // 1. Let locale be loc.[[Locale]].
    auto const& locale = locale_object.locale();

    // 2. Assert: locale matches the unicode_locale_id production.
    VERIFY(Unicode::parse_unicode_locale_id(locale).has_value());

    // 3. If the default general ordering of characters (characterOrder) within a line in locale is right-to-left, return "rtl".
    if (Unicode::is_locale_character_ordering_right_to_left(locale))
        return "rtl"sv;

    // 4. Return "ltr".
    return "ltr"sv;
}

struct FirstDayStringAndValue {
    StringView weekday;
    StringView string;
    u8 value { 0 };
};

// Table 1: First Day String and Value, https://tc39.es/proposal-intl-locale-info/#table-locale-first-day-option-value
static constexpr auto first_day_string_and_value_table = to_array<FirstDayStringAndValue>({
    { "0"sv, "sun"sv, 7 },
    { "1"sv, "mon"sv, 1 },
    { "2"sv, "tue"sv, 2 },
    { "3"sv, "wed"sv, 3 },
    { "4"sv, "thu"sv, 4 },
    { "5"sv, "fri"sv, 5 },
    { "6"sv, "sat"sv, 6 },
    { "7"sv, "sun"sv, 7 },
});

// 1.1.8 WeekdayToString ( fw ), https://tc39.es/proposal-intl-locale-info/#sec-weekday-to-string
StringView weekday_to_string(StringView weekday)
{
    // 1. For each row of Table 1, except the header row, in table order, do
    for (auto const& row : first_day_string_and_value_table) {
        // a. Let w be the name given in the Weekday column of the row.
        // b. Let s be the name given in the String column of the row.
        // c. If fw is equal to w, return s.
        if (weekday == row.weekday)
            return row.string;
    }

    // 2. Return fw.
    return weekday;
}

// 1.1.9 StringToWeekdayValue ( fw ), https://tc39.es/proposal-intl-locale-info/#sec-string-to-weekday-value
Optional<u8> string_to_weekday_value(StringView weekday)
{
    // 1. For each row of Table 1, except the header row, in table order, do
    for (auto const& row : first_day_string_and_value_table) {
        // a. Let s be the name given in the String column of the row.
        // b. Let v be the name given in the Value column of the row.
        // c. If fw is equal to s, return v.
        if (weekday == row.string)
            return row.value;
    }

    // 2. Return undefined.
    return {};
}

static u8 weekday_to_integer(Optional<Unicode::Weekday> const& weekday, Unicode::Weekday falllback)
{
    // NOTE: This fallback will be used if the ICU data lookup failed. Its value should be that of the
    //       default region ("001") in the CLDR.
    switch (weekday.value_or(falllback)) {
    case Unicode::Weekday::Monday:
        return 1;
    case Unicode::Weekday::Tuesday:
        return 2;
    case Unicode::Weekday::Wednesday:
        return 3;
    case Unicode::Weekday::Thursday:
        return 4;
    case Unicode::Weekday::Friday:
        return 5;
    case Unicode::Weekday::Saturday:
        return 6;
    case Unicode::Weekday::Sunday:
        return 7;
    }
    VERIFY_NOT_REACHED();
}

static Vector<u8> weekend_of_locale(ReadonlySpan<Unicode::Weekday> const& weekend_days)
{
    Vector<u8> weekend;
    weekend.ensure_capacity(weekend_days.size());

    for (auto day : weekend_days)
        weekend.unchecked_append(weekday_to_integer(day, day));

    quick_sort(weekend);
    return weekend;
}

// 1.1.10 WeekInfoOfLocale ( loc ), https://tc39.es/proposal-intl-locale-info/#sec-week-info-of-locale
WeekInfo week_info_of_locale(Locale const& locale_object)
{
    // 1. Let locale be loc.[[Locale]].
    auto const& locale = locale_object.locale();

    // 2. Assert: locale matches the unicode_locale_id production.
    VERIFY(Unicode::parse_unicode_locale_id(locale).has_value());

    // 3. Let r be a record whose fields are defined by Table 2, with values based on locale.
    auto locale_week_info = Unicode::week_info_of_locale(locale);

    WeekInfo week_info {};
    week_info.minimal_days = locale_week_info.minimal_days_in_first_week;
    week_info.first_day = weekday_to_integer(locale_week_info.first_day_of_week, Unicode::Weekday::Monday);
    week_info.weekend = weekend_of_locale(locale_week_info.weekend_days);

    Optional<u8> first_day_of_week;

    if (locale_object.has_first_day_of_week()) {
        // 4. Let fws be loc.[[FirstDayOfWeek]].
        auto const& first_day_of_week_string = locale_object.first_day_of_week();

        // 5. Let fw be !StringToWeekdayValue(fws).
        first_day_of_week = string_to_weekday_value(first_day_of_week_string);
    }

    // 6. If fw is not undefined, then
    if (first_day_of_week.has_value()) {
        // a. Set r.[[FirstDay]] to fw.
        week_info.first_day = *first_day_of_week;
    }

    // 7. Return r.
    return week_info;
}

}
