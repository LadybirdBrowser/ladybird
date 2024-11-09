/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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

Vector<String> available_keyword_values(StringView locale, StringView key)
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

Vector<String> const& available_calendars()
{
    static auto calendars = []() {
        auto calendars = available_calendars("und"sv);

        quick_sort(calendars);
        return calendars;
    }();

    return calendars;
}

Vector<String> available_calendars(StringView locale)
{
    UErrorCode status = U_ZERO_ERROR;

    auto locale_data = LocaleData::for_locale(locale);
    if (!locale_data.has_value())
        return {};

    auto keywords = adopt_own_if_nonnull(icu::Calendar::getKeywordValuesForLocale("calendar", locale_data->locale(), 0, status));
    if (icu_failure(status))
        return {};

    auto calendars = icu_string_enumeration_to_list(move(keywords));

    for (auto& calendar : calendars) {
        if (calendar == "gregorian"sv)
            calendar = "gregory"_string;
        else if (calendar == "ethiopic-amete-alem"sv)
            calendar = "ethioaa"_string;
    }

    return calendars;
}

Vector<String> const& available_currencies()
{
    static auto currencies = []() -> Vector<String> {
        UErrorCode status = U_ZERO_ERROR;

        auto* currencies = ucurr_openISOCurrencies(UCURR_ALL, &status);
        ScopeGuard guard { [&]() { uenum_close(currencies); } };

        if (icu_failure(status))
            return {};

        Vector<String> result;

        while (true) {
            i32 length = 0;
            char const* next = uenum_next(currencies, &length, &status);

            if (icu_failure(status))
                return {};
            if (next == nullptr)
                break;

            // https://unicode-org.atlassian.net/browse/ICU-21687
            if (StringView currency { next, static_cast<size_t>(length) }; currency != "LSM"sv)
                result.append(MUST(String::from_utf8(currency)));
        }

        quick_sort(result);
        return result;
    }();

    return currencies;
}

Vector<String> const& available_collation_case_orderings()
{
    static Vector<String> case_orderings { "false"_string, "lower"_string, "upper"_string };
    return case_orderings;
}

Vector<String> const& available_collation_numeric_orderings()
{
    static Vector<String> case_orderings { "false"_string, "true"_string };
    return case_orderings;
}

Vector<String> const& available_collations()
{
    // FIXME: Implement this when we fully support Intl.Collator.
    static Vector<String> collations { "default"_string };
    return collations;
}

Vector<String> available_collations(StringView)
{
    // FIXME: Implement this when we fully support Intl.Collator.
    return available_collations();
}

Vector<String> const& available_hour_cycles()
{
    static Vector<String> case_orderings { "h11"_string, "h12"_string, "h23"_string, "h24"_string };
    return case_orderings;
}

Vector<String> available_hour_cycles(StringView locale)
{
    auto preferred_hour_cycle = default_hour_cycle(locale);
    if (!preferred_hour_cycle.has_value())
        return available_hour_cycles();

    Vector<String> hour_cycles;
    hour_cycles.append(MUST(String::from_utf8(hour_cycle_to_string(*preferred_hour_cycle))));

    for (auto const& hour_cycle : available_hour_cycles()) {
        if (hour_cycle != hour_cycles[0])
            hour_cycles.append(hour_cycle);
    }

    return hour_cycles;
}

Vector<String> const& available_number_systems()
{
    static auto number_systems = []() -> Vector<String> {
        UErrorCode status = U_ZERO_ERROR;

        auto keywords = adopt_own_if_nonnull(icu::NumberingSystem::getAvailableNames(status));
        if (icu_failure(status))
            return {};

        auto number_systems = icu_string_enumeration_to_list(move(keywords), [&](char const* keyword) {
            auto system = adopt_own_if_nonnull(icu::NumberingSystem::createInstanceByName(keyword, status));
            if (icu_failure(status))
                return false;

            return !static_cast<bool>(system->isAlgorithmic());
        });

        quick_sort(number_systems);
        return number_systems;
    }();

    return number_systems;
}

Vector<String> available_number_systems(StringView locale)
{
    auto locale_data = LocaleData::for_locale(locale);
    if (!locale_data.has_value())
        return {};

    Vector<String> number_systems;

    auto const* preferred_number_system = locale_data->numbering_system().getName();
    number_systems.append(MUST(String::from_utf8({ preferred_number_system, strlen(preferred_number_system) })));

    for (auto const& number_system : available_number_systems()) {
        if (number_system != number_systems[0])
            number_systems.append(number_system);
    }

    return number_systems;
}

}
