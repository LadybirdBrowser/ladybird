/*
 * Copyright (c) 2025, Manuel Zahariev <manuel@duck.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Time.h>
#include <LibCore/DateTime.h>
#include <LibJS/Runtime/Date.h>
#include <LibRegex/RegexPattern.h>

// Parse DateTime regex patterns
class DateRegex : public RegexPattern {
protected:
    enum DateTimeGroupEnum {
        SIGN = 'A',
        YEAR,
        MONTH,
        DAY,
        HH,
        MM,
        SS,
        MS,
        MS_PERMISSIVE, // .1 .12 .123
        HHAM,
        HHPM,
        ISO8601, // Whether a ISO8601 simplified represention was parsed.

        TZSIGN,
        TZHH,
        TZMM,
        TZSS,
        TZNS,
        TZHHMM,
        TZUTC, // Catch Z, GMT or UTC. Stand-alone, indicates GMT. It can also introduce a timezone offset (GMT+12:34).

        NUMBER,
        SIGNED_NUMBER,
        ONE_NUMBER, // the input string contains only one number

        PUNCT,
        JUNK,
        FAIL,

        _AFTER_UPPERCASE,

        JAN = 'a', // WARNING: keep enum in exact order of months
        FEB,
        MAR,
        APR,
        MAY, // The month of May
        JUN,
        JUL,
        AUG,
        SEP, // September
        OCT,
        NOV,
        DEC, // December

        // US mainland time zones
        EDT, // WARNING: keep enum in exact order of time offset
        EST, // Eastern Standard Time
        CDT,
        CST,
        MDT,
        MST,
        PDT,
        PST,

        _AFTER_LAST
    };

    static_assert(_AFTER_UPPERCASE <= 'Z' + 1, "DateTimeGroupEnum uppercase overflow");
    static_assert(_AFTER_LAST <= 'z' + 1, "DateTimeGroupEnum overflow");

    ALWAYS_INLINE static Optional<u64> string_to_ns(StringView const& string)
    {
        return string_to_decimals<u64>(string, 9);
    }
    ALWAYS_INLINE static Optional<u16> string_to_ms(StringView const& string)
    {
        return string_to_decimals<u16>(string, 3);
    }
    ALWAYS_INLINE static u64 guess_year(u64 number)
    {
        if (number <= 49)
            return 2000 + number;
        if (number <= 99)
            return 1900 + number;

        return number;
    }
    // Guess in this order: year, month, day
    ALWAYS_INLINE static bool guess_date_from_1_number(Vector<u64> const& numbers, Optional<i32>& year, Optional<u8>& month, Optional<u8>& day)
    {
        VERIFY(numbers.size() == 1);

        u64 const number0 = numbers.at(0);

        if (!year.has_value()) { // the number must be a year
            year = guess_year(number0);
            return true;
        }

        if (!month.has_value()) { // the number must be a month
            if (number0 > 12)
                return false;
            month = number0;
            return true;
        }

        // At this point, the year and month must have been specified some other way (e.g. "Feb +002002").
        if (number0 > 31)
            return false;
        day = number0;

        return true;
    }
    // Guess default order "day-year" or adapt
    ALWAYS_INLINE static bool guess_date_from_2_numbers(Vector<u64> const& numbers, Optional<i32>& year, Optional<u8>& month, Optional<u8>& day)
    {
        VERIFY(numbers.size() == 2);

        u64 const number0 = numbers.at(0);
        u64 const number1 = numbers.at(1);

        if (year.has_value() && month.has_value())
            return false; // Too many numbers

        if (year.has_value()) {
            if (number0 <= 12) { // ... is a month
                if (number1 > 31)
                    return false;
                month = number0;
                day = number1;
                return true;
            }

            if (number0 <= 31) { // ... is a day
                if (number1 > 12)
                    return false;
                month = number1;
                day = number0;
                return true;
            }
        }

        // At this point, one of the numbers is the year
        if (!month.has_value())
            return false; // Firefox fails on guessing 2 numbers. We do the same

        // At this point, the month has been read from a month name
        if ((number0 > 31) && (number1 > 31))
            return false;                       // Neither of the numbers can be a day
        if ((number0 > 31) || (number0 == 0)) { // ... is a year
            day = number1;
            year = guess_year(number0);
            return true;
        }

        // Default order is day -> year
        day = number0;
        year = guess_year(number1);

        return true;
    }
    // Only "month-day-year" (default) and "year-month-day" are supported (same as firefox and chrome)
    ALWAYS_INLINE static bool guess_date_from_3_numbers(Vector<u64> const& numbers, Optional<i32>& year, Optional<u8>& month, Optional<u8>& day)
    {
        VERIFY(numbers.size() == 3);

        u64 const number0 = numbers.at(0);
        u64 const number1 = numbers.at(1);
        u64 const number2 = numbers.at(2);

        if (year.has_value() || month.has_value())
            return false; // Too many numbers

        if ((number0 > 31) || number0 == 0) { // YMD
            if (number1 > 12)
                return false;
            if (number2 > 31)
                return false;
            month = number1;
            day = number2;
            year = guess_year(number0);

            return true;
        }

        // MDY
        if (number0 > 12)
            return false; // Both Firefox and Chrome fail for the first number >12 and <=31. Weird. We do the same
        if (number1 > 31)
            return false;

        month = number0;
        day = number1;
        year = guess_year(number2);

        return true;
    }
    ALWAYS_INLINE static bool guess_date_from_numbers(Vector<u64> const& numbers, Optional<i32>& year, Optional<u8>& month, Optional<u8>& day)
    {
        switch (numbers.size()) {
        case 0:
            return true; // The year and possibly month may have already been calculated. Verify later
        case 1:
            return guess_date_from_1_number(numbers, year, month, day);
        case 2:
            return guess_date_from_2_numbers(numbers, year, month, day);
        case 3:
            return guess_date_from_3_numbers(numbers, year, month, day);
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

public:
    DateRegex(RegexPattern const& pattern)
        : RegexPattern(pattern)
    {
    }
    DateRegex(String const& string, Precedence type = Precedence::Character)
        : RegexPattern(string, type)
    {
    }

    ALWAYS_INLINE Optional<double> parse(StringView const& string, regex::PosixFlags flags = regex::PosixFlags::Insensitive) const
    {
        RegexResult result = match(string, flags);
        warnln("DEBUG regex: {}", this->string());
        warnln("DEBUG matches: {} {} {} {} {} {}",
            result.success, result.count, result.n_named_capture_groups, result.capture_group_matches.size(), result.n_operations, string);

        if (!result.success)
            return Optional<double> {};
        Vector<u64> numbers;

        Optional<i8> sign;
        Optional<i32> year;
        Optional<u8> month;
        Optional<u8> day;
        Optional<u8> hours;
        Optional<u8> minutes;
        Optional<u8> seconds;
        Optional<u16> milliseconds;

        // True if GMT/UTC/Z specified and there is no timezone offset; false if timezone offset.
        // No value if there is no timezone information: we have to guess whether the date was given in GMT or local time.
        Optional<bool> timezone_utc;
        Optional<i8> timezone_sign; // +1 or -1 if there is a timezone offset.
        Optional<u8> timezone_hours;
        Optional<u8> timezone_minutes;
        Optional<u8> timezone_seconds;
        Optional<u64> timezone_nanoseconds;
        Optional<u16> timezone_hhmin;

        bool date_iso8601 = false; // Whether this is an ISO8601 simplified date.
        u32 one_number;

        for (size_t i = 0; i < result.capture_group_matches.size(); ++i) {
            auto groups = result.capture_group_matches.at(i);
            warnln("DEBUG capture_group_matches {}: {}", i, groups.size());
            for (size_t j = 0; j < groups.size(); ++j) {
                auto const& group = groups.at(j);
                auto const name = group_name_first_char(group);
                if (!name.has_value())
                    continue;

                warnln("DEBUG group matches {} {} {}", j, name.value(), group.view.to_string().value());

                switch (name.value()) {
                case YEAR:
                    year = number<i32>(group);
                    break;
                case SIGN:
                    sign = does_group_start_with_char(group, '-') ? -1 : 1;
                    break;
                case MONTH:
                    month = number<u8>(group);
                    break;
                case DAY:
                    day = number<u8>(group);
                    break;
                case HH:
                    hours = number<u8>(group);
                    break;
                case HHAM:
                    warnln("DEBUG AM:{}", hours);
                    if (!hours.has_value() || (hours.value() > 12))
                        return NAN;
                    if (hours.value() == 12)
                        hours = 0;
                    break;
                case HHPM:
                    if (!hours.has_value() || (hours.value() > 12))
                        return NAN;
                    if (hours.value() < 12)
                        hours.value() += 12;
                    break;
                case MM:
                    minutes = number<u8>(group);
                    break;
                case SS:
                    seconds = number<u8>(group);
                    break;
                case MS:
                    milliseconds = number<u16>(group);
                    break;
                case MS_PERMISSIVE:
                    milliseconds = string_to_ms(group.view.string_view());
                    break;
                case ISO8601:
                    date_iso8601 = true;
                    break;

                case NUMBER:
                    numbers.append(number<u32>(group).value());
                    break;

                case SIGNED_NUMBER:
                    if (!hours.has_value()) { // a signed number before reading time is just a number like the "-10" in "12-10-2024"
                        numbers.append(number<u32>(group).value());
                        sign = 1; // the "minus" before the number is just a dash (does not indicate sign)
                        break;
                    }
                    // At this point (after time had been read) a signed number is a military timezone offset

                    if (timezone_utc.has_value()) // there can be only one explicit timezone
                        return NAN;               // like Firefox

                    military_offset(group.view.string_view(), timezone_hours, timezone_minutes);
                    timezone_sign = sign;
                    timezone_utc = false;
                    break;

                case ONE_NUMBER:
                    one_number = number<u32>(group).value();
                    warnln("DEBUG one_number: {}", one_number);

                    if (one_number == 0) {
                        year = 2000;
                        break;
                    }

                    if (one_number <= 12) {
                        // Firefox and Chrome interpret standalone numbers below 12 as months in 2001. Weird! We do the same.
                        year = 2001;
                        month = one_number;
                        break;
                    }

                    // Firefox and Chrome fail on standalone numbers between 12 and 31. Weird! We do the same.
                    if (one_number <= 31)
                        return NAN;

                    year = guess_year(one_number);

                    break;
                case TZUTC:
                    timezone_utc = true; // Simply indicates that Z/GMT/UTC was caught. Can be overwriten later.

                    timezone_sign = {}; // Reset all timezone components
                    timezone_hhmin = {};
                    timezone_hours = {};
                    timezone_minutes = {};
                    timezone_seconds = {};
                    timezone_nanoseconds = {};
                    break;
                case TZSIGN:
                    if (timezone_sign.has_value()) // Firefox fails if there is more than one explicit time zone. We do the same.
                        return NAN;
                    timezone_utc = false;
                    timezone_sign = does_group_start_with_char(group, '-') ? -1 : +1;
                    break;
                case TZHH:
                    timezone_hours = number<u8>(group);
                    break;
                case TZMM:
                    timezone_minutes = number<u8>(group);
                    break;
                case TZSS:
                    timezone_seconds = number<u8>(group);
                    break;
                case TZNS:
                    timezone_nanoseconds = string_to_ns(group.view.string_view());
                    break;

                case TZHHMM: // GMT +1[2[3[4]]]
                    military_offset(group.view.string_view(), timezone_hours, timezone_minutes);
                    timezone_utc = false;
                    warnln("DEBUG TZHHMIN {} {} {}:{}", group.view.string_view(), timezone_sign, timezone_hours, timezone_minutes);
                    break;

                case JUNK: // Accept junk only at the beginning.
                    if (numbers.size() > 0)
                        return NAN;
                    if (hours.has_value())
                        return NAN;
                    break;

                case PUNCT: // accept punctuation only at beginning
                    if (hours.has_value())
                        return NAN;
                    break;

                case FAIL:
                    return NAN;

                case JAN:
                case FEB:
                case MAR:
                case APR:
                case MAY:
                case JUN:
                case JUL:
                case AUG:
                case SEP:
                case OCT:
                case NOV:
                case DEC:
                    month = 1 + name.value() - JAN; // If multiple values are encountered, the last one is used.
                    break;

                case EDT:
                case EST:
                case CDT:
                case CST:
                case MDT:
                case MST:
                case PDT:
                case PST:
                    if ((numbers.size() == 0) && !month.has_value())
                        break; // Ignore timezone before date.

                    // Both Firefox and Chrome support any number of US timezones in the input string.
                    // Last one matters. We do the same.
                    timezone_utc = false;
                    timezone_sign = -1;
                    // This works because DT is identical to the next ST (e.g. MDT == PST).
                    // WARNING: Relies on a fixed order of US timezones in the DateTimeGroupEnum.
                    timezone_hours = 4 + (name.value() - DateTimeGroupEnum::EDT + 1) / 2;
                    break;

                default:
                    warnln("DEBUG default: {}", name.value());
                    break;
                }
            }
        }

        warnln("DEBUG --- numbers: {}", numbers.size());
        for (auto const& n : numbers)
            warnln("DEBUG number: {}", n);

        if (!guess_date_from_numbers(numbers, year, month, day))
            return NAN;

        warnln("DEBUG {}-{}-{} {}:{}:{}.{} {} tz {}:{}:{}", year, month, day, hours, minutes, seconds, milliseconds, timezone_utc, timezone_hours, timezone_minutes, timezone_seconds);

        if (!year.has_value())
            return NAN;

        AK::UnixDateTime time = AK::UnixDateTime::from_unix_time_parts( // local time
            year.value(),
            month.value_or(1),
            day.value_or(1),
            hours.value_or(0),
            minutes.value_or(0),
            seconds.value_or(0),
            milliseconds.value_or(0));

        double time_ms = static_cast<double>(time.milliseconds_since_epoch()); // Assume the date was given in UTC.

        warnln("DEBUG timezone_utc: {} date_iso8601: {} timezone_sign: {}", timezone_utc, date_iso8601, timezone_sign);

        if (timezone_sign.has_value()) { // A timezone offset was specified.
            warnln("DEBUG tz: {}", time_ms);
            VERIFY(!timezone_utc.has_value() || !timezone_utc.value()); // Cannot have a timezone offset and "GMT/UTC/Z" at the same time.

            time_ms -= 1.0 * timezone_sign.value() * static_cast<double>( // Convert to a UTC timestamp: local timestamp minus timezone offset.
                           timezone_hours.value_or(0) * 3'600'000 + timezone_minutes.value_or(0) * 60'000 + timezone_seconds.value_or(0) * 1'000 + static_cast<u64>(timezone_nanoseconds.value_or(0) / 1'000'000));
        } else if (!timezone_utc.has_value() && (!date_iso8601 || hours.has_value())) {
            // A timezone offset or GMT/UTC/Z was not specified and:
            // - Either this is not an ISO8601 [simplified] date
            // - Or this is a date-time form [of an ISO8601 date].
            // https://tc39.es/ecma262/#sec-date.parse:
            // "When the UTC offset representation is absent, date-only forms are interpreted as a UTC time and date-time forms are interpreted as a local time."

            time_ms = JS::utc_time(time_ms); // The date was given in local time; convert it to a UTC timestamp.
        } else {                             // An ISO8601 date-only form
            ;                                // nop; leave timestamp as UTC
        }

        warnln("DEBUG time_ms: {} ~ {}", time_ms, string);
        // return time_ms;
        return JS::time_clip(time_ms);
    }

    // Canonical patterns created from strings. These must match exactly the patterns output from DateRegexGenerator
    ALWAYS_INLINE static DateRegex es_datetime_string_format()
    {
        return DateRegex { "^(?<L>(?<B>[0-9]{4}))(?:-(?<C>0[0-9]|1[0-2])(?:-(?<D>[0-2][0-9]|3[01]))?)?(?:T(?<E>[01][0-9]|2[0-4]):(?<F>[0-5][0-9])(?::(?<G>[0-5][0-9])(?:[.](?<H>[0-9]{3}))?)?(?:(?<S>Z)|(?<M>[+\\-])(?<N>[01][0-9]|2[0-4]):(?<O>[0-5][0-9]))?)?$"_string };
    }
    ALWAYS_INLINE static DateRegex simplified_iso_8601_datetime()
    {
        return DateRegex { "-(?<T>000000)|^(?<L>(?<B>[0-9]{4})|(?<B>[+\\-][0-9]{6}))(?:-(?<C>0[0-9]|1[0-2])(?:-(?<D>[0-2][0-9]|3[01]))?)?(?:T(?<E>[01][0-9]|2[0-4]):(?<F>[0-5][0-9])(?::(?<G>[0-5][0-9])(?:[.](?<H>[0-9]{3}))?)?(?:(?<S>Z)|(?<M>[+\\-])(?<N>[01][0-9]|2[0-4]):(?<O>[0-5][0-9])(?::(?<P>[0-5][0-9])(?:[.](?<Q>[0-9]{1,9}))?)?|(?<M>[+\\-])(?<R>[0-9]{4})|(?<Y>[ ]*[+\\-][0-9]{1,3}))?)?$"_string };
    }
    ALWAYS_INLINE static DateRegex es_datetime_tostring()
    {
        return DateRegex { "^(?:Sun|Mon|Tue|Wed|Thu|Fri|Sat)[ ](?:(?<a>Jan)|(?<b>Feb)|(?<c>Mar)|(?<d>Apr)|(?<e>May)|(?<f>Jun)|(?<g>Jul)|(?<i>Sep)|(?<j>Oct)|(?<k>Nov)|(?<l>Dec))[ ](?<D>[0-2][0-9]|3[01])[ ](?<A>[0-9]{4})[ ](?<E>[01][0-9]|2[0-4]):(?<F>[0-5][0-9]):(?<G>[0-5][0-9])[ ](?<S>GMT)(?:(?<M>[+\\-])(?<R>[0-9]{4})(?:[ ][(][a-z ]+[)])?)?$"_string };
    }
    ALWAYS_INLINE static DateRegex es_datetime_toutcstring()
    {
        return DateRegex { "^(?:Sun|Mon|Tue|Wed|Thu|Fri|Sat),[ ](?<D>[0-2][0-9]|3[01])[ ](?:(?<a>Jan)|(?<b>Feb)|(?<c>Mar)|(?<d>Apr)|(?<e>May)|(?<f>Jun)|(?<g>Jul)|(?<i>Sep)|(?<j>Oct)|(?<k>Nov)|(?<l>Dec))[ ](?<A>[0-9]{4})[ ](?<E>[01][0-9]|2[0-4]):(?<F>[0-5][0-9]):(?<G>[0-5][0-9])[ ](?<S>GMT)$"_string };
    }

    ALWAYS_INLINE static DateRegex es_date_parse()
    {
        return DateRegex { "-(?<T>000000)|^(?<L>(?<B>[0-9]{4})|(?<B>[+\\-][0-9]{6}))(?:-(?<C>0[0-9]|1[0-2])(?:-(?<D>[0-2][0-9]|3[01]))?)?(?:T(?<E>[01][0-9]|2[0-4]):(?<F>[0-5][0-9])(?::(?<G>[0-5][0-9])(?:[.](?<H>[0-9]{3}))?)?(?:(?<S>Z)|(?<M>[+\\-])(?<N>[01][0-9]|2[0-4]):(?<O>[0-5][0-9])(?::(?<P>[0-5][0-9])(?:[.](?<Q>[0-9]{1,9}))?)?|(?<M>[+\\-])(?<R>[0-9]{4})|(?<Y>[ ]*[+\\-][0-9]{1,3}))?)?$|(?<Y>[0-9]{3}:)|T?(?<E>[01][0-9]|2[0-4]|[0-9]):[ ]*(?:(?<F>[0-5][0-9])(?::[ ]*(?<G>[0-5][0-9])(?:[.](?<I>[0-9]{1,3})[0-9]*)?)?)?[ ]*(?:(?<J>am)|(?<K>pm))?|(?<S>GMT|UTC|Z)?[ ]*(?<M>[+\\-])(?<N>[01][0-9]|2[0-4]):(?<O>[0-5][0-9])(?::(?<P>[0-5][0-9])(?:[.](?<Q>[0-9]{1,9}))?)?|(?<S>GMT|UTC|Z)[ ]*(?<M>[+\\-])(?<R>[0-9]{1,4})(?<Y>[0-9]{1,3})?|^[+-./, ]*(?<V>[0-9]{1,6})[+-./, ]*$|(?<B>[+\\-][0-9]{6})|(?<A>[+\\-])(?<U>[0-9]{1,4})|(?<T>[0-9]{1,6})|(?<a>jan[a-z]{0,8})|(?<b>feb[a-z]{0,8})|(?<c>mar[a-z]{0,8})|(?<d>apr[a-z]{0,8})|(?<e>may[a-z]{0,8})|(?<f>jun[a-z]{0,8})|(?<g>jul[a-z]{0,8})|(?<i>sep[a-z]{0,8})|(?<j>oct[a-z]{0,8})|(?<k>nov[a-z]{0,8})|(?<l>dec[a-z]{0,8})|(?<n>est)|(?<m>edt)|(?<p>cst)|(?<o>cdt)|(?<r>mst)|(?<q>mdt)|(?<t>pst)|(?<s>pdt)|(?<S>GMT|UTC|Z)|[(][^)]+[)]?$|(?<W>[+-./,:]+)|(?<X>[A-Za-z][A-Za-z+-./,:]*)|[ ]+"_string };
    }
};

// Use this class in tests and debugging
class DateRegexGenerator : protected DateRegex {
public:
    struct DatePatterns {
        DateRegex ecma_script_datetime;
        DateRegex iso8601_simplified_datetime;
        DateRegex date_tostring;
        DateRegex date_toutcstring;

        DateRegex permissive;
        DateRegex date_parse = permissive;
    };

    // Create all supported date patterns incrementally, from components
    static DatePatterns create_date_patterns();
};
