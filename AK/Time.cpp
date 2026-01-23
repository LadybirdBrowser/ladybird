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
#include <AK/Utf16String.h>

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

Duration Duration::from_time_units(i64 time_units, u32 numerator, u32 denominator)
{
    VERIFY(numerator != 0);
    VERIFY(denominator != 0);

    auto seconds_checked = Checked<i64>(time_units);
    seconds_checked.mul(numerator);
    seconds_checked.div(denominator);
    if (time_units < 0)
        seconds_checked.sub(1);

    if (seconds_checked.has_overflow())
        return Duration(time_units >= 0 ? NumericLimits<i64>::max() : NumericLimits<i64>::min(), 0);
    auto seconds = seconds_checked.value_unchecked();
    auto seconds_in_time_units = seconds * denominator / numerator;
    auto remainder_in_time_units = time_units - seconds_in_time_units;
    auto nanoseconds = ((remainder_in_time_units * 1'000'000'000 * numerator) + (denominator / 2)) / denominator;
    if (nanoseconds == 1'000'000'000) {
        seconds++;
        nanoseconds = 0;
    }
    VERIFY(nanoseconds >= 0);
    VERIFY(nanoseconds < 1'000'000'000);
    return Duration(seconds, static_cast<u32>(nanoseconds));
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
    return m_seconds < 0 ? NumericLimits<i64>::min() : NumericLimits<i64>::max();
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
    return m_seconds < 0 ? NumericLimits<i64>::min() : NumericLimits<i64>::max();
}

i64 Duration::to_seconds() const
{
    VERIFY(m_nanoseconds < 1'000'000'000);
    if (m_seconds >= 0 && m_nanoseconds) {
        Checked<i64> seconds(m_seconds);
        seconds++;
        return seconds.has_overflow() ? NumericLimits<i64>::max() : seconds.value();
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
    return m_seconds < 0 ? NumericLimits<i64>::min() : NumericLimits<i64>::max();
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
    return m_seconds < 0 ? NumericLimits<i64>::min() : NumericLimits<i64>::max();
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
    return m_seconds < 0 ? NumericLimits<i64>::min() : NumericLimits<i64>::max();
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

i64 Duration::to_time_units(u32 numerator, u32 denominator) const
{
    VERIFY(numerator != 0);
    VERIFY(denominator != 0);

    auto seconds_product = Checked<i64>::saturating_mul(m_seconds, denominator);
    auto time_units = seconds_product / numerator;
    auto remainder = seconds_product % numerator;

    auto remainder_in_nanoseconds = remainder * 1'000'000'000;
    auto rounding_half = static_cast<i64>(numerator) * 500'000'000;
    time_units = Checked<i64>::saturating_add(time_units, ((static_cast<i64>(m_nanoseconds) * denominator + remainder_in_nanoseconds + rounding_half) / numerator) / 1'000'000'000);

    return time_units;
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

ErrorOr<void> Formatter<Duration>::format(FormatBuilder& builder, Duration value)
{
    if (value.m_nanoseconds >= 1'000'000'000)
        return builder.put_string("{ INVALID }"sv);

    auto align = m_align;
    if (align == FormatBuilder::Align::Default)
        align = FormatBuilder::Align::Right;

    auto sign_mode = m_sign_mode;
    if (sign_mode == FormatBuilder::SignMode::Default)
        sign_mode = FormatBuilder::SignMode::OnlyIfNeeded;

    auto align_width = m_width.value_or(0);

    u8 base;
    bool upper_case = false;
    if (m_mode == Mode::Default || m_mode == Mode::FixedPoint) {
        base = 10;
    } else if (m_mode == Mode::Hexfloat) {
        base = 16;
    } else if (m_mode == Mode::HexfloatUppercase) {
        base = 16;
        upper_case = true;
    } else if (m_mode == Mode::Binary) {
        base = 2;
    } else if (m_mode == Mode::BinaryUppercase) {
        base = 2;
        upper_case = true;
    } else if (m_mode == Mode::Octal) {
        base = 8;
    } else {
        VERIFY_NOT_REACHED();
    }

    auto is_negative = value.m_seconds < 0;
    auto seconds = is_negative ? 0 - static_cast<u64>(value.m_seconds) : static_cast<u64>(value.m_seconds);
    auto nanoseconds = value.m_nanoseconds;
    if (is_negative && nanoseconds > 0) {
        seconds--;
        nanoseconds = 1'000'000'000 - nanoseconds;
    }

    VERIFY(nanoseconds < 1'000'000'000);

    size_t integer_width = 1;
    if (seconds != 0) {
        auto remaining_seconds = seconds / 10;
        while (remaining_seconds != 0) {
            remaining_seconds /= base;
            integer_width++;
        }
    }
    if (sign_mode != FormatBuilder::SignMode::OnlyIfNeeded)
        integer_width++;

    constexpr size_t nanoseconds_length = 9;
    size_t precision = 0;
    u64 nanoseconds_to_precision = nanoseconds;
    if (m_precision.has_value()) {
        precision = min(m_precision.value(), nanoseconds_length);
        for (size_t i = nanoseconds_length; i > precision; i--)
            nanoseconds_to_precision /= base;
    } else if (nanoseconds_to_precision != 0) {
        auto trailing_zeroes = 0;
        while ((nanoseconds_to_precision % base) == 0) {
            nanoseconds_to_precision /= base;
            trailing_zeroes++;
        }
        precision = nanoseconds_length - trailing_zeroes;
    }

    size_t non_integer_width = 0;
    if (precision != 0)
        non_integer_width = precision + 1;
    if (m_alternative_form)
        non_integer_width++;

    auto total_width = integer_width + non_integer_width;

    size_t integer_align_width = 0;
    if (align == FormatBuilder::Align::Right)
        integer_align_width = Checked<size_t>::saturating_sub(align_width, non_integer_width);
    else if (align == FormatBuilder::Align::Center)
        integer_align_width = integer_width + Checked<size_t>::saturating_sub(align_width, total_width) / 2;
    TRY(builder.put_u64(seconds, base, false, upper_case, m_zero_pad, m_use_separator, FormatBuilder::Align::Right, integer_align_width, m_fill, m_sign_mode, is_negative));

    if (nanoseconds_to_precision != 0) {
        TRY(builder.builder().try_append('.'));
        TRY(builder.put_u64(nanoseconds_to_precision, base, false, upper_case, true, m_use_separator, FormatBuilder::Align::Right, precision));
        if (m_precision.has_value() && m_precision.value() > nanoseconds_length) {
            auto zeroes = m_precision.value() - nanoseconds_length;
            TRY(builder.put_padding('0', zeroes));
        }
    }

    if (m_alternative_form)
        TRY(builder.builder().try_append('s'));

    if (align_width > 0 && align != FormatBuilder::Align::Right) {
        auto padding_width = Checked<size_t>::saturating_sub(align_width, max(integer_width, integer_align_width) + non_integer_width);
        TRY(builder.builder().try_append_repeated(m_fill, padding_width));
    }

    return {};
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
    static f64 ticks_per_nanosecond;
    if (ticks_per_second.QuadPart == 0) {
        QueryPerformanceFrequency(&ticks_per_second);
        VERIFY(ticks_per_second.QuadPart != 0);
        ticks_per_nanosecond = static_cast<f64>(ticks_per_second.QuadPart) / 1'000'000'000.0;
    }

    LARGE_INTEGER now_time {};
    QueryPerformanceCounter(&now_time);
    return Duration::from_nanoseconds(static_cast<i64>(now_time.QuadPart / ticks_per_nanosecond));
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
