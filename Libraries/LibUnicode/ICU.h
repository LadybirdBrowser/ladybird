/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibUnicode/DurationFormat.h>

#include <unicode/locid.h>
#include <unicode/strenum.h>
#include <unicode/stringpiece.h>
#include <unicode/unistr.h>
#include <unicode/utypes.h>
#include <unicode/uversion.h>

U_NAMESPACE_BEGIN
class DateTimePatternGenerator;
class LocaleDisplayNames;
class NumberingSystem;
class TimeZone;
class TimeZoneNames;
U_NAMESPACE_END

namespace Unicode {

class LocaleData {
public:
    static Optional<LocaleData&> for_locale(StringView locale);

    ALWAYS_INLINE icu::Locale& locale() { return m_locale; }

    String to_string();

    icu::LocaleDisplayNames& standard_display_names();
    icu::LocaleDisplayNames& dialect_display_names();

    icu::NumberingSystem& numbering_system();

    icu::DateTimePatternGenerator& date_time_pattern_generator();

    icu::TimeZoneNames& time_zone_names();

    Optional<DigitalFormat> const& digital_format() { return m_digital_format; }
    void set_digital_format(DigitalFormat digital_format) { m_digital_format = move(digital_format); }

private:
    explicit LocaleData(icu::Locale locale);

    icu::Locale m_locale;
    Optional<String> m_locale_string;

    OwnPtr<icu::LocaleDisplayNames> m_standard_display_names;
    OwnPtr<icu::LocaleDisplayNames> m_dialect_display_names;
    OwnPtr<icu::NumberingSystem> m_numbering_system;
    OwnPtr<icu::DateTimePatternGenerator> m_date_time_pattern_generator;
    OwnPtr<icu::TimeZoneNames> m_time_zone_names;
    Optional<DigitalFormat> m_digital_format;
};

class TimeZoneData {
public:
    static Optional<TimeZoneData&> for_time_zone(StringView time_zone);

    ALWAYS_INLINE icu::TimeZone& time_zone() { return *m_time_zone; }

private:
    explicit TimeZoneData(NonnullOwnPtr<icu::TimeZone>);

    NonnullOwnPtr<icu::TimeZone> m_time_zone;
};

constexpr bool icu_success(UErrorCode code)
{
    return static_cast<bool>(U_SUCCESS(code));
}

constexpr bool icu_failure(UErrorCode code)
{
    return static_cast<bool>(U_FAILURE(code));
}

ALWAYS_INLINE icu::StringPiece icu_string_piece(StringView string)
{
    return { string.characters_without_null_termination(), static_cast<i32>(string.length()) };
}

ALWAYS_INLINE icu::UnicodeString icu_string(StringView string)
{
    return icu::UnicodeString::fromUTF8(icu_string_piece(string));
}

Vector<icu::UnicodeString> icu_string_list(ReadonlySpan<String> strings);

String icu_string_to_string(icu::UnicodeString const& string);
String icu_string_to_string(UChar const*, i32 length);

template<typename Filter>
Vector<String> icu_string_enumeration_to_list(OwnPtr<icu::StringEnumeration> enumeration, Filter&& filter)
{
    UErrorCode status = U_ZERO_ERROR;
    Vector<String> result;

    if (!enumeration)
        return {};

    while (true) {
        i32 length = 0;
        auto const* keyword = enumeration->next(&length, status);

        if (icu_failure(status) || keyword == nullptr)
            break;

        if (!filter(keyword))
            continue;

        result.append(MUST(String::from_utf8({ keyword, static_cast<size_t>(length) })));
    }

    return result;
}

ALWAYS_INLINE Vector<String> icu_string_enumeration_to_list(OwnPtr<icu::StringEnumeration> enumeration)
{
    return icu_string_enumeration_to_list(move(enumeration), [](char const*) { return true; });
}

}
