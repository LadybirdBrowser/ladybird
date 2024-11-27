/*
 * Copyright (c) 2021-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/IterationDecision.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Time.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibUnicode/Forward.h>

namespace Unicode {

enum class DateTimeStyle {
    Full,
    Long,
    Medium,
    Short,
};
DateTimeStyle date_time_style_from_string(StringView);
StringView date_time_style_to_string(DateTimeStyle);

enum class Weekday {
    Sunday,
    Monday,
    Tuesday,
    Wednesday,
    Thursday,
    Friday,
    Saturday,
};

enum class HourCycle {
    H11,
    H12,
    H23,
    H24,
};
HourCycle hour_cycle_from_string(StringView hour_cycle);
StringView hour_cycle_to_string(HourCycle hour_cycle);
Optional<HourCycle> default_hour_cycle(StringView locale);

enum class CalendarPatternStyle {
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
    enum class Field {
        Era,
        Year,
        Month,
        Weekday,
        Day,
        DayPeriod,
        Hour,
        Minute,
        Second,
        FractionalSecondDigits,
        TimeZoneName,
    };

    static CalendarPattern create_from_pattern(StringView);
    String to_pattern() const;

    template<typename Callback>
    void for_each_calendar_field_zipped_with(CalendarPattern& other, ReadonlySpan<Field> filter, Callback&& callback) const
    {
        auto invoke_callback_for_field = [&](auto field) {
            switch (field) {
            case Field::Era:
                return callback(era, other.era);
            case Field::Year:
                return callback(year, other.year);
            case Field::Month:
                return callback(month, other.month);
            case Field::Weekday:
                return callback(weekday, other.weekday);
            case Field::Day:
                return callback(day, other.day);
            case Field::DayPeriod:
                return callback(day_period, other.day_period);
            case Field::Hour:
                return callback(hour, other.hour);
            case Field::Minute:
                return callback(minute, other.minute);
            case Field::Second:
                return callback(second, other.second);
            case Field::FractionalSecondDigits:
                return callback(fractional_second_digits, other.fractional_second_digits);
            case Field::TimeZoneName:
                return callback(time_zone_name, other.time_zone_name);
            }
            VERIFY_NOT_REACHED();
        };

        for (auto field : filter) {
            if (invoke_callback_for_field(field) == IterationDecision::Break)
                break;
        }
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

struct WeekInfo {
    u8 minimal_days_in_first_week { 1 };
    Optional<Weekday> first_day_of_week;
    Vector<Weekday> weekend_days;
};
WeekInfo week_info_of_locale(StringView locale);

}
