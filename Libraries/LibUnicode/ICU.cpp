/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Utf16View.h>
#include <LibUnicode/ICU.h>

#include <unicode/dtptngen.h>
#include <unicode/locdspnm.h>
#include <unicode/numsys.h>
#include <unicode/tznames.h>

namespace Unicode {

static HashMap<String, OwnPtr<LocaleData>> s_locale_cache;
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
        VERIFY(icu_success(status));

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
            VERIFY(icu_success(status));
        }
    }

    return *m_numbering_system;
}

icu::DateTimePatternGenerator& LocaleData::date_time_pattern_generator()
{
    if (!m_date_time_pattern_generator) {
        UErrorCode status = U_ZERO_ERROR;

        m_date_time_pattern_generator = adopt_own(*icu::DateTimePatternGenerator::createInstance(locale(), status));
        VERIFY(icu_success(status));
    }

    return *m_date_time_pattern_generator;
}

icu::TimeZoneNames& LocaleData::time_zone_names()
{
    if (!m_time_zone_names) {
        UErrorCode status = U_ZERO_ERROR;

        m_time_zone_names = adopt_own(*icu::TimeZoneNames::createInstance(locale(), status));
        VERIFY(icu_success(status));
    }

    return *m_time_zone_names;
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

Vector<icu::UnicodeString> icu_string_list(ReadonlySpan<String> strings)
{
    Vector<icu::UnicodeString> result;
    result.ensure_capacity(strings.size());

    for (auto const& string : strings) {
        auto view = string.bytes_as_string_view();
        icu::UnicodeString icu_string(view.characters_without_null_termination(), static_cast<i32>(view.length()));
        result.unchecked_append(move(icu_string));
    }

    return result;
}

String icu_string_to_string(icu::UnicodeString const& string)
{
    return icu_string_to_string(string.getBuffer(), string.length());
}

String icu_string_to_string(UChar const* string, i32 length)
{
    ReadonlySpan<u16> view { reinterpret_cast<u16 const*>(string), static_cast<size_t>(length) };
    return MUST(Utf16View { view }.to_utf8());
}

}
