/*
 * Copyright (c) 2024-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Utf16View.h>
#include <LibUnicode/ICU.h>

#include <unicode/dtptngen.h>
#include <unicode/gregocal.h>
#include <unicode/locdspnm.h>
#include <unicode/numsys.h>
#include <unicode/tznames.h>

namespace Unicode {

static HashMap<String, OwnPtr<LocaleData>> s_locale_cache;
static HashMap<String, OwnPtr<CalendarData>> s_calendar_cache;
static HashMap<String, OwnPtr<TimeZoneData>> s_time_zone_cache;

Optional<LocaleData&> LocaleData::for_locale(StringView locale)
{
    auto locale_data = s_locale_cache.get(locale);

    if (!locale_data.has_value()) {
        locale_data = s_locale_cache.ensure(MUST(String::from_utf8(locale)), [&]() -> OwnPtr<LocaleData> {
            UErrorCode status = U_ZERO_ERROR;

            auto icu_locale = icu::Locale::forLanguageTag(icu_string_piece(locale), status);
            if (icu_failure(status))
                return nullptr;

            return adopt_own(*new LocaleData { move(icu_locale) });
        });
    }

    if (locale_data.value())
        return *locale_data.value();
    return {};
}

LocaleData::LocaleData(icu::Locale locale)
    : m_locale(move(locale))
{
}

String LocaleData::to_string()
{
    if (!m_locale_string.has_value()) {
        UErrorCode status = U_ZERO_ERROR;

        auto result = locale().toLanguageTag<StringBuilder>(status);
        verify_icu_success(status);

        m_locale_string = MUST(result.to_string());
    }

    return *m_locale_string;
}

icu::LocaleDisplayNames& LocaleData::standard_display_names()
{
    if (!m_standard_display_names)
        m_standard_display_names = adopt_own(*icu::LocaleDisplayNames::createInstance(locale()));
    return *m_standard_display_names;
}

icu::LocaleDisplayNames& LocaleData::dialect_display_names()
{
    if (!m_dialect_display_names)
        m_dialect_display_names = adopt_own(*icu::LocaleDisplayNames::createInstance(locale(), ULDN_DIALECT_NAMES));
    return *m_dialect_display_names;
}

icu::NumberingSystem& LocaleData::numbering_system()
{
    if (!m_numbering_system) {
        UErrorCode status = U_ZERO_ERROR;
        m_numbering_system = adopt_own_if_nonnull(icu::NumberingSystem::createInstance(locale(), status));

        if (icu_failure(status)) {
            status = U_ZERO_ERROR;

            m_numbering_system = adopt_own_if_nonnull(icu::NumberingSystem::createInstance("und", status));
            verify_icu_success(status);
        }
    }

    return *m_numbering_system;
}

icu::DateTimePatternGenerator& LocaleData::date_time_pattern_generator()
{
    if (!m_date_time_pattern_generator) {
        UErrorCode status = U_ZERO_ERROR;

        m_date_time_pattern_generator = adopt_own(*icu::DateTimePatternGenerator::createInstance(locale(), status));
        verify_icu_success(status);
    }

    return *m_date_time_pattern_generator;
}

icu::TimeZoneNames& LocaleData::time_zone_names()
{
    if (!m_time_zone_names) {
        UErrorCode status = U_ZERO_ERROR;

        m_time_zone_names = adopt_own(*icu::TimeZoneNames::createInstance(locale(), status));
        verify_icu_success(status);
    }

    return *m_time_zone_names;
}

Optional<CalendarData&> CalendarData::for_calendar(String const& calendar)
{
    auto& calendar_data = s_calendar_cache.ensure(calendar, [&]() -> OwnPtr<CalendarData> {
        UErrorCode status = U_ZERO_ERROR;

        auto const* legacy_calendar = uloc_toLegacyType("calendar", ByteString(calendar).characters());
        if (!legacy_calendar)
            return nullptr;

        auto locale_data = LocaleData::for_locale("und"sv);
        VERIFY(locale_data.has_value());

        locale_data->locale().setKeywordValue("calendar", legacy_calendar, status);
        if (icu_failure(status))
            return nullptr;

        auto icu_calendar = adopt_own_if_nonnull(icu::Calendar::createInstance(locale_data->locale(), status));
        if (icu_failure(status))
            return nullptr;

        return adopt_own(*new CalendarData { icu_calendar.release_nonnull() });
    });

    if (calendar_data)
        return *calendar_data;
    return {};
}

void CalendarData::adjust_time_range_for_proleptic_calendar(icu::Calendar& icu_calendar)
{
    // https://tc39.es/ecma262/#sec-time-values-and-time-range
    // A time value supports a slightly smaller range of -8,640,000,000,000,000 to 8,640,000,000,000,000 milliseconds.
    static constexpr UDate ECMA_262_MINIMUM_TIME = -8.64E15;
    UErrorCode status = U_ZERO_ERROR;

    if (auto* gregorian_calendar = as_if<icu::GregorianCalendar>(icu_calendar)) {
        gregorian_calendar->setGregorianChange(ECMA_262_MINIMUM_TIME, status);
        verify_icu_success(status);
    }
}

CalendarData::CalendarData(NonnullOwnPtr<icu::Calendar> calendar)
    : m_calendar(move(calendar))
{
    adjust_time_range_for_proleptic_calendar(*m_calendar);
}

Optional<TimeZoneData&> TimeZoneData::for_time_zone(StringView time_zone)
{
    auto time_zone_data = s_time_zone_cache.get(time_zone);

    if (!time_zone_data.has_value()) {
        time_zone_data = s_time_zone_cache.ensure(MUST(String::from_utf8(time_zone)), [&]() -> OwnPtr<TimeZoneData> {
            auto icu_time_zone = adopt_own_if_nonnull(icu::TimeZone::createTimeZone(icu_string(time_zone)));
            if (!icu_time_zone || *icu_time_zone == icu::TimeZone::getUnknown())
                return nullptr;

            return adopt_own(*new TimeZoneData { icu_time_zone.release_nonnull() });
        });
    }

    if (time_zone_data.value())
        return *time_zone_data.value();
    return {};
}

TimeZoneData::TimeZoneData(NonnullOwnPtr<icu::TimeZone> time_zone)
    : m_time_zone(move(time_zone))
{
}

Vector<icu::UnicodeString> icu_string_list(ReadonlySpan<Utf16String> strings)
{
    Vector<icu::UnicodeString> result;
    result.ensure_capacity(strings.size());

    for (auto const& string : strings)
        result.unchecked_append(icu_string(string));

    return result;
}

String icu_string_to_string(icu::UnicodeString const& string)
{
    return icu_string_to_string(string.getBuffer(), string.length());
}

String icu_string_to_string(UChar const* string, i32 length)
{
    return MUST(Utf16View { string, static_cast<size_t>(length) }.to_utf8());
}

Utf16String icu_string_to_utf16_string(icu::UnicodeString const& string)
{
    return icu_string_to_utf16_string(string.getBuffer(), string.length());
}

Utf16String icu_string_to_utf16_string(UChar const* string, i32 length)
{
    return Utf16String::from_utf16({ string, static_cast<size_t>(length) });
}

Utf16View icu_string_to_utf16_view(icu::UnicodeString const& string)
{
    return { string.getBuffer(), static_cast<size_t>(string.length()) };
}

UCharIterator icu_string_iterator(Utf16View const& string)
{
    UCharIterator iterator;

    if (string.has_ascii_storage())
        uiter_setUTF8(&iterator, string.ascii_span().data(), static_cast<i32>(string.length_in_code_units()));
    else
        uiter_setString(&iterator, string.utf16_span().data(), static_cast<i32>(string.length_in_code_units()));

    return iterator;
}

}
