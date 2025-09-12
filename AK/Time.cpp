/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <AK/DateConstants.h>
#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <AK/Time.h>
#include <AK/Utf16String.h>

#ifdef AK_OS_WINDOWS
#    include <AK/Windows.h>
#    define localtime_r(time, tm) localtime_s(tm, time)
#    define gmtime_r(time, tm) gmtime_s(tm, time)
#    define tzname _tzname
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

f64 Duration::to_seconds_f64() const
{
    VERIFY(m_nanoseconds < 1'000'000'000);
    return static_cast<double>(m_seconds) + (m_nanoseconds / 1'000'000'000.0);
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
    auto day_of_week_january_4th = (day_of_week(week_year, 1, 4) + 6) % 7;
    int ordinal_day = (7 * week) - day_of_week_january_4th - 3;

    if (ordinal_day < 1)
        return UnixDateTime::from_ordinal_date(week_year - 1, ordinal_day + days_in_year(week_year - 1));
    if (auto days_in_week_year = days_in_year(week_year); static_cast<unsigned>(ordinal_day) > days_in_week_year)
        return UnixDateTime::from_ordinal_date(week_year + 1, ordinal_day - days_in_week_year);
    return UnixDateTime::from_ordinal_date(week_year, ordinal_day);
}

UnixDateTime UnixDateTime::from_ordinal_date(u32 year, u32 day)
{
    static constexpr Array<u32, 12> month_starts_normal = { 1, 32, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335 };
    static constexpr Array<u32, 12> month_starts_leap = { 1, 32, 61, 92, 122, 153, 183, 214, 245, 275, 306, 336 };

    auto const& month_starts = is_leap_year(year) ? month_starts_leap : month_starts_normal;

    // Estimate month using integer division (approx 30.6 days per month)
    auto estimated_month = (day * 12 + 6) / 367; // Gives 0-based month index

    // Correct month if estimate overshot
    if (day < month_starts[estimated_month])
        --estimated_month;

    auto month = estimated_month + 1; // convert to 1-based month
    auto day_of_month = day - month_starts[estimated_month] + 1;

    return UnixDateTime::from_unix_time_parts(year, month, day_of_month, 0, 0, 0, 0);
}

UnixDateTime UnixDateTime::now()
{
    return UnixDateTime { now_time_from_clock(CLOCK_REALTIME) };
}

UnixDateTime UnixDateTime::now_coarse()
{
    return UnixDateTime { now_time_from_clock(CLOCK_REALTIME_COARSE) };
}

ErrorOr<void> UnixDateTime::to_string_impl(StringBuilder& builder, StringView format, LocalTime local_time) const
{
    struct tm tm;

    auto timestamp = m_offset.to_timespec().tv_sec;
    if (local_time == LocalTime::Yes)
        (void)localtime_r(&timestamp, &tm);
    else
        (void)gmtime_r(&timestamp, &tm);

    size_t const format_len = format.length();

    for (size_t i = 0; i < format_len; ++i) {
        if (format[i] != '%') {
            TRY(builder.try_append(format[i]));
        } else {
            if (++i == format_len) {
                builder.clear();
                return {};
            }

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
                TRY(builder.try_append(StringView { timezone_name, strlen(timezone_name) }));
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

    return {};
}

ErrorOr<String> UnixDateTime::to_string(StringView format, LocalTime local_time) const
{
    StringBuilder builder;
    TRY(to_string_impl(builder, format, local_time));
    return builder.to_string();
}

Utf16String UnixDateTime::to_utf16_string(StringView format, LocalTime local_time) const
{
    StringBuilder builder(StringBuilder::Mode::UTF16);
    MUST(to_string_impl(builder, format, local_time));
    return builder.to_utf16_string();
}

ByteString UnixDateTime::to_byte_string(StringView format, LocalTime local_time) const
{
    StringBuilder builder;
    MUST(to_string_impl(builder, format, local_time));
    return builder.to_byte_string();
}

}
