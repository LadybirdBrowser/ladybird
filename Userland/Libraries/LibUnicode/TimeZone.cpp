/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AK_DONT_REPLACE_STD

#include <AK/Array.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/QuickSort.h>
#include <LibUnicode/ICU.h>
#include <LibUnicode/TimeZone.h>

#include <unicode/timezone.h>
#include <unicode/ucal.h>

namespace Unicode {

String current_time_zone()
{
    UErrorCode status = U_ZERO_ERROR;

    auto time_zone = adopt_own_if_nonnull(icu::TimeZone::detectHostTimeZone());
    if (!time_zone)
        return "UTC"_string;

    icu::UnicodeString time_zone_id;
    time_zone->getID(time_zone_id);

    icu::UnicodeString time_zone_name;
    time_zone->getCanonicalID(time_zone_id, time_zone_name, status);

    if (icu_failure(status))
        return "UTC"_string;

    return icu_string_to_string(time_zone_name);
}

// https://github.com/unicode-org/icu/blob/main/icu4c/source/tools/tzcode/icuzones
static constexpr bool is_legacy_non_iana_time_zone(StringView time_zone)
{
    constexpr auto legacy_zones = to_array({
        "ACT"sv,
        "AET"sv,
        "AGT"sv,
        "ART"sv,
        "AST"sv,
        "BET"sv,
        "BST"sv,
        "Canada/East-Saskatchewan"sv,
        "CAT"sv,
        "CNT"sv,
        "CST"sv,
        "CTT"sv,
        "EAT"sv,
        "ECT"sv,
        "IET"sv,
        "IST"sv,
        "JST"sv,
        "MIT"sv,
        "NET"sv,
        "NST"sv,
        "PLT"sv,
        "PNT"sv,
        "PRT"sv,
        "PST"sv,
        "SST"sv,
        "US/Pacific-New"sv,
        "VST"sv,
    });

    if (time_zone.starts_with("SystemV/"sv))
        return true;

    return legacy_zones.contains_slow(time_zone);
}

Vector<String> const& available_time_zones()
{
    static auto time_zones = []() -> Vector<String> {
        UErrorCode status = U_ZERO_ERROR;

        auto time_zone_enumerator = adopt_own_if_nonnull(icu::TimeZone::createEnumeration(status));
        if (icu_failure(status))
            return { "UTC"_string };

        auto time_zones = icu_string_enumeration_to_list(move(time_zone_enumerator), [](char const* zone) {
            return !is_legacy_non_iana_time_zone({ zone, strlen(zone) });
        });

        quick_sort(time_zones);
        return time_zones;
    }();

    return time_zones;
}

Optional<String> resolve_primary_time_zone(StringView time_zone)
{
    UErrorCode status = U_ZERO_ERROR;

    icu::UnicodeString iana_id;
    icu::TimeZone::getIanaID(icu_string(time_zone), iana_id, status);

    if (icu_failure(status))
        return {};

    return icu_string_to_string(iana_id);
}

}
