/*
 * Copyright (c) 2024-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/StringView.h>
#include <AK/Utf16String.h>
#include <LibUnicode/Locale.h>
#include <LibUnicode/TimeZone.h>

namespace Unicode {

enum class LanguageDisplay {
    Standard,
    Dialect,
};

LanguageDisplay language_display_from_string(StringView language_display);
StringView language_display_to_string(LanguageDisplay language_display);

Optional<Utf16String> language_display_name(StringView locale, StringView language, LanguageDisplay);
Optional<Utf16String> region_display_name(StringView locale, StringView region);
Optional<Utf16String> script_display_name(StringView locale, StringView script);
Optional<Utf16String> calendar_display_name(StringView locale, StringView calendar);
Optional<Utf16String> date_time_field_display_name(StringView locale, StringView field, Style);
Optional<Utf16String> time_zone_display_name(StringView locale, StringView time_zone_identifier, TimeZoneOffset::InDST, double time);
Optional<Utf16String> currency_display_name(StringView locale, StringView currency, Style);
Optional<Utf16String> currency_numeric_display_name(StringView locale, StringView currency);

}
