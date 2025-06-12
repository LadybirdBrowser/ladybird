/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <LibUnicode/Locale.h>
#include <LibUnicode/TimeZone.h>

namespace Unicode {

enum class LanguageDisplay {
    Standard,
    Dialect,
};

LanguageDisplay language_display_from_string(StringView language_display);
StringView language_display_to_string(LanguageDisplay language_display);

Optional<String> language_display_name(StringView locale, StringView language, LanguageDisplay);
Optional<String> region_display_name(StringView locale, StringView region);
Optional<String> script_display_name(StringView locale, StringView script);
Optional<String> calendar_display_name(StringView locale, StringView calendar);
Optional<String> date_time_field_display_name(StringView locale, StringView field, Style);
Optional<String> time_zone_display_name(StringView locale, StringView time_zone_identifier, TimeZoneOffset::InDST, double time);
Optional<String> currency_display_name(StringView locale, StringView currency, Style);
Optional<String> currency_numeric_display_name(StringView locale, StringView currency);

}
