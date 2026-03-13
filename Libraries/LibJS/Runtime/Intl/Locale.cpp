/*
 * Copyright (c) 2021-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Intl/Locale.h>
#include <LibUnicode/DateTimeFormat.h>
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

Unicode::LocaleID const& Locale::locale_id() const
{
    return m_cached_locale_id.ensure([&] { return Unicode::parse_unicode_locale_id(locale()); });
}

// 15.5.5 GetLocaleVariants ( locale ), https://tc39.es/ecma402/#sec-getlocalevariants
Optional<String> get_locale_variants(Unicode::LocaleID const& locale)
{
    // 1. Let baseName be GetLocaleBaseName(locale).
    auto const& base_name = locale.language_id;

    // 2. NOTE: Each subtag in baseName that is preceded by "-" is either a unicode_script_subtag, unicode_region_subtag,
    //    or unicode_variant_subtag, but any substring matched by unicode_variant_subtag is strictly longer than any
    //    prefix thereof which could also be matched by one of the other productions.

    // 3. Let variants be the longest suffix of baseName that starts with a "-" followed by a substring that is matched
    //    by the unicode_variant_subtag Unicode locale nonterminal. If there is no such suffix, return undefined.
    if (base_name.variants.is_empty())
        return {};

    // 4. Return the substring of variants from 1.
    return MUST(String::join("-"sv, base_name.variants));
}

// 15.5.9 CalendarsOfLocale ( loc ), https://tc39.es/ecma402/#sec-calendarsoflocale
GC::Ref<Array> calendars_of_locale(VM& vm, Locale const& locale_object)
{
    auto& realm = *vm.current_realm();

    // 1. If loc.[[Calendar]] is not undefined, then
    if (locale_object.has_calendar()) {
        // a. Return CreateArrayFromList(« loc.[[Calendar]] »).
        return Array::create_from(realm, { PrimitiveString::create(vm, locale_object.calendar()) });
    }

    // 2. Let preference be RegionPreference(loc.[[Locale]]).
    // 3. Let region be preference.[[Region]].
    // 4. Let regionOverride be preference.[[RegionOverride]].
    // 5. If regionOverride is not undefined and calendar preference data for regionOverride are available, then
    //     a. Let lookupRegion be regionOverride.
    // 6. Else,
    //     a. Let lookupRegion be region.
    // 7. Let list be a List of unique calendar types in canonical form (6.9), sorted in descending preference of those
    //    in common use for date and time formatting in lookupRegion. The list is empty if no calendar preference data
    //    for lookupRegion is available.
    // 8. If list is empty, set list to « "gregory" ».
    auto list = Unicode::available_calendars(locale_object.locale());

    // 9. Return CreateArrayFromList(list).
    return Array::create_from<String>(realm, list, [&vm](auto const& value) {
        return PrimitiveString::create(vm, value);
    });
}

// 15.5.10 CollationsOfLocale ( loc ), https://tc39.es/ecma402/#sec-collationsoflocale
GC::Ref<Array> collations_of_locale(VM& vm, Locale const& locale_object)
{
    auto& realm = *vm.current_realm();

    // 1. If loc.[[Collation]] is not undefined, then
    if (locale_object.has_collation()) {
        // a. Return CreateArrayFromList(« loc.[[Collation]] »).
        return Array::create_from(realm, { PrimitiveString::create(vm, locale_object.collation()) });
    }

    // 2. Let language be GetLocaleLanguage(loc.[[Locale]]).
    // 3. If language is not "und", then
    //     a. Let r be LookupMatchingLocaleByPrefix(%Intl.Collator%.[[AvailableLocales]], « loc.[[Locale]] »).
    //     b. If r is not undefined, then
    //         i. Let foundLocale be r.[[locale]].
    //     c. Else,
    //         i. Let foundLocale be DefaultLocale().
    //     d. Let foundLocaleData be %Intl.Collator%.[[SortLocaleData]].[[<foundLocale>]].
    //     e. Let list be a copy of foundLocaleData.[[co]].
    //     f. Assert: list[0] is null.
    //     g. Remove the first element from list.
    // 4. Else,
    //     a. Let list be « "emoji", "eor" ».
    // 5. Let sorted be a copy of list, sorted according to lexicographic code unit order.
    auto list = Unicode::available_collations(locale_object.locale());

    // 6. Return CreateArrayFromList(sorted).
    return Array::create_from<String>(realm, list, [&vm](auto const& value) {
        return PrimitiveString::create(vm, value);
    });
}

// 15.5.11 HourCyclesOfLocale ( loc ), https://tc39.es/ecma402/#sec-hourcyclesoflocale
GC::Ref<Array> hour_cycles_of_locale(VM& vm, Locale const& locale_object)
{
    auto& realm = *vm.current_realm();

    // 1. If loc.[[HourCycle]] is not undefined, then
    if (locale_object.has_hour_cycle()) {
        // a. Return CreateArrayFromList(« loc.[[HourCycle]] »).
        return Array::create_from(realm, { PrimitiveString::create(vm, locale_object.hour_cycle()) });
    }

    // 2. Let preference be RegionPreference(loc.[[Locale]]).
    // 3. Let region be preference.[[Region]].
    // 4. Let regionOverride be preference.[[RegionOverride]].
    // 5. If regionOverride is not undefined and time data for regionOverride are available, then
    //     a. Let lookupRegion be regionOverride.
    // 6. Else,
    //     a. Let lookupRegion be region.
    // 7. Let list be a List of unique hour cycle identifiers, which must be lower case String values indicating either the 12-hour format ("h11", "h12") or the 24-hour format ("h23", "h24"), sorted in descending preference of those in common use for date and time formatting in lookupRegion. The list is empty if no time data for lookupRegion is available.
    // 8. If list is empty, set list to « "h23" ».
    auto list = Unicode::available_hour_cycles(locale_object.locale());

    // 9. Return CreateArrayFromList(list).
    return Array::create_from<String>(realm, list, [&vm](auto const& value) {
        return PrimitiveString::create(vm, value);
    });
}

// 15.5.12 NumberingSystemsOfLocale ( loc ), https://tc39.es/ecma402/#sec-numberingsystemsoflocale
GC::Ref<Array> numbering_systems_of_locale(VM& vm, Locale const& locale_object)
{
    auto& realm = *vm.current_realm();

    // 1. If loc.[[NumberingSystem]] is not undefined, then
    if (locale_object.has_numbering_system()) {
        // a. Return CreateArrayFromList(« loc.[[NumberingSystem]] »).
        return Array::create_from(realm, { PrimitiveString::create(vm, locale_object.numbering_system()) });
    }

    // 2. Let r be LookupMatchingLocaleByPrefix(%Intl.NumberFormat%.[[AvailableLocales]], « loc.[[Locale]] »).
    // 3. If r is not undefined, then
    //     a. Let foundLocale be r.[[locale]].
    //     b. Let foundLocaleData be %Intl.NumberFormat%.[[LocaleData]].[[<foundLocale>]].
    //     c. Let numberingSystems be foundLocaleData.[[nu]].
    //     d. Let list be « numberingSystems[0] ».
    // 4. Else,
    //     a. Let list be « "latn" ».
    auto list = Unicode::available_number_systems(locale_object.locale());

    // 5. Return CreateArrayFromList(list).
    return Array::create_from<String>(realm, list, [&vm](auto const& value) {
        return PrimitiveString::create(vm, value);
    });
}

// 15.5.13 TimeZonesOfLocale ( loc ), https://tc39.es/ecma402/#sec-timezonesoflocale
Value time_zones_of_locale(VM& vm, Locale const& locale_object)
{
    auto& realm = *vm.current_realm();

    // 1. Let region be GetLocaleRegion(loc.[[Locale]]).
    auto const& region = locale_object.locale_id().language_id.region;

    // 2. If region is undefined, return undefined.
    if (!region.has_value())
        return js_undefined();

    // 3. Let list be a List of unique canonical time zone identifiers, which must be String values indicating a
    //    canonical Zone name of the IANA Time Zone Database, of those in common use in region. The list is empty if no
    //    time zones are commonly used in region. The list is sorted according to lexicographic code unit order.
    auto list = Unicode::available_time_zones_in_region(*region);

    // 4. Return CreateArrayFromList( list ).
    return Array::create_from<String>(realm, list, [&vm](auto const& value) {
        return PrimitiveString::create(vm, value);
    });
}

// 15.5.14 TextDirectionOfLocale ( loc ), https://tc39.es/ecma402/#sec-textdirectionoflocale
StringView text_direction_of_locale(Locale const& locale_object)
{
    // 1. Let locale be loc.[[Locale]].
    auto const& locale = locale_object.locale();

    // 2. Let script be GetLocaleScript(locale).
    // 3. If script is undefined, then
    //     a. Let maximal be the result of the Add Likely Subtags algorithm applied to locale. If an error is signaled, return undefined.
    //     b. Set script to GetLocaleScript(maximal).
    //     c. If script is undefined, return undefined.
    // NB: ICU handles maximizing the locale if there is no script.

    // 4. If the default general ordering of characters within a line in script is right-to-left, return "rtl".
    // 5. If the default general ordering of characters within a line in script is left-to-right, return "ltr".
    // 6. Return undefined.
    // FIXME: ICU does not provide a method to determine if a locale is neither rtl nor ltr.
    return Unicode::is_locale_character_ordering_right_to_left(locale) ? "rtl"sv : "ltr"sv;
}

struct FirstDayStringAndValue {
    StringView weekday;
    StringView string;
    u8 value { 0 };
};

// Table 26: Weekday String and Value, https://tc39.es/ecma402/#table-locale-weekday-string-value
static constexpr auto WEEKDAY_STRING_AND_VALUE = to_array<FirstDayStringAndValue>({
    { "0"sv, "sun"sv, 7 },
    { "1"sv, "mon"sv, 1 },
    { "2"sv, "tue"sv, 2 },
    { "3"sv, "wed"sv, 3 },
    { "4"sv, "thu"sv, 4 },
    { "5"sv, "fri"sv, 5 },
    { "6"sv, "sat"sv, 6 },
    { "7"sv, "sun"sv, 7 },
});

// 15.5.15 WeekdayToUValue ( fw ), https://tc39.es/ecma402/#sec-weekdaytouvalue
StringView weekday_to_u_value(StringView weekday)
{
    // 1. For each row of Table 26, except the header row, in table order, do
    for (auto const& row : WEEKDAY_STRING_AND_VALUE) {
        // a. Let w be the Weekday value of the current row.
        // b. Let s be the String value of the current row.
        // c. If fw is equal to w, return s.
        if (weekday == row.weekday)
            return row.string;
    }

    // 2. Return fw.
    return weekday;
}

// 15.5.16 WeekdayUValueToNumber ( fw ), https://tc39.es/ecma402/#sec-weekdayuvaluetonumber
Optional<u8> weekday_u_value_to_number(StringView weekday)
{
    // 1. For each row of Table 26, except the header row, in table order, do
    for (auto const& row : WEEKDAY_STRING_AND_VALUE) {
        // a. Let s be the String value of the current row.
        // b. Let v be the Value value of the current row.
        // c. If fw is equal to s, return v.
        if (weekday == row.string)
            return row.value;
    }

    // 2. Return undefined.
    return {};
}

static u8 weekday_to_integer(Optional<Unicode::Weekday> const& weekday, Unicode::Weekday falllback)
{
    // NB: This fallback will be used if the ICU data lookup failed. Its value should be that of the default region
    //     ("001") in the CLDR.
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

// 15.5.17 WeekInfoOfLocale ( loc ), https://tc39.es/ecma402/#sec-weekinfooflocale
WeekInfo week_info_of_locale(Locale const& locale_object)
{
    // 1. Let locale be loc.[[Locale]].
    auto const& locale = locale_object.locale();

    // 2. Let r be a Record whose fields are defined by Table 27, with values based on locale.
    auto locale_week_info = Unicode::week_info_of_locale(locale);

    WeekInfo week_info {};
    week_info.first_day = weekday_to_integer(locale_week_info.first_day_of_week, Unicode::Weekday::Monday);
    week_info.weekend = weekend_of_locale(locale_week_info.weekend_days);

    Optional<u8> first_day_of_week;

    if (locale_object.has_first_day_of_week()) {
        // 3. Let fws be loc.[[FirstDayOfWeek]].
        auto const& first_day_of_week_string = locale_object.first_day_of_week();

        // 4. Let fw be WeekdayUValueToNumber(fws).
        first_day_of_week = weekday_u_value_to_number(first_day_of_week_string);
    }

    // 5. If fw is not undefined, then
    if (first_day_of_week.has_value()) {
        // a. Set r.[[FirstDay]] to fw.
        week_info.first_day = *first_day_of_week;
    }

    // 6. Return r.
    return week_info;
}

}
