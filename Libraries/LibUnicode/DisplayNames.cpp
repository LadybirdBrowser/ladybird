/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibUnicode/DisplayNames.h>
#include <LibUnicode/ICU.h>

#include <unicode/dtptngen.h>
#include <unicode/localebuilder.h>
#include <unicode/locdspnm.h>
#include <unicode/tznames.h>
#include <unicode/udatpg.h>

namespace Unicode {

LanguageDisplay language_display_from_string(StringView language_display)
{
    if (language_display == "standard"sv)
        return LanguageDisplay::Standard;
    if (language_display == "dialect"sv)
        return LanguageDisplay::Dialect;
    VERIFY_NOT_REACHED();
}

StringView language_display_to_string(LanguageDisplay language_display)
{
    switch (language_display) {
    case LanguageDisplay::Standard:
        return "standard"sv;
    case LanguageDisplay::Dialect:
        return "dialect"sv;
    default:
        VERIFY_NOT_REACHED();
    }
}

Optional<String> language_display_name(StringView locale, StringView language, LanguageDisplay display)
{
    auto locale_data = LocaleData::for_locale(locale);
    if (!locale_data.has_value())
        return {};

    auto language_data = LocaleData::for_locale(language);
    if (!language_data.has_value())
        return {};

    auto& display_names = display == LanguageDisplay::Standard
        ? locale_data->standard_display_names()
        : locale_data->dialect_display_names();

    icu::UnicodeString result;
    display_names.localeDisplayName(language_data->locale().getName(), result);

    return icu_string_to_string(result);
}

Optional<String> region_display_name(StringView locale, StringView region)
{
    UErrorCode status = U_ZERO_ERROR;

    auto locale_data = LocaleData::for_locale(locale);
    if (!locale_data.has_value())
        return {};

    auto icu_region = icu::LocaleBuilder().setRegion(icu_string_piece(region)).build(status);
    if (icu_failure(status))
        return {};

    icu::UnicodeString result;
    locale_data->standard_display_names().regionDisplayName(icu_region.getCountry(), result);

    return icu_string_to_string(result);
}

Optional<String> script_display_name(StringView locale, StringView script)
{
    UErrorCode status = U_ZERO_ERROR;

    auto locale_data = LocaleData::for_locale(locale);
    if (!locale_data.has_value())
        return {};

    auto icu_script = icu::LocaleBuilder().setScript(icu_string_piece(script)).build(status);
    if (icu_failure(status))
        return {};

    icu::UnicodeString result;
    locale_data->standard_display_names().scriptDisplayName(icu_script.getScript(), result);

    return icu_string_to_string(result);
}

Optional<String> calendar_display_name(StringView locale, StringView calendar)
{
    auto locale_data = LocaleData::for_locale(locale);
    if (!locale_data.has_value())
        return {};

    if (calendar == "gregory"sv)
        calendar = "gregorian"sv;
    if (calendar == "islamicc"sv)
        calendar = "islamic-civil"sv;
    if (calendar == "ethioaa"sv)
        calendar = "ethiopic-amete-alem"sv;

    icu::UnicodeString result;
    locale_data->standard_display_names().keyValueDisplayName("calendar", ByteString(calendar).characters(), result);

    return icu_string_to_string(result);
}

static constexpr UDateTimePatternField icu_date_time_field(StringView field)
{
    if (field == "day"sv)
        return UDATPG_DAY_FIELD;
    if (field == "dayPeriod"sv)
        return UDATPG_DAYPERIOD_FIELD;
    if (field == "era"sv)
        return UDATPG_ERA_FIELD;
    if (field == "hour"sv)
        return UDATPG_HOUR_FIELD;
    if (field == "minute"sv)
        return UDATPG_MINUTE_FIELD;
    if (field == "month"sv)
        return UDATPG_MONTH_FIELD;
    if (field == "quarter"sv)
        return UDATPG_QUARTER_FIELD;
    if (field == "second"sv)
        return UDATPG_SECOND_FIELD;
    if (field == "timeZoneName"sv)
        return UDATPG_ZONE_FIELD;
    if (field == "weekOfYear"sv)
        return UDATPG_WEEK_OF_YEAR_FIELD;
    if (field == "weekday"sv)
        return UDATPG_WEEKDAY_FIELD;
    if (field == "year"sv)
        return UDATPG_YEAR_FIELD;
    VERIFY_NOT_REACHED();
}

static constexpr UDateTimePGDisplayWidth icu_date_time_style(Style style)
{
    switch (style) {
    case Style::Long:
        return UDATPG_WIDE;
    case Style::Short:
        return UDATPG_ABBREVIATED;
    case Style::Narrow:
        return UDATPG_NARROW;
    }

    VERIFY_NOT_REACHED();
}

Optional<String> date_time_field_display_name(StringView locale, StringView field, Style style)
{
    auto locale_data = LocaleData::for_locale(locale);
    if (!locale_data.has_value())
        return {};

    auto icu_field = icu_date_time_field(field);
    auto icu_style = icu_date_time_style(style);

    icu::UnicodeString result;
    result = locale_data->date_time_pattern_generator().getFieldDisplayName(icu_field, icu_style);

    return icu_string_to_string(result);
}

Optional<String> time_zone_display_name(StringView locale, StringView time_zone_identifier, TimeZoneOffset::InDST in_dst, double time)
{
    auto locale_data = LocaleData::for_locale(locale);
    if (!locale_data.has_value())
        return {};

    icu::UnicodeString time_zone_name;
    auto type = in_dst == TimeZoneOffset::InDST::Yes ? UTZNM_LONG_DAYLIGHT : UTZNM_LONG_STANDARD;

    locale_data->time_zone_names().getDisplayName(icu_string(time_zone_identifier), type, time, time_zone_name);
    if (static_cast<bool>(time_zone_name.isBogus()))
        return {};

    return icu_string_to_string(time_zone_name);
}

static constexpr Array<UChar, 4> icu_currency_code(StringView currency)
{
    VERIFY(currency.length() == 3);

    return to_array({
        static_cast<UChar>(currency[0]),
        static_cast<UChar>(currency[1]),
        static_cast<UChar>(currency[2]),
        u'\0',
    });
}

static constexpr UCurrNameStyle icu_currency_style(Style style)
{
    switch (style) {
    case Style::Long:
        return UCURR_LONG_NAME;
    case Style::Short:
        return UCURR_SYMBOL_NAME;
    case Style::Narrow:
        return UCURR_NARROW_SYMBOL_NAME;
    }

    VERIFY_NOT_REACHED();
}

Optional<String> currency_display_name(StringView locale, StringView currency, Style style)
{
    UErrorCode status = U_ZERO_ERROR;

    auto locale_data = LocaleData::for_locale(locale);
    if (!locale_data.has_value())
        return {};

    auto icu_currency = icu_currency_code(currency);

    i32 length = 0;
    UChar const* result = ucurr_getName(icu_currency.data(), locale_data->locale().getName(), icu_currency_style(style), nullptr, &length, &status);

    if (icu_failure(status))
        return {};
    if ((status == U_USING_DEFAULT_WARNING) && (result == icu_currency.data()))
        return {};

    return icu_string_to_string(result, length);
}

Optional<String> currency_numeric_display_name(StringView locale, StringView currency)
{
    UErrorCode status = U_ZERO_ERROR;

    auto locale_data = LocaleData::for_locale(locale);
    if (!locale_data.has_value())
        return {};

    auto icu_currency = icu_currency_code(currency);

    i32 length = 0;
    UChar const* result = ucurr_getPluralName(icu_currency.data(), locale_data->locale().getName(), nullptr, "other", &length, &status);

    if (icu_failure(status))
        return {};
    if ((status == U_USING_DEFAULT_WARNING) && (result == icu_currency.data()))
        return {};

    return icu_string_to_string(result, length);
}

}
