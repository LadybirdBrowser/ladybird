/*
 * Copyright (c) 2021-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Time.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibLocale/Forward.h>
#include <LibTimeZone/TimeZone.h>

namespace Locale {

enum class DateTimeStyle {
    Full,
    Long,
    Medium,
    Short,
};
DateTimeStyle date_time_style_from_string(StringView);
StringView date_time_style_to_string(DateTimeStyle);

enum class Weekday : u8 {
    Sunday,
    Monday,
    Tuesday,
    Wednesday,
    Thursday,
    Friday,
    Saturday,
};

enum class HourCycle : u8 {
    H11,
    H12,
    H23,
    H24,
};
HourCycle hour_cycle_from_string(StringView hour_cycle);
StringView hour_cycle_to_string(HourCycle hour_cycle);

enum class CalendarPatternStyle : u8 {
    Narrow,
    Short,
    Long,
    Numeric,
    TwoDigit,
    ShortOffset,
    LongOffset,
    ShortGeneric,
    LongGeneric,
};
CalendarPatternStyle calendar_pattern_style_from_string(StringView style);
StringView calendar_pattern_style_to_string(CalendarPatternStyle style);

struct CalendarPattern {
    static CalendarPattern create_from_pattern(StringView);
    String to_pattern() const;

    template<typename Callback>
    void for_each_calendar_field_zipped_with(CalendarPattern const& other, Callback&& callback)
    {
        callback(hour_cycle, other.hour_cycle);
        callback(era, other.era);
        callback(year, other.year);
        callback(month, other.month);
        callback(weekday, other.weekday);
        callback(day, other.day);
        callback(day_period, other.day_period);
        callback(hour, other.hour);
        callback(minute, other.minute);
        callback(second, other.second);
        callback(fractional_second_digits, other.fractional_second_digits);
        callback(time_zone_name, other.time_zone_name);
    }

    Optional<HourCycle> hour_cycle;
    Optional<bool> hour12;

    // https://unicode.org/reports/tr35/tr35-dates.html#Calendar_Fields
    Optional<CalendarPatternStyle> era;
    Optional<CalendarPatternStyle> year;
    Optional<CalendarPatternStyle> month;
    Optional<CalendarPatternStyle> weekday;
    Optional<CalendarPatternStyle> day;
    Optional<CalendarPatternStyle> day_period;
    Optional<CalendarPatternStyle> hour;
    Optional<CalendarPatternStyle> minute;
    Optional<CalendarPatternStyle> second;
    Optional<u8> fractional_second_digits;
    Optional<CalendarPatternStyle> time_zone_name;
};

struct TimeZoneFormat {
    StringView symbol_ahead_sign {};
    StringView symbol_ahead_separator {};

    StringView symbol_behind_sign {};
    StringView symbol_behind_separator {};

    StringView gmt_format {};
    StringView gmt_zero_format {};
};

Optional<HourCycleRegion> hour_cycle_region_from_string(StringView hour_cycle_region);
Vector<HourCycle> get_regional_hour_cycles(StringView region);
Vector<HourCycle> get_locale_hour_cycles(StringView locale);
Optional<HourCycle> get_default_regional_hour_cycle(StringView locale);

Optional<MinimumDaysRegion> minimum_days_region_from_string(StringView minimum_days_region);
Optional<u8> get_regional_minimum_days(StringView region);
Optional<u8> get_locale_minimum_days(StringView locale);

Optional<FirstDayRegion> first_day_region_from_string(StringView first_day_region);
Optional<Weekday> get_regional_first_day(StringView region);
Optional<Weekday> get_locale_first_day(StringView locale);

Optional<WeekendStartRegion> weekend_start_region_from_string(StringView weekend_start_region);
Optional<Weekday> get_regional_weekend_start(StringView region);
Optional<Weekday> get_locale_weekend_start(StringView locale);

Optional<WeekendEndRegion> weekend_end_region_from_string(StringView weekend_end_region);
Optional<Weekday> get_regional_weekend_end(StringView region);
Optional<Weekday> get_locale_weekend_end(StringView locale);

Optional<StringView> get_time_zone_name(StringView locale, StringView time_zone, CalendarPatternStyle style, TimeZone::InDST in_dst);
Optional<TimeZoneFormat> get_time_zone_format(StringView locale);

class DateTimeFormat {
public:
    static NonnullOwnPtr<DateTimeFormat> create_for_date_and_time_style(
        StringView locale,
        StringView time_zone_identifier,
        Optional<HourCycle> const& hour_cycle,
        Optional<bool> const& hour12,
        Optional<DateTimeStyle> const& date_style,
        Optional<DateTimeStyle> const& time_style);

    static NonnullOwnPtr<DateTimeFormat> create_for_pattern_options(
        StringView locale,
        StringView time_zone_identifier,
        CalendarPattern const&);

    virtual ~DateTimeFormat() = default;

    struct Partition {
        StringView type;
        String value;
        StringView source;
    };

    virtual CalendarPattern const& chosen_pattern() const = 0;

    virtual String format(double) const = 0;
    virtual Vector<Partition> format_to_parts(double) const = 0;

    virtual String format_range(double, double) const = 0;
    virtual Vector<Partition> format_range_to_parts(double, double) const = 0;

protected:
    DateTimeFormat() = default;
};

}
