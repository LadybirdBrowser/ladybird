/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibUnicode/Calendar.h>
#include <LibUnicode/RustFFI.h>

namespace Unicode {

// https://tc39.es/proposal-temporal/#prod-MonthCode
static constexpr bool is_valid_month_code_string(StringView month_code)
{
    // MonthCode :::
    //     M00L
    //     M0 NonZeroDigit L[opt]
    //     M NonZeroDigit DecimalDigit L[opt]
    auto length = month_code.length();

    if (length != 3 && length != 4)
        return false;

    if (month_code[0] != 'M')
        return false;

    if (!is_ascii_digit(month_code[1]) || !is_ascii_digit(month_code[2]))
        return false;

    if (length == 3 && month_code[1] == '0' && month_code[2] == '0')
        return false;
    if (length == 4 && month_code[3] != 'L')
        return false;

    return true;
}

// 12.2.1 ParseMonthCode ( argument ), https://tc39.es/proposal-temporal/#sec-temporal-parsemonthcode
Optional<MonthCode> parse_month_code(StringView month_code)
{
    // 3. If ParseText(StringToCodePoints(monthCode), MonthCode) is a List of errors, throw a RangeError exception.
    if (!is_valid_month_code_string(month_code))
        return {};

    // 4. Let isLeapMonth be false.
    auto is_leap_month = false;

    // 5. If the length of monthCode = 4, then
    if (month_code.length() == 4) {
        // a. Assert: The fourth code unit of monthCode is 0x004C (LATIN CAPITAL LETTER L).
        VERIFY(month_code[3] == 'L');

        // b. Set isLeapMonth to true.
        is_leap_month = true;
    }

    // 6. Let monthCodeDigits be the substring of monthCode from 1 to 3.
    auto month_code_digits = month_code.substring_view(1, 2);

    // 7. Let monthNumber be ℝ(StringToNumber(monthCodeDigits)).
    auto month_number = month_code_digits.to_number<u8>().value();

    // 8. Return the Record { [[MonthNumber]]: monthNumber, [[IsLeapMonth]]: isLeapMonth }.
    return MonthCode { month_number, is_leap_month };
}

// 12.2.2 CreateMonthCode ( monthNumber, isLeapMonth ), https://tc39.es/proposal-temporal/#sec-temporal-createmonthcode
String create_month_code(u8 month_number, bool is_leap_month)
{
    // 1. Assert: If isLeapMonth is false, monthNumber > 0.
    if (!is_leap_month)
        VERIFY(month_number > 0);

    // 2. Let numberPart be ToZeroPaddedDecimalString(monthNumber, 2).

    // 3. If isLeapMonth is true, then
    if (is_leap_month) {
        // a. Return the string-concatenation of the code unit 0x004D (LATIN CAPITAL LETTER M), numberPart, and the
        //    code unit 0x004C (LATIN CAPITAL LETTER L).
        return MUST(String::formatted("M{:02}L", month_number));
    }

    // 4. Return the string-concatenation of the code unit 0x004D (LATIN CAPITAL LETTER M) and numberPart.
    return MUST(String::formatted("M{:02}", month_number));
}

CalendarDate iso_date_to_calendar_date(String const& calendar, ISODate iso_date)
{
    auto result = FFI::icu_iso_date_to_calendar_date(calendar.bytes().data(), calendar.bytes().size(), iso_date.year, iso_date.month, iso_date.day);

    return CalendarDate {
        .era = {},
        .era_year = {},
        .year = result.year,
        .month = result.month,
        .month_code = String::from_utf8_without_validation({ result.month_code, result.month_code_length }),
        .day = result.day,
        .day_of_week = result.day_of_week,
        .day_of_year = result.day_of_year,
        .week_of_year = {},
        .days_in_week = result.days_in_week,
        .days_in_month = result.days_in_month,
        .days_in_year = result.days_in_year,
        .months_in_year = result.months_in_year,
        .in_leap_year = result.in_leap_year,
    };
}

Optional<ISODate> calendar_date_to_iso_date(String const& calendar, i32 year, u8 month, u8 day)
{
    auto result = FFI::icu_calendar_date_to_iso_date(calendar.bytes().data(), calendar.bytes().size(), year, month, day);
    if (!result.has_value)
        return {};

    return ISODate { result.iso_date.year, result.iso_date.month, result.iso_date.day };
}

Optional<ISODate> iso_year_and_month_code_to_iso_date(String const& calendar, i32 year, StringView month_code, u8 day)
{
    auto result = FFI::icu_iso_year_and_month_code_to_iso_date(calendar.bytes().data(), calendar.bytes().size(), year, month_code.bytes().data(), month_code.length(), day);
    if (!result.has_value)
        return {};

    return ISODate { result.iso_date.year, result.iso_date.month, result.iso_date.day };
}

Optional<ISODate> calendar_year_and_month_code_to_iso_date(String const& calendar, i32 arithmetic_year, StringView month_code, u8 day)
{
    auto result = FFI::icu_calendar_year_and_month_code_to_iso_date(calendar.bytes().data(), calendar.bytes().size(), arithmetic_year, month_code.bytes().data(), month_code.length(), day);
    if (!result.has_value)
        return {};

    return ISODate { result.iso_date.year, result.iso_date.month, result.iso_date.day };
}

u8 calendar_months_in_year(String const& calendar, i32 arithmetic_year)
{
    return FFI::icu_calendar_months_in_year(calendar.bytes().data(), calendar.bytes().size(), arithmetic_year);
}

u8 calendar_days_in_month(String const& calendar, i32 arithmetic_year, u8 ordinal_month)
{
    return FFI::icu_calendar_days_in_month(calendar.bytes().data(), calendar.bytes().size(), arithmetic_year, ordinal_month);
}

u8 calendar_max_days_in_month_code(String const& calendar, StringView month_code)
{
    return FFI::icu_calendar_max_days_in_month_code(calendar.bytes().data(), calendar.bytes().size(), month_code.bytes().data(), month_code.length());
}

bool calendar_year_contains_month_code(String const& calendar, i32 arithmetic_year, StringView month_code)
{
    return FFI::icu_year_contains_month_code(calendar.bytes().data(), calendar.bytes().size(), arithmetic_year, month_code.bytes().data(), month_code.length());
}

}
