/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Assertions.h>
#include <AK/Badge.h>
#include <AK/Checked.h>
#include <AK/Platform.h>
#include <AK/Types.h>
#ifdef AK_OS_WINDOWS
// https://learn.microsoft.com/en-us/windows/win32/api/winsock/ns-winsock-timeval
struct timeval {
    long tv_sec { 0 };
    long tv_usec { 0 };
};
#else
#    include <sys/time.h>
#endif
#include <time.h>

namespace AK {

constexpr inline double ms_per_day = 86'400'000;
constexpr inline double seconds_per_day = 86'400;
constexpr inline double seconds_per_year = 60.0 * 60.0 * 24.0 * 365.2425;
constexpr inline auto seconds_per_hour = 3'600;
constexpr inline auto seconds_per_minute = 60;

// Concept to detect types which look like timespec without requiring the type.
template<typename T>
concept TimeSpecType = requires(T t) {
    t.tv_sec;
    t.tv_nsec;
};

constexpr bool is_leap_year(int year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

// Month and day start at 1. Month must be >= 1 and <= 12.
// The return value is 0-indexed, that is 0 is Sunday, 1 is Monday, etc.
// Day may be negative or larger than the number of days
// in the given month.
unsigned day_of_week(int year, unsigned month, int day);

// Month and day start at 1. Month must be >= 1 and <= 12.
// The return value is 0-indexed, that is Jan 1 is day 0.
// Day may be negative or larger than the number of days
// in the given month. If day is negative enough, the result
// can be negative.
constexpr int day_of_year(int year, unsigned month, int day)
{
    if (is_constant_evaluated())
        VERIFY(month >= 1 && month <= 12); // Note that this prevents bad constexpr months, but never actually prints anything.
    else if (!(month >= 1 && month <= 12))
        return 0;

    constexpr Array seek_table = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    int day_of_year = seek_table[month - 1] + day - 1;

    if (is_leap_year(year) && month >= 3)
        day_of_year++;

    return day_of_year;
}

// Month starts at 1. Month must be >= 1 and <= 12.
int days_in_month(int year, unsigned month);

constexpr int days_in_year(int year)
{
    return 365 + (is_leap_year(year) ? 1 : 0);
}

constexpr int weeks_in_year(int year)
{
    return is_leap_year(year) ? 53 : 52;
}

namespace Detail {

// Integer division rounding towards negative infinity.
// TODO: This feels like there should be an easier way to do this.
template<int divisor>
constexpr i64 floor_div_by(i64 dividend)
{
    static_assert(divisor > 1);
    int is_negative = dividend < 0;
    return (dividend + is_negative) / divisor - is_negative;
}

// Counts how many integers n are in the interval [begin, end) with n % positive_mod == 0.
// NOTE: "end" is not considered to be part of the range, hence "[begin, end)".
template<int positive_mod>
constexpr i64 mod_zeros_in_range(i64 begin, i64 end)
{
    return floor_div_by<positive_mod>(end - 1) - floor_div_by<positive_mod>(begin - 1);
}

}

constexpr i64 years_to_days_since_epoch(int year)
{
    int begin_year, end_year, leap_sign;
    if (year < 1970) {
        begin_year = year;
        end_year = 1970;
        leap_sign = -1;
    } else {
        begin_year = 1970;
        end_year = year;
        leap_sign = +1;
    }
    i64 year_i64 = year;
    // This duplicates the logic of 'is_leap_year', with the advantage of not needing any loops.
    // Given that the definition of leap years is not expected to change, this should be a good trade-off.
    i64 days = 365 * (year_i64 - 1970);
    i64 extra_leap_days = 0;
    extra_leap_days += Detail::mod_zeros_in_range<4>(begin_year, end_year);
    extra_leap_days -= Detail::mod_zeros_in_range<100>(begin_year, end_year);
    extra_leap_days += Detail::mod_zeros_in_range<400>(begin_year, end_year);
    return days + extra_leap_days * leap_sign;
}

constexpr i64 days_since_epoch(int year, int month, int day)
{
    return years_to_days_since_epoch(year) + day_of_year(year, month, day);
}

constexpr i64 seconds_since_epoch_to_year(i64 seconds)
{
    // NOTE: We are not using floor() from <math.h> to avoid LibC / DynamicLoader dependency issues.
    auto round_down = [](double value) -> i64 {
        auto as_i64 = static_cast<i64>(value);

        if ((value == as_i64) || (as_i64 >= 0))
            return as_i64;
        return as_i64 - 1;
    };

    auto years_since_epoch = static_cast<double>(seconds) / seconds_per_year;
    return 1970 + round_down(years_since_epoch);
}

// Represents a duration in a "safe" way.
// Minimum: -(2**63) seconds, 0 nanoseconds
// Maximum: 2**63-1 seconds, 999'999'999 nanoseconds
// If any operation (e.g. 'from_timeval' or operator-) would over- or underflow, the closest legal value is returned instead.
// Inputs (e.g. to 'from_timespec') are allowed to be in non-normal form (e.g. "1 second, 2'012'345'678 nanoseconds" or "1 second, -2 microseconds").
// Outputs (e.g. from 'to_timeval') are always in normal form.
//
// NOTE: This class is naive. It may represent either absolute offsets or relative durations. It does not have a reference point in itself,
//       and therefore comparing multiple instances of this class is only sensible if you are sure that their reference point is identical.
//       You should not be using this class directly to represent absolute time.
class Duration {
public:
    constexpr Duration() = default;
    constexpr Duration(Duration const&) = default;
    constexpr Duration& operator=(Duration const&) = default;

    constexpr Duration(Duration&& other)
        : m_seconds(exchange(other.m_seconds, 0))
        , m_nanoseconds(exchange(other.m_nanoseconds, 0))
    {
    }
    constexpr Duration& operator=(Duration&& other)
    {
        if (this != &other) {
            m_seconds = exchange(other.m_seconds, 0);
            m_nanoseconds = exchange(other.m_nanoseconds, 0);
        }
        return *this;
    }

private:
    // This must be part of the header in order to make the various 'from_*' functions constexpr.
    // However, sane_mod can only deal with a limited range of values for 'denominator', so this can't be made public.
    ALWAYS_INLINE static constexpr i64 sane_mod(i64& numerator, i64 denominator)
    {
        VERIFY(2 <= denominator && denominator <= 1'000'000'000);
        // '%' in C/C++ does not work in the obvious way:
        // For example, -9 % 7 is -2, not +5.
        // However, we want a representation like "(-2)*7 + (+5)".
        i64 dividend = numerator / denominator;
        numerator %= denominator;
        if (numerator < 0) {
            // Does not overflow: different signs.
            numerator += denominator;
            // Does not underflow: denominator >= 2.
            dividend -= 1;
        }
        return dividend;
    }
    ALWAYS_INLINE static constexpr i32 sane_mod(i32& numerator, i32 denominator)
    {
        i64 numerator_64 = numerator;
        i64 dividend = sane_mod(numerator_64, denominator);
        // Does not underflow: numerator can only become smaller.
        numerator = static_cast<i32>(numerator_64);
        // Does not overflow: Will be smaller than original value of 'numerator'.
        return static_cast<i32>(dividend);
    }

public:
    [[nodiscard]] constexpr static Duration from_seconds(i64 seconds) { return Duration(seconds, 0); }
    [[nodiscard]] constexpr static Duration from_nanoseconds(i64 nanoseconds)
    {
        i64 seconds = sane_mod(nanoseconds, 1'000'000'000);
        return Duration(seconds, nanoseconds);
    }
    [[nodiscard]] constexpr static Duration from_microseconds(i64 microseconds)
    {
        i64 seconds = sane_mod(microseconds, 1'000'000);
        return Duration(seconds, microseconds * 1'000);
    }
    [[nodiscard]] constexpr static Duration from_milliseconds(i64 milliseconds)
    {
        i64 seconds = sane_mod(milliseconds, 1'000);
        return Duration(seconds, milliseconds * 1'000'000);
    }
    [[nodiscard]] static Duration from_ticks(clock_t, time_t);
    [[nodiscard]] static Duration from_timespec(const struct timespec&);
    [[nodiscard]] static Duration from_timeval(const struct timeval&);
    // We don't pull in <stdint.h> for the pretty min/max definitions because this file is also included in the Kernel
    [[nodiscard]] constexpr static Duration min() { return Duration(-__INT64_MAX__ - 1LL, 0); }
    [[nodiscard]] constexpr static Duration zero() { return Duration(0, 0); }
    [[nodiscard]] constexpr static Duration max() { return Duration(__INT64_MAX__, 999'999'999); }

    // Truncates towards zero (2.8s to 2s, -2.8s to -2s).
    [[nodiscard]] i64 to_truncated_seconds() const;
    [[nodiscard]] i64 to_truncated_milliseconds() const;
    [[nodiscard]] i64 to_truncated_microseconds() const;
    // Rounds away from zero (2.3s to 3s, -2.3s to -3s).
    [[nodiscard]] i64 to_seconds() const;
    [[nodiscard]] i64 to_milliseconds() const;
    [[nodiscard]] i64 to_microseconds() const;
    [[nodiscard]] i64 to_nanoseconds() const;
    [[nodiscard]] timespec to_timespec() const;
    // Rounds towards -inf (it was the easiest to implement).
    [[nodiscard]] timeval to_timeval() const;

    [[nodiscard]] bool is_zero() const { return (m_seconds == 0) && (m_nanoseconds == 0); }
    [[nodiscard]] bool is_negative() const { return m_seconds < 0; }

    constexpr Duration operator+(Duration const& other) const
    {
        VERIFY(m_nanoseconds < 1'000'000'000);
        VERIFY(other.m_nanoseconds < 1'000'000'000);

        u32 new_nsecs = m_nanoseconds + other.m_nanoseconds;
        u32 extra_secs = new_nsecs / 1'000'000'000;
        new_nsecs %= 1'000'000'000;

        i64 this_secs = m_seconds;
        i64 other_secs = other.m_seconds;
        // We would like to just add "this_secs + other_secs + extra_secs".
        // However, computing this naively may overflow even though the result is in-bounds.
        // Example in 8-bit: (-127) + (-2) + (+1) = (-128), which fits in an i8.
        // Example in 8-bit, the other way around: (-2) + (127) + (+1) = 126.
        // So we do something more sophisticated:
        if (extra_secs) {
            VERIFY(extra_secs == 1);
            if (this_secs != 0x7fff'ffff'ffff'ffff) {
                this_secs += 1;
            } else if (other_secs != 0x7fff'ffff'ffff'ffff) {
                other_secs += 1;
            } else {
                /* If *both* are INT64_MAX, then adding them will overflow in any case. */
                return Duration::max();
            }
        }

        Checked<i64> new_secs { this_secs };
        new_secs += other_secs;
        if (new_secs.has_overflow()) {
            if (other_secs > 0)
                return Duration::max();
            else
                return Duration::min();
        }

        return Duration { new_secs.value(), new_nsecs };
    }

    constexpr Duration& operator+=(Duration const& other)
    {
        *this = *this + other;
        return *this;
    }

    constexpr Duration operator-(Duration const& other) const
    {
        VERIFY(m_nanoseconds < 1'000'000'000);
        VERIFY(other.m_nanoseconds < 1'000'000'000);

        if (other.m_nanoseconds)
            return *this + Duration((i64) ~(u64)other.m_seconds, 1'000'000'000 - other.m_nanoseconds);

        if (other.m_seconds != (i64)-0x8000'0000'0000'0000)
            return *this + Duration(-other.m_seconds, 0);

        // Only remaining case: We want to subtract -0x8000'0000'0000'0000 seconds,
        // i.e. add a very large number.

        if (m_seconds >= 0)
            return Duration::max();
        return Duration { (m_seconds + 0x4000'0000'0000'0000) + 0x4000'0000'0000'0000, m_nanoseconds };
    }

    constexpr Duration& operator-=(Duration const& other)
    {
        *this = *this - other;
        return *this;
    }

    constexpr bool operator==(Duration const& other) const = default;
    constexpr int operator<=>(Duration const& other) const
    {
        if (int cmp = (m_seconds > other.m_seconds ? 1 : m_seconds < other.m_seconds ? -1
                                                                                     : 0);
            cmp != 0)
            return cmp;
        if (int cmp = (m_nanoseconds > other.m_nanoseconds ? 1 : m_nanoseconds < other.m_nanoseconds ? -1
                                                                                                     : 0);
            cmp != 0)
            return cmp;
        return 0;
    }

private:
    constexpr explicit Duration(i64 seconds, u32 nanoseconds)
        : m_seconds(seconds)
        , m_nanoseconds(nanoseconds)
    {
    }

    [[nodiscard]] static Duration from_half_sanitized(i64 seconds, i32 extra_seconds, u32 nanoseconds);

    i64 m_seconds { 0 };
    u32 m_nanoseconds { 0 }; // Always less than 1'000'000'000
};

namespace Detail {

// Common base class for all unaware time types.
// Naive, or unaware, in the time context means to make heavily simplifying assumptions about time.
// In the case of this class and its children, they are not timezone-aware and strictly ordered.
class UnawareTime {
public:
    constexpr UnawareTime(UnawareTime const&) = default;
    constexpr UnawareTime& operator=(UnawareTime const&) = default;

    [[nodiscard]] timespec to_timespec() const { return m_offset.to_timespec(); }
    // Rounds towards -inf.
    [[nodiscard]] timeval to_timeval() const { return m_offset.to_timeval(); }

    // We intentionally do not define a comparison operator here to avoid accidentally comparing incompatible time types.

protected:
    constexpr explicit UnawareTime(Duration offset)
        : m_offset(offset)
    {
    }

    Duration m_offset {};
};

}

// Naive UNIX time, representing an offset from 1970-01-01 00:00:00Z, without accounting for UTC leap seconds.
// This class is mainly intended for interoperating with anything that expects a unix timestamp.
class UnixDateTime : public Detail::UnawareTime {
public:
    constexpr UnixDateTime()
        : Detail::UnawareTime(Duration::zero())
    {
    }

    constexpr static UnixDateTime epoch()
    {
        return UnixDateTime {};
    }

    // Creates UNIX time from an ISO 8601 Week, such as 2025-W06, with year 2025 and week 6.
    [[nodiscard]] static UnixDateTime from_iso8601_week(u32 year, u32 week);

    // Creates UNIX time from a unix timestamp.
    // Note that the returned time is probably not equivalent to the same timestamp in UTC time, since UNIX time does not observe leap seconds.
    [[nodiscard]] constexpr static UnixDateTime from_unix_time_parts(i32 year, u8 month, u8 day, u8 hour, u8 minute, u8 second, u16 millisecond)
    {
        i64 days = days_since_epoch(year, month, day);
        // With year=2'147'483'648, we can end up with days=569'603'931'504.
        // Expressing that in milliseconds would require more than 64 bits,
        // so we must choose seconds here, and not milliseconds.
        i64 seconds_since_epoch = days * seconds_per_day;

        seconds_since_epoch += hour * seconds_per_hour;
        seconds_since_epoch += minute * seconds_per_minute;
        seconds_since_epoch += second;
        return from_seconds_since_epoch(seconds_since_epoch) + Duration::from_milliseconds(millisecond);
    }

    [[nodiscard]] constexpr static UnixDateTime from_seconds_since_epoch(i64 seconds)
    {
        return UnixDateTime { Duration::from_seconds(seconds) };
    }

    [[nodiscard]] constexpr static UnixDateTime from_milliseconds_since_epoch(i64 milliseconds)
    {
        return UnixDateTime { Duration::from_milliseconds(milliseconds) };
    }

    [[nodiscard]] constexpr static UnixDateTime from_nanoseconds_since_epoch(i64 nanoseconds)
    {
        return UnixDateTime { Duration::from_nanoseconds(nanoseconds) };
    }

    [[nodiscard]] static UnixDateTime from_unix_timespec(struct timespec const& time)
    {
        return UnixDateTime { Duration::from_timespec(time) };
    }

    // Earliest and latest representable UNIX timestamps.
    [[nodiscard]] constexpr static UnixDateTime earliest() { return UnixDateTime { Duration::min() }; }
    [[nodiscard]] constexpr static UnixDateTime latest() { return UnixDateTime { Duration::max() }; }

    [[nodiscard]] constexpr Duration offset_to_epoch() const { return m_offset; }
    // May return an epoch offset *after* what this UnixDateTime contains, because rounding to seconds occurs.
    [[nodiscard]] i64 seconds_since_epoch() const { return m_offset.to_seconds(); }
    [[nodiscard]] i64 milliseconds_since_epoch() const { return m_offset.to_milliseconds(); }
    [[nodiscard]] i64 nanoseconds_since_epoch() const { return m_offset.to_nanoseconds(); }
    // Never returns a point after this UnixDateTime, since fractional seconds are cut off.
    [[nodiscard]] i64 truncated_seconds_since_epoch() const { return m_offset.to_truncated_seconds(); }

    // Offsetting a UNIX time by a duration yields another UNIX time.
    constexpr UnixDateTime operator+(Duration const& other) const { return UnixDateTime { m_offset + other }; }
    constexpr UnixDateTime& operator+=(Duration const& other)
    {
        this->m_offset = this->m_offset + other;
        return *this;
    }

    constexpr UnixDateTime operator-(Duration const& other) const { return UnixDateTime { m_offset - other }; }
    constexpr UnixDateTime& operator-=(Duration const& other)
    {
        m_offset = m_offset - other;
        return *this;
    }

    // Subtracting two UNIX times yields their time difference.
    constexpr Duration operator-(UnixDateTime const& other) const { return m_offset - other.m_offset; }

    [[nodiscard]] static UnixDateTime now();
    [[nodiscard]] static UnixDateTime now_coarse();

    constexpr bool operator==(UnixDateTime const& other) const
    {
        return this->m_offset == other.m_offset;
    }
    constexpr int operator<=>(UnixDateTime const& other) const { return this->m_offset <=> other.m_offset; }

private:
    constexpr explicit UnixDateTime(Duration offset)
        : Detail::UnawareTime(offset)
    {
    }
};

// Monotonic time represents time returned from the CLOCK_MONOTONIC clock, which has an arbitrary fixed reference point.
class MonotonicTime : private Detail::UnawareTime {
public:
    // Monotonic time does not have a defined reference point.
    // A MonotonicTime at the reference point is therefore meaningless.
    MonotonicTime() = delete;
    constexpr MonotonicTime(MonotonicTime const&) = default;
    constexpr MonotonicTime(MonotonicTime&&) = default;
    constexpr MonotonicTime& operator=(MonotonicTime const&) = default;
    constexpr MonotonicTime& operator=(MonotonicTime&&) = default;

    [[nodiscard]] static MonotonicTime now();
    [[nodiscard]] static MonotonicTime now_coarse();

    [[nodiscard]] i64 seconds() const { return m_offset.to_seconds(); }
    [[nodiscard]] i64 milliseconds() const { return m_offset.to_milliseconds(); }
    [[nodiscard]] i64 nanoseconds() const { return m_offset.to_nanoseconds(); }
    // Never returns a point in the future, since fractional seconds are cut off.
    [[nodiscard]] i64 truncated_seconds() const { return m_offset.to_truncated_seconds(); }
    [[nodiscard]] i64 nanoseconds_within_second() const { return to_timespec().tv_nsec; }

    constexpr bool operator==(MonotonicTime const& other) const { return this->m_offset == other.m_offset; }
    constexpr int operator<=>(MonotonicTime const& other) const { return this->m_offset <=> other.m_offset; }

    constexpr MonotonicTime operator+(Duration const& other) const { return MonotonicTime { m_offset + other }; }
    constexpr MonotonicTime& operator+=(Duration const& other)
    {
        this->m_offset = this->m_offset + other;
        return *this;
    }
    constexpr MonotonicTime operator-(Duration const& other) const { return MonotonicTime { m_offset - other }; }
    constexpr Duration operator-(MonotonicTime const& other) const { return m_offset - other.m_offset; }

private:
    constexpr explicit MonotonicTime(Duration offset)
        : Detail::UnawareTime(offset)
    {
    }
};

template<typename TimevalType>
inline void timeval_sub(TimevalType const& a, TimevalType const& b, TimevalType& result)
{
    result.tv_sec = a.tv_sec - b.tv_sec;
    result.tv_usec = a.tv_usec - b.tv_usec;
    if (result.tv_usec < 0) {
        --result.tv_sec;
        result.tv_usec += 1'000'000;
    }
}

template<typename TimevalType>
inline void timeval_add(TimevalType const& a, TimevalType const& b, TimevalType& result)
{
    result.tv_sec = a.tv_sec + b.tv_sec;
    result.tv_usec = a.tv_usec + b.tv_usec;
    if (result.tv_usec >= 1'000'000) {
        ++result.tv_sec;
        result.tv_usec -= 1'000'000;
    }
}

template<typename TimespecType>
inline void timespec_sub(TimespecType const& a, TimespecType const& b, TimespecType& result)
{
    result.tv_sec = a.tv_sec - b.tv_sec;
    result.tv_nsec = a.tv_nsec - b.tv_nsec;
    if (result.tv_nsec < 0) {
        --result.tv_sec;
        result.tv_nsec += 1'000'000'000;
    }
}

template<typename TimespecType>
inline void timespec_add(TimespecType const& a, TimespecType const& b, TimespecType& result)
{
    result.tv_sec = a.tv_sec + b.tv_sec;
    result.tv_nsec = a.tv_nsec + b.tv_nsec;
    if (result.tv_nsec >= 1000'000'000) {
        ++result.tv_sec;
        result.tv_nsec -= 1000'000'000;
    }
}

template<typename TimespecType, typename TimevalType>
inline void timespec_add_timeval(TimespecType const& a, TimevalType const& b, TimespecType& result)
{
    result.tv_sec = a.tv_sec + b.tv_sec;
    result.tv_nsec = a.tv_nsec + b.tv_usec * 1000;
    if (result.tv_nsec >= 1000'000'000) {
        ++result.tv_sec;
        result.tv_nsec -= 1000'000'000;
    }
}

template<typename TimevalType, typename TimespecType>
inline void timeval_to_timespec(TimevalType const& tv, TimespecType& ts)
{
    ts.tv_sec = tv.tv_sec;
    ts.tv_nsec = tv.tv_usec * 1000;
}

template<typename TimespecType, typename TimevalType>
inline void timespec_to_timeval(TimespecType const& ts, TimevalType& tv)
{
    tv.tv_sec = ts.tv_sec;
    tv.tv_usec = ts.tv_nsec / 1000;
}

// To use these, add a ``using namespace AK::TimeLiterals`` at block or file scope
namespace TimeLiterals {

constexpr Duration operator""_ns(unsigned long long nanoseconds) { return Duration::from_nanoseconds(static_cast<i64>(nanoseconds)); }
constexpr Duration operator""_us(unsigned long long microseconds) { return Duration::from_microseconds(static_cast<i64>(microseconds)); }
constexpr Duration operator""_ms(unsigned long long milliseconds) { return Duration::from_milliseconds(static_cast<i64>(milliseconds)); }
constexpr Duration operator""_sec(unsigned long long seconds) { return Duration::from_seconds(static_cast<i64>(seconds)); }

}

}

#if USING_AK_GLOBALLY
using AK::day_of_week;
using AK::day_of_year;
using AK::days_in_month;
using AK::days_in_year;
using AK::days_since_epoch;
using AK::is_leap_year;
using AK::MonotonicTime;
using AK::seconds_since_epoch_to_year;
using AK::timespec_add;
using AK::timespec_add_timeval;
using AK::timespec_sub;
using AK::timespec_to_timeval;
using AK::timeval_add;
using AK::timeval_sub;
using AK::timeval_to_timespec;
using AK::UnixDateTime;
using AK::weeks_in_year;
using AK::years_to_days_since_epoch;
#endif
