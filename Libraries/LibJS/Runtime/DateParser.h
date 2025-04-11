/*
 * Copyright (c) 2025, Manuel Zahariev <manuel@duck.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/GenericLexer.h>
#include <AK/Optional.h>
#include <AK/StringView.h>
#include <AK/Time.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibJS/Runtime/Date.h>

#include <cctype> // Not included by default on macOS.

// Parse simplified ISO8601 and non-standard date formats to milliseconds
// from epoch (double). Synopsis:
// (1) Try to parse the string as simplified ISO8601 (case sensitive).
//   - if that worked, assemble result (4 below)
//   - hard fail (return NAN) if the input string "looks like" ISO8601,
//     but deviates.
// (2) If parsing ISO8601 "soft" fails (unlike "hard" above), continue
//     shallow parsing (case insensitive) date string components: time,
//     timezone, keywords, numbers...
// (3) Guess ambiguous date parts, like "1/2/3" --> Jan 2, 2003
// (4) Assemble result from parts (year, month,...) and convert to milliseconds
//     from epoch.
//
// Overall objectives:
// - compliance with ECMA date time string format, incl. ISO8601 extensions
//   for signed 6-digit year and extended time offset format (:SS.nanosecs)
//   https://tc39.es/ecma262/#sec-date-time-string-format
// - Support for Date.toString and Date.toUTCString formats.
// - Compatible with Mozilla Firefox 134.0.1 and Chromium 131.0.6778.264. Within
//   reason. Differences indicated in comments.
//   - Where Firefox and Chrome agree on a parse, support it.
//   - Where they disagree, support the one that seems more sane.
//   - In very limited cases, pick our own way. Example: "<number> Month":
//     - Firefox fails on all "<number> Month" date strings.
//     - In most cases, Chrome interprets "<number> Month" as "Month 01, Year".
//     - Chrome parses "7 Feb" as "Feb 7, 2001".
//     - We always parse as "Month 01, Year".
// - Support Firefox less permissive punctuation but more permissive punctuation
//   syntax.
class DateParser : public GenericLexer {
private:
    Vector<u64> m_numbers;

    Optional<i8> m_sign;
    Optional<i64> m_year;
    Optional<u8> m_month;
    Optional<u8> m_day;
    Optional<u8> m_hours;
    Optional<u8> m_minutes;
    Optional<u8> m_seconds;
    Optional<u16> m_milliseconds;

    // True if GMT/UTC/Z specified and there is no timezone offset; false if timezone offset.
    // No value if there is no timezone information: we have to guess whether the date was given in GMT or local time.
    Optional<bool> m_timezone_utc;
    Optional<i8> m_timezone_sign; // +1 or -1 if there is a timezone offset.
    Optional<u8> m_timezone_hours;
    Optional<u8> m_timezone_minutes;
    Optional<u8> m_timezone_seconds;
    Optional<u64> m_timezone_nanoseconds;

    constexpr static u64 pow10(size_t pow)
    {
        switch (pow) {
        case 0:
            return 1;
        case 1:
            return 10;
        case 2:
            return 100;
        case 3:
            return 1'000;
        case 4:
            return 10'000;
        case 5:
            return 100'000;
        case 6:
            return 1'000'000;
        case 7:
            return 10'000'000;
        case 8:
            return 100'000'000;
        case 9:
            return 1'000'000'000;
        }

        VERIFY_NOT_REACHED();
    }
    // Reads a contiguous sequence of digits. Ignores digits read past MaxLength.
    // Returns the numeric value of the sequence and the number of digits not ignored.
    template<typename T, size_t MaxLength>
    ALWAYS_INLINE std::pair<T, size_t> read_number()
    {
        static_assert(std::is_unsigned<T>(), "Type must be unsigned.");
        static_assert(MaxLength < 11, "Number too long.");
        static_assert((sizeof(T) >= 1) || (MaxLength <= 2), "Number too long.");
        static_assert((sizeof(T) >= 2) || (MaxLength <= 4), "Number too long.");
        static_assert((sizeof(T) >= 4) || (MaxLength <= 9), "Number too long.");
        static_assert((sizeof(T) >= 8) || (MaxLength <= 19), "Number too long.");

        auto result = std::make_pair(T(0), size_t(0));
        for (result.second = 0; result.second < MaxLength; ++result.second) {
            if (is_eof() || !next_is(isdigit))
                return result;
            result.first *= 10;
            result.first += consume() - '0';
        }

        ignore_while(isdigit);
        return result;
    }

    ALWAYS_INLINE bool guess_date_from_numbers() // Guess an ambiguous date, like 1/1/1.
    {
        switch (m_numbers.size()) {
        case 0:
            return true; // The year and possibly month may have already been calculated. Verify later
        case 1:
            return guess_date_from_1_number();
        case 2:
            return guess_date_from_2_numbers();
        case 3:
            return guess_date_from_3_numbers();
        }

        return false; // Too many numbers
    }
    ALWAYS_INLINE bool guess_date_from_3_numbers() // Only "month-day-year" (default) and "year-month-day" are supported (same as firefox and chrome).
    {
        VERIFY(m_numbers.size() == 3);

        u64 const number0 = m_numbers.at(0);
        u64 const number1 = m_numbers.at(1);
        u64 const number2 = m_numbers.at(2);

        if (m_year.has_value() || m_month.has_value())
            return false; // Too many numbers

        if ((number0 > 31) || number0 == 0) { // YMD
            if (number1 > 12)
                return false;
            if (number2 > 31)
                return false;

            if ((number1 == 0) || (number2 == 0)) // 0 for day or month
                return false;

            m_month = number1;
            m_day = number2;
            m_year = guess_year(number0);

            return true;
        }

        // MDY
        if (number0 > 12)
            return false; // Both Firefox and Chrome fail for the first number >12 and <=31. Weird. We do the same
        if (number1 > 31)
            return false;

        if ((number0 == 0) || (number1 == 0)) // 0 for day or month
            return false;

        m_month = number0;
        m_day = number1;
        m_year = guess_year(number2);

        return true;
    }
    ALWAYS_INLINE bool guess_date_from_2_numbers() // Guess default order "day-year" or adapt.
    {
        VERIFY(m_numbers.size() == 2);

        u64 const number0 = m_numbers.at(0);
        u64 const number1 = m_numbers.at(1);

        if (m_year.has_value() && m_month.has_value())
            return false; // Too many numbers

        if (m_year.has_value()) {
            if (number0 <= 12) { // ... is a month
                if (number1 > 31)
                    return false;
                if ((number0 == 0) || (number1 == 0)) // 0 for day or month
                    return false;

                m_month = number0;
                m_day = number1;
                return true;
            }

            if (number0 <= 31) { // ... is a day
                if (number1 > 12)
                    return false;

                if ((number0 == 0) || (number1 == 0)) // 0 for day or month
                    return false;

                m_month = number1;
                m_day = number0;
                return true;
            }
        }

        // At this point, one of the numbers is the year
        if (!m_month.has_value())
            return false; // Firefox fails on guessing 2 numbers. We do the same

        // At this point, the month has been read from a month name
        if ((number0 > 31) && (number1 > 31))
            return false;                       // Neither of the numbers can be a day
        if ((number0 > 31) || (number0 == 0)) { // ... is a year
            if (number1 == 0)                   // 0 for day
                return false;

            m_day = number1;
            m_year = guess_year(number0);
            return true;
        }

        // Default order is day -> year
        if (number0 == 0) // 0 for day
            return false;

        m_day = number0;
        m_year = guess_year(number1);

        return true;
    }
    ALWAYS_INLINE bool guess_date_from_1_number() // Guess in this order: year, month, day.
    {
        VERIFY(m_numbers.size() == 1);

        u64 const number0 = m_numbers.at(0);

        if (!m_year.has_value() && !m_month.has_value())
            return one_number(number0);

        if (!m_year.has_value()) { // the number must be a year
            m_year = guess_year(number0);
            return true;
        }

        if (!m_month.has_value()) // Firefox fails on two numbers. So do we. Chrome is weird.
            return false;

        // At this point, the year and month must have been specified some other way (e.g. "Feb +002002").
        if ((number0 > 31) || (number0 == 0)) // Invalid day number.
            return false;

        m_day = number0;

        return true;
    }
    ALWAYS_INLINE static u64 guess_year(u64 const number) // Guess one- or two-digit year.
    {
        switch (number) {
        case 0 ... 49:
            return 2000 + number;
        case 50 ... 99:
            return 1900 + number;
        }

        return number;
    }
    ALWAYS_INLINE bool one_number(u64 const number) // The whole input string is just one stand-alone number.}
    {
        switch (number) {
        case 0:
            m_year = 2000;
            return true;
        case 1 ... 12:
            m_year = 2001;
            m_month = number; // Firefox and Chrome interpret standalone numbers below 12 as months in 2001. Weird! We do the same.
            return true;
        case 13 ... 31:
            return false; // Firefox and Chrome fail on standalone numbers between 12 and 31. Weird! We do the same.
        case 32 ... 49:
            m_year = 2000 + number;
            return true;
        case 50 ... 99:
            m_year = 1900 + number;
            return true;
        }
        m_year = number;
        return true;
    }

    // Permissive, greedy shallow date parser for date components.
    // Returns:
    // - false: Hard fail. Some invalid input condition has been found. Caller should return NAN.
    // - true: Parsing can continue.
    ALWAYS_INLINE bool loop()
    {
        switch (peek()) {
        case '0' ... '9':
            return maybe_number(); // Also captures time.
        case '+':
        case '-':
            return maybe_sign(); // Also captures signed 6-digit year and timezone offset.
        case 'A' ... 'Z':
            return maybe_word(); // Captures all date string "keywords". Accepts any kind of "junk" before date and time..
        case ' ':
        case '.':
        case ',':
        case '/':
            // Firefox seems to accept (ignore) this punctuation. So do we.
            // Firefox also accepts a bare '+' sometimes. We do not.
            // Chrome is a lot more permissive.
            ignore(); // Ignore punctuation.
            return true;
        case '(':
            ignore_until(')'); // Consume time zone name (Anything in brackets).
            ignore();
            ignore_while(isspace);
            return true;
        }

        return false;
    }
    ALWAYS_INLINE bool maybe_ampm() // Side effect: consumes space at the end of a time component, even if there is no AM/PM.
    {
        VERIFY(m_hours.has_value());

        consume_while(isspace);

        if (consume_specific("AM"sv)) {
            if (!separator())
                return false; // 12:34 AMsomething

            if (m_hours.value() > 12)
                return false; // 14:45AM
            if (m_hours.value() == 12)
                m_hours = 0; // 12:05AM -> 00:05
            return true;
        }

        if (consume_specific("PM"sv)) {
            if (!separator())
                return false; // 12:34 PMsomething
            if (m_hours.value() > 12)
                return false; // 14:45PM
            if (m_hours.value() < 12)
                m_hours = m_hours.value() + 12;
            return true;
        }

        return true;
    }
    ALWAYS_INLINE bool maybe_time(std::pair<u32, size_t> const& hours) // H[H]:MM[:SS[.mss[...]]][ ][AM|PM] At this point, H[H]: has already been read.
    {
        if (m_hours.has_value())
            return false; // Time has already been read.

        if (hours.second > 2)
            return false; // 123:
        if (hours.first > 24)
            return false;
        m_hours = hours.first; // No precision loss during the conversion u32 -> u8 since the hours has at most 2 digits.

        auto const minutes = read_number<u32, 3>();
        if (minutes.second != 2)
            return false; // 12:345 or 12:3
        if (minutes.first > 59)
            return false;
        m_minutes = minutes.first;

        if (consume_specific('.'))
            return false; // 12:34.
        if (!consume_specific(':'))
            return true;

        auto const seconds = read_number<u32, 3>();
        if (seconds.second != 2)
            return false; // 12:34:567 or 12:34:5
        if (seconds.first > 59)
            return false;
        m_seconds = seconds.first;

        if (!consume_specific('.'))
            return true;

        auto const milliseconds = read_number<u32, 3>();
        switch (milliseconds.second) {
        case 0:
            return false; // 12:34:56.
        case 1:
            m_milliseconds = milliseconds.first * 100;
            break;
        case 2:
            m_milliseconds = milliseconds.first * 10;
            break;
        case 3:
            m_milliseconds = milliseconds.first;
            break;
        default:
            VERIFY_NOT_REACHED();
        }

        return true;
    }
    ALWAYS_INLINE bool maybe_number()
    {
        auto const number = read_number<u32, 7>();
        VERIFY(number.second > 0);

        if (number.second > 6)
            return false; // 1234567

        if (consume_specific(':')) {
            if (!maybe_time(number))
                return false;
            if (!maybe_ampm())
                return false;
            return true;
        }

        m_numbers.append(number.first);

        return separator(); // Must be followed by a separator.
    }
    ALWAYS_INLINE bool maybe_sign()
    {
        i8 const sign = (consume() == '-') ? -1 : +1;
        auto const number = read_number<u32, 7>();

        switch (number.second) {
        case 0: // Not a sign after all; '+' is forbidden if it is not a sign. '-' is ignored as punctuation.
            return (sign == -1);
        case 1 ... 5: // Too small to be a signed year;
            if (m_hours.has_value())
                break; // Candidate for timezone offset.

            m_numbers.append(number.first); // Ignore the sign and treat it as a number.
            return true;
        case 6:
            if (m_hours.has_value())
                break; // Candidate for timezone offset.

            if (m_year.has_value())
                return false; // To many digits to be anything else than a signed year.

            m_year = sign * number.first; // Candidate for signed year
            return true;
        default:
            return false;
        }

        m_timezone_sign = sign;
        return tz_offset(number);
    }

    ALWAYS_INLINE bool tz_offset() // Read a full timezone offset, including the sign.
    {
        m_timezone_sign = (consume() == '-') ? -1 : +1;
        auto const number = read_number<u32, 7>();
        return tz_offset(number);
    }
    // Continue reading a timezone offset, after the sign and the first number have beeen read.
    ALWAYS_INLINE bool tz_offset(std::pair<u32, size_t> const& number, bool iso_8601_format = false)
    {
        if (m_timezone_hours.has_value()) // Cannot have more than one timezone offset or a timezone name followed by a timezone offset.
            return false;

        m_timezone_utc = false;
        switch (number.second) {
        case 0:
            return false;
        case 1:
            if (iso_8601_format)
                return false; // +1
            [[fallthrough]];
        case 2:
            break; // Candidate for timezone offset with colon.
        case 3:
            if (iso_8601_format)
                return false; // +123
            [[fallthrough]];
        case 4: { // "Military" timezone offset
            u16 const tz_hhmm = number.first;
            m_timezone_hours = tz_hhmm / 100;
            m_timezone_minutes = tz_hhmm % 100;
            return true;
        }
        case 5:
            if (iso_8601_format)
                return false; // +12345
            [[fallthrough]];
        case 6: { // "Military" timezone offset
            u32 const tz_hhmmss = number.first;
            m_timezone_hours = tz_hhmmss / 10000;
            m_timezone_minutes = tz_hhmmss % 10000 / 100;
            m_timezone_seconds = tz_hhmmss % 100;
            return true;
        }
        default:
            return false;
        }

        m_timezone_hours = number.first; // Guaranteed to be a 1- or 2-digit number.

        if (!consume_specific(':'))
            return true; // +H[H] "military" time offset

        // Timezone with colon
        auto const minutes = read_number<u16, 3>();
        if (minutes.second != 2)
            return false;
        m_timezone_minutes = minutes.first;

        if (!consume_specific(':'))
            return true;

        auto const seconds = read_number<u16, 3>();
        if (seconds.second != 2)
            return false;
        m_timezone_seconds = seconds.first;

        if (!consume_specific('.'))
            return true;

        auto const nanoseconds = read_number<u64, 10>();
        if (nanoseconds.second == 0)
            return false;
        if (nanoseconds.second > 9)
            return false;

        m_timezone_nanoseconds = nanoseconds.first * pow10(9 - nanoseconds.second);

        return true;
    }
    ALWAYS_INLINE bool separator() // Ignore space and Firefox punctuation.
    {
        if (is_eof())
            return true;
        switch (peek()) {
        case ' ':
        case ',':
        case '.':
        case '/':
        case '-':
            ignore();
            return true;
        }
        return false;
    }
    ALWAYS_INLINE bool gmt(StringView const& str) // Z or GMT or UTC can be used interchangeably.
    {
        if (!consume_specific(str))
            return false;

        m_timezone_utc = true;

        bool space = consume_while(isspace).length() > 0;
        switch (peek()) {
        case '+':
        case '-':
            return tz_offset(); // GMT+1234
        }

        return space || separator();
    }
    // Same as Chrome and Firefox, we only support abbreviations for timezones covering the US mainland.
    ALWAYS_INLINE bool us_timezone(StringView const& str, u8 hours, u8 minutes = 0, i8 sign = -1)
    {
        VERIFY(str.length() == 3); // only 3-letter timezone names
        if (!consume_specific(str))
            return false;

        if (!m_hours.has_value() && m_numbers.is_empty() && !m_year.has_value()) // Ignore timezone before date or time.
            return false;

        m_timezone_sign = sign;
        m_timezone_hours = hours;
        m_timezone_minutes = minutes;

        return separator(); // Must end with a separator.
    }
    ALWAYS_INLINE bool month_name(StringView const& str, u8 month)
    {
        VERIFY(str.length() == 3); // Only looking for 3-letter month prefixes.
        for (size_t i = 0; i < 3; ++i)
            if (str[i] != peek(i))
                return false;

        ignore_while(isalpha); // ... which can followed by anything. Just like Firefox and Chrome.
        m_month = month;

        return separator(); // Must end with a separator.
    }
    ALWAYS_INLINE bool word() // Alphanumeric strings that are not date "keywords".
    {
        std::ignore = consume_while(isalpha);
        // Just like Firefox and Chrome:
        // - Ignore junk (bare words) at the beginning (before time or a date fragment has been read).
        // - Fail if a word is read later in the date string (exception: final time zone name, in brackets).
        return (m_numbers.is_empty() && !m_hours.has_value() && !m_year.has_value());
    }
    ALWAYS_INLINE bool maybe_word() // The top of a trie catching date "keywords".
    {
        switch (peek()) {
        case 'A': // APR AUG
            return month_name("APR"sv, 4) || month_name("AUG"sv, 8) || word();
        case 'C': // CST CDT
            return us_timezone("CST"sv, 6) || us_timezone("CDT"sv, 5) || word();
        case 'D': // DEC
            return month_name("DEC"sv, 12) || word();
        case 'E': // EST EDT
            return us_timezone("EST"sv, 5) || us_timezone("EDT"sv, 4) || word();
        case 'F': // FEB
            return month_name("FEB"sv, 2) || word();
        case 'G': // GMT
            return gmt("GMT"sv) || word();
        case 'J': // JAN JUN JUL
            return month_name("JAN"sv, 1) || month_name("JUN"sv, 6) || month_name("JUL"sv, 7) || word();
        case 'M': // MAR MAY MST MDT
            return month_name("MAR"sv, 3) || month_name("MAY"sv, 5) || us_timezone("MST"sv, 7) || us_timezone("MDT"sv, 6) || word();
        case 'N': // NOV
            return month_name("NOV"sv, 11) || word();
        case 'O': // OCT
            return month_name("OCT"sv, 10) || word();
        case 'P': // PST PDT
            return us_timezone("PST"sv, 8) || us_timezone("PDT"sv, 7) || word();
        case 'S': // SEP
            return month_name("SEP"sv, 9) || word();
        case 'U': // UTC
            return gmt("UTC"sv) || word();
        case 'Z': // Z
            return gmt("Z"sv) || word();
        }

        return word();
    }

    // Capture simplified ISO8601 date format. https://tc39.es/ecma262/#sec-date-time-string-format
    // All of the iso_... functions return:
    // - true: if the input can be parsed as an ISO8601 date.
    // - false: cannot be parsed as an ISO8601 date. Will be defered to a non-standard date string.
    // - Error: hard fail; caller is supposed to return NAN.
    ALWAYS_INLINE ErrorOr<bool> maybe_iso_8601()
    {
        for (size_t step = 0; step < 4; ++step) {
            switch (step) {
            case 0:
                if (!TRY(maybe_iso_year()))
                    return false;
                break;
            case 1:
                if (!TRY(maybe_iso_month_day()))
                    return false;
                break;
            case 2:
                if (!TRY(maybe_iso_time()))
                    return false;
                break;
            case 3:
                TRY(maybe_iso_tz());
                break;
            default:
                VERIFY_NOT_REACHED();
            }

            if (is_eof())
                return true;
        }

        return Error::from_string_literal("Read ISO8601 format, but have some input left over.");
    }
    ALWAYS_INLINE ErrorOr<bool> maybe_iso_year()
    {
        switch (peek()) {
        case '0' ... '9':
            if (TRY(maybe_iso_year4()))
                return true;
            break;
        case '+':
        case '-':
            if (TRY(maybe_iso_signed_year6()))
                return true;
            break;
        default:
            return false;
        }

        return false;
    }
    ALWAYS_INLINE ErrorOr<bool> maybe_iso_year4() // Like "2025".
    {
        auto number = read_number<u32, 7>();

        switch (number.second) {
        case 0:
            return false; // no digits
        case 1:
        case 2:
            if (next_is(':')) { // This may not be a year after all but the start of a "time" component.
                ignore();
                if (!maybe_time(number))
                    return Error::from_string_literal("Cannot parse time.");
                if (!maybe_ampm())
                    return Error::from_string_literal("Cannot parse am/pm.");
                return false;
            }
            [[fallthrough]];
        case 3:
        case 5:
        case 6: // Six-digit year number; no sign means not ISO8601 date.
            // At this point, this is not an ISO8601 date.
            m_numbers.append(number.first);
            return false;
        case 4: // Four-digit year number.
            m_year = number.first;
            return true; // This can be the start of an ISO8601 date.
        }

        return Error::from_string_literal("String too long to be a year.");
    }
    ALWAYS_INLINE ErrorOr<bool> maybe_iso_signed_year6() // Like "+002025".
    {
        i64 sign = +1;
        if (consume() == '-')
            sign = -1;
        auto number = read_number<u32, 7>();

        switch (number.second) {
        case 0:
            if (sign == +1) // Standalone '+' is invalid.
                return Error::from_string_literal("Invalid character in date string ('+').");
            return false;
        case 1 ... 5:
            m_numbers.append(number.first); // This is not an ISO8601 date.
            return false;
        case 6:
            break;
        default:
            return Error::from_string_literal("String too long to be a 6-digit signed year.");
        }

        m_year = sign * number.first;

        // "The representation of the year 0 as -000000 is invalid." https://tc39.es/ecma262/#sec-expanded-years
        // Firefox interprets "-000000" as "Jan 1, 2000".
        if ((sign == -1) && (m_year == 0))
            return Error::from_string_literal("The representation of the year 0 as '-000000' is invalid.");

        return true;
    }
    ALWAYS_INLINE ErrorOr<bool> maybe_iso_month_day() // [-MM[-DD]]
    {
        if (!consume_specific('-'))
            return true;

        auto month = read_number<u16, 3>();
        if (consume_specific(':')) { // Like "2000-12:34". Firefox and Chrome parse it correctly. We do the same.
            if (maybe_time(month))
                return false;
            return Error::from_string_literal("Found something that looks like time, but is not.");
        }

        switch (month.second) {
        case 0:
            return false; // Not ISO8601 date format. Continue reading.
        case 1:
            m_month = month.first;
            if (m_month == 0)
                return Error::from_string_literal("Month number cannot be zero.");
            return false;
        case 2:
            break;
        default: // month number too long
            return Error::from_string_literal("Month number too long.");
        }

        m_month = month.first;

        if ((m_month == 0) || (m_month.value() > 12))
            return Error::from_string_literal("Invalid month number.");

        if (!consume_specific('-'))
            return true;

        if (is_eof())
            return Error::from_string_literal("Expecting month number. Got eof.");

        auto day = read_number<u16, 3>();

        switch (day.second) {
        case 0:
            return false; // Not ISO8601 date format. Continue reading.
        case 1:
            m_day = day.first;
            if (m_day == 0)
                return Error::from_string_literal("Day number cannot be zero.");
            return false;
        case 2:
            break;
        default: // month number too long
            return Error::from_string_literal("Day number too long.");
        }

        m_day = day.first;
        if ((m_day == 0) || (m_day.value() > 31))
            return Error::from_string_literal("Invalid day number.");

        return true;
    }
    ALWAYS_INLINE ErrorOr<bool> maybe_iso_time() // THH:MM[:SS[.M[SS...]]]
    {
        // The ECMA date string format requires uppercase 'T' and 'Z' https://tc39.es/ecma262/#sec-date-time-string-format
        // - Chrome supports lower case occurrences.
        // - Firefox and us do not.
        if (!consume_specific('T'))
            return false;

        // After reading the 'T', any failures return NAN (failed parse)
        auto hours = read_number<u16, 3>();

        if (!consume_specific(':')) // 12
            return Error::from_string_literal("Well specified time needs minutes.");

        if (hours.second != 2)
            return Error::from_string_literal("Hours: invalid length.");

        if (!maybe_time(hours)) // The only difference is that ISO8601 requires 2-digit hours.
            return Error::from_string_literal("Cannot parse time.");

        return true;
    }
    ALWAYS_INLINE ErrorOr<void> maybe_iso_tz() // After the 'T' for iso_time has been read, reading an ISO8601 timezone either succeeds or the whole parse fails.
    {
        switch (consume()) {
        case 'Z':
            m_timezone_utc = true;
            return {};
        case '-':
            m_timezone_sign = -1;
            break;
        case '+':
            m_timezone_sign = +1;
            break;
        default:
            return Error::from_string_literal("Invalid timezone offset format.");
        }

        if (!tz_offset(read_number<u32, 7>(), true)) // A sign and a number have been read. Continue reading a timezone offset..
            return Error::from_string_literal("Invalid timezone offset format.");

        return {};
    }

    ALWAYS_INLINE double build_date(bool is_iso8601_date = false) // Build a date (milliseconds since epoch) from parts collected.
    {
        VERIFY(is_eof());

        if (!m_year.has_value())
            return NAN; // Needs at least one year.

        if ((m_hours == 24) && ((m_minutes.value_or(0) > 0) || (m_seconds.value_or(0) > 0)))
            return NAN; // 24:01:02

        AK::UnixDateTime time = AK::UnixDateTime::from_unix_time_parts( // local time
            m_year.value(),
            m_month.value_or(1),
            m_day.value_or(1),
            m_hours.value_or(0),
            m_minutes.value_or(0),
            m_seconds.value_or(0),
            m_milliseconds.value_or(0));

        double time_ms = static_cast<double>(time.milliseconds_since_epoch()); // Assume the date was given in UTC.

        if (m_timezone_sign.has_value()) { // A timezone offset was specified.
            if (m_timezone_hours.has_value() && (m_timezone_hours.value() > 24))
                return NAN;
            if (m_timezone_minutes.has_value() && (m_timezone_minutes.value() > 59))
                return NAN;
            if (m_timezone_seconds.has_value() && (m_timezone_seconds.value() > 59))
                return NAN;

            time_ms -= 1.0 * m_timezone_sign.value() * static_cast<double>( // Convert to a UTC timestamp: local timestamp minus timezone offset.
                           m_timezone_hours.value_or(0) * 3'600'000 + m_timezone_minutes.value_or(0) * 60'000 + m_timezone_seconds.value_or(0) * 1'000 + static_cast<u64>(m_timezone_nanoseconds.value_or(0) / 1'000'000));
        } else if (!m_timezone_utc.has_value() && (!is_iso8601_date || m_hours.has_value())) {
            // If a timezone offset or GMT/UTC/Z was not specified and:
            // - Either this is not an ISO8601 [simplified] date
            // - Or this is a date-time form [of an ISO8601 date].
            // https://tc39.es/ecma262/#sec-date.parse:
            // "When the UTC offset representation is absent, date-only forms are interpreted as a UTC time and date-time forms are interpreted as a local time."

            time_ms = JS::utc_time(time_ms); // The date was given in local time; convert it to a UTC timestamp.
        } else {                             // An ISO8601 date-only form
            ;                                // nop; leave timestamp as UTC
        }

        return JS::time_clip(time_ms);
    }
    ALWAYS_INLINE ErrorOr<double> parse()
    {
        if (TRY(maybe_iso_8601()))
            return build_date(true);

        // Convert the input string to uppercase only ~after~ parsing ISO8601 failed.
        // This saves some time (two string copies) if parsing a ISO8601 date succeeds.
        // The index stays exactly where it was before converting to uppercase.
        auto str_uppercase = m_input.to_uppercase_string();
        m_input = str_uppercase;
        // FIXME: Two full string copies could be avoided, if to_uppercase can be done in place.
        // The underlying StringView m_input protects itself from modifying its contents. Bummer.

        while (!is_eof())
            if (!loop())
                return Error::from_string_literal("Cannot parse date components.");

        if (!guess_date_from_numbers())
            return Error::from_string_literal("Cannot guess date.");

        return build_date();
    }
    DateParser(StringView const& str)
        : GenericLexer(str)
    {
    }

public:
    ALWAYS_INLINE static double parse(StringView const& str)
    {
        return DateParser(str).parse().value_or(NAN);
    }
};
