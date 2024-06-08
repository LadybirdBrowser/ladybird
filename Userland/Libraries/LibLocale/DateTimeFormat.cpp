/*
 * Copyright (c) 2021-2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/StringBuilder.h>
#include <LibLocale/DateTimeFormat.h>
#include <LibLocale/Locale.h>
#include <LibLocale/NumberFormat.h>
#include <stdlib.h>

namespace Locale {

HourCycle hour_cycle_from_string(StringView hour_cycle)
{
    if (hour_cycle == "h11"sv)
        return HourCycle::H11;
    if (hour_cycle == "h12"sv)
        return HourCycle::H12;
    if (hour_cycle == "h23"sv)
        return HourCycle::H23;
    if (hour_cycle == "h24"sv)
        return HourCycle::H24;
    VERIFY_NOT_REACHED();
}

StringView hour_cycle_to_string(HourCycle hour_cycle)
{
    switch (hour_cycle) {
    case HourCycle::H11:
        return "h11"sv;
    case HourCycle::H12:
        return "h12"sv;
    case HourCycle::H23:
        return "h23"sv;
    case HourCycle::H24:
        return "h24"sv;
    default:
        VERIFY_NOT_REACHED();
    }
}

CalendarPatternStyle calendar_pattern_style_from_string(StringView style)
{
    if (style == "narrow"sv)
        return CalendarPatternStyle::Narrow;
    if (style == "short"sv)
        return CalendarPatternStyle::Short;
    if (style == "long"sv)
        return CalendarPatternStyle::Long;
    if (style == "numeric"sv)
        return CalendarPatternStyle::Numeric;
    if (style == "2-digit"sv)
        return CalendarPatternStyle::TwoDigit;
    if (style == "shortOffset"sv)
        return CalendarPatternStyle::ShortOffset;
    if (style == "longOffset"sv)
        return CalendarPatternStyle::LongOffset;
    if (style == "shortGeneric"sv)
        return CalendarPatternStyle::ShortGeneric;
    if (style == "longGeneric"sv)
        return CalendarPatternStyle::LongGeneric;
    VERIFY_NOT_REACHED();
}

StringView calendar_pattern_style_to_string(CalendarPatternStyle style)
{
    switch (style) {
    case CalendarPatternStyle::Narrow:
        return "narrow"sv;
    case CalendarPatternStyle::Short:
        return "short"sv;
    case CalendarPatternStyle::Long:
        return "long"sv;
    case CalendarPatternStyle::Numeric:
        return "numeric"sv;
    case CalendarPatternStyle::TwoDigit:
        return "2-digit"sv;
    case CalendarPatternStyle::ShortOffset:
        return "shortOffset"sv;
    case CalendarPatternStyle::LongOffset:
        return "longOffset"sv;
    case CalendarPatternStyle::ShortGeneric:
        return "shortGeneric"sv;
    case CalendarPatternStyle::LongGeneric:
        return "longGeneric"sv;
    default:
        VERIFY_NOT_REACHED();
    }
}

Optional<HourCycleRegion> __attribute__((weak)) hour_cycle_region_from_string(StringView) { return {}; }
Vector<HourCycle> __attribute__((weak)) get_regional_hour_cycles(StringView) { return {}; }

template<typename T, typename GetRegionalValues>
static T find_regional_values_for_locale(StringView locale, GetRegionalValues&& get_regional_values)
{
    auto has_value = [](auto const& container) {
        if constexpr (requires { container.has_value(); })
            return container.has_value();
        else
            return !container.is_empty();
    };

    if (auto regional_values = get_regional_values(locale); has_value(regional_values))
        return regional_values;

    auto return_default_values = [&]() { return get_regional_values("001"sv); };

    auto language = parse_unicode_language_id(locale);
    if (!language.has_value())
        return return_default_values();

    if (!language->region.has_value()) {
        if (auto maximized = add_likely_subtags(language->to_string()); maximized.has_value())
            language = parse_unicode_language_id(*maximized);
    }

    if (!language.has_value() || !language->region.has_value())
        return return_default_values();

    if (auto regional_values = get_regional_values(*language->region); has_value(regional_values))
        return regional_values;

    return return_default_values();
}

// https://unicode.org/reports/tr35/tr35-dates.html#Date_Field_Symbol_Table
Vector<HourCycle> get_locale_hour_cycles(StringView locale)
{
    return find_regional_values_for_locale<Vector<HourCycle>>(locale, get_regional_hour_cycles);
}

Optional<HourCycle> get_default_regional_hour_cycle(StringView locale)
{
    if (auto hour_cycles = get_locale_hour_cycles(locale); !hour_cycles.is_empty())
        return hour_cycles.first();
    return {};
}

Optional<MinimumDaysRegion> __attribute__((weak)) minimum_days_region_from_string(StringView) { return {}; }
Optional<u8> __attribute__((weak)) get_regional_minimum_days(StringView) { return {}; }

Optional<u8> get_locale_minimum_days(StringView locale)
{
    return find_regional_values_for_locale<Optional<u8>>(locale, get_regional_minimum_days);
}

Optional<FirstDayRegion> __attribute__((weak)) first_day_region_from_string(StringView) { return {}; }
Optional<Weekday> __attribute__((weak)) get_regional_first_day(StringView) { return {}; }

Optional<Weekday> get_locale_first_day(StringView locale)
{
    return find_regional_values_for_locale<Optional<Weekday>>(locale, get_regional_first_day);
}

Optional<WeekendStartRegion> __attribute__((weak)) weekend_start_region_from_string(StringView) { return {}; }
Optional<Weekday> __attribute__((weak)) get_regional_weekend_start(StringView) { return {}; }

Optional<Weekday> get_locale_weekend_start(StringView locale)
{
    return find_regional_values_for_locale<Optional<Weekday>>(locale, get_regional_weekend_start);
}

Optional<WeekendEndRegion> __attribute__((weak)) weekend_end_region_from_string(StringView) { return {}; }
Optional<Weekday> __attribute__((weak)) get_regional_weekend_end(StringView) { return {}; }

Optional<Weekday> get_locale_weekend_end(StringView locale)
{
    return find_regional_values_for_locale<Optional<Weekday>>(locale, get_regional_weekend_end);
}

String combine_skeletons(StringView first, StringView second)
{
    // https://unicode.org/reports/tr35/tr35-dates.html#availableFormats_appendItems
    constexpr auto field_order = Array {
        "G"sv,       // Era
        "yYuUr"sv,   // Year
        "ML"sv,      // Month
        "dDFg"sv,    // Day
        "Eec"sv,     // Weekday
        "abB"sv,     // Period
        "hHKk"sv,    // Hour
        "m"sv,       // Minute
        "sSA"sv,     // Second
        "zZOvVXx"sv, // Zone
    };

    StringBuilder builder;

    auto append_from_skeleton = [&](auto skeleton, auto ch) {
        auto first_index = skeleton.find(ch);
        if (!first_index.has_value())
            return false;

        auto last_index = skeleton.find_last(ch);

        builder.append(skeleton.substring_view(*first_index, *last_index - *first_index + 1));
        return true;
    };

    for (auto fields : field_order) {
        for (auto ch : fields) {
            if (append_from_skeleton(first, ch))
                break;
            if (append_from_skeleton(second, ch))
                break;
        }
    }

    return MUST(builder.to_string());
}

Optional<CalendarFormat> __attribute__((weak)) get_calendar_date_format(StringView, StringView) { return {}; }
Optional<CalendarFormat> __attribute__((weak)) get_calendar_time_format(StringView, StringView) { return {}; }
Optional<CalendarFormat> __attribute__((weak)) get_calendar_date_time_format(StringView, StringView) { return {}; }

Optional<CalendarFormat> get_calendar_format(StringView locale, StringView calendar, CalendarFormatType type)
{
    switch (type) {
    case CalendarFormatType::Date:
        return get_calendar_date_format(locale, calendar);
    case CalendarFormatType::Time:
        return get_calendar_time_format(locale, calendar);
    case CalendarFormatType::DateTime:
        return get_calendar_date_time_format(locale, calendar);
    default:
        VERIFY_NOT_REACHED();
    }
}

Vector<CalendarPattern> __attribute__((weak)) get_calendar_available_formats(StringView, StringView) { return {}; }
Optional<CalendarRangePattern> __attribute__((weak)) get_calendar_default_range_format(StringView, StringView) { return {}; }
Vector<CalendarRangePattern> __attribute__((weak)) get_calendar_range_formats(StringView, StringView, StringView) { return {}; }
Vector<CalendarRangePattern> __attribute__((weak)) get_calendar_range12_formats(StringView, StringView, StringView) { return {}; }
Optional<StringView> __attribute__((weak)) get_calendar_era_symbol(StringView, StringView, CalendarPatternStyle, Era) { return {}; }
Optional<StringView> __attribute__((weak)) get_calendar_month_symbol(StringView, StringView, CalendarPatternStyle, Month) { return {}; }
Optional<StringView> __attribute__((weak)) get_calendar_weekday_symbol(StringView, StringView, CalendarPatternStyle, Weekday) { return {}; }
Optional<StringView> __attribute__((weak)) get_calendar_day_period_symbol(StringView, StringView, CalendarPatternStyle, DayPeriod) { return {}; }
Optional<StringView> __attribute__((weak)) get_calendar_day_period_symbol_for_hour(StringView, StringView, CalendarPatternStyle, u8) { return {}; }

Optional<StringView> __attribute__((weak)) get_time_zone_name(StringView, StringView, CalendarPatternStyle, TimeZone::InDST) { return {}; }
Optional<TimeZoneFormat> __attribute__((weak)) get_time_zone_format(StringView) { return {}; }

static Optional<String> format_time_zone_offset(StringView locale, CalendarPatternStyle style, i64 offset_seconds)
{
    auto formats = get_time_zone_format(locale);
    if (!formats.has_value())
        return {};

    auto number_system = get_preferred_keyword_value_for_locale(locale, "nu"sv);
    if (!number_system.has_value())
        return {};

    if (offset_seconds == 0)
        return MUST(String::from_utf8(formats->gmt_zero_format));

    auto sign = offset_seconds > 0 ? formats->symbol_ahead_sign : formats->symbol_behind_sign;
    auto separator = offset_seconds > 0 ? formats->symbol_ahead_separator : formats->symbol_behind_separator;
    offset_seconds = llabs(offset_seconds);

    auto offset_hours = offset_seconds / 3'600;
    offset_seconds %= 3'600;

    auto offset_minutes = offset_seconds / 60;
    offset_seconds %= 60;

    StringBuilder builder;
    builder.append(sign);

    switch (style) {
    // The long format always uses 2-digit hours field and minutes field, with optional 2-digit seconds field.
    case CalendarPatternStyle::LongOffset:
        builder.appendff("{:02}{}{:02}", offset_hours, separator, offset_minutes);
        if (offset_seconds > 0)
            builder.appendff("{}{:02}", separator, offset_seconds);
        break;

    // The short format is intended for the shortest representation and uses hour fields without leading zero, with optional 2-digit minutes and seconds fields.
    case CalendarPatternStyle::ShortOffset:
        builder.appendff("{}", offset_hours);
        if (offset_minutes > 0) {
            builder.appendff("{}{:02}", separator, offset_minutes);
            if (offset_seconds > 0)
                builder.appendff("{}{:02}", separator, offset_seconds);
        }
        break;

    default:
        VERIFY_NOT_REACHED();
    }

    // The digits used for hours, minutes and seconds fields in this format are the locale's default decimal digits.
    auto result = replace_digits_for_number_system(*number_system, builder.string_view());
    return MUST(MUST(String::from_utf8(formats->gmt_format)).replace("{0}"sv, result, ReplaceMode::FirstOnly));
}

// https://unicode.org/reports/tr35/tr35-dates.html#Time_Zone_Format_Terminology
String format_time_zone(StringView locale, StringView time_zone, CalendarPatternStyle style, AK::UnixDateTime time)
{
    auto offset = TimeZone::get_time_zone_offset(time_zone, time);
    if (!offset.has_value())
        return MUST(String::from_utf8(time_zone));

    switch (style) {
    case CalendarPatternStyle::Short:
    case CalendarPatternStyle::Long:
    case CalendarPatternStyle::ShortGeneric:
    case CalendarPatternStyle::LongGeneric:
        if (auto name = get_time_zone_name(locale, time_zone, style, offset->in_dst); name.has_value())
            return MUST(String::from_utf8(*name));
        break;

    case CalendarPatternStyle::ShortOffset:
    case CalendarPatternStyle::LongOffset:
        if (auto formatted_offset = format_time_zone_offset(locale, style, offset->seconds); formatted_offset.has_value())
            return formatted_offset.release_value();
        return MUST(String::from_utf8(time_zone));

    default:
        VERIFY_NOT_REACHED();
    }

    // If more styles are added, consult the following table to ensure always falling back to GMT offset is still correct:
    // https://unicode.org/reports/tr35/tr35-dates.html#dfst-zone
    switch (style) {
    case CalendarPatternStyle::Short:
    case CalendarPatternStyle::ShortGeneric:
        return format_time_zone(locale, time_zone, CalendarPatternStyle::ShortOffset, time);

    case CalendarPatternStyle::Long:
    case CalendarPatternStyle::LongGeneric:
        return format_time_zone(locale, time_zone, CalendarPatternStyle::LongOffset, time);

    default:
        VERIFY_NOT_REACHED();
    }
}

}
