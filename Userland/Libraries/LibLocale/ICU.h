/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#define AK_DONT_REPLACE_STD

#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/String.h>
#include <AK/StringView.h>

#include <unicode/locid.h>
#include <unicode/stringpiece.h>
#include <unicode/utypes.h>
#include <unicode/uversion.h>

U_NAMESPACE_BEGIN
class DateTimePatternGenerator;
class LocaleDisplayNames;
class TimeZoneNames;
class UnicodeString;
U_NAMESPACE_END

namespace Locale {

class LocaleData {
public:
    static Optional<LocaleData&> for_locale(StringView locale);

    ALWAYS_INLINE icu::Locale& locale() { return m_locale; }

    String to_string();

    icu::LocaleDisplayNames& standard_display_names();
    icu::LocaleDisplayNames& dialect_display_names();

    icu::DateTimePatternGenerator& date_time_pattern_generator();

    icu::TimeZoneNames& time_zone_names();

private:
    explicit LocaleData(icu::Locale locale);

    icu::Locale m_locale;
    Optional<String> m_locale_string;

    OwnPtr<icu::LocaleDisplayNames> m_standard_display_names;
    OwnPtr<icu::LocaleDisplayNames> m_dialect_display_names;
    OwnPtr<icu::DateTimePatternGenerator> m_date_time_pattern_generator;
    OwnPtr<icu::TimeZoneNames> m_time_zone_names;
};

static constexpr bool icu_success(UErrorCode code)
{
    return static_cast<bool>(U_SUCCESS(code));
}

static constexpr bool icu_failure(UErrorCode code)
{
    return static_cast<bool>(U_FAILURE(code));
}

icu::UnicodeString icu_string(StringView string);
icu::StringPiece icu_string_piece(StringView string);

Vector<icu::UnicodeString> icu_string_list(ReadonlySpan<String> strings);

String icu_string_to_string(icu::UnicodeString const& string);
String icu_string_to_string(UChar const*, i32 length);

}
