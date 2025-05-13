/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <AK/Time.h>

#ifdef AK_OS_WINDOWS
#    include <AK/Windows.h>
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

}
