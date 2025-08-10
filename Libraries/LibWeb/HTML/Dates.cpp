/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Adam Hodgen <ant1441@gmail.com>
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2023-2025, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2023, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericLexer.h>
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
bool is_valid_week_string(Utf16View const& value)
{
    // A string is a valid week string representing a week-year year and week week if it consists of the following components in the given order:

    // 1. Four or more ASCII digits, representing year, where year > 0
    // 2. A U+002D HYPHEN-MINUS character (-)
    // 3. A U+0057 LATIN CAPITAL LETTER W character (W)
    // 4. Two ASCII digits, representing the week week, in the range 1 ≤ week ≤ maxweek, where maxweek is the week number
    //    of the last day of week-year year
    auto parts = value.split_view('-', SplitBehavior::KeepEmpty);
    if (parts.size() != 2)
        return false;

    if (parts[0].length_in_code_units() < 4)
        return false;
    if (parts[1].length_in_code_units() != 3)
        return false;

    for (auto digit : parts[0])
        if (!is_ascii_digit(digit))
            return false;

    if (!parts[1].starts_with('W'))
        return false;
    if (!is_ascii_digit(parts[1].code_unit_at(1)))
        return false;
    if (!is_ascii_digit(parts[1].code_unit_at(2)))
        return false;

    u64 year = 0;
    for (auto d : parts[0]) {
        year *= 10;
        year += parse_ascii_digit(d);
    }

    auto week = (parse_ascii_digit(parts[1].code_unit_at(1)) * 10) + parse_ascii_digit(parts[1].code_unit_at(2));
    return week >= 1 && week <= week_number_of_the_last_day(year);
}

// https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#valid-month-string
bool is_valid_month_string(Utf16View const& value)
{
    // A string is a valid month string representing a year year and month month if it consists of the following components in the given order:

    // 1. Four or more ASCII digits, representing year, where year > 0
    // 2. A U+002D HYPHEN-MINUS character (-)
    // 3. Two ASCII digits, representing the month month, in the range 1 ≤ month ≤ 12

    auto parts = value.split_view('-', SplitBehavior::KeepEmpty);
    if (parts.size() != 2)
        return false;

    if (parts[0].length_in_code_units() < 4)
        return false;
    if (parts[1].length_in_code_units() != 2)
        return false;

    for (auto digit : parts[0])
        if (!is_ascii_digit(digit))
            return false;

    if (!is_ascii_digit(parts[1].code_unit_at(0)))
        return false;
    if (!is_ascii_digit(parts[1].code_unit_at(1)))
        return false;

    auto month = (parse_ascii_digit(parts[1].code_unit_at(0)) * 10) + parse_ascii_digit(parts[1].code_unit_at(1));
    return month >= 1 && month <= 12;
}

// https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#valid-date-string
bool is_valid_date_string(Utf16View const& value)
{
    // A string is a valid date string representing a year year, month month, and day day if it consists of the following components in the given order:

    // 1. A valid month string, representing year and month
    // 2. A U+002D HYPHEN-MINUS character (-)
    // 3. Two ASCII digits, representing day, in the range 1 ≤ day ≤ maxday where maxday is the number of days in the
    //    month month and year year
    auto parts = value.split_view('-', SplitBehavior::KeepEmpty);
    if (parts.size() != 3)
        return false;

    auto month_string = value.substring_view(0, parts[0].length_in_code_units() + 1 + parts[1].length_in_code_units());
    if (!is_valid_month_string(month_string))
        return false;

    if (parts[2].length_in_code_units() != 2)
        return false;

    i64 year = 0;
    for (auto d : parts[0]) {
        year *= 10;
        year += parse_ascii_digit(d);
    }

    auto month = (parse_ascii_digit(parts[1].code_unit_at(0)) * 10) + parse_ascii_digit(parts[1].code_unit_at(1));
    i64 day = (parse_ascii_digit(parts[2].code_unit_at(0)) * 10) + parse_ascii_digit(parts[2].code_unit_at(1));

    return day >= 1 && day <= AK::days_in_month(year, month);
}

// https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#valid-local-date-and-time-string
bool is_valid_local_date_and_time_string(Utf16View const& value)
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
Utf16String normalize_local_date_and_time_string(Utf16String const& value)
{
    // A string is a valid normalized local date and time string representing a date and time if it consists of the following components in the given order:

    // 1. A valid date string representing the date
    // 2. A U+0054 LATIN CAPITAL LETTER T character (T)
    // 3. A valid time string representing the time, expressed as the shortest possible string for the given time (e.g. omitting the seconds component entirely if the given time is zero seconds past the minute)

    auto value_with_normalized_t = value;
    if (auto spaces = value.count(" "sv); spaces > 0) {
        VERIFY(spaces == 1);
        value_with_normalized_t = value.replace(" "sv, "T"sv, ReplaceMode::FirstOnly);
    }

    auto parts = value_with_normalized_t.split_view('T', SplitBehavior::KeepEmpty);
    VERIFY(parts.size() == 2);

    auto normalized_length = parts[1].length_in_code_points();
    while (normalized_length > 9) {
        if (parts[1].code_point_at(normalized_length - 1) != '0') {
            return Utf16String::formatted("{}T{}", parts[0], parts[1].unicode_substring_view(0, normalized_length));
        }
        normalized_length--;
    }

    if (normalized_length > 5) {
        auto time_without_milliseconds = parts[1].unicode_substring_view(0, 8);
        return Utf16String::formatted("{}T{}", parts[0], parts[1].unicode_substring_view(0, time_without_milliseconds.ends_with(":00"sv) ? 5 : 8));
    }

    return value_with_normalized_t;
}

// https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#valid-time-string
bool is_valid_time_string(Utf16View const& value)
{
    // A string is a valid time string representing an hour hour, a minute minute, and a second second if it consists of the following components in the given order:

    // 1. Two ASCII digits, representing hour, in the range 0 ≤ hour ≤ 23
    // 2. A U+003A COLON character (:)
    // 3. Two ASCII digits, representing minute, in the range 0 ≤ minute ≤ 59
    // 4. If second is nonzero, or optionally if second is zero:
    //     1. A U+003A COLON character (:)
    //     2. Two ASCII digits, representing the integer part of second, in the range 0 ≤ s ≤ 59
    //     3. If second is not an integer, or optionally if second is an integer:
    //         1. A U+002E FULL STOP character (.)
    //         2. One, two, or three ASCII digits, representing the fractional part of second
    auto parts = value.split_view(':', SplitBehavior::KeepEmpty);
    if (parts.size() != 2 && parts.size() != 3)
        return false;

    if (parts[0].length_in_code_units() != 2)
        return false;
    if (parts[1].length_in_code_units() != 2)
        return false;

    if (!is_ascii_digit(parts[0].code_unit_at(0)) || !is_ascii_digit(parts[0].code_unit_at(1)))
        return false;

    auto hour = (parse_ascii_digit(parts[0].code_unit_at(0)) * 10) + parse_ascii_digit(parts[0].code_unit_at(1));
    if (hour > 23)
        return false;

    if (!is_ascii_digit(parts[1].code_unit_at(0)) || !is_ascii_digit(parts[1].code_unit_at(1)))
        return false;

    auto minute = (parse_ascii_digit(parts[1].code_unit_at(0)) * 10) + parse_ascii_digit(parts[1].code_unit_at(1));
    if (minute > 59)
        return false;

    if (parts.size() == 3) {
        if (parts[2].length_in_code_units() < 2)
            return false;

        if (!is_ascii_digit(parts[2].code_unit_at(0)) || !is_ascii_digit(parts[2].code_unit_at(1)))
            return false;

        auto second = (parse_ascii_digit(parts[2].code_unit_at(0)) * 10) + parse_ascii_digit(parts[2].code_unit_at(1));
        if (second > 59)
            return false;

        if (parts[2].length_in_code_units() > 2) {
            auto fractional = parts[2].split_view('.', SplitBehavior::KeepEmpty);
            if (fractional.size() != 2)
                return false;

            if (fractional[1].length_in_code_units() < 1 || fractional[1].length_in_code_units() > 3)
                return false;

            for (auto digit : fractional[1])
                if (!is_ascii_digit(digit))
                    return false;
        }
    }

    return true;
}

// https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#parse-a-month-component
static Optional<YearAndMonth> parse_a_month_component(GenericLexer& input)
{
    // 1. Collect a sequence of code points that are ASCII digits from input given position. If the collected sequence is
    //    not at least four characters long, then fail. Otherwise, interpret the resulting sequence as a base-ten integer.
    //    Let year be that number.
    auto year_string = input.consume_while(is_ascii_digit);
    if (year_string.length() < 4)
        return {};
    auto maybe_year = year_string.to_number<u32>();
    if (!maybe_year.has_value())
        return {};
    auto year = maybe_year.value();

    // 2. If year is not a number greater than zero, then fail.
    if (year < 1)
        return {};

    // 3. If position is beyond the end of input or if the character at position is not a U+002D HYPHEN-MINUS character, then
    //    fail. Otherwise, move position forwards one character.
    if (!input.consume_specific('-'))
        return {};

    // 4. Collect a sequence of code points that are ASCII digits from input given position. If the collected sequence is not
    //    exactly two characters long, then fail. Otherwise, interpret the resulting sequence as a base-ten integer. Let month
    //    be that number.
    auto month_string = input.consume_while(is_ascii_digit);
    if (month_string.length() != 2)
        return {};
    auto month = month_string.to_number<u32>().value();

    // 5. If month is not a number in the range 1 ≤ month ≤ 12, then fail.
    if (month < 1 || month > 12)
        return {};

    // 6. Return year and month.
    return YearAndMonth { year, month };
}

// https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#parse-a-month-string
Optional<YearAndMonth> parse_a_month_string(StringView input_view)
{
    // 1. Let input be the string being parsed.
    // 2. Let position be a pointer into input, initially pointing at the start of the string.
    GenericLexer input { input_view };

    // 3. Parse a month component to obtain year and month. If this returns nothing, then fail.
    auto year_and_month = parse_a_month_component(input);
    if (!year_and_month.has_value())
        return {};

    // 4. If position is not beyond the end of input, then fail.
    if (!input.is_eof())
        return {};

    // 5. Return year and month.
    return year_and_month;
}

i32 number_of_months_since_unix_epoch(YearAndMonth year_and_month)
{
    return (year_and_month.year - 1970) * 12 + year_and_month.month - 1;
}

// https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#parse-a-week-string
Optional<WeekYearAndWeek> parse_a_week_string(StringView input_view)
{
    // 1. Let input be the string being parsed.
    // 2. Let position be a pointer into input, initially pointing at the start of the string.
    GenericLexer input { input_view };

    // 3. Collect a sequence of code points that are ASCII digits from input given position. If the collected sequence is
    //    not at least four characters long, then fail. Otherwise, interpret the resulting sequence as a base-ten integer.
    //    Let year be that number.
    auto year_string = input.consume_while(is_ascii_digit);
    if (year_string.length() < 4)
        return {};
    auto maybe_year = year_string.to_number<u32>();
    if (!maybe_year.has_value())
        return {};
    auto year = maybe_year.value();

    // 4. If year is not a number greater than zero, then fail.
    if (year < 1)
        return {};

    // 5. If position is beyond the end of input or if the character at position is not a U+002D HYPHEN-MINUS character, then
    //    fail. Otherwise, move position forwards one character.
    if (!input.consume_specific('-'))
        return {};

    // 6. If position is beyond the end of input or if the character at position is not a U+0057 LATIN CAPITAL LETTER W character
    //    (W), then fail. Otherwise, move position forwards one character.
    if (!input.consume_specific('W'))
        return {};

    // 7. Collect a sequence of code points that are ASCII digits from input given position. If the collected sequence is not
    //    exactly two characters long, then fail. Otherwise, interpret the resulting sequence as a base-ten integer. Let week
    //    be that number.
    auto week_string = input.consume_while(is_ascii_digit);
    if (week_string.length() != 2)
        return {};
    auto week = week_string.to_number<u32>().value();

    // 8. Let maxweek be the week number of the last day of year year.
    auto maxweek = week_number_of_the_last_day(year);

    // 9. If week is not a number in the range 1 ≤ week ≤ maxweek, then fail.
    if (week < 1 || week > maxweek)
        return {};

    // 10. If position is not beyond the end of input, then fail.
    if (!input.is_eof())
        return {};

    // 11. Return the week-year number year and the week number week.
    return WeekYearAndWeek { year, week };
}

// https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#parse-a-date-component
static Optional<YearMonthDay> parse_a_date_component(GenericLexer& input)
{
    // 1. Parse a month component to obtain year and month. If this returns nothing, then fail.
    auto maybe_month_component = parse_a_month_component(input);
    if (!maybe_month_component.has_value())
        return {};
    auto month_component = maybe_month_component.value();

    // 2. Let maxday be the number of days in month month of year year.
    u32 maxday = AK::days_in_month(month_component.year, month_component.month);

    // 3. If position is beyond the end of input or if the character at position is not a U+002D HYPHEN-MINUS character, then fail.
    //    Otherwise, move position forwards one character.
    if (!input.consume_specific('-'))
        return {};

    // 4. Collect a sequence of code points that are ASCII digits from input given position. If the collected sequence is not
    //    exactly two characters long, then fail. Otherwise, interpret the resulting sequence as a base-ten integer. Let day
    //    be that number.
    auto day_string = input.consume_while(is_ascii_digit);
    if (day_string.length() != 2)
        return {};
    auto day = day_string.to_number<u32>().value();

    // 5. If day is not a number in the range 1 ≤ day ≤ maxday, then fail.
    if (day < 1 || day > maxday)
        return {};

    // 6. Return year, month, and day.
    return YearMonthDay { month_component.year, month_component.month, day };
}

// https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#parse-a-date-string
Optional<YearMonthDay> parse_a_date_string(StringView input_view)
{
    // 1. Let input be the string being parsed.
    // 2. Let position be a pointer into input, initially pointing at the start of the string.
    GenericLexer input { input_view };

    // 3. Parse a date component to obtain year, month, and day. If this returns nothing, then fail.
    auto year_month_day = parse_a_date_component(input);
    if (!year_month_day.has_value())
        return {};

    // 4. If position is not beyond the end of input, then fail.
    if (!input.is_eof())
        return {};

    // 5. Let date be the date with year year, month month, and day day.
    // 6. Return date.
    return year_month_day.release_value();
}

// https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#parse-a-time-component
static Optional<HourMinuteSecond> parse_a_time_component(GenericLexer& input)
{
    // 1. Collect a sequence of code points that are ASCII digits from input given position. If the collected sequence
    //    is not exactly two characters long, then fail.  Otherwise, interpret the resulting sequence as a base-ten
    //    integer. Let hour be that number.
    auto hour_string = input.consume_while(is_ascii_digit);
    if (hour_string.length() != 2)
        return {};
    auto maybe_hour = hour_string.to_number<i32>();
    if (!maybe_hour.has_value())
        return {};
    auto hour = maybe_hour.value();

    // 2. If hour is not a number in the range 0 ≤ hour ≤ 23, then fail.
    if (hour < 0 || hour > 23)
        return {};

    // 3. If position is beyond the end of input or if the character at position is not a U+003A COLON character, then
    //    fail. Otherwise, move position forwards one character.
    if (!input.consume_specific(':'))
        return {};

    // 4. Collect a sequence of code points that are ASCII digits from input given position. If the collected sequence
    //    is not exactly two characters long, then fail. Otherwise, interpret the resulting sequence as a base-ten integer.
    //    Let minute be that number.
    auto minute_string = input.consume_while(is_ascii_digit);
    if (minute_string.length() != 2)
        return {};
    auto maybe_minute = minute_string.to_number<i32>();
    if (!maybe_minute.has_value())
        return {};
    auto minute = maybe_minute.value();

    // 5. If minute is not a number in the range 0 ≤ minute ≤ 59, then fail.
    if (minute < 0 || hour > 59)
        return {};

    // 6. Let second be 0.
    f32 second = 0;

    // 7. If position is not beyond the end of input and the character at position is U+003A (:), then:
    if (input.consume_specific(':')) {
        // 1. Advance position to the next character in input.
        // 2. If position is beyond the end of input, or at the last character in input, or if the next two characters in
        //    input starting at position are not both ASCII digits, then fail.
        if (input.is_eof() || input.tell_remaining() == 1 || (!is_ascii_digit(input.peek()) && !is_ascii_digit(input.peek(1))))
            return {};

        // 3. Collect a sequence of code points that are either ASCII digits or U+002E FULL STOP characters from input
        //    given position.
        auto second_string = input.consume_while([](auto ch) { return is_ascii_digit(ch) || ch == '.'; });
        // If the collected sequence is three characters long, or if it is longer than three characters long and the third
        // character is not a U+002E FULL STOP character, or if it has more than one U+002E FULL STOP character, then fail.
        if (second_string.length() == 3)
            return {};
        if (second_string.length() > 3 && second_string[2] != '.')
            return {};
        if (second_string.find_all("."sv).size() > 1)
            return {};
        // Otherwise, interpret the resulting sequence as a base-ten number (possibly with a fractional part). Set second
        // to that number.
        auto maybe_second = second_string.to_number<f32>();
        if (!maybe_second.has_value())
            return {};
        second = maybe_second.value();

        // 4. If second is not a number in the range 0 ≤ second < 60, then fail.
        if (second < 0 || second >= 60)
            return {};
    }

    // 8. Return hour, minute, and second.
    return HourMinuteSecond { hour, minute, second };
}

// https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#parse-a-time-string
WebIDL::ExceptionOr<GC::Ref<JS::Date>> parse_time_string(JS::Realm& realm, StringView value)
{
    // 1. Let input be the string being parsed.
    // 2. Let position be a pointer into input, initially pointing at the start of the string.
    GenericLexer input { value };

    // 3. Parse a time component to obtain hour, minute, and second. If this returns nothing, then fail.
    auto hour_minute_second = parse_a_time_component(input);
    if (!hour_minute_second.has_value())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Can't parse time string"sv };

    // 4. If position is not beyond the end of input, then fail.
    if (!input.is_eof())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Can't parse time string"sv };

    // 5. Let time be the time with hour hour, minute minute, and second second.
    // 6. Return time.
    return JS::Date::create(realm, JS::make_time(hour_minute_second->hour, hour_minute_second->minute, hour_minute_second->second, static_cast<i32>(hour_minute_second->second * 1000) % 1000));
}

// https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#parse-a-local-date-and-time-string
Optional<DateAndTime> parse_a_local_date_and_time_string(StringView input_view)
{
    // 1. Let input be the string being parsed.
    // 2. Let position be a pointer into input, initially pointing at the start of the string.
    GenericLexer input { input_view };
    // 3. Parse a date component to obtain year, month, and day. If this returns nothing, then fail.
    auto year_month_day = parse_a_date_component(input);
    if (!year_month_day.has_value())
        return {};
    // 4. If position is beyond the end of input or if the character at position is neither a U+0054 LATIN CAPITAL
    //    LETTER T character (T) nor a U+0020 SPACE character, then fail. Otherwise, move position forwards one character.
    if (!input.consume_specific('T') && !input.consume_specific(' '))
        return {};
    // 5. Parse a time component to obtain hour, minute, and second. If this returns nothing, then fail.
    auto hour_minute_second = parse_a_time_component(input);
    if (!hour_minute_second.has_value())
        return {};
    // 6. If position is not beyond the end of input, then fail.
    if (!input.is_eof())
        return {};
    // 7. Let date be the date with year year, month month, and day day.
    // 8. Let time be the time with hour hour, minute minute, and second second.
    // 9. Return date and time.
    return DateAndTime { year_month_day.release_value(), hour_minute_second.release_value() };
}

}
