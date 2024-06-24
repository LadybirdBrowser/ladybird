/*
 * Copyright (c) 2022-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/StringView.h>
#include <AK/Time.h>
#include <LibTimeZone/TimeZone.h>

using enum TimeZone::InDST;

static void test_offset(StringView time_zone, i64 time, i64 expected_offset, TimeZone::InDST expected_in_dst)
{
    auto actual_offset = TimeZone::get_time_zone_offset(time_zone, AK::UnixDateTime::from_seconds_since_epoch(time));
    VERIFY(actual_offset.has_value());
    EXPECT_EQ(actual_offset->seconds, expected_offset);
    EXPECT_EQ(actual_offset->in_dst, expected_in_dst);
}

#if ENABLE_TIME_ZONE_DATA

#    include <LibTimeZone/TimeZoneData.h>

class TimeZoneGuard {
public:
    explicit TimeZoneGuard(char const* tz)
        : m_tz(getenv("TZ"))
    {
        setenv("TZ", tz, 1);
    }

    ~TimeZoneGuard()
    {
        if (m_tz)
            setenv("TZ", m_tz, 1);
        else
            unsetenv("TZ");
    }

private:
    char const* m_tz { nullptr };
};

TEST_CASE(time_zone_from_string)
{
    EXPECT_EQ(TimeZone::time_zone_from_string("America/New_York"sv), TimeZone::TimeZone::America_New_York);
    EXPECT_EQ(TimeZone::time_zone_from_string("Europe/Paris"sv), TimeZone::TimeZone::Europe_Paris);
    EXPECT_EQ(TimeZone::time_zone_from_string("Etc/GMT+2"sv), TimeZone::TimeZone::Etc_GMT_Ahead_2);
    EXPECT_EQ(TimeZone::time_zone_from_string("Etc/GMT-5"sv), TimeZone::TimeZone::Etc_GMT_Behind_5);

    EXPECT(!TimeZone::time_zone_from_string("I don't exist"sv).has_value());
}

TEST_CASE(time_zone_from_string_link)
{
    auto test_link = [](auto tz1, auto tz2) {
        auto result1 = TimeZone::time_zone_from_string(tz1);
        EXPECT(result1.has_value());

        auto result2 = TimeZone::time_zone_from_string(tz2);
        EXPECT(result2.has_value());

        EXPECT_EQ(*result1, *result2);
    };

    test_link("America/New_York"sv, "US/Eastern"sv);

    test_link("Etc/GMT"sv, "GMT"sv);
    test_link("Etc/GMT+0"sv, "GMT"sv);
    test_link("Etc/GMT-0"sv, "GMT"sv);

    test_link("Etc/UTC"sv, "UTC"sv);
    test_link("Etc/Universal"sv, "UTC"sv);
    test_link("Universal"sv, "UTC"sv);
}

TEST_CASE(case_insensitive_time_zone_from_string)
{
    EXPECT_EQ(TimeZone::time_zone_from_string("UTC"sv), TimeZone::TimeZone::UTC);
    EXPECT_EQ(TimeZone::time_zone_from_string("utc"sv), TimeZone::TimeZone::UTC);
    EXPECT_EQ(TimeZone::time_zone_from_string("uTc"sv), TimeZone::TimeZone::UTC);
}

TEST_CASE(time_zone_to_string)
{
    EXPECT_EQ(TimeZone::time_zone_to_string(TimeZone::TimeZone::America_New_York), "America/New_York"sv);
    EXPECT_EQ(TimeZone::time_zone_to_string(TimeZone::TimeZone::Europe_Paris), "Europe/Paris"sv);
    EXPECT_EQ(TimeZone::time_zone_to_string(TimeZone::TimeZone::Etc_GMT_Ahead_2), "Etc/GMT+2"sv);
    EXPECT_EQ(TimeZone::time_zone_to_string(TimeZone::TimeZone::Etc_GMT_Behind_5), "Etc/GMT-5"sv);
}

TEST_CASE(time_zone_to_string_link)
{
    EXPECT_EQ(TimeZone::time_zone_to_string(TimeZone::TimeZone::Etc_UTC), "Etc/UTC"sv);
    EXPECT_EQ(TimeZone::time_zone_to_string(TimeZone::TimeZone::UTC), "Etc/UTC"sv);
    EXPECT_EQ(TimeZone::time_zone_to_string(TimeZone::TimeZone::Universal), "Etc/UTC"sv);
    EXPECT_EQ(TimeZone::time_zone_to_string(TimeZone::TimeZone::Etc_Universal), "Etc/UTC"sv);
}

TEST_CASE(canonicalize_time_zone)
{
    EXPECT_EQ(TimeZone::canonicalize_time_zone("America/New_York"sv), "America/New_York"sv);
    EXPECT_EQ(TimeZone::canonicalize_time_zone("AmErIcA/NeW_YoRk"sv), "America/New_York"sv);

    EXPECT_EQ(TimeZone::canonicalize_time_zone("UTC"sv), "UTC"sv);
    EXPECT_EQ(TimeZone::canonicalize_time_zone("GMT"sv), "UTC"sv);
    EXPECT_EQ(TimeZone::canonicalize_time_zone("GMT+0"sv), "UTC"sv);
    EXPECT_EQ(TimeZone::canonicalize_time_zone("GMT-0"sv), "UTC"sv);
    EXPECT_EQ(TimeZone::canonicalize_time_zone("Etc/UTC"sv), "UTC"sv);
    EXPECT_EQ(TimeZone::canonicalize_time_zone("Etc/GMT"sv), "UTC"sv);

    EXPECT(!TimeZone::canonicalize_time_zone("I don't exist"sv).has_value());
}

TEST_CASE(invalid_time_zone)
{
    TimeZoneGuard guard { "ladybird" };
    EXPECT_EQ(TimeZone::current_time_zone(), "UTC"sv);
}

static i64 offset(i64 sign, i64 hours, i64 minutes, i64 seconds)
{
    return sign * ((hours * 3600) + (minutes * 60) + seconds);
}

TEST_CASE(get_time_zone_offset)
{
    test_offset("America/Chicago"sv, -2717647201, offset(-1, 5, 50, 36), No); // Sunday, November 18, 1883 5:59:59 PM
    test_offset("America/Chicago"sv, -2717647200, offset(-1, 6, 00, 00), No); // Sunday, November 18, 1883 6:00:00 PM
    test_offset("America/Chicago"sv, -1067810460, offset(-1, 6, 00, 00), No); // Sunday, March 1, 1936 1:59:00 AM
    test_offset("America/Chicago"sv, -1067810400, offset(-1, 5, 00, 00), No); // Sunday, March 1, 1936 2:00:00 AM
    test_offset("America/Chicago"sv, -1045432860, offset(-1, 5, 00, 00), No); // Sunday, November 15, 1936 1:59:00 AM
    test_offset("America/Chicago"sv, -1045432800, offset(-1, 6, 00, 00), No); // Sunday, November 15, 1936 2:00:00 AM

    test_offset("Europe/London"sv, -3852662401, offset(-1, 0, 01, 15), No); // Tuesday, November 30, 1847 11:59:59 PM
    test_offset("Europe/London"sv, -3852662400, offset(+1, 0, 00, 00), No); // Wednesday, December 1, 1847 12:00:00 AM
    test_offset("Europe/London"sv, -37238401, offset(+1, 0, 00, 00), No);   // Saturday, October 26, 1968 11:59:59 PM
    test_offset("Europe/London"sv, -37238400, offset(+1, 1, 00, 00), No);   // Sunday, October 27, 1968 12:00:00 AM
    test_offset("Europe/London"sv, 57722399, offset(+1, 1, 00, 00), No);    // Sunday, October 31, 1971 1:59:59 AM
    test_offset("Europe/London"sv, 57722400, offset(+1, 0, 00, 00), No);    // Sunday, October 31, 1971 2:00:00 AM

    test_offset("UTC"sv, -1641846268, offset(+1, 0, 00, 00), No);
    test_offset("UTC"sv, 0, offset(+1, 0, 00, 00), No);
    test_offset("UTC"sv, 1641846268, offset(+1, 0, 00, 00), No);

    test_offset("Etc/GMT+4"sv, -1641846268, offset(-1, 4, 00, 00), No);
    test_offset("Etc/GMT+5"sv, 0, offset(-1, 5, 00, 00), No);
    test_offset("Etc/GMT+6"sv, 1641846268, offset(-1, 6, 00, 00), No);

    test_offset("Etc/GMT-12"sv, -1641846268, offset(+1, 12, 00, 00), No);
    test_offset("Etc/GMT-13"sv, 0, offset(+1, 13, 00, 00), No);
    test_offset("Etc/GMT-14"sv, 1641846268, offset(+1, 14, 00, 00), No);

    EXPECT(!TimeZone::get_time_zone_offset("I don't exist"sv, {}).has_value());
}

TEST_CASE(get_time_zone_offset_with_dst)
{
    test_offset("America/New_York"sv, 1642558528, offset(-1, 5, 00, 00), No);  // Wednesday, January 19, 2022 2:15:28 AM
    test_offset("America/New_York"sv, 1663553728, offset(-1, 4, 00, 00), Yes); // Monday, September 19, 2022 2:15:28 AM
    test_offset("America/New_York"sv, 1671453238, offset(-1, 5, 00, 00), No);  // Monday, December 19, 2022 12:33:58 PM

    // Phoenix does not observe DST.
    test_offset("America/Phoenix"sv, 1642558528, offset(-1, 7, 00, 00), No); // Wednesday, January 19, 2022 2:15:28 AM
    test_offset("America/Phoenix"sv, 1663553728, offset(-1, 7, 00, 00), No); // Monday, September 19, 2022 2:15:28 AM
    test_offset("America/Phoenix"sv, 1671453238, offset(-1, 7, 00, 00), No); // Monday, December 19, 2022 12:33:58 PM

    // Moscow's observed DST changed several times in 1919.
    test_offset("Europe/Moscow"sv, -1609459200, offset(+1, 2, 31, 19), No);  // Wednesday, January 1, 1919 12:00:00 AM
    test_offset("Europe/Moscow"sv, -1596412800, offset(+1, 4, 31, 19), Yes); // Sunday, June 1, 1919 12:00:00 AM
    test_offset("Europe/Moscow"sv, -1592611200, offset(+1, 4, 00, 00), Yes); // Tuesday, July 15, 1919 12:00:00 AM
    test_offset("Europe/Moscow"sv, -1589068800, offset(+1, 3, 00, 00), No);  // Monday, August 25, 1919 12:00:00 AM

    // Paraguay begins the year in DST.
    test_offset("America/Asuncion"sv, 1642558528, offset(-1, 3, 00, 00), Yes); // Wednesday, January 19, 2022 2:15:28 AM
    test_offset("America/Asuncion"sv, 1663553728, offset(-1, 4, 00, 00), No);  // Monday, September 19, 2022 2:15:28 AM
    test_offset("America/Asuncion"sv, 1671453238, offset(-1, 3, 00, 00), Yes); // Monday, December 19, 2022 12:33:58 PM
}

#else

TEST_CASE(time_zone_from_string)
{
    EXPECT(TimeZone::time_zone_from_string("UTC"sv).has_value());

    EXPECT(!TimeZone::time_zone_from_string("Europe/Paris"sv).has_value());
    EXPECT(!TimeZone::time_zone_from_string("Etc/UTC"sv).has_value());
    EXPECT(!TimeZone::time_zone_from_string("I don't exist"sv).has_value());
}

TEST_CASE(get_time_zone_offset)
{
    test_offset("UTC"sv, 123456, 0, No);

    EXPECT(!TimeZone::get_time_zone_offset("Europe/Paris"sv, {}).has_value());
    EXPECT(!TimeZone::get_time_zone_offset("Etc/UTC"sv, {}).has_value());
    EXPECT(!TimeZone::get_time_zone_offset("I don't exist"sv, {}).has_value());
}

#endif
