/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AK_DONT_REPLACE_STD

#include <AK/NonnullOwnPtr.h>
#include <LibUnicode/ICU.h>
#include <LibUnicode/TimeZone.h>

#include <unicode/timezone.h>

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

}
