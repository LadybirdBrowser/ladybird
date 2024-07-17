/*
 * Copyright (c) 2021, Ben Wiederhake <BenWiederhake.GitHub@gmx.de>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/Time.h>
#include <sys/time.h>

using AK::Duration;

#if defined(__TIMESIZE) && __TIMESIZE < 64
#    define TIME_T_IS_32BIT
#endif

#define EXPECT_DURATION(t, s, ns)    \
    do {                             \
        auto ts = (t).to_timespec(); \
        EXPECT_EQ(ts.tv_sec, (s));   \
        EXPECT_EQ(ts.tv_nsec, (ns)); \
    } while (0)

TEST_CASE(is_sane)
{
    auto t0 = Duration::from_seconds(0);
    auto t2 = Duration::from_seconds(2);
    auto t5 = Duration::from_seconds(5);
    auto tn3 = Duration::from_seconds(-3);
    EXPECT(t0 == t0);
    EXPECT(t2 == t2);
    EXPECT(t5 == t5);
    EXPECT(t0 != t2);
    EXPECT(t2 != tn3);
    EXPECT(t2 != t5);
    EXPECT_DURATION(t0, 0, 0);
    EXPECT_DURATION(t2, 2, 0);
    EXPECT_DURATION(t5, 5, 0);
    EXPECT_DURATION(t2 + t5, 7, 0);
    EXPECT_DURATION(tn3 + t2, -1, 0);
    EXPECT_DURATION(tn3 + t5, 2, 0);
}

TEST_CASE(limits)
{
    EXPECT_DURATION(Duration::min(), (i64)-0x8000'0000'0000'0000, 0);
    EXPECT_DURATION(Duration::max(), 0x7fff'ffff'ffff'ffff, 999'999'999);
}

TEST_CASE(seconds_parsing)
{
    EXPECT_DURATION(Duration::from_seconds(0), 0, 0);
    EXPECT_DURATION(Duration::from_seconds(42), 42, 0);
    EXPECT_DURATION(Duration::from_seconds(-1), -1, 0);

    // "6.4.4.1.5: The type of an integer constant is the first of the corresponding list in which its value can be represented."
    // In the case of "0x8000'0000", the list is "int, unsigned int, …", and unsigned int (u32) matches.
    // Then the unary minus: On unsigned 32-bit integers, -0x8000'0000 == 0x8000'0000, which only then is made signed again.
    // So we would pass a medium-large *positive* number to 'from_seconds', which is not what we want to test here.
    // That's why this is the only place that needs an "LL" suffix.
    EXPECT_DURATION(Duration::from_seconds(-0x8000'0000LL), -0x8000'0000LL, 0);
    EXPECT_DURATION(Duration::from_seconds(-0x8000'0000'0000'0000), (i64)-0x8000'0000'0000'0000, 0);
    EXPECT_DURATION(Duration::from_seconds(0x7fff'ffff'ffff'ffff), 0x7fff'ffff'ffff'ffff, 0);
}

TEST_CASE(timespec_parsing)
{
    EXPECT_DURATION(Duration::from_timespec(timespec { 2, 4 }), 2, 4);
    EXPECT_DURATION(Duration::from_timespec(timespec { 1234, 5678 }), 1234, 5678);

    EXPECT_DURATION(Duration::from_timespec(timespec { 0, 1'000'000'000 }), 1, 0);
    EXPECT_DURATION(Duration::from_timespec(timespec { 8, 2'000'000'000 }), 10, 0);
    EXPECT_DURATION(Duration::from_timespec(timespec { 0, 2'147'483'647 }), 2, 147'483'647);

    EXPECT_DURATION(Duration::from_timespec(timespec { 1, -1 }), 0, 999'999'999);
    EXPECT_DURATION(Duration::from_timespec(timespec { 0, -1 }), -1, 999'999'999);
    EXPECT_DURATION(Duration::from_timespec(timespec { -1, 0 }), -1, 0);
    EXPECT_DURATION(Duration::from_timespec(timespec { -1, 1'000'000'001 }), 0, 1);
    EXPECT_DURATION(Duration::from_timespec(timespec { -2, 2'000'000'003 }), 0, 3);
    EXPECT_DURATION(Duration::from_timespec(timespec { -2, 1'999'999'999 }), -1, 999'999'999);

#ifndef TIME_T_IS_32BIT
    EXPECT_DURATION(Duration::from_timespec(timespec { 0x7fff'ffff'ffff'fffe, 999'999'998 }), 0x7fff'ffff'ffff'fffe, 999'999'998);
    EXPECT_DURATION(Duration::from_timespec(timespec { 0x7fff'ffff'ffff'fffe, 1'999'999'998 }), 0x7fff'ffff'ffff'ffff, 999'999'998);
    EXPECT_DURATION(Duration::from_timespec(timespec { 0x7fff'ffff'ffff'fffe, 1'999'999'999 }), 0x7fff'ffff'ffff'ffff, 999'999'999);
    EXPECT_DURATION(Duration::from_timespec(timespec { 0x7fff'ffff'ffff'fffe, 2'000'000'000 }), 0x7fff'ffff'ffff'ffff, 999'999'999);

    EXPECT_DURATION(Duration::from_timespec(timespec { -0x7fff'ffff'ffff'fffe, -1 }), -0x7fff'ffff'ffff'ffff, 999'999'999);
    EXPECT_DURATION(Duration::from_timespec(timespec { -0x7fff'ffff'ffff'fffe, -999'999'999 }), -0x7fff'ffff'ffff'ffff, 1);
    EXPECT_DURATION(Duration::from_timespec(timespec { -0x7fff'ffff'ffff'fffe, -1'999'999'999 }), (i64)-0x8000'0000'0000'0000, 1);
    EXPECT_DURATION(Duration::from_timespec(timespec { -0x7fff'ffff'ffff'fffe, -2'000'000'000 }), (i64)-0x8000'0000'0000'0000, 0);
    EXPECT_DURATION(Duration::from_timespec(timespec { -0x7fff'ffff'ffff'fffe, -2'000'000'001 }), (i64)-0x8000'0000'0000'0000, 0);
#endif
}

TEST_CASE(timeval_parsing)
{
    EXPECT_DURATION(Duration::from_timeval(timeval { 2, 4 }), 2, 4'000);
    EXPECT_DURATION(Duration::from_timeval(timeval { 1234, 5'678 }), 1234, 5'678'000);
    EXPECT_DURATION(Duration::from_timeval(timeval { -123, -45'678 }), -124, 954'322'000);

    EXPECT_DURATION(Duration::from_timeval(timeval { 0, 1'000'000 }), 1, 0);
    EXPECT_DURATION(Duration::from_timeval(timeval { 0, 1'000'000'000 }), 1'000, 0);
    EXPECT_DURATION(Duration::from_timeval(timeval { 8, 2'000'000 }), 10, 0);
    EXPECT_DURATION(Duration::from_timeval(timeval { 0, 2'147'483'647 }), 2'147, 483'647'000);

    EXPECT_DURATION(Duration::from_timeval(timeval { 1, -1 }), 0, 999'999'000);
    EXPECT_DURATION(Duration::from_timeval(timeval { 0, -1 }), -1, 999'999'000);
    EXPECT_DURATION(Duration::from_timeval(timeval { -1, 0 }), -1, 0);
    EXPECT_DURATION(Duration::from_timeval(timeval { -1, 1'000'001 }), 0, 1'000);
    EXPECT_DURATION(Duration::from_timeval(timeval { -2, 2'000'003 }), 0, 3'000);
    EXPECT_DURATION(Duration::from_timeval(timeval { -2, 1'999'999 }), -1, 999'999'000);

#ifndef TIME_T_IS_32BIT
    EXPECT_DURATION(Duration::from_timeval(timeval { 0x7fff'ffff'ffff'fffe, 999'998 }), 0x7fff'ffff'ffff'fffe, 999'998'000);
    EXPECT_DURATION(Duration::from_timeval(timeval { 0x7fff'ffff'ffff'fffe, 1'999'998 }), 0x7fff'ffff'ffff'ffff, 999'998'000);
    EXPECT_DURATION(Duration::from_timeval(timeval { 0x7fff'ffff'ffff'fffe, 1'999'999 }), 0x7fff'ffff'ffff'ffff, 999'999'000);
    EXPECT_DURATION(Duration::from_timeval(timeval { 0x7fff'ffff'ffff'fffe, 2'000'000 }), 0x7fff'ffff'ffff'ffff, 999'999'999);

    EXPECT_DURATION(Duration::from_timeval(timeval { -0x7fff'ffff'ffff'fffe, -1 }), -0x7fff'ffff'ffff'ffff, 999'999'000);
    EXPECT_DURATION(Duration::from_timeval(timeval { -0x7fff'ffff'ffff'fffe, -999'999 }), -0x7fff'ffff'ffff'ffff, 1'000);
    EXPECT_DURATION(Duration::from_timeval(timeval { -0x7fff'ffff'ffff'fffe, -1'999'999 }), (i64)-0x8000'0000'0000'0000, 1'000);
    EXPECT_DURATION(Duration::from_timeval(timeval { -0x7fff'ffff'ffff'fffe, -2'000'000 }), (i64)-0x8000'0000'0000'0000, 0);
    EXPECT_DURATION(Duration::from_timeval(timeval { -0x7fff'ffff'ffff'fffe, -2'000'001 }), (i64)-0x8000'0000'0000'0000, 0);
#endif
}

#define DURATION(s, ns) \
    Duration::from_timespec(timespec { (s), (ns) })

TEST_CASE(addition)
{
#define EXPECT_ADDITION(s1, ns1, s2, ns2, sr, nsr)                       \
    do {                                                                 \
        EXPECT_DURATION(DURATION(s1, ns1) + DURATION(s2, ns2), sr, nsr); \
        EXPECT_DURATION(DURATION(s2, ns2) + DURATION(s1, ns1), sr, nsr); \
        auto t = DURATION(s1, ns1);                                      \
        t += DURATION(s2, ns2);                                          \
        EXPECT_DURATION(t, sr, nsr);                                     \
    } while (0)

    EXPECT_ADDITION(11, 123'456'789, 22, 900'000'000, 34, 23'456'789);

#ifndef TIME_T_IS_32BIT
    EXPECT_ADDITION(0, 0, 9223372036854775807LL, 999'999'998, 0x7fff'ffff'ffff'ffff, 999'999'998);
    EXPECT_ADDITION(0, 1, 9223372036854775807LL, 999'999'998, 0x7fff'ffff'ffff'ffff, 999'999'999);
    EXPECT_ADDITION(0, 2, 9223372036854775807LL, 999'999'998, 0x7fff'ffff'ffff'ffff, 999'999'999);

    EXPECT_ADDITION(0x80, 40, 0x7fff'ffff'ffff'ff7f, 999'999'958, 0x7fff'ffff'ffff'ffff, 999'999'998);
    EXPECT_ADDITION(0x80, 41, 0x7fff'ffff'ffff'ff7f, 999'999'958, 0x7fff'ffff'ffff'ffff, 999'999'999);
    EXPECT_ADDITION(0x80, 42, 0x7fff'ffff'ffff'ff7f, 999'999'958, 0x7fff'ffff'ffff'ffff, 999'999'999);
#endif

    EXPECT_ADDITION(-2, 5, -3, 7, -5, 12);
    EXPECT_ADDITION(-2, 999'999'995, -3, 999'999'997, -4, 999'999'992);

#ifndef TIME_T_IS_32BIT
    EXPECT_ADDITION(-0x7fff'ffff'ffff'ffff, 999'999'995, -1, 6, -0x7fff'ffff'ffff'ffff, 1);
    EXPECT_ADDITION(-0x7fff'ffff'ffff'ffff, 999'999'995, -2, 6, (i64)-0x8000'0000'0000'0000, 1);
    EXPECT_ADDITION(-0x7fff'ffff'ffff'ffff, 999'999'995, -2, 5, (i64)-0x8000'0000'0000'0000, 0);
    EXPECT_ADDITION(-0x7fff'ffff'ffff'ffff, 999'999'995, -2, 4, (i64)-0x8000'0000'0000'0000, 0);

    EXPECT_ADDITION((i64)-0x8000'0000'0000'0000, 999'999'995, 0x7fff'ffff'ffff'ffff, 4, -1, 999'999'999);
    EXPECT_ADDITION((i64)-0x8000'0000'0000'0000, 999'999'995, 0x7fff'ffff'ffff'ffff, 5, 0, 0);
    EXPECT_ADDITION((i64)-0x8000'0000'0000'0000, 999'999'995, 0x7fff'ffff'ffff'ffff, 6, 0, 1);
#endif

#undef EXPECT_ADDITION
}

TEST_CASE(subtraction)
{
#define EXPECT_SUBTRACTION(s1, ns1, s2, ns2, sr, nsr)                    \
    do {                                                                 \
        EXPECT_DURATION(DURATION(s1, ns1) - DURATION(s2, ns2), sr, nsr); \
        auto t = DURATION(s1, ns1);                                      \
        t -= DURATION(s2, ns2);                                          \
        EXPECT_DURATION(t, sr, nsr);                                     \
    } while (0)

    EXPECT_SUBTRACTION(5, 0, 3, 0, 2, 0);
    EXPECT_SUBTRACTION(0, 0, 0, 0, 0, 0);
    EXPECT_SUBTRACTION(0, 5, 0, 3, 0, 2);
#ifndef TIME_T_IS_32BIT
    EXPECT_SUBTRACTION(0x7fff'ffff'ffff'ffff, 999'999'999, 8, 123, 0x7fff'ffff'ffff'fff7, 999'999'876);
#endif

    EXPECT_SUBTRACTION(1, 0, 0, 999'999'999, 0, 1);
#ifndef TIME_T_IS_32BIT
    EXPECT_SUBTRACTION(0x7fff'ffff'ffff'ffff, 0, 1, 999'999'999, 0x7fff'ffff'ffff'fffd, 1);
#endif

    EXPECT_SUBTRACTION(3, 0, 5, 0, -2, 0);
    EXPECT_SUBTRACTION(0, 3, 0, 5, -1, 999'999'998);
#ifndef TIME_T_IS_32BIT
    EXPECT_SUBTRACTION(0, 0, 0x7fff'ffff'ffff'ffff, 999'999'999, (i64)-0x8000'0000'0000'0000, 1);
    EXPECT_SUBTRACTION(0, 0, (i64)-0x8000'0000'0000'0000, 0, 0x7fff'ffff'ffff'ffff, 999'999'999);
    EXPECT_SUBTRACTION(-1, 999'999'999, (i64)-0x8000'0000'0000'0000, 0, 0x7fff'ffff'ffff'ffff, 999'999'999);
    EXPECT_SUBTRACTION(-1, 999'999'998, (i64)-0x8000'0000'0000'0000, 0, 0x7fff'ffff'ffff'ffff, 999'999'998);
#endif

    EXPECT_SUBTRACTION(123, 456, 123, 455, 0, 1);
    EXPECT_SUBTRACTION(123, 456, 123, 456, 0, 0);
    EXPECT_SUBTRACTION(123, 456, 123, 457, -1, 999'999'999);

    EXPECT_SUBTRACTION(124, 456, 123, 455, 1, 1);
    EXPECT_SUBTRACTION(124, 456, 123, 456, 1, 0);
    EXPECT_SUBTRACTION(124, 456, 123, 457, 0, 999'999'999);

#ifndef TIME_T_IS_32BIT
    EXPECT_SUBTRACTION(-0x7fff'ffff'ffff'ffff, 999'999'995, 1, 999'999'994, (i64)-0x8000'0000'0000'0000, 1);
    EXPECT_SUBTRACTION(-0x7fff'ffff'ffff'ffff, 999'999'995, 1, 999'999'995, (i64)-0x8000'0000'0000'0000, 0);
    EXPECT_SUBTRACTION(-0x7fff'ffff'ffff'ffff, 999'999'995, 1, 999'999'996, (i64)-0x8000'0000'0000'0000, 0);
#endif
}

TEST_CASE(rounding)
{
    EXPECT_EQ(DURATION(2, 800'800'800).to_seconds(), 3);
    EXPECT_EQ(DURATION(2, 800'800'800).to_milliseconds(), 2'801);
    EXPECT_EQ(DURATION(2, 800'800'800).to_microseconds(), 2'800'801);
    EXPECT_EQ(DURATION(2, 800'800'800).to_nanoseconds(), 2'800'800'800);
    EXPECT_EQ(DURATION(-2, 800'800'800).to_seconds(), -2);
    EXPECT_EQ(DURATION(-2, 800'800'800).to_milliseconds(), -1'200);
    EXPECT_EQ(DURATION(-2, 800'800'800).to_microseconds(), -1'199'200);
    EXPECT_EQ(DURATION(-2, 800'800'800).to_nanoseconds(), -1'199'199'200);

    EXPECT_EQ(DURATION(0, 0).to_seconds(), 0);
    EXPECT_EQ(DURATION(0, 0).to_milliseconds(), 0);
    EXPECT_EQ(DURATION(0, 0).to_microseconds(), 0);
    EXPECT_EQ(DURATION(0, 0).to_nanoseconds(), 0);

    EXPECT_EQ(DURATION(0, 1).to_seconds(), 1);
    EXPECT_EQ(DURATION(0, 1).to_milliseconds(), 1);
    EXPECT_EQ(DURATION(0, 1).to_microseconds(), 1);
    EXPECT_EQ(DURATION(0, 1).to_nanoseconds(), 1);
    EXPECT_EQ(DURATION(0, -1).to_seconds(), -1);
    EXPECT_EQ(DURATION(0, -1).to_milliseconds(), -1);
    EXPECT_EQ(DURATION(0, -1).to_microseconds(), -1);
    EXPECT_EQ(DURATION(0, -1).to_nanoseconds(), -1);

#ifndef TIME_T_IS_32BIT
    EXPECT_EQ(DURATION(-9223372037, 145'224'191).to_nanoseconds(), (i64)-0x8000'0000'0000'0000);
    EXPECT_EQ(DURATION(-9223372037, 145'224'192).to_nanoseconds(), (i64)-0x8000'0000'0000'0000);
    EXPECT_EQ(DURATION(-9223372037, 145'224'193).to_nanoseconds(), -0x7fff'ffff'ffff'ffff);
    EXPECT_EQ(DURATION(9223372036, 854'775'806).to_nanoseconds(), 0x7fff'ffff'ffff'fffe);
    EXPECT_EQ(DURATION(9223372036, 854'775'807).to_nanoseconds(), 0x7fff'ffff'ffff'ffff);
    EXPECT_EQ(DURATION(9223372036, 854'775'808).to_nanoseconds(), 0x7fff'ffff'ffff'ffff);
#endif
}

TEST_CASE(truncation)
{
    // Sanity
    EXPECT_EQ(DURATION(2, 0).to_truncated_seconds(), 2);
    EXPECT_EQ(DURATION(-2, 0).to_truncated_seconds(), -2);
    EXPECT_EQ(DURATION(2, 800'800'800).to_truncated_seconds(), 2);
    EXPECT_EQ(DURATION(2, 800'800'800).to_truncated_milliseconds(), 2'800);
    EXPECT_EQ(DURATION(2, 800'800'800).to_truncated_microseconds(), 2'800'800);
    EXPECT_EQ(DURATION(-2, -800'800'800).to_truncated_seconds(), -2);
    EXPECT_EQ(DURATION(-2, -800'800'800).to_truncated_milliseconds(), -2'800);
    EXPECT_EQ(DURATION(-2, -800'800'800).to_truncated_microseconds(), -2'800'800);

    // Overflow, seconds
    EXPECT_EQ(Duration::min().to_truncated_seconds(), (i64)-0x8000'0000'0000'0000);
    EXPECT_EQ(Duration::max().to_truncated_seconds(), 0x7fff'ffff'ffff'ffff);

#ifndef TIME_T_IS_32BIT
    // Overflow, milliseconds
    EXPECT_EQ(DURATION(-9223372036854776, 191'000'000).to_truncated_milliseconds(), (i64)-0x8000'0000'0000'0000);
    EXPECT_EQ(DURATION(-9223372036854776, 192'000'000).to_truncated_milliseconds(), (i64)-0x8000'0000'0000'0000);
    EXPECT_EQ(DURATION(-9223372036854776, 192'000'001).to_truncated_milliseconds(), -0x7fff'ffff'ffff'ffff);
    EXPECT_EQ(DURATION(-9223372036854776, 193'000'000).to_truncated_milliseconds(), -0x7fff'ffff'ffff'ffff);
    EXPECT_EQ(DURATION(9223372036854775, 806'000'000).to_truncated_milliseconds(), 0x7fff'ffff'ffff'fffe);
    EXPECT_EQ(DURATION(9223372036854775, 806'999'999).to_truncated_milliseconds(), 0x7fff'ffff'ffff'fffe);
    EXPECT_EQ(DURATION(9223372036854775, 807'000'000).to_truncated_milliseconds(), 0x7fff'ffff'ffff'ffff);
    EXPECT_EQ(DURATION(9223372036854775, 808'000'000).to_truncated_milliseconds(), 0x7fff'ffff'ffff'ffff);

    // Overflow, microseconds
    EXPECT_EQ(DURATION(-9223372036855, 224'191'000).to_truncated_microseconds(), (i64)-0x8000'0000'0000'0000);
    EXPECT_EQ(DURATION(-9223372036855, 224'192'000).to_truncated_microseconds(), (i64)-0x8000'0000'0000'0000);
    EXPECT_EQ(DURATION(-9223372036855, 224'192'001).to_truncated_microseconds(), (i64)-0x7fff'ffff'ffff'ffff);
    EXPECT_EQ(DURATION(-9223372036855, 224'193'000).to_truncated_microseconds(), (i64)-0x7fff'ffff'ffff'ffff);
    EXPECT_EQ(DURATION(9223372036854, 775'806'000).to_truncated_microseconds(), 0x7fff'ffff'ffff'fffe);
    EXPECT_EQ(DURATION(9223372036854, 775'806'999).to_truncated_microseconds(), 0x7fff'ffff'ffff'fffe);
    EXPECT_EQ(DURATION(9223372036854, 775'807'000).to_truncated_microseconds(), 0x7fff'ffff'ffff'ffff);
    EXPECT_EQ(DURATION(9223372036854, 775'808'000).to_truncated_microseconds(), 0x7fff'ffff'ffff'ffff);
#endif
}

TEST_CASE(is_negative)
{
    auto small = Duration::from_nanoseconds(10);
    auto large = Duration::from_nanoseconds(15);
    auto result = small - large;
    EXPECT_EQ(result.to_nanoseconds(), -5);
    EXPECT(result.is_negative());

    result = large - small;
    EXPECT_EQ(result.to_nanoseconds(), 5);
    EXPECT(!result.is_negative());
}

struct YearAndDays {
    int year;
    i64 days;
};

TEST_CASE(years_to_days_since_epoch_points)
{
    Array<YearAndDays, 24> test_data = { {
        { 1969, -365 },
        { 1970, 0 },
        { 1971, 365 },
        { 1900, -25567 },
        { 2023, 19358 },
        { 1800, -62091 },
        { 2100, 47482 },
        { 0, -719528 },
        { -1, -719893 },
        { -2, -720258 },
        { -3, -720623 },
        { -4, -720989 },
        { -5, -721354 },
        { -6, -721719 },
        { 4000, 741442 },
        { -10000, -4371953 },
        { 10000, 2932897 },
        { -1000000, -365962028 },
        { 1000000, 364522972 },
        { -5877640, -2147483456 },
        { 5881474, 2147444740 },
        // Very important year: https://github.com/SerenityOS/serenity/pull/16760#issuecomment-1369054745
        { -999999, -365961662 },
        // The following two values haven't been verified by any other algorithm, but are very close to "year * 365.2425", and prove that there is no UB due to signed overflow:
        { 2147483647, 784351576412 },
        { -2147483648, -784353015833 },
    } };
    for (auto entry : test_data) {
        int year = entry.year;
        i64 expected_days = entry.days;
        i64 actual_days = years_to_days_since_epoch(year);
        EXPECT_EQ(actual_days, expected_days);
    }
}

BENCHMARK_CASE(years_to_days_since_epoch_benchmark)
{
    // This benchmark takes consistently "0ms" on Linux, and "0ms" on Serenity.
    for (size_t i = 0; i < 100; ++i) {
        i64 actual_days = years_to_days_since_epoch(-5877640);
        (void)actual_days;
        EXPECT_EQ(actual_days, -2147483456);
    }
}

TEST_CASE(days_since_epoch)
{
    EXPECT_EQ(days_since_epoch(1970, 1, 1), 0);
    EXPECT_EQ(days_since_epoch(1970, 1, 2), 1);
    EXPECT_EQ(days_since_epoch(1970, 2, 1), 31);
    EXPECT_EQ(days_since_epoch(1970, 2, 27), 57);
    EXPECT_EQ(days_since_epoch(1970, 2, 28), 58);
    EXPECT_EQ(days_since_epoch(1970, 2, 29), 59); // doesn't really exist
    EXPECT_EQ(days_since_epoch(1970, 3, 1), 59);
    EXPECT_EQ(days_since_epoch(1971, 1, 1), 365);
    EXPECT_EQ(days_since_epoch(1972, 1, 1), 730);
    EXPECT_EQ(days_since_epoch(1972, 2, 1), 761);
    EXPECT_EQ(days_since_epoch(1972, 2, 27), 787);
    EXPECT_EQ(days_since_epoch(1972, 2, 28), 788);
    EXPECT_EQ(days_since_epoch(1972, 2, 29), 789);
    EXPECT_EQ(days_since_epoch(1972, 3, 1), 790);

    // At least shouldn't crash:
    EXPECT_EQ(days_since_epoch(1971, 1, 0), 364);
    EXPECT_EQ(days_since_epoch(1971, 0, 1), 365);
    EXPECT_EQ(days_since_epoch(1971, 0, 0), 365);
    EXPECT_EQ(days_since_epoch(1971, 13, 3), 365);

    // I can't easily verify that these values are perfectly exact and correct, but they're close enough.
    // Also, for these "years" the most important thing is to avoid crashing (i.e. signed overflow UB).
    // Observe that these are very close to the naive guess of 365.2425 days per year.
    EXPECT_EQ(days_since_epoch(0, 1, 1), -719528);
    EXPECT_EQ(days_since_epoch(-1'000'000, 1, 1), -365962028);
    EXPECT_EQ(days_since_epoch(-2'147'483'648, 1, 1), -784353015833); // Guess: 784353015832
    EXPECT_EQ(days_since_epoch(1'000'000, 1, 1), 364522972);
    EXPECT_EQ(days_since_epoch(2'147'483'647, 1, 1), 784351576412);   // Guess: 784351576411
    EXPECT_EQ(days_since_epoch(2'147'483'647, 12, 31), 784351576776); // Guess: 784351576777
    EXPECT_EQ(days_since_epoch(2'147'483'647, 12, 255), 784351577000);
    // FIXME shouldn't crash: EXPECT_EQ(days_since_epoch(2'147'483'647, 255, 255), 784351577000);
    // FIXME: Restrict interface to only take sensible types, and ensure callers pass only sensible values for that type.
}

TEST_CASE(div_floor_by)
{
    EXPECT_EQ(AK::Detail::floor_div_by<4>(-5), -2);
    EXPECT_EQ(AK::Detail::floor_div_by<4>(-4), -1);
    EXPECT_EQ(AK::Detail::floor_div_by<4>(-3), -1);
    EXPECT_EQ(AK::Detail::floor_div_by<4>(-2), -1);
    EXPECT_EQ(AK::Detail::floor_div_by<4>(-1), -1);
    EXPECT_EQ(AK::Detail::floor_div_by<4>(+0), +0);
    EXPECT_EQ(AK::Detail::floor_div_by<4>(+1), +0);
    EXPECT_EQ(AK::Detail::floor_div_by<4>(+2), +0);
    EXPECT_EQ(AK::Detail::floor_div_by<4>(+3), +0);
    EXPECT_EQ(AK::Detail::floor_div_by<4>(+4), +1);
    EXPECT_EQ(AK::Detail::floor_div_by<4>(+5), +1);
    EXPECT_EQ(AK::Detail::floor_div_by<4>(+6), +1);
    EXPECT_EQ(AK::Detail::floor_div_by<4>(+7), +1);
    EXPECT_EQ(AK::Detail::floor_div_by<4>(+8), +2);
}

TEST_CASE(mod_zeros_in_range)
{
    EXPECT_EQ(AK::Detail::mod_zeros_in_range<4>(0, 0), 0);
    EXPECT_EQ(AK::Detail::mod_zeros_in_range<4>(0, 1), 1);
    EXPECT_EQ(AK::Detail::mod_zeros_in_range<4>(0, 2), 1);
    EXPECT_EQ(AK::Detail::mod_zeros_in_range<4>(0, 3), 1);
    EXPECT_EQ(AK::Detail::mod_zeros_in_range<4>(0, 4), 1);
    EXPECT_EQ(AK::Detail::mod_zeros_in_range<4>(0, 5), 2);
    EXPECT_EQ(AK::Detail::mod_zeros_in_range<4>(0, 6), 2);

    EXPECT_EQ(AK::Detail::mod_zeros_in_range<4>(1, 1), 0);
    EXPECT_EQ(AK::Detail::mod_zeros_in_range<4>(1, 2), 0);
    EXPECT_EQ(AK::Detail::mod_zeros_in_range<4>(1, 3), 0);
    EXPECT_EQ(AK::Detail::mod_zeros_in_range<4>(1, 4), 0);
    EXPECT_EQ(AK::Detail::mod_zeros_in_range<4>(1, 5), 1);
    EXPECT_EQ(AK::Detail::mod_zeros_in_range<4>(1, 6), 1);

    EXPECT_EQ(AK::Detail::mod_zeros_in_range<4>(2, 2), 0);
    EXPECT_EQ(AK::Detail::mod_zeros_in_range<4>(2, 3), 0);
    EXPECT_EQ(AK::Detail::mod_zeros_in_range<4>(2, 4), 0);
    EXPECT_EQ(AK::Detail::mod_zeros_in_range<4>(2, 5), 1);
    EXPECT_EQ(AK::Detail::mod_zeros_in_range<4>(2, 6), 1);

    EXPECT_EQ(AK::Detail::mod_zeros_in_range<4>(3, 3), 0);
    EXPECT_EQ(AK::Detail::mod_zeros_in_range<4>(3, 4), 0);
    EXPECT_EQ(AK::Detail::mod_zeros_in_range<4>(3, 5), 1);
    EXPECT_EQ(AK::Detail::mod_zeros_in_range<4>(3, 6), 1);

    EXPECT_EQ(AK::Detail::mod_zeros_in_range<4>(4, 4), 0);
    EXPECT_EQ(AK::Detail::mod_zeros_in_range<4>(4, 5), 1);
    EXPECT_EQ(AK::Detail::mod_zeros_in_range<4>(4, 6), 1);

    EXPECT_EQ(AK::Detail::mod_zeros_in_range<4>(5, 5), 0);
    EXPECT_EQ(AK::Detail::mod_zeros_in_range<4>(5, 6), 0);

    EXPECT_EQ(AK::Detail::mod_zeros_in_range<4>(6, 6), 0);

    EXPECT_EQ(AK::Detail::mod_zeros_in_range<4>(-5, 3), 2);
    EXPECT_EQ(AK::Detail::mod_zeros_in_range<4>(-4, 3), 2);
    EXPECT_EQ(AK::Detail::mod_zeros_in_range<4>(-3, 3), 1);
    EXPECT_EQ(AK::Detail::mod_zeros_in_range<4>(-2, 3), 1);
    EXPECT_EQ(AK::Detail::mod_zeros_in_range<4>(-1, 3), 1);
    EXPECT_EQ(AK::Detail::mod_zeros_in_range<4>(-0, 3), 1);
    EXPECT_EQ(AK::Detail::mod_zeros_in_range<4>(+1, 3), 0);
}

TEST_CASE(years_to_days_since_epoch_span)
{
    auto test_data_start_year = 1900;
    // Data was pre-computed with a slow, but known-correct implementation.
    // clang-format off
    auto test_data = Array {
        -25567, -25202, -24837, -24472, -24107, -23741, -23376, -23011,
        -22646, -22280, -21915, -21550, -21185, -20819, -20454, -20089,
        -19724, -19358, -18993, -18628, -18263, -17897, -17532, -17167,
        -16802, -16436, -16071, -15706, -15341, -14975, -14610, -14245,
        -13880, -13514, -13149, -12784, -12419, -12053, -11688, -11323,
        -10958, -10592, -10227, -9862, -9497, -9131, -8766, -8401, -8036,
        -7670, -7305, -6940, -6575, -6209, -5844, -5479, -5114, -4748, -4383,
        -4018, -3653, -3287, -2922, -2557, -2192, -1826, -1461, -1096, -731,
        -365, 0, 365, 730, 1096, 1461, 1826, 2191, 2557, 2922, 3287, 3652,
        4018, 4383, 4748, 5113, 5479, 5844, 6209, 6574, 6940, 7305, 7670,
        8035, 8401, 8766, 9131, 9496, 9862, 10227, 10592, 10957, 11323, 11688,
        12053, 12418, 12784, 13149, 13514, 13879, 14245, 14610, 14975, 15340,
        15706, 16071, 16436, 16801, 17167, 17532, 17897, 18262, 18628, 18993,
        19358, 19723, 20089, 20454, 20819, 21184, 21550, 21915, 22280, 22645,
        23011, 23376, 23741, 24106, 24472, 24837, 25202, 25567, 25933, 26298,
        26663, 27028, 27394, 27759, 28124, 28489, 28855, 29220, 29585, 29950,
        30316, 30681, 31046, 31411, 31777, 32142, 32507, 32872, 33238, 33603,
        33968, 34333, 34699, 35064, 35429, 35794, 36160, 36525, 36890, 37255,
        37621, 37986, 38351, 38716, 39082, 39447, 39812, 40177, 40543, 40908,
        41273, 41638, 42004, 42369, 42734, 43099, 43465, 43830, 44195, 44560,
        44926, 45291, 45656, 46021, 46387, 46752, 47117, 47482, 47847, 48212,
        48577, 48942, 49308, 49673, 50038, 50403, 50769, 51134, 51499, 51864,
        52230, 52595, 52960, 53325, 53691, 54056, 54421, 54786, 55152, 55517,
        55882, 56247, 56613, 56978, 57343, 57708, 58074, 58439, 58804, 59169,
        59535, 59900, 60265, 60630, 60996, 61361, 61726, 62091, 62457, 62822,
        63187, 63552, 63918, 64283, 64648, 65013, 65379, 65744, 66109, 66474,
        66840, 67205, 67570, 67935, 68301, 68666, 69031, 69396, 69762, 70127,
        70492, 70857, 71223, 71588, 71953, 72318, 72684, 73049, 73414, 73779,
        74145, 74510, 74875, 75240, 75606, 75971, 76336, 76701, 77067, 77432,
        77797, 78162, 78528, 78893, 79258, 79623, 79989, 80354, 80719, 81084,
        81450, 81815, 82180, 82545, 82911, 83276, 83641, 84006, 84371, 84736,
        85101, 85466, 85832, 86197, 86562, 86927, 87293, 87658, 88023, 88388,
        88754, 89119, 89484, 89849, 90215, 90580, 90945, 91310, 91676, 92041,
        92406, 92771, 93137, 93502, 93867, 94232, 94598, 94963, 95328, 95693,
        96059, 96424, 96789, 97154, 97520, 97885, 98250, 98615, 98981, 99346,
        99711, 100076, 100442, 100807, 101172, 101537, 101903, 102268, 102633,
        102998, 103364, 103729, 104094, 104459, 104825, 105190, 105555,
        105920, 106286, 106651, 107016, 107381, 107747, 108112, 108477,
        108842, 109208, 109573, 109938, 110303, 110669, 111034, 111399,
        111764, 112130, 112495, 112860, 113225, 113591, 113956, 114321,
        114686, 115052, 115417, 115782, 116147, 116513, 116878, 117243,
        117608, 117974, 118339, 118704, 119069, 119435, 119800, 120165,
        120530, 120895, 121260, 121625, 121990, 122356, 122721, 123086,
        123451, 123817, 124182, 124547, 124912, 125278, 125643, 126008,
        126373, 126739, 127104, 127469, 127834, 128200, 128565, 128930,
        129295, 129661, 130026, 130391, 130756, 131122, 131487, 131852,
        132217, 132583, 132948, 133313, 133678, 134044, 134409, 134774,
        135139, 135505, 135870, 136235, 136600, 136966, 137331, 137696,
        138061, 138427, 138792, 139157, 139522, 139888, 140253, 140618,
        140983, 141349, 141714, 142079, 142444, 142810, 143175, 143540,
        143905, 144271, 144636, 145001, 145366, 145732, 146097, 146462,
        146827, 147193, 147558, 147923, 148288, 148654, 149019, 149384,
        149749, 150115, 150480, 150845, 151210, 151576, 151941, 152306,
        152671, 153037, 153402, 153767, 154132, 154498, 154863, 155228,
        155593, 155959, 156324, 156689, 157054, 157420, 157785, 158150,
        158515, 158881, 159246, 159611, 159976, 160342, 160707, 161072,
        161437, 161803, 162168, 162533, 162898, 163264, 163629, 163994,
        164359, 164725, 165090, 165455, 165820, 166186, 166551, 166916,
        167281, 167647, 168012, 168377, 168742, 169108, 169473, 169838,
        170203, 170569, 170934, 171299, 171664, 172030, 172395, 172760,
        173125, 173491, 173856, 174221, 174586, 174952, 175317, 175682,
        176047, 176413, 176778, 177143, 177508, 177874, 178239, 178604,
        178969, 179335, 179700, 180065, 180430, 180796, 181161, 181526,
        181891, 182257, 182622, 182987, 183352, 183718, 184083, 184448,
        184813, 185179, 185544, 185909, 186274, 186640, 187005, 187370,
        187735, 188101, 188466, 188831, 189196, 189562, 189927, 190292,
        190657, 191023, 191388, 191753, 192118, 192484, 192849, 193214,
        193579, 193944, 194309, 194674, 195039, 195405,
    };
    // clang-format on
    for (size_t offset = 0; offset < test_data.size(); ++offset) {
        int year = offset + test_data_start_year;
        i64 expected_days = test_data[offset];
        i64 actual_days = years_to_days_since_epoch(year);
        EXPECT_EQ(actual_days, expected_days);
    }
}

TEST_CASE(user_defined_literals)
{
    using namespace AK::TimeLiterals;
    static_assert(Duration::from_nanoseconds(123) == 123_ns, "Factory is same as UDL");

    static_assert(100_ms > 10_ms, "LT UDL");
    static_assert(1000_ns == 1_us, "EQ UDL");
    static_assert(1_sec > 1_ms, "GT UDL");
    static_assert(100_ms >= 100'000_us, "GE UDL (eq)");
    static_assert(100_ms >= 99'999_us, "GE UDL (gt)");
    static_assert(100_ms <= 100'000_us, "LE UDL (eq)");
    static_assert(100_ms <= 100'001_us, "LE UDL (lt)");
    static_assert(1_sec != 2_sec, "NE UDL");
}

TEST_CASE(from_unix_time_parts_common_values)
{
    // Non-negative "common" values.
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1970, 1, 1, 0, 0, 0, 0).offset_to_epoch(), 0, 0);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1970, 1, 1, 0, 0, 0, 1).offset_to_epoch(), 0, 1'000'000);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1970, 1, 1, 0, 0, 0, 999).offset_to_epoch(), 0, 999'000'000);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1970, 1, 1, 0, 0, 1, 2).offset_to_epoch(), 1, 2'000'000);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1970, 1, 1, 0, 0, 59, 0).offset_to_epoch(), 59, 0);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1970, 1, 1, 0, 1, 0, 0).offset_to_epoch(), 60, 0);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1970, 1, 1, 0, 59, 0, 0).offset_to_epoch(), 3540, 0);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1970, 1, 1, 1, 0, 0, 0).offset_to_epoch(), 3600, 0);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1970, 1, 1, 23, 0, 0, 0).offset_to_epoch(), 82800, 0);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1970, 1, 2, 0, 0, 0, 0).offset_to_epoch(), 86400, 0);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1970, 2, 1, 0, 0, 0, 0).offset_to_epoch(), 2678400, 0);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1970, 2, 27, 0, 0, 0, 0).offset_to_epoch(), 4924800, 0);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1970, 2, 28, 0, 0, 0, 0).offset_to_epoch(), 5011200, 0);
    // Note that this day does *not* exist:
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1970, 2, 29, 0, 0, 0, 0).offset_to_epoch(), 5097600, 0);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1970, 3, 0, 0, 0, 0, 0).offset_to_epoch(), 5011200, 0);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1970, 3, 1, 0, 0, 0, 0).offset_to_epoch(), 5097600, 0);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1970, 3, 2, 0, 0, 0, 0).offset_to_epoch(), 5184000, 0);

    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1971, 1, 1, 0, 0, 0, 0).offset_to_epoch(), 31536000, 0);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1972, 1, 1, 0, 0, 0, 0).offset_to_epoch(), 63072000, 0);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1972, 2, 1, 0, 0, 0, 0).offset_to_epoch(), 65750400, 0);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1972, 2, 27, 0, 0, 0, 0).offset_to_epoch(), 67996800, 0);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1972, 2, 28, 0, 0, 0, 0).offset_to_epoch(), 68083200, 0);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1972, 2, 29, 0, 0, 0, 0).offset_to_epoch(), 68169600, 0);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1972, 3, 1, 0, 0, 0, 0).offset_to_epoch(), 68256000, 0);

    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(2023, 5, 24, 18, 44, 40, 0).offset_to_epoch(), 1684953880, 0);
}

TEST_CASE(from_unix_time_parts_negative)
{
    // Negative "common" values. These aren't really that well-defined, but we must make sure we don't crash.
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1970, 1, 0, 23, 0, 0, 0).offset_to_epoch(), -3600, 0);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1970, 1, 0, 24, 0, 0, 0).offset_to_epoch(), 0, 0);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1970, 0, 31, 0, 0, 0, 0).offset_to_epoch(), 0, 0);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1970, 11, 30, 0, 0, 0, 0).offset_to_epoch(), 28771200, 0);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1970, 12, 1, 0, 0, 0, 0).offset_to_epoch(), 28857600, 0);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1970, 12, 31, 0, 0, 0, 0).offset_to_epoch(), 31449600, 0);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1971, 0, 0, 0, 0, 0, 0).offset_to_epoch(), 31536000, 0);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1971, 0, 1, 0, 0, 0, 0).offset_to_epoch(), 31536000, 0);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1971, 1, 0, 0, 0, 0, 0).offset_to_epoch(), 31449600, 0);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1971, 1, 1, 0, 0, 0, 0).offset_to_epoch(), 31536000, 0);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1969, 1, 1, 0, 0, 0, 0).offset_to_epoch(), -31536000, 0);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1968, 3, 1, 0, 0, 0, 0).offset_to_epoch(), -57974400, 0);
    // Leap day!
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1968, 2, 29, 0, 0, 0, 0).offset_to_epoch(), -58060800, 0);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1968, 2, 28, 0, 0, 0, 0).offset_to_epoch(), -58147200, 0);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1968, 2, 27, 0, 0, 0, 0).offset_to_epoch(), -58233600, 0);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1968, 2, 1, 0, 0, 0, 0).offset_to_epoch(), -60480000, 0);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1968, 1, 1, 0, 0, 0, 0).offset_to_epoch(), -63158400, 0);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1967, 1, 1, 0, 0, 0, 0).offset_to_epoch(), -94694400, 0);
}

TEST_CASE(from_milliseconds)
{
    EXPECT_DURATION(Duration::from_milliseconds(0), 0, 0);
    EXPECT_DURATION(Duration::from_milliseconds(42), 0, 42'000'000);
    EXPECT_DURATION(Duration::from_milliseconds(-1), -1, 999'000'000);
    EXPECT_DURATION(Duration::from_milliseconds(-1'000'000'000), -1'000'000, 0);
    EXPECT_DURATION(Duration::from_milliseconds(1'000'000'000), 1'000'000, 0);
    EXPECT_DURATION(Duration::from_milliseconds(9223372036854775807), 9223372036854775, 807'000'000);
    EXPECT_DURATION(Duration::from_milliseconds((i64)-0x8000'0000'0000'0000), -9223372036854776, 192'000'000);
}

TEST_CASE(from_unix_time_parts_overflow)
{
    // Negative overflow
    // I can't easily verify that these values are perfectly exact and correct, but they're close enough.
    // Also, for these "years" the most important thing is to avoid crashing (i.e. signed overflow UB).
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(0, 1, 1, 0, 0, 0, 0).offset_to_epoch(), -62167219200, 0);                    // Guess: -62167195440, off by 23760 seconds
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(-1'000'000, 1, 1, 0, 0, 0, 0).offset_to_epoch(), -31619119219200, 0);        // Guess: -31619119195440, off by the same 23760 seconds
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(-2'147'483'648, 1, 1, 0, 0, 0, 0).offset_to_epoch(), -67768100567971200, 0); // Guess: -67768100567916336, off by 54864 seconds
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(-2'147'483'648, 1, 0, 0, 0, 0, 0).offset_to_epoch(), -67768100568057600, 0); // Guess: -67768100568002736, off by the same 54864 seconds
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(-2'147'483'648, 0, 0, 0, 0, 0, 0).offset_to_epoch(), -67768100567971200, 0);

    // Positive overflow
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1970, 1, 1, 0, 0, 0, 65535).offset_to_epoch(), 65, 535'000'000);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1970, 1, 1, 0, 0, 255, 0).offset_to_epoch(), 255, 0);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1970, 1, 1, 0, 255, 0, 0).offset_to_epoch(), 15300, 0);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1970, 1, 1, 255, 0, 0, 0).offset_to_epoch(), 918000, 0);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1970, 1, 255, 0, 0, 0, 0).offset_to_epoch(), 21945600, 0);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1970, 12, 1, 0, 0, 0, 0).offset_to_epoch(), 28857600, 0);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1970, 255, 1, 0, 0, 0, 0).offset_to_epoch(), 0, 0);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(1'000'000, 1, 1, 0, 0, 0, 0).offset_to_epoch(), 31494784780800, 0);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(2'147'483'647, 1, 1, 0, 0, 0, 0).offset_to_epoch(), 67767976201996800, 0);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(2'147'483'647, 12, 255, 0, 0, 0, 0).offset_to_epoch(), 67767976252800000, 0);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(2'147'483'647, 12, 255, 255, 255, 255, 65535).offset_to_epoch(), 67767976253733620, 535'000'000);
    EXPECT_DURATION(UnixDateTime::from_unix_time_parts(2'147'483'647, 255, 255, 255, 255, 255, 65535).offset_to_epoch(), 67767976202930420, 535'000'000);
}
