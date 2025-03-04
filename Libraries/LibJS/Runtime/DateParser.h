/*
 * Copyright (c) 2025, Manuel Zahariev <manuel@duck.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/GenericLexer.h>
#include <AK/StringView.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibJS/Runtime/Date.h>

#include <cctype>
#include <cmath>

template<typename T>
void print(Vector<T> const& v)
{
    for (auto const& e : v)
        warnln("DEBUG {}", e);
}

class DateStringParser : public GenericLexer {
private:
    Vector<u64> m_numbers;

    Optional<i8> m_sign;
    Optional<i32> m_year;
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

    void debug_print()
    {
        warnln("DEBUG DateStringParser {}-{}-{}T{}:{}:{}.{} {} {}:{}:{}.{} utc: {}",
            m_year,
            m_month,
            m_day,
            m_hours,
            m_minutes,
            m_seconds,
            m_milliseconds,
            m_timezone_sign, // +1 or -1 if there is a timezone offset.
            m_timezone_hours,
            m_timezone_minutes,
            m_timezone_seconds,
            m_timezone_nanoseconds,
            m_timezone_utc);
    }

    // Useful for parsing numbers with variable length decimals, e.g. nanosecond decimals
    // Example decimals=9: .123 --> 123000000 .123456 -> 123456000
    template<typename T>
    ALWAYS_INLINE static Optional<T> string_to_decimals(StringView const& str, size_t decimals)
    {
        AK::StringBuilder sb;
        AK::FormatBuilder fb(sb);
        if (fb.put_string(str, AK::FormatBuilder::Align::Left, decimals, decimals, '0').is_error())
            return {};

        return fb.builder().to_string().release_value().to_number<T>();
    }

    ALWAYS_INLINE static Optional<u64> string_to_ns(StringView const& str)
    {
        return string_to_decimals<u64>(str, 9);
    }
    ALWAYS_INLINE static Optional<u16> string_to_ms(StringView const& str)
    {
        return string_to_decimals<u16>(str, 3);
    }
    ALWAYS_INLINE static u64 guess_year(u64 const number)
    {
        switch (number) {
        case 0 ... 49:
            return 2000 + number;
        case 50 ... 99:
            return 1900 + number;
        }

        return number;
    }

    ALWAYS_INLINE bool one_number(u64 const number) // The whole input string is just one stand-alone number.
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
    // Guess in this order: year, month, day
    ALWAYS_INLINE bool guess_date_from_1_number()
    {
        VERIFY(m_numbers.size() == 1);

        u64 const number0 = m_numbers.at(0);

        if (!m_year.has_value() && !m_month.has_value())
            return one_number(number0);

        if (!m_year.has_value()) { // the number must be a year
            m_year = guess_year(number0);
            return true;
        }

        if (!m_month.has_value()) { // the number must be a month
            if (number0 > 12)
                return false;
            m_month = number0;
            return true;
        }

        // At this point, the year and month must have been specified some other way (e.g. "Feb +002002").
        if ((number0 > 31) || (number0 == 0)) // Invalid day number.
            return false;

        m_day = number0;

        return true;
    }
    // Guess default order "day-year" or adapt
    ALWAYS_INLINE bool guess_date_from_2_numbers()
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
    // Only "month-day-year" (default) and "year-month-day" are supported (same as firefox and chrome)
    ALWAYS_INLINE bool guess_date_from_3_numbers()
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
    ALWAYS_INLINE bool guess_date_from_numbers()
    {
        warnln("DEBUG guess_date_from_numbers {}", m_numbers.size());
        print(m_numbers);
        debug_print();

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
    ALWAYS_INLINE static void military_offset(StringView const& value, Optional<u8>& hours, Optional<u8>& minutes)
    {
        Optional<u16> const timezone_hhmin = value.to_number<u16>();

        VERIFY(timezone_hhmin.has_value());
        if (value.length() < 3) {
            hours = timezone_hhmin.value();
            return;
        }
        hours = timezone_hhmin.value() / 100;
        minutes = timezone_hhmin.value() % 100;
    }

    bool append_number(StringView number)
    {
        m_numbers.append(number.to_number<u64>().value());
        return true;
    }

    bool loop()
    {
        warnln("DEBUG loop {}", peek());
        switch (peek()) {
        case '0' ... '9':
            return numberish();
        case '+':
        case '-':
            return signish();
        case 'a' ... 'z':
            return wordish();
        case ' ':
        case '.':
        case ',':
        case '/': // FIXME ?
            ignore();
            return true; // punctuation
        case '(':
            break;
        default:
            return false;
        }

        // Consume time zone name (anything in brackets).
        ignore_until(')');
        ignore();
        ignore_while(isspace);

        return is_eof();
    }

    bool ampm()
    { // FIXME: combine with timeish
        VERIFY(m_hours.has_value());

        bool const space = consume_while(isspace).length() > 0; // FIXME: Tight coupling with timeish.

        if (consume_specific("am"sv)) {
            if (!separator())
                return false; // 12:34 AMsomething
            if (m_hours.value() > 12)
                return false; // 14:45AM
            if (m_hours.value() == 12)
                m_hours = 0; // 12:05AM -> 00:05
            return true;
        }

        if (consume_specific("pm"sv)) {
            if (!separator())
                return false; // 12:34 PMsomething
            if (m_hours.value() > 12)
                return false; // 14:45PM
            if (m_hours.value() < 12)
                m_hours = m_hours.value() + 12;
            return true;
        }

        return space || separator();
    }
    bool timeish(StringView const& hours)
    { // H[H]:MM[:SS[.mss[...]]][AM|PM]
        if (m_hours.has_value())
            return false; // Time has already been read.

        if (hours.length() > 3)
            return false; // 123:
        m_hours = hours.to_number<u8>().value();

        StringView minutes = consume_while(isdigit);
        if (minutes.length() != 2)
            return false; // 12.3
        m_minutes = minutes.to_number<u8>().value();

        switch (peek()) {
        case '.':
            return false; // 12:34.
        case '+':
        case '-':
            return tz_offset();
        case ':':
            ignore();
            break;
        case 'z':
            ignore();
            m_timezone_utc = true;
            return separator();
        default:
            return true;
        }

        StringView seconds = consume_while(isdigit);
        if (seconds.length() != 2)
            return false; // 12:34:5
        m_seconds = seconds.to_number<u8>().value();

        switch (peek()) {
        case '.':
            ignore();
            break;
        case '+':
        case '-':
            return tz_offset();
        case 'z':
            m_timezone_utc = true;
            ignore();
            return separator();
        default:
            return true;
        }

        StringView milliseconds = consume_while(isdigit);
        switch (milliseconds.length()) {
        case 0:
            return false; // 12:34:56.
        case 1 ... 3:
            m_milliseconds = string_to_ms(milliseconds);
            break;
        default:
            m_milliseconds = string_to_ms(milliseconds.substring_view(0, 3)); // FIXME: do not read long milliseconds into buffer
        }

        switch (peek()) {
        case '+':
        case '-':
            return tz_offset();
        case 'z':
            ignore();
            m_timezone_utc = true;
            return separator();
        }

        return true;
    }

    bool numberish()
    {
        StringView number = consume_while(isdigit);
        VERIFY(number.length() > 0);

        if (number.length() > 6)
            return false; // 1234567

        if (consume_specific(':')) {
            if (!timeish(number))
                return false;
            return ampm();
        }

        m_numbers.append(number.to_number<u64>().value());

        return separator(); // Must be followed by a separator.
    }

    bool signish()
    {
        i8 const sign = (consume() == '-') ? -1 : +1;
        StringView const number = consume_while(isdigit);

        switch (number.length()) {
        case 0:
            return (sign == -1); // Not a sign after all; '+' is forbidden if it is not a sign; otherwise this is punctuation
        case 1 ... 5:
            break;
        case 6: // signed year
            if (m_year.has_value())
                return false;
            m_year = sign * number.to_number<u16>().value();
            return true;
        default:
            return false;
        }

        if (!m_hours.has_value()) {
            append_number(number); // no "bare" timezone offset without time
            return true;
        }

        m_timezone_sign = sign;
        return tz_offset(number);
    }

    bool tz_offset()
    {
        m_timezone_sign = (consume() == '-') ? -1 : +1;
        StringView const number = consume_while(isdigit);
        return tz_offset(number);
    }

    bool tz_offset(StringView const& number)
    {
        if (m_timezone_hours.has_value()) // Cannot have more than one timezone offset or a timezone name followed by a timezone offset.
            return false;

        m_timezone_utc = false;
        switch (number.length()) {
        case 0:
            return false;
        case 1:
        case 2:
            break;
        case 3: // "Military" timezone offset
        case 4: {
            u16 const tz_hhmm = number.to_number<u16>().value();
            m_timezone_hours = tz_hhmm / 100;
            m_timezone_minutes = tz_hhmm % 100;
            return true;
        }
        default:
            return false;
        }

        m_timezone_hours = number.to_number<u8>().value();

        if (!consume_specific(':'))
            return true; // +H[H] "military" time offset

        // Timezone with colon
        StringView const minutes = consume_while(isdigit);
        if (minutes.length() != 2)
            return false;
        m_timezone_minutes = minutes.to_number<u8>();

        if (!consume_specific(':'))
            return true;

        StringView const seconds = consume_while(isdigit);
        if (seconds.length() != 2)
            return false;
        m_timezone_seconds = seconds.to_number<u8>();

        if (!consume_specific('.'))
            return true;

        StringView nanoseconds = consume_while(isdigit);
        if (nanoseconds.length() == 0)
            return false;
        if (nanoseconds.length() > 9)
            return false;
        m_timezone_nanoseconds = string_to_ns(nanoseconds);

        return true;
    }

    bool gmt(StringView const& str)
    {
        warnln("DEBUG gmt {}", str);
        if (!consume_specific(str))
            return false;

        m_timezone_utc = true;

        bool space = consume_while(isspace).length() > 0;
        switch (peek()) {
        case '+':
        case '-':
            return tz_offset();
        }

        return space || separator();
    }
    bool separator()
    {
        if (is_eof())
            return true;
        switch (consume()) {
        case ' ':
        case ',':
        case '.':
        case '/':
        case '-':
            return true;
        }
        return false;
    }

    bool word()
    {
        std::ignore = consume_while(isalpha);
        if (m_numbers.size() > 0)
            return false;
        if (m_hours.has_value())
            return false;

        return true; // ignore junk at the beginning
    }

    bool us_timezone(StringView const& str, u8 hours, u8 minutes = 0, i8 sign = -1)
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

    bool month_name(StringView const& str, u8 month)
    {
        VERIFY(str.length() == 3); // only 3-letter month prefixes
        for (size_t i = 0; i < 3; ++i)
            if (str[i] != peek(i))
                return false;

        ignore_while(isalpha);
        m_month = month;

        return separator(); // Must end with a separator.
    }

    bool wordish()
    { // The top of a trie catching date "keywords".
        switch (peek()) {
        case 'a': // apr aug
            return month_name("apr"sv, 4) || month_name("aug"sv, 8) || word();
        case 'c': // cst cdt
            return us_timezone("cst"sv, 6) || us_timezone("cdt"sv, 5) || word();
        case 'd': // dec
            return month_name("dec"sv, 12) || word();
        case 'e': // est edt
            return us_timezone("est"sv, 5) || us_timezone("edt"sv, 4) || word();
        case 'f': // feb
            return month_name("feb"sv, 2) || word();
        case 'g': // gmt
            return gmt("gmt"sv) || word();
        case 'j': // jan jun jul
            return month_name("jan"sv, 1) || month_name("jun"sv, 6) || month_name("jul"sv, 7) || word();
        case 'm': // mar may mst mdt
            return month_name("mar"sv, 3) || month_name("may"sv, 5) || us_timezone("mst"sv, 7) || us_timezone("mdt"sv, 6) || word();
        case 'n': // nov
            return month_name("nov"sv, 11) || word();
        case 'o': // oct
            return month_name("oct"sv, 10) || word();
        case 'p': // pst pdt
            warnln("DEBUG wordish {}", peek());
            return us_timezone("pst"sv, 8) || us_timezone("pdt"sv, 7) || word();
        case 's': // sep
            return month_name("sep"sv, 9) || word();
        case 'u': // utc
            return gmt("utc"sv) || word();
        case 'z': // z
            return gmt("z"sv) || word();
        }

        return word();
    }

    // Returns
    //  - {} if the string is not in iso_8601 format; side-effect: may fill numbers
    //  - double in case of success
    //  - NAN in case of failure
    Optional<double> iso_8601()
    {
        switch (peek()) {
        case '0' ... '9':
            return iso_year();
        case '+':
        case '-':
            return iso_signed_year();
        }

        return {};
    }

    double build_iso_date()
    {
        if (!is_eof())
            return NAN; // Garbage after end of date string.
        VERIFY(m_numbers.size() < 4);

        switch ((m_year.has_value() ? 4 : 0) + m_numbers.size()) { // Combine whether-has-year and number-count into one switch.
        case 7:
            VERIFY_NOT_REACHED(); // too many numbers
        case 6:
            m_day = m_numbers.at(1);
            [[fallthrough]];
        case 5:
            m_month = m_numbers.at(0);
            [[fallthrough]];
        case 4:
            break;
        // Above: a "signed 6-digit year" has been read.

        // Below: there is no year
        case 3:
            m_day = m_numbers.at(2);
            [[fallthrough]];
        case 2:
            m_month = m_numbers.at(1);
            [[fallthrough]];
        case 1:
            m_year = m_numbers.at(0);
            break;
        case 0: // Year not specified
        default:
            return NAN;
        }

        return build_date(true);
    }

    double build_date(bool date_iso8601 = false)
    {
        VERIFY(is_eof());

        if (!m_year.has_value())
            return NAN;

        if (m_month.has_value() && ((m_month.value() == 0) || (m_month.value() > 12)))
            return NAN;
        if (m_day.has_value() && ((m_day.value() == 0) || (m_day.value() > 31)))
            return NAN;

        if (m_hours.has_value() && (m_hours.value() > 24))
            return NAN;
        if (m_minutes.has_value() && (m_minutes.value() > 59))
            return NAN;
        if (m_seconds.has_value() && (m_seconds.value() > 59))
            return NAN;

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
        } else if (!m_timezone_utc.has_value() && (!date_iso8601 || m_hours.has_value())) {
            // A timezone offset or GMT/UTC/Z was not specified and:
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

    Optional<double> iso_month()
    {
        StringView number = consume_while(isdigit);

        switch (number.length()) {
        case 0:
            return {};
        case 1:
            append_number(number);
            return {};
        case 2:
            break;
        default: // month number too long
            return NAN;
        }

        append_number(number);

        if (is_eof())
            return build_iso_date();
        if (consume_specific('-'))
            return iso_day();
        if (consume_specific('t'))
            return iso_time();

        return {};
    }
    Optional<double> iso_day()
    {
        StringView number = consume_while(isdigit);

        switch (number.length()) {
        case 0:
            return {};
        case 1:
            append_number(number);
            return {};
        case 2:
            break;
        default: // month number too long
            return NAN;
        }

        append_number(number);
        if (is_eof())
            return build_iso_date();
        if (consume_specific('t'))
            return iso_time();

        return {};
    }
    Optional<double> iso_time()
    {
        // After reading the 'T', any failures return NAN (failed parse)
        StringView hours = consume_while(isdigit);

        if (hours.length() != 2)
            return NAN;
        m_hours = hours.to_number<u8>().value();

        if (!consume_specific(':'))
            return NAN; // needs at least HH:MM

        StringView minutes = consume_while(isdigit);

        if (minutes.length() != 2)
            return NAN;
        m_minutes = minutes.to_number<u8>().value();

        if (is_eof())
            return build_iso_date();
        if (consume_specific(':'))
            return iso_seconds();
        return iso_tz();
    }
    Optional<double> iso_seconds()
    {
        StringView seconds = consume_while(isdigit);
        if (seconds.length() != 2)
            return NAN;
        m_seconds = seconds.to_number<u8>().value();

        if (is_eof())
            return build_iso_date();
        if (consume_specific('.'))
            return iso_milliseconds();

        return iso_tz();
    }
    Optional<double> iso_milliseconds()
    {
        StringView milliseconds = consume_while(isdigit);
        switch (milliseconds.length()) {
        case 0:
            return NAN;
        case 1:
            m_milliseconds = milliseconds.to_number<u16>().value() * 100;
            break;
        case 2:
            m_milliseconds = milliseconds.to_number<u16>().value() * 10;
            break;
        case 3:
            m_milliseconds = milliseconds.to_number<u16>().value();
            break;
        default:
            m_milliseconds = milliseconds.substring_view(0, 3).to_number<u16>().value();
        }

        if (is_eof())
            return build_iso_date();
        return iso_tz();
    }
    Optional<double> iso_tz()
    {
        switch (consume()) {
        case 'z':
            m_timezone_utc = true;
            return build_iso_date();
        case '-':
            return iso_tz_offset(-1);
        case '+':
            return iso_tz_offset();
        }

        return NAN;
    }
    Optional<double> iso_tz_offset(i64 sign = 1)
    {
        StringView number = consume_while(isdigit);
        if (number.length() == 4)
            return iso_tz_military(number, sign);
        if (number.length() != 2)
            return NAN;

        m_timezone_sign = sign;
        m_timezone_hours = number.to_number<u8>().value();
        if (!consume_specific(':'))
            return NAN;
        StringView minutes = consume_while(isdigit);
        if (minutes.length() != 2)
            return NAN;
        m_timezone_minutes = minutes.to_number<u8>().value();

        if (is_eof())
            return build_iso_date();
        if (consume_specific(':'))
            return iso_tz_seconds();

        return NAN;
    }
    Optional<double> iso_tz_military(StringView const& offset, i64 sign = 1)
    {
        VERIFY(offset.length() == 4);
        u64 hhmm = offset.to_number<u16>().value();

        m_timezone_sign = sign;
        m_timezone_hours = hhmm / 100;
        m_timezone_minutes = hhmm % 100;

        if (is_eof())
            return build_iso_date();

        return NAN;
    }
    Optional<double> iso_tz_seconds()
    {
        StringView seconds = consume_while(isdigit);
        if (seconds.length() != 2)
            return NAN;

        m_timezone_seconds = seconds.to_number<u8>().value();
        if (is_eof())
            return build_iso_date();
        if (!consume_specific('.'))
            return NAN;

        return iso_tz_nanoseconds();
    }
    Optional<double> iso_tz_nanoseconds()
    {
        StringView nanoseconds = consume_while(isdigit);
        if (nanoseconds.length() > 9)
            return NAN;

        m_timezone_nanoseconds = string_to_decimals<u64>(nanoseconds, 9);

        if (is_eof())
            return build_iso_date();
        return NAN;
    }
    Optional<double> iso_year()
    { // 4-digit year
        StringView number = consume_while(isdigit);

        switch (number.length()) {
        case 0:
            return {}; // no digits
        case 1:
        case 2:
            if (next_is(':')) {
                ignore();
                if (!timeish(number))
                    return NAN;
                if (!ampm())
                    return NAN;
                return {};
            }
            [[fallthrough]];
        case 3:
        case 5:
        case 6: // six-digit year number; no sign means not iso_8601
            append_number(number);
            return {};
        case 4: // four-digit year number
            break;
        default: // number is too long
            return NAN;
        }

        append_number(number);

        if (is_eof())
            return build_iso_date();
        if (consume_specific('-'))
            return iso_month();
        if (consume_specific('t'))
            return iso_time();

        return {};
    }

    Optional<double> iso_signed_year()
    { // six-digit signed year
        i64 sign = +1;
        if (consume() == '-')
            sign = -1;
        StringView number = consume_while(isdigit);

        switch (number.length()) {
        case 0:
            if (sign == +1) // Standalone '+' is invalid.
                return NAN;
            return {};
        case 1 ... 5:
            append_number(number); // This is not an ISO 8601 date string.
            return {};
        case 6:
            break;
        default: // empty or too long
            return NAN;
        }

        m_year = sign * number.to_number<u64>().value();

        // "The representation of the year 0 as -000000 is invalid." https://tc39.es/ecma262/#sec-expanded-years
        // Firefox interprets "-000000" as "Jan 1, 2000".
        if ((sign == -1) && (m_year == 0))
            return NAN;

        if (is_eof())
            return build_iso_date();
        if (consume_specific('-'))
            return iso_month();
        if (consume_specific('t'))
            return iso_time();

        return {};
    }

    DateStringParser(StringView const& str)
        : GenericLexer(str.to_lowercase_string())
    {
    }

    double _parse()
    {
        auto const iso = iso_8601();
        if (iso.has_value())
            return iso.value();

        warnln("DEBUG post iso");

        while (!is_eof())
            if (!loop())
                return NAN;

        if (!guess_date_from_numbers())
            return NAN;
        return build_date();
    }

public:
    ALWAYS_INLINE static double parse(StringView const& str)
    {
        DateStringParser parser(str);
        return parser._parse();
    }
};
