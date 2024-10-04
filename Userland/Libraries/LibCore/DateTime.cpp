/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/DateConstants.h>
#include <AK/GenericLexer.h>
#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <AK/Time.h>
#include <LibCore/DateTime.h>
#include <LibUnicode/TimeZone.h>
#include <errno.h>
#include <time.h>

namespace Core {

static Optional<StringView> parse_time_zone_name(GenericLexer& lexer)
{
    auto const& time_zones = Unicode::available_time_zones();
    auto start_position = lexer.tell();

    Optional<StringView> canonicalized_time_zone;

    lexer.ignore_until([&](auto) {
        auto time_zone = lexer.input().substring_view(start_position, lexer.tell() - start_position + 1);

        auto it = time_zones.find_if([&](auto const& candidate) { return time_zone.equals_ignoring_ascii_case(candidate); });
        if (it == time_zones.end())
            return false;

        canonicalized_time_zone = *it;
        return true;
    });

    if (canonicalized_time_zone.has_value())
        lexer.ignore();

    return canonicalized_time_zone;
}

static void apply_time_zone_offset(StringView time_zone, UnixDateTime& time)
{
    if (auto offset = Unicode::time_zone_offset(time_zone, time); offset.has_value())
        time -= offset->offset;
}

DateTime DateTime::now()
{
    return from_timestamp(time(nullptr));
}

DateTime DateTime::create(int year, int month, int day, int hour, int minute, int second)
{
    DateTime dt;
    dt.set_time(year, month, day, hour, minute, second);
    return dt;
}

DateTime DateTime::from_timestamp(time_t timestamp)
{
    struct tm tm;
    localtime_r(&timestamp, &tm);
    DateTime dt;
    dt.m_year = tm.tm_year + 1900;
    dt.m_month = tm.tm_mon + 1;
    dt.m_day = tm.tm_mday;
    dt.m_hour = tm.tm_hour;
    dt.m_minute = tm.tm_min;
    dt.m_second = tm.tm_sec;
    dt.m_timestamp = timestamp;
    return dt;
}

unsigned DateTime::weekday() const
{
    return ::day_of_week(m_year, m_month, m_day);
}

unsigned DateTime::days_in_month() const
{
    return ::days_in_month(m_year, m_month);
}

unsigned DateTime::day_of_year() const
{
    return ::day_of_year(m_year, m_month, m_day);
}

bool DateTime::is_leap_year() const
{
    return ::is_leap_year(m_year);
}

void DateTime::set_time(int year, int month, int day, int hour, int minute, int second)
{
    struct tm tm = {};
    tm.tm_sec = second;
    tm.tm_min = minute;
    tm.tm_hour = hour;
    tm.tm_mday = day;
    tm.tm_mon = month - 1;
    tm.tm_year = year - 1900;
    tm.tm_isdst = -1;
    // mktime() doesn't read tm.tm_wday and tm.tm_yday, no need to fill them in.

    m_timestamp = mktime(&tm);

    // mktime() normalizes the components to the right ranges (Jan 32 -> Feb 1 etc), so read fields back out from tm.
    m_year = tm.tm_year + 1900;
    m_month = tm.tm_mon + 1;
    m_day = tm.tm_mday;
    m_hour = tm.tm_hour;
    m_minute = tm.tm_min;
    m_second = tm.tm_sec;
}

void DateTime::set_time_only(int hour, int minute, Optional<int> second)
{
    set_time(year(), month(), day(), hour, minute, second.has_value() ? second.release_value() : this->second());
}

void DateTime::set_date(Core::DateTime const& other)
{
    set_time(other.year(), other.month(), other.day(), hour(), minute(), second());
}

ErrorOr<String> DateTime::to_string(StringView format, LocalTime local_time) const
{
    struct tm tm;

    if (local_time == LocalTime::Yes)
        localtime_r(&m_timestamp, &tm);
    else
        gmtime_r(&m_timestamp, &tm);

    StringBuilder builder;
    size_t const format_len = format.length();

    auto format_time_zone_offset = [&](bool with_separator) -> ErrorOr<void> {
        struct tm gmt_tm;
        gmtime_r(&m_timestamp, &gmt_tm);

        gmt_tm.tm_isdst = -1;
        auto gmt_timestamp = mktime(&gmt_tm);

        auto offset_seconds = static_cast<time_t>(difftime(m_timestamp, gmt_timestamp));
        StringView offset_sign;

        if (offset_seconds >= 0) {
            offset_sign = "+"sv;
        } else {
            offset_sign = "-"sv;
            offset_seconds *= -1;
        }

        auto offset_hours = offset_seconds / 3600;
        auto offset_minutes = (offset_seconds % 3600) / 60;
        auto separator = with_separator ? ":"sv : ""sv;

        TRY(builder.try_appendff("{}{:02}{}{:02}", offset_sign, offset_hours, separator, offset_minutes));
        return {};
    };

    for (size_t i = 0; i < format_len; ++i) {
        if (format[i] != '%') {
            TRY(builder.try_append(format[i]));
        } else {
            if (++i == format_len)
                return String {};

            switch (format[i]) {
            case 'a':
                TRY(builder.try_append(short_day_names[tm.tm_wday]));
                break;
            case 'A':
                TRY(builder.try_append(long_day_names[tm.tm_wday]));
                break;
            case 'b':
                TRY(builder.try_append(short_month_names[tm.tm_mon]));
                break;
            case 'B':
                TRY(builder.try_append(long_month_names[tm.tm_mon]));
                break;
            case 'C':
                TRY(builder.try_appendff("{:02}", (tm.tm_year + 1900) / 100));
                break;
            case 'd':
                TRY(builder.try_appendff("{:02}", tm.tm_mday));
                break;
            case 'D':
                TRY(builder.try_appendff("{:02}/{:02}/{:02}", tm.tm_mon + 1, tm.tm_mday, (tm.tm_year + 1900) % 100));
                break;
            case 'e':
                TRY(builder.try_appendff("{:2}", tm.tm_mday));
                break;
            case 'h':
                TRY(builder.try_append(short_month_names[tm.tm_mon]));
                break;
            case 'H':
                TRY(builder.try_appendff("{:02}", tm.tm_hour));
                break;
            case 'I': {
                int display_hour = tm.tm_hour % 12;
                if (display_hour == 0)
                    display_hour = 12;
                TRY(builder.try_appendff("{:02}", display_hour));
                break;
            }
            case 'j':
                TRY(builder.try_appendff("{:03}", tm.tm_yday + 1));
                break;
            case 'l': {
                int display_hour = tm.tm_hour % 12;
                if (display_hour == 0)
                    display_hour = 12;
                TRY(builder.try_appendff("{:2}", display_hour));
                break;
            }
            case 'm':
                TRY(builder.try_appendff("{:02}", tm.tm_mon + 1));
                break;
            case 'M':
                TRY(builder.try_appendff("{:02}", tm.tm_min));
                break;
            case 'n':
                TRY(builder.try_append('\n'));
                break;
            case 'p':
                TRY(builder.try_append(tm.tm_hour < 12 ? "AM"sv : "PM"sv));
                break;
            case 'r': {
                int display_hour = tm.tm_hour % 12;
                if (display_hour == 0)
                    display_hour = 12;
                TRY(builder.try_appendff("{:02}:{:02}:{:02} {}", display_hour, tm.tm_min, tm.tm_sec, tm.tm_hour < 12 ? "AM" : "PM"));
                break;
            }
            case 'R':
                TRY(builder.try_appendff("{:02}:{:02}", tm.tm_hour, tm.tm_min));
                break;
            case 'S':
                TRY(builder.try_appendff("{:02}", tm.tm_sec));
                break;
            case 't':
                TRY(builder.try_append('\t'));
                break;
            case 'T':
                TRY(builder.try_appendff("{:02}:{:02}:{:02}", tm.tm_hour, tm.tm_min, tm.tm_sec));
                break;
            case 'u':
                TRY(builder.try_appendff("{}", tm.tm_wday ? tm.tm_wday : 7));
                break;
            case 'U': {
                int const wday_of_year_beginning = (tm.tm_wday + 6 * tm.tm_yday) % 7;
                int const week_number = (tm.tm_yday + wday_of_year_beginning) / 7;
                TRY(builder.try_appendff("{:02}", week_number));
                break;
            }
            case 'V': {
                int const wday_of_year_beginning = (tm.tm_wday + 6 + 6 * tm.tm_yday) % 7;
                int week_number = (tm.tm_yday + wday_of_year_beginning) / 7 + 1;
                if (wday_of_year_beginning > 3) {
                    if (tm.tm_yday >= 7 - wday_of_year_beginning)
                        --week_number;
                    else {
                        int const days_of_last_year = days_in_year(tm.tm_year + 1900 - 1);
                        int const wday_of_last_year_beginning = (wday_of_year_beginning + 6 * days_of_last_year) % 7;
                        week_number = (days_of_last_year + wday_of_last_year_beginning) / 7 + 1;
                        if (wday_of_last_year_beginning > 3)
                            --week_number;
                    }
                }
                TRY(builder.try_appendff("{:02}", week_number));
                break;
            }
            case 'w':
                TRY(builder.try_appendff("{}", tm.tm_wday));
                break;
            case 'W': {
                int const wday_of_year_beginning = (tm.tm_wday + 6 + 6 * tm.tm_yday) % 7;
                int const week_number = (tm.tm_yday + wday_of_year_beginning) / 7;
                TRY(builder.try_appendff("{:02}", week_number));
                break;
            }
            case 'y':
                TRY(builder.try_appendff("{:02}", (tm.tm_year + 1900) % 100));
                break;
            case 'Y':
                TRY(builder.try_appendff("{}", tm.tm_year + 1900));
                break;
            case 'z':
                TRY(format_time_zone_offset(false));
                break;
            case ':':
                if (++i == format_len) {
                    TRY(builder.try_append("%:"sv));
                    break;
                }
                if (format[i] != 'z') {
                    TRY(builder.try_append("%:"sv));
                    TRY(builder.try_append(format[i]));
                    break;
                }
                TRY(format_time_zone_offset(true));
                break;
            case 'Z': {
                auto const* timezone_name = tzname[tm.tm_isdst == 0 ? 0 : 1];
                TRY(builder.try_append({ timezone_name, strlen(timezone_name) }));
                break;
            }
            case '%':
                TRY(builder.try_append('%'));
                break;
            default:
                TRY(builder.try_append('%'));
                TRY(builder.try_append(format[i]));
                break;
            }
        }
    }

    return builder.to_string();
}

ByteString DateTime::to_byte_string(StringView format, LocalTime local_time) const
{
    return MUST(to_string(format, local_time)).to_byte_string();
}

Optional<DateTime> DateTime::parse(StringView format, StringView string)
{
    unsigned format_pos = 0;

    struct tm tm = {};
    tm.tm_isdst = -1;

    auto parsing_failed = false;
    auto tm_represents_utc_time = false;
    Optional<StringView> parsed_time_zone;

    GenericLexer string_lexer(string);

    auto parse_number = [&] {
        auto result = string_lexer.consume_decimal_integer<int>();
        if (result.is_error()) {
            parsing_failed = true;
            return 0;
        }
        return result.value();
    };

    auto consume = [&](char c) {
        if (!string_lexer.consume_specific(c))
            parsing_failed = true;
    };

    auto consume_specific_ascii_case_insensitive = [&](StringView name) {
        auto next_string = string_lexer.peek_string(name.length());
        if (next_string.has_value() && next_string->equals_ignoring_ascii_case(name)) {
            string_lexer.consume(name.length());
            return true;
        }
        return false;
    };

    while (format_pos < format.length() && !string_lexer.is_eof()) {
        if (format[format_pos] != '%') {
            consume(format[format_pos]);
            format_pos++;
            continue;
        }

        format_pos++;
        if (format_pos == format.length())
            return {};

        switch (format[format_pos]) {
        case 'a': {
            auto wday = 0;
            for (auto name : short_day_names) {
                if (consume_specific_ascii_case_insensitive(name)) {
                    tm.tm_wday = wday;
                    break;
                }
                ++wday;
            }
            if (wday == 7)
                return {};
            break;
        }
        case 'A': {
            auto wday = 0;
            for (auto name : long_day_names) {
                if (consume_specific_ascii_case_insensitive(name)) {
                    tm.tm_wday = wday;
                    break;
                }
                ++wday;
            }
            if (wday == 7)
                return {};
            break;
        }
        case 'h':
        case 'b': {
            auto mon = 0;
            for (auto name : short_month_names) {
                if (consume_specific_ascii_case_insensitive(name)) {
                    tm.tm_mon = mon;
                    break;
                }
                ++mon;
            }
            if (mon == 12)
                return {};
            break;
        }
        case 'B': {
            auto mon = 0;
            for (auto name : long_month_names) {
                if (consume_specific_ascii_case_insensitive(name)) {
                    tm.tm_mon = mon;
                    break;
                }
                ++mon;
            }
            if (mon == 12)
                return {};
            break;
        }
        case 'C': {
            int num = parse_number();
            tm.tm_year = (num - 19) * 100;
            break;
        }
        case 'd':
            tm.tm_mday = parse_number();
            break;
        case 'D': {
            int mon = parse_number();
            consume('/');
            int day = parse_number();
            consume('/');
            int year = parse_number();
            tm.tm_mon = mon + 1;
            tm.tm_mday = day;
            tm.tm_year = (year + 1900) % 100;
            break;
        }
        case 'e':
            tm.tm_mday = parse_number();
            break;
        case 'H':
            tm.tm_hour = parse_number();
            break;
        case 'I': {
            int num = parse_number();
            tm.tm_hour = num % 12;
            break;
        }
        case 'j':
            // a little trickery here... we can get mktime() to figure out mon and mday using out of range values.
            // yday is not used so setting it is pointless.
            tm.tm_mday = parse_number();
            tm.tm_mon = 0;
            mktime(&tm);
            break;
        case 'm': {
            int num = parse_number();
            tm.tm_mon = num - 1;
            break;
        }
        case 'M':
            tm.tm_min = parse_number();
            break;
        case 'n':
        case 't':
            string_lexer.consume_while(is_ascii_blank);
            break;
        case 'r':
        case 'p': {
            auto ampm = string_lexer.consume(2);
            if (ampm == "PM") {
                if (tm.tm_hour < 12)
                    tm.tm_hour += 12;
            } else if (ampm != "AM") {
                return {};
            }
            break;
        }
        case 'R':
            tm.tm_hour = parse_number();
            consume(':');
            tm.tm_min = parse_number();
            break;
        case 'S':
            tm.tm_sec = parse_number();
            break;
        case 'T':
            tm.tm_hour = parse_number();
            consume(':');
            tm.tm_min = parse_number();
            consume(':');
            tm.tm_sec = parse_number();
            break;
        case 'w':
            tm.tm_wday = parse_number();
            break;
        case 'y': {
            int year = parse_number();
            tm.tm_year = year <= 99 && year > 69 ? 1900 + year : 2000 + year;
            break;
        }
        case 'Y': {
            int year = parse_number();
            tm.tm_year = year - 1900;
            break;
        }
        case 'z': {
            tm_represents_utc_time = true;
            if (string_lexer.consume_specific('Z')) {
                // UTC time
                break;
            }
            int sign;

            if (string_lexer.consume_specific('+'))
                sign = -1;
            else if (string_lexer.consume_specific('-'))
                sign = +1;
            else
                return {};

            auto hours = parse_number();
            int minutes;
            if (string_lexer.consume_specific(':')) {
                minutes = parse_number();
            } else {
                minutes = hours % 100;
                hours = hours / 100;
            }

            tm.tm_hour += sign * hours;
            tm.tm_min += sign * minutes;
            break;
        }
        case 'x': {
            tm_represents_utc_time = true;
            auto hours = parse_number();
            int minutes;
            if (string_lexer.consume_specific(':')) {
                minutes = parse_number();
            } else {
                minutes = hours % 100;
                hours = hours / 100;
            }

            tm.tm_hour -= hours;
            tm.tm_min -= minutes;
            break;
        }
        case 'X': {
            if (!string_lexer.consume_specific('.'))
                return {};
            auto discarded = parse_number();
            (void)discarded; // NOTE: the tm structure does not support sub second precision, so drop this value.
            break;
        }
        case 'Z':
            parsed_time_zone = parse_time_zone_name(string_lexer);
            if (!parsed_time_zone.has_value())
                return {};

            tm_represents_utc_time = true;
            break;
        case '+': {
            Optional<char> next_format_character;

            if (format_pos + 1 < format.length()) {
                next_format_character = format[format_pos + 1];

                // Disallow another formatter directly after %+. This is to avoid ambiguity when parsing a string like
                // "ignoreJan" with "%+%b", as it would be non-trivial to know that where the %b field begins.
                if (next_format_character == '%')
                    return {};
            }

            auto discarded = string_lexer.consume_until([&](auto ch) { return ch == next_format_character; });
            if (discarded.is_empty())
                return {};

            break;
        }
        case '%':
            consume('%');
            break;
        default:
            parsing_failed = true;
            break;
        }

        if (parsing_failed)
            return {};

        format_pos++;
    }

    if (!string_lexer.is_eof() || format_pos != format.length())
        return {};

    // If an explicit time zone offset was present, the time in tm was shifted to UTC. If a time zone name was present,
    // the time in tm needs to be shifted to UTC. In both cases, convert the result to local time, as that is what is
    // expected by `mktime`.
    if (tm_represents_utc_time) {
        auto utc_time = UnixDateTime::from_seconds_since_epoch(timegm(&tm));

        if (parsed_time_zone.has_value())
            apply_time_zone_offset(*parsed_time_zone, utc_time);

        time_t utc_time_t = utc_time.seconds_since_epoch();
        localtime_r(&utc_time_t, &tm);
    }

    return DateTime::from_timestamp(mktime(&tm));
}

}
