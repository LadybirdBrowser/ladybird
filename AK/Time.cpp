/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <AK/DateConstants.h>
#include <AK/GenericLexer.h>
#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <AK/Time.h>

#ifdef AK_OS_WINDOWS
#    include <AK/Windows.h>
#    define localtime_r(time, tm) localtime_s(tm, time)
#    define gmtime_r(time, tm) gmtime_s(tm, time)
#    define tzname _tzname
#    define timegm _mkgmtime
#endif

namespace AK {

int days_in_month(int year, unsigned month)
{
    VERIFY(month >= 1 && month <= 12);
    if (month == 2)
        return is_leap_year(year) ? 29 : 28;

    bool is_long_month = (month == 1 || month == 3 || month == 5 || month == 7 || month == 8 || month == 10 || month == 12);
    return is_long_month ? 31 : 30;
}

unsigned day_of_week(int year, unsigned month, int day)
{
    VERIFY(month >= 1 && month <= 12);
    constexpr Array seek_table = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (month < 3)
        --year;

    return (year + year / 4 - year / 100 + year / 400 + seek_table[month - 1] + day) % 7;
}

Duration Duration::from_ticks(clock_t ticks, time_t ticks_per_second)
{
    auto secs = ticks % ticks_per_second;

    i32 nsecs = 1'000'000'000 * (ticks - (ticks_per_second * secs)) / ticks_per_second;
    i32 extra_secs = sane_mod(nsecs, 1'000'000'000);
    return Duration::from_half_sanitized(secs, extra_secs, nsecs);
}

Duration Duration::from_timespec(const struct timespec& ts)
{
    i32 nsecs = ts.tv_nsec;
    i32 extra_secs = sane_mod(nsecs, 1'000'000'000);
    return Duration::from_half_sanitized(ts.tv_sec, extra_secs, nsecs);
}

Duration Duration::from_timeval(const struct timeval& tv)
{
    i32 usecs = tv.tv_usec;
    i32 extra_secs = sane_mod(usecs, 1'000'000);
    VERIFY(0 <= usecs && usecs < 1'000'000);
    return Duration::from_half_sanitized(tv.tv_sec, extra_secs, usecs * 1'000);
}

i64 Duration::to_truncated_seconds() const
{
    VERIFY(m_nanoseconds < 1'000'000'000);
    if (m_seconds < 0 && m_nanoseconds) {
        // Since m_seconds is negative, adding 1 can't possibly overflow
        return m_seconds + 1;
    }
    return m_seconds;
}

i64 Duration::to_truncated_milliseconds() const
{
    VERIFY(m_nanoseconds < 1'000'000'000);
    Checked<i64> milliseconds((m_seconds < 0) ? m_seconds + 1 : m_seconds);
    milliseconds *= 1'000;
    milliseconds += m_nanoseconds / 1'000'000;
    if (m_seconds < 0) {
        if (m_nanoseconds % 1'000'000 != 0) {
            // Does not overflow: milliseconds <= 1'999.
            milliseconds++;
        }
        // We dropped one second previously, put it back in now that we have handled the rounding.
        milliseconds -= 1'000;
    }
    if (!milliseconds.has_overflow())
        return milliseconds.value();
    return m_seconds < 0 ? -0x8000'0000'0000'0000LL : 0x7fff'ffff'ffff'ffffLL;
}

i64 Duration::to_truncated_microseconds() const
{
    VERIFY(m_nanoseconds < 1'000'000'000);
    Checked<i64> microseconds((m_seconds < 0) ? m_seconds + 1 : m_seconds);
    microseconds *= 1'000'000;
    microseconds += m_nanoseconds / 1'000;
    if (m_seconds < 0) {
        if (m_nanoseconds % 1'000 != 0) {
            // Does not overflow: microseconds <= 1'999'999.
            microseconds++;
        }
        // We dropped one second previously, put it back in now that we have handled the rounding.
        microseconds -= 1'000'000;
    }
    if (!microseconds.has_overflow())
        return microseconds.value();
    return m_seconds < 0 ? -0x8000'0000'0000'0000LL : 0x7fff'ffff'ffff'ffffLL;
}

i64 Duration::to_seconds() const
{
    VERIFY(m_nanoseconds < 1'000'000'000);
    if (m_seconds >= 0 && m_nanoseconds) {
        Checked<i64> seconds(m_seconds);
        seconds++;
        return seconds.has_overflow() ? 0x7fff'ffff'ffff'ffffLL : seconds.value();
    }
    return m_seconds;
}

i64 Duration::to_milliseconds() const
{
    VERIFY(m_nanoseconds < 1'000'000'000);
    Checked<i64> milliseconds((m_seconds < 0) ? m_seconds + 1 : m_seconds);
    milliseconds *= 1'000;
    milliseconds += m_nanoseconds / 1'000'000;
    if (m_seconds >= 0 && m_nanoseconds % 1'000'000 != 0)
        milliseconds++;
    if (m_seconds < 0) {
        // We dropped one second previously, put it back in now that we have handled the rounding.
        milliseconds -= 1'000;
    }
    if (!milliseconds.has_overflow())
        return milliseconds.value();
    return m_seconds < 0 ? -0x8000'0000'0000'0000LL : 0x7fff'ffff'ffff'ffffLL;
}

i64 Duration::to_microseconds() const
{
    VERIFY(m_nanoseconds < 1'000'000'000);
    Checked<i64> microseconds((m_seconds < 0) ? m_seconds + 1 : m_seconds);
    microseconds *= 1'000'000;
    microseconds += m_nanoseconds / 1'000;
    if (m_seconds >= 0 && m_nanoseconds % 1'000 != 0)
        microseconds++;
    if (m_seconds < 0) {
        // We dropped one second previously, put it back in now that we have handled the rounding.
        microseconds -= 1'000'000;
    }
    if (!microseconds.has_overflow())
        return microseconds.value();
    return m_seconds < 0 ? -0x8000'0000'0000'0000LL : 0x7fff'ffff'ffff'ffffLL;
}

i64 Duration::to_nanoseconds() const
{
    VERIFY(m_nanoseconds < 1'000'000'000);
    Checked<i64> nanoseconds((m_seconds < 0) ? m_seconds + 1 : m_seconds);
    nanoseconds *= 1'000'000'000;
    nanoseconds += m_nanoseconds;
    if (m_seconds < 0) {
        // We dropped one second previously, put it back in now that we have handled the rounding.
        nanoseconds -= 1'000'000'000;
    }
    if (!nanoseconds.has_overflow())
        return nanoseconds.value();
    return m_seconds < 0 ? -0x8000'0000'0000'0000LL : 0x7fff'ffff'ffff'ffffLL;
}

timespec Duration::to_timespec() const
{
    VERIFY(m_nanoseconds < 1'000'000'000);
    return { static_cast<time_t>(m_seconds), static_cast<long>(m_nanoseconds) };
}

timeval Duration::to_timeval() const
{
    VERIFY(m_nanoseconds < 1'000'000'000);
    // This is done because winsock defines tv_sec and tv_usec as long, and Linux64 as long int.
    using sec_type = decltype(declval<timeval>().tv_sec);
    using usec_type = decltype(declval<timeval>().tv_usec);
    return { static_cast<sec_type>(m_seconds), static_cast<usec_type>(m_nanoseconds) / 1000 };
}

Duration Duration::from_half_sanitized(i64 seconds, i32 extra_seconds, u32 nanoseconds)
{
    VERIFY(nanoseconds < 1'000'000'000);

    if ((seconds <= 0 && extra_seconds > 0) || (seconds >= 0 && extra_seconds < 0)) {
        // Opposite signs mean that we can definitely add them together without fear of overflowing i64:
        seconds += extra_seconds;
        extra_seconds = 0;
    }

    // Now the only possible way to become invalid is overflowing i64 towards positive infinity:
    if (Checked<i64>::addition_would_overflow<i64, i64>(seconds, extra_seconds)) {
        if (seconds < 0) {
            return Duration::min();
        } else {
            return Duration::max();
        }
    }

    return Duration { seconds + extra_seconds, nanoseconds };
}

namespace {

#if defined(AK_OS_WINDOWS)
#    define CLOCK_REALTIME 0
#    define CLOCK_MONOTONIC 1

// Ref https://stackoverflow.com/a/51974214
Duration now_time_from_filetime()
{
    FILETIME ft {};
    GetSystemTimeAsFileTime(&ft);

    // Units: 1 LSB == 100 ns
    ULARGE_INTEGER hundreds_of_nanos {
        .LowPart = ft.dwLowDateTime,
        .HighPart = ft.dwHighDateTime
    };

    constexpr u64 num_hundred_nanos_per_sec = 1000ULL * 1000ULL * 10ULL;
    constexpr u64 seconds_from_jan_1601_to_jan_1970 = 11644473600ULL;

    // To convert to Unix Epoch, subtract the number of hundred nanosecond intervals from Jan 1, 1601 to Jan 1, 1970.
    hundreds_of_nanos.QuadPart -= (seconds_from_jan_1601_to_jan_1970 * num_hundred_nanos_per_sec);

    return Duration::from_nanoseconds(hundreds_of_nanos.QuadPart * 100);
}

Duration now_time_from_query_performance_counter()
{
    static LARGE_INTEGER ticks_per_second;
    // FIXME: Limit to microseconds for now, but could probably use nanos?
    static float ticks_per_microsecond;
    if (ticks_per_second.QuadPart == 0) {
        QueryPerformanceFrequency(&ticks_per_second);
        VERIFY(ticks_per_second.QuadPart != 0);
        ticks_per_microsecond = static_cast<float>(ticks_per_second.QuadPart) / 1'000'000.0F;
    }

    LARGE_INTEGER now_time {};
    QueryPerformanceCounter(&now_time);
    return Duration::from_microseconds(static_cast<i64>(now_time.QuadPart / ticks_per_microsecond));
}

Duration now_time_from_clock(int clock_id)
{
    if (clock_id == CLOCK_REALTIME)
        return now_time_from_filetime();
    return now_time_from_query_performance_counter();
}
#else
static Duration now_time_from_clock(clockid_t clock_id)
{
    timespec now_spec {};
    ::clock_gettime(clock_id, &now_spec);
    return Duration::from_timespec(now_spec);
}
#endif

}

MonotonicTime MonotonicTime::now()
{
    return MonotonicTime { now_time_from_clock(CLOCK_MONOTONIC) };
}

MonotonicTime MonotonicTime::now_coarse()
{
    return MonotonicTime { now_time_from_clock(CLOCK_MONOTONIC_COARSE) };
}

UnixDateTime UnixDateTime::from_iso8601_week(u32 week_year, u32 week)
{
    auto january_1_weekday = day_of_week(week_year, 1, 1);
    i32 offset_to_monday = (january_1_weekday <= 3) ? -january_1_weekday : 7 - january_1_weekday;
    i32 first_monday_of_year = 1 + offset_to_monday;
    i32 day_of_year = (first_monday_of_year + (week - 1) * 7) + 1;

    // FIXME: There should be a more efficient way to do this that doesn't require a loop.
    u8 month = 1;
    while (true) {
        auto days = days_in_month(week_year, month);
        if (day_of_year <= days)
            break;

        day_of_year -= days;
        ++month;
    }

    return UnixDateTime::from_unix_time_parts(week_year, month, static_cast<u8>(day_of_year), 0, 0, 0, 0);
}

UnixDateTime UnixDateTime::now()
{
    return UnixDateTime { now_time_from_clock(CLOCK_REALTIME) };
}

UnixDateTime UnixDateTime::now_coarse()
{
    return UnixDateTime { now_time_from_clock(CLOCK_REALTIME_COARSE) };
}

ErrorOr<String> UnixDateTime::to_string(StringView format, LocalTime local_time) const
{
    struct tm tm;

    auto timestamp = m_offset.to_timespec().tv_sec;
    if (local_time == LocalTime::Yes)
        (void)localtime_r(&timestamp, &tm);
    else
        (void)gmtime_r(&timestamp, &tm);

    StringBuilder builder;
    size_t const format_len = format.length();

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
                int week_number = ((tm.tm_yday + wday_of_year_beginning) / 7) + 1;
                if (wday_of_year_beginning > 3) {
                    if (tm.tm_yday >= 7 - wday_of_year_beginning) {
                        --week_number;
                    } else {
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

ByteString UnixDateTime::to_byte_string(StringView format, LocalTime local_time) const
{
    return MUST(to_string(format, local_time)).to_byte_string();
}

Optional<UnixDateTime> UnixDateTime::parse(StringView format, StringView string, bool from_gmt)
{
    unsigned format_pos = 0;

    struct tm tm = {};
    tm.tm_isdst = -1;

    auto parsing_failed = false;

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
            tm.tm_year = (num - 19) * 100 + (tm.tm_year % 100);
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
            tm.tm_mon = mon - 1;
            tm.tm_mday = day;
            tm.tm_year = year > 1900 ? year - 1900 : (year <= 99 && year > 69 ? year : 100 + year);
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
            (void)mktime(&tm);
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
            string_lexer.consume_while(is_ascii_space);
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
        case 'x': {
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

    if (from_gmt) {
        // When from_gmt is true, the parsed time is in GMT and needs to be converted to Unix time
        tm.tm_isdst = 0; // GMT doesn't have daylight saving time
        auto gmt_time = timegm(&tm);
        if (gmt_time == -1)
            return {};
        return UnixDateTime::from_seconds_since_epoch(gmt_time);
    }

    return UnixDateTime::from_unix_time_parts(
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec, 0);
}

}
