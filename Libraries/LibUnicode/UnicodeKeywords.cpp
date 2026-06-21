/*
 * Copyright (c) 2024-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <AK/QuickSort.h>
#include <AK/ScopeGuard.h>
#include <LibUnicode/DateTimeFormat.h>
#include <LibUnicode/ICU.h>
#include <LibUnicode/UnicodeKeywords.h>

#include <unicode/calendar.h>
#include <unicode/coll.h>
#include <unicode/locid.h>
#include <unicode/numsys.h>
#include <unicode/ucurr.h>

namespace Unicode {

Vector<Utf16String> available_keyword_values(Utf16View locale, Utf16View key)
{
    if (key == "ca"sv)
        return available_calendars(locale);
    if (key == "co"sv)
        return available_collations(locale);
    if (key == "hc"sv)
        return available_hour_cycles(locale);
    if (key == "kf"sv)
        return available_collation_case_orderings();
    if (key == "kn"sv)
        return available_collation_numeric_orderings();
    if (key == "nu"sv)
        return available_number_systems(locale);
    TODO();
}

static bool is_available_calendar(char const* value, size_t value_length)
{
    // "islamic" and "islamic-rgsa" are deprecated calendar types that DateTimeFormat resolves to other calendars,
    // so they should not be advertised as available.
    return !StringView { value, value_length }.is_one_of("islamic"sv, "islamic-rgsa"sv);
}

Vector<Utf16String> const& available_calendars()
{
    static NeverDestroyed<Vector<Utf16String>> calendars { []() -> Vector<Utf16String> {
        auto calendars = available_calendars(Utf16View { "und"sv });

        quick_sort(calendars);
        return calendars;
    }() };

    return *calendars;
}

Vector<Utf16String> available_calendars(Utf16View locale)
{
    UErrorCode status = U_ZERO_ERROR;

    auto locale_data = LocaleData::for_locale(locale.bytes());
    if (!locale_data.has_value())
        return {};

    auto keywords = adopt_own_if_nonnull(icu::Calendar::getKeywordValuesForLocale("calendar", locale_data->locale(), 0, status));
    if (icu_failure(status))
        return {};

    Vector<Utf16String> result;
    while (true) {
        i32 length = 0;
        auto const* value = keywords->next(&length, status);

        if (icu_failure(status) || value == nullptr)
            break;

        if (!is_available_calendar(value, static_cast<size_t>(length)))
            continue;

        if (auto const* bcp47_value = uloc_toUnicodeLocaleType("ca", value))
            result.append(Utf16String::from_ascii_without_validation(StringView { bcp47_value, strlen(bcp47_value) }.bytes()));
    }

    return result;
}

Vector<Utf16String> const& available_currencies()
{
    static NeverDestroyed<Vector<Utf16String>> currencies { []() -> Vector<Utf16String> {
        UErrorCode status = U_ZERO_ERROR;

        auto* currencies = ucurr_openISOCurrencies(UCURR_ALL, &status);
        ScopeGuard guard { [&]() { uenum_close(currencies); } };

        if (icu_failure(status))
            return {};

        Vector<Utf16String> result;

        while (true) {
            i32 length = 0;
            char const* next = uenum_next(currencies, &length, &status);

            if (icu_failure(status))
                return {};
            if (next == nullptr)
                break;

            // https://unicode-org.atlassian.net/browse/ICU-21687
            if (StringView currency { next, static_cast<size_t>(length) }; currency != "LSM"sv)
                result.append(Utf16String::from_utf8(currency));
        }

        quick_sort(result);
        return result;
    }() };

    return *currencies;
}

Vector<Utf16String> const& available_collation_case_orderings()
{
    static NeverDestroyed<Vector<Utf16String>> case_orderings { [] {
        return Vector<Utf16String> {
            Utf16String::from_ascii_without_validation("false"sv.bytes()),
            Utf16String::from_ascii_without_validation("lower"sv.bytes()),
            Utf16String::from_ascii_without_validation("upper"sv.bytes()),
        };
    }() };
    return *case_orderings;
}

Vector<Utf16String> const& available_collation_numeric_orderings()
{
    static NeverDestroyed<Vector<Utf16String>> numeric_orderings { [] {
        return Vector<Utf16String> {
            Utf16String::from_ascii_without_validation("false"sv.bytes()),
            Utf16String::from_ascii_without_validation("true"sv.bytes()),
        };
    }() };
    return *numeric_orderings;
}

Vector<Utf16String> const& available_collations()
{
    static NeverDestroyed<Vector<Utf16String>> collations { []() -> Vector<Utf16String> {
        UErrorCode status = U_ZERO_ERROR;

        auto keywords = adopt_own_if_nonnull(icu::Collator::getKeywordValues("collation", status));
        if (icu_failure(status))
            return {};

        Vector<Utf16String> collations;
        while (true) {
            i32 length = 0;
            auto const* value = keywords->next(&length, status);

            if (icu_failure(status) || value == nullptr)
                break;

            // https://tc39.es/ecma402/#sec-properties-of-intl-collator-instances
            // the values "standard" and "search" are not allowed
            if (StringView { value, static_cast<size_t>(length) }.is_one_of("standard"sv, "search"sv))
                continue;

            if (auto const* bcp47_value = uloc_toUnicodeLocaleType("co", value))
                collations.append(Utf16String::from_ascii_without_validation(StringView { bcp47_value, strlen(bcp47_value) }.bytes()));
        }

        quick_sort(collations);
        return collations;
    }() };

    return *collations;
}

Vector<Utf16String> available_collations(Utf16View locale)
{
    UErrorCode status = U_ZERO_ERROR;

    auto locale_data = LocaleData::for_locale(locale.bytes());
    if (!locale_data.has_value())
        return {};

    auto keywords = adopt_own_if_nonnull(icu::Collator::getKeywordValuesForLocale("collation", locale_data->locale(), true, status));
    if (icu_failure(status))
        return {};

    Vector<Utf16String> collations;
    while (true) {
        i32 length = 0;
        auto const* value = keywords->next(&length, status);

        if (icu_failure(status) || value == nullptr)
            break;

        // https://tc39.es/ecma402/#sec-properties-of-intl-collator-instances
        // the values "standard" and "search" are not allowed
        if (StringView { value, static_cast<size_t>(length) }.is_one_of("standard"sv, "search"sv))
            continue;

        if (auto const* bcp47_value = uloc_toUnicodeLocaleType("co", value))
            collations.append(Utf16String::from_ascii_without_validation(StringView { bcp47_value, strlen(bcp47_value) }.bytes()));
    }

    auto default_collation = Utf16String::from_ascii_without_validation("default"sv.bytes());
    if (!collations.contains_slow(default_collation))
        collations.prepend(default_collation);

    return collations;
}

Vector<Utf16String> const& available_hour_cycles()
{
    static NeverDestroyed<Vector<Utf16String>> hour_cycles { [] {
        return Vector<Utf16String> {
            Utf16String::from_ascii_without_validation("h11"sv.bytes()),
            Utf16String::from_ascii_without_validation("h12"sv.bytes()),
            Utf16String::from_ascii_without_validation("h23"sv.bytes()),
            Utf16String::from_ascii_without_validation("h24"sv.bytes()),
        };
    }() };
    return *hour_cycles;
}

Vector<Utf16String> available_hour_cycles(Utf16View locale)
{
    auto preferred_hour_cycle = default_hour_cycle(locale);
    if (!preferred_hour_cycle.has_value())
        return available_hour_cycles();

    Vector<Utf16String> hour_cycles;
    hour_cycles.append(available_hour_cycles()[to_underlying(*preferred_hour_cycle)]);

    for (auto const& hour_cycle : available_hour_cycles()) {
        if (hour_cycle != hour_cycles[0])
            hour_cycles.append(hour_cycle);
    }

    return hour_cycles;
}

Vector<Utf16String> const& available_number_systems()
{
    static NeverDestroyed<Vector<Utf16String>> number_systems { []() -> Vector<Utf16String> {
        UErrorCode status = U_ZERO_ERROR;

        auto keywords = adopt_own_if_nonnull(icu::NumberingSystem::getAvailableNames(status));
        if (icu_failure(status))
            return {};

        Vector<Utf16String> number_systems;
        while (true) {
            i32 length = 0;
            auto const* keyword = keywords->next(&length, status);

            if (icu_failure(status) || keyword == nullptr)
                break;

            auto system = adopt_own_if_nonnull(icu::NumberingSystem::createInstanceByName(keyword, status));
            if (icu_failure(status))
                return {};

            if (!static_cast<bool>(system->isAlgorithmic())) {
                if (auto const* bcp47_value = uloc_toUnicodeLocaleType("nu", keyword))
                    number_systems.append(Utf16String::from_ascii_without_validation(StringView { bcp47_value, strlen(bcp47_value) }.bytes()));
            }
        }

        quick_sort(number_systems);
        return number_systems;
    }() };

    return *number_systems;
}

Vector<Utf16String> available_number_systems(Utf16View locale)
{
    auto locale_data = LocaleData::for_locale(locale.bytes());
    if (!locale_data.has_value())
        return {};

    Vector<Utf16String> number_systems;

    auto const* preferred_number_system = locale_data->numbering_system().getName();
    number_systems.append(Utf16String::from_ascii_without_validation(StringView { preferred_number_system, strlen(preferred_number_system) }.bytes()));

    for (auto const& number_system : available_number_systems()) {
        if (number_system != number_systems[0])
            number_systems.append(number_system);
    }

    return number_systems;
}

}
