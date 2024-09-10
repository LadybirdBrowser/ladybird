/*
 * Copyright (c) 2018-2023, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2022, Adam Hodgen <ant1441@gmail.com>
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2023, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Time.h>
#include <LibWeb/HTML/Dates.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#week-number-of-the-last-day
u32 week_number_of_the_last_day(u64 year)
{
    // https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#weeks
    // NOTE: A year is considered to have 53 weeks if either of the following conditions are satisfied:
    // - January 1 of that year is a Thursday.
    // - January 1 of that year is a Wednesday and the year is divisible by 400, or divisible by 4, but not 100.

    // Note: Gauss's algorithm for determining the day of the week with D = 1, and M = 0
    // https://en.wikipedia.org/wiki/Determination_of_the_day_of_the_week#Gauss's_algorithm
    u8 day_of_week = (1 + 5 * ((year - 1) % 4) + 4 * ((year - 1) % 100) + 6 * ((year - 1) % 400)) % 7;

    if (day_of_week == 4 || (day_of_week == 3 && (year % 400 == 0 || (year % 4 == 0 && year % 100 != 0))))
        return 53;

    return 52;
}

// https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#valid-week-string
bool is_valid_week_string(StringView value)
{
    // A string is a valid week string representing a week-year year and week week if it consists of the following components in the given order:

    // 1. Four or more ASCII digits, representing year, where year > 0
    // 2. A U+002D HYPHEN-MINUS character (-)
    // 3. A U+0057 LATIN CAPITAL LETTER W character (W)
    // 4. Two ASCII digits, representing the week week, in the range 1 ≤ week ≤ maxweek, where maxweek is the week number of the last day of week-year year
    auto parts = value.split_view('-', SplitBehavior::KeepEmpty);
    if (parts.size() != 2)
        return false;
    if (parts[0].length() < 4)
        return false;
    for (auto digit : parts[0])
        if (!is_ascii_digit(digit))
            return false;
    if (parts[1].length() != 3)
        return false;

    if (!parts[1].starts_with('W'))
        return false;
    if (!is_ascii_digit(parts[1][1]))
        return false;
    if (!is_ascii_digit(parts[1][2]))
        return false;

    u64 year = 0;
    for (auto d : parts[0]) {
        year *= 10;
        year += parse_ascii_digit(d);
    }
    auto week = (parse_ascii_digit(parts[1][1]) * 10) + parse_ascii_digit(parts[1][2]);

    return week >= 1 && week <= week_number_of_the_last_day(year);
}

// https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#valid-month-string
bool is_valid_month_string(StringView value)
{
    // A string is a valid month string representing a year year and month month if it consists of the following components in the given order:

    // 1. Four or more ASCII digits, representing year, where year > 0
    // 2. A U+002D HYPHEN-MINUS character (-)
    // 3. Two ASCII digits, representing the month month, in the range 1 ≤ month ≤ 12

    auto parts = value.split_view('-', SplitBehavior::KeepEmpty);
    if (parts.size() != 2)
        return false;

    if (parts[0].length() < 4)
        return false;
    for (auto digit : parts[0])
        if (!is_ascii_digit(digit))
            return false;

    if (parts[1].length() != 2)
        return false;

    if (!is_ascii_digit(parts[1][0]))
        return false;
    if (!is_ascii_digit(parts[1][1]))
        return false;

    auto month = (parse_ascii_digit(parts[1][0]) * 10) + parse_ascii_digit(parts[1][1]);
    return month >= 1 && month <= 12;
}

// https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#valid-date-string
bool is_valid_date_string(StringView value)
{
    // A string is a valid date string representing a year year, month month, and day day if it consists of the following components in the given order:

    // 1. A valid month string, representing year and month
    // 2. A U+002D HYPHEN-MINUS character (-)
    // 3. Two ASCII digits, representing day, in the range 1 ≤ day ≤ maxday where maxday is the number of days in the month month and year year
    auto parts = value.split_view('-', SplitBehavior::KeepEmpty);
    if (parts.size() != 3)
        return false;

    if (!is_valid_month_string(ByteString::formatted("{}-{}", parts[0], parts[1])))
        return false;

    if (parts[2].length() != 2)
        return false;

    i64 year = 0;
    for (auto d : parts[0]) {
        year *= 10;
        year += parse_ascii_digit(d);
    }
    auto month = (parse_ascii_digit(parts[1][0]) * 10) + parse_ascii_digit(parts[1][1]);
    i64 day = (parse_ascii_digit(parts[2][0]) * 10) + parse_ascii_digit(parts[2][1]);

    return day >= 1 && day <= AK::days_in_month(year, month);
}

// https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#parse-a-date-string
WebIDL::ExceptionOr<JS::NonnullGCPtr<JS::Date>> parse_date_string(JS::Realm& realm, StringView value)
{
    // FIXME: Implement spec compliant date string parsing
    auto parts = value.split_view('-', SplitBehavior::KeepEmpty);
    if (parts.size() >= 3) {
        if (auto year = parts.at(0).to_number<u32>(); year.has_value()) {
            if (auto month = parts.at(1).to_number<u32>(); month.has_value()) {
                if (auto day_of_month = parts.at(2).to_number<u32>(); day_of_month.has_value())
                    return JS::Date::create(realm, JS::make_date(JS::make_day(*year, *month - 1, *day_of_month), 0));
            }
        }
    }
    return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Can't parse date string"sv };
}

// https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#valid-local-date-and-time-string
bool is_valid_local_date_and_time_string(StringView value)
{
    auto parts_split_by_T = value.split_view('T', SplitBehavior::KeepEmpty);
    if (parts_split_by_T.size() == 2)
        return is_valid_date_string(parts_split_by_T[0]) && is_valid_time_string(parts_split_by_T[1]);
    auto parts_split_by_space = value.split_view(' ', SplitBehavior::KeepEmpty);
    if (parts_split_by_space.size() == 2)
        return is_valid_date_string(parts_split_by_space[0]) && is_valid_time_string(parts_split_by_space[1]);

    return false;
}

// https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#valid-normalised-local-date-and-time-string
String normalize_local_date_and_time_string(String const& value)
{
    if (auto spaces = value.count(" "sv); spaces > 0) {
        VERIFY(spaces == 1);
        return MUST(value.replace(" "sv, "T"sv, ReplaceMode::FirstOnly));
    }

    VERIFY(value.count("T"sv) == 1);
    return value;
}

// https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#valid-time-string
bool is_valid_time_string(StringView value)
{
    // A string is a valid time string representing an hour hour, a minute minute, and a second second if it consists of the following components in the given order:

    // 1. Two ASCII digits, representing hour, in the range 0 ≤ hour ≤ 23
    // 2. A U+003A COLON character (:)
    // 3. Two ASCII digits, representing minute, in the range 0 ≤ minute ≤ 59
    // 4. If second is nonzero, or optionally if second is zero:
    // 1. A U+003A COLON character (:)
    // 2. Two ASCII digits, representing the integer part of second, in the range 0 ≤ s ≤ 59
    // 3. If second is not an integer, or optionally if second is an integer:
    // 1. A U+002E FULL STOP character (.)
    // 2. One, two, or three ASCII digits, representing the fractional part of second
    auto parts = value.split_view(':', SplitBehavior::KeepEmpty);
    if (parts.size() != 2 && parts.size() != 3)
        return false;
    if (parts[0].length() != 2)
        return false;
    if (!(is_ascii_digit(parts[0][0]) && is_ascii_digit(parts[0][1])))
        return false;
    auto hour = (parse_ascii_digit(parts[0][0]) * 10) + parse_ascii_digit(parts[0][1]);
    if (hour > 23)
        return false;
    if (parts[1].length() != 2)
        return false;
    if (!(is_ascii_digit(parts[1][0]) && is_ascii_digit(parts[1][1])))
        return false;
    auto minute = (parse_ascii_digit(parts[1][0]) * 10) + parse_ascii_digit(parts[1][1]);
    if (minute > 59)
        return false;
    if (parts.size() == 2)
        return true;

    if (parts[2].length() < 2)
        return false;
    if (!(is_ascii_digit(parts[2][0]) && is_ascii_digit(parts[2][1])))
        return false;
    auto second = (parse_ascii_digit(parts[2][0]) * 10) + parse_ascii_digit(parts[2][1]);
    if (second > 59)
        return false;
    if (parts[2].length() == 2)
        return true;
    auto second_parts = parts[2].split_view('.', SplitBehavior::KeepEmpty);
    if (second_parts.size() != 2)
        return false;
    if (second_parts[1].length() < 1 || second_parts[1].length() > 3)
        return false;
    for (auto digit : second_parts[1])
        if (!is_ascii_digit(digit))
            return false;

    return true;
}

// https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#parse-a-time-string
WebIDL::ExceptionOr<JS::NonnullGCPtr<JS::Date>> parse_time_string(JS::Realm& realm, StringView value)
{
    // FIXME: Implement spec compliant time string parsing
    auto parts = value.split_view(':', SplitBehavior::KeepEmpty);
    if (parts.size() >= 2) {
        if (auto hours = parts.at(0).to_number<u32>(); hours.has_value()) {
            if (auto minutes = parts.at(1).to_number<u32>(); minutes.has_value()) {
                if (parts.size() >= 3) {
                    if (auto seconds = parts.at(2).to_number<u32>(); seconds.has_value())
                        return JS::Date::create(realm, JS::make_time(*hours, *minutes, *seconds, 0));
                }
                return JS::Date::create(realm, JS::make_date(0, JS::make_time(*hours, *minutes, 0, 0)));
            }
        }
    }
    return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Can't parse time string"sv };
}

}
