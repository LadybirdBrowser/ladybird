/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/StringView.h>
#include <LibCore/Environment.h>
#include <LibUnicode/TimeZone.h>

class TimeZoneGuard {
public:
    explicit TimeZoneGuard(StringView time_zone)
        : m_time_zone(Core::Environment::get("TZ"sv))
    {
        MUST(Core::Environment::set("TZ"sv, time_zone, Core::Environment::Overwrite::Yes));
    }

    ~TimeZoneGuard()
    {
        if (m_time_zone.has_value())
            MUST(Core::Environment::set("TZ"sv, *m_time_zone, Core::Environment::Overwrite::Yes));
        else
            MUST(Core::Environment::unset("TZ"sv));
    }

private:
    Optional<StringView> m_time_zone;
};

TEST_CASE(current_time_zone)
{
    {
        TimeZoneGuard guard { "America/New_York"sv };
        EXPECT_EQ(Unicode::current_time_zone(), "America/New_York"sv);
    }
    {
        TimeZoneGuard guard { "ladybird"sv };
        EXPECT_EQ(Unicode::current_time_zone(), "UTC"sv);
    }
}

TEST_CASE(available_time_zones)
{
    auto const& time_zones = Unicode::available_time_zones();
    EXPECT(time_zones.contains_slow("UTC"sv));
    EXPECT(!time_zones.contains_slow("EAT"sv));
}

TEST_CASE(available_time_zones_in_region)
{
    {
        auto time_zones = Unicode::available_time_zones_in_region("AD"sv);
        EXPECT_EQ(time_zones, to_array({ "Europe/Andorra"_string }));
    }
    {
        auto time_zones = Unicode::available_time_zones_in_region("ES"sv);
        EXPECT_EQ(time_zones, to_array({ "Africa/Ceuta"_string, "Atlantic/Canary"_string, "Europe/Madrid"_string }));
    }
}

TEST_CASE(resolve_primary_time_zone)
{
    EXPECT_EQ(Unicode::resolve_primary_time_zone("UTC"sv), "Etc/UTC"sv);
    EXPECT_EQ(Unicode::resolve_primary_time_zone("Asia/Katmandu"sv), "Asia/Kathmandu"sv);
    EXPECT_EQ(Unicode::resolve_primary_time_zone("Australia/Canberra"sv), "Australia/Sydney"sv);
}

using enum Unicode::TimeZoneOffset::InDST;

static void test_offset(StringView time_zone, i64 time, AK::Duration expected_offset, Unicode::TimeZoneOffset::InDST expected_in_dst)
{
    auto actual_offset = Unicode::time_zone_offset(time_zone, AK::UnixDateTime::from_seconds_since_epoch(time));
    VERIFY(actual_offset.has_value());

    EXPECT_EQ(actual_offset->offset, expected_offset);
    EXPECT_EQ(actual_offset->in_dst, expected_in_dst);
}

static constexpr AK::Duration offset(i64 sign, i64 hours, i64 minutes, i64 seconds)
{
    return AK::Duration::from_seconds(sign * ((hours * 3600) + (minutes * 60) + seconds));
}

// Useful website to convert times in the TZDB (which sometimes are and aren't UTC) to UTC and the desired local time:
// https://www.epochconverter.com/#tools
//
// In the tests below, if only UTC time is shown as a comment, then the corresponding Rule change in the TZDB was specified
// as UTC. Otherwise, the TZDB time was local, and was converted to a UTC timestamp for that test.
TEST_CASE(time_zone_offset)
{
    EXPECT(!Unicode::time_zone_offset("I don't exist"sv, {}).has_value());

    test_offset("America/Chicago"sv, -2717647201, offset(-1, 5, 50, 36), No); // November 18, 1883 5:59:59 PM UTC
    test_offset("America/Chicago"sv, -2717647200, offset(-1, 6, 0, 0), No);   // November 18, 1883 6:00:00 PM UTC
    test_offset("America/Chicago"sv, -1067788860, offset(-1, 6, 0, 0), No);   // March 1, 1936 1:59:00 AM Chicago (March 1, 1936 7:59:00 AM UTC)
    test_offset("America/Chicago"sv, -1067788800, offset(-1, 5, 0, 0), No);   // March 1, 1936 3:00:00 AM Chicago (March 1, 1936 8:00:00 AM UTC)
    test_offset("America/Chicago"sv, -1045414860, offset(-1, 5, 0, 0), No);   // November 15, 1936 1:59:00 AM Chicago (November 15, 1936 6:59:00 AM UTC)
    test_offset("America/Chicago"sv, -1045411200, offset(-1, 6, 0, 0), No);   // November 15, 1936 2:00:00 AM Chicago (November 15, 1936 8:00:00 AM UTC)

    test_offset("Europe/London"sv, -3852662326, offset(-1, 0, 1, 15), No); // November 30, 1847 11:59:59 PM London (December 1, 1847 12:01:14 AM UTC)
    test_offset("Europe/London"sv, -3852662325, offset(+1, 0, 0, 0), No);  // December 1, 1847 12:01:15 AM London (December 1, 1847 12:01:15 AM UTC)
    test_offset("Europe/London"sv, -59004001, offset(+1, 0, 0, 0), No);    // February 18, 1968 1:59:59 AM London (February 18, 1968 1:59:59 AM UTC)
    test_offset("Europe/London"sv, -59004000, offset(+1, 1, 0, 0), Yes);   // February 18, 1968 3:00:00 AM London (February 18, 1968 2:00:00 AM UTC)
    test_offset("Europe/London"sv, 57722399, offset(+1, 1, 0, 0), No);     // October 31, 1971 1:59:59 AM UTC
    test_offset("Europe/London"sv, 57722400, offset(+1, 0, 0, 0), No);     // October 31, 1971 2:00:00 AM UTC

    test_offset("UTC"sv, -1641846268, offset(+1, 0, 00, 00), No);
    test_offset("UTC"sv, 0, offset(+1, 0, 00, 00), No);
    test_offset("UTC"sv, 1641846268, offset(+1, 0, 00, 00), No);

    test_offset("Etc/GMT+4"sv, -1641846268, offset(-1, 4, 00, 00), No);
    test_offset("Etc/GMT+5"sv, 0, offset(-1, 5, 00, 00), No);
    test_offset("Etc/GMT+6"sv, 1641846268, offset(-1, 6, 00, 00), No);

    test_offset("Etc/GMT-12"sv, -1641846268, offset(+1, 12, 00, 00), No);
    test_offset("Etc/GMT-13"sv, 0, offset(+1, 13, 00, 00), No);
    test_offset("Etc/GMT-14"sv, 1641846268, offset(+1, 14, 00, 00), No);
}

TEST_CASE(time_zone_offset_with_dst)
{
    test_offset("America/New_York"sv, 1642576528, offset(-1, 5, 00, 00), No);  // January 19, 2022 2:15:28 AM New York (January 19, 2022 7:15:28 AM UTC)
    test_offset("America/New_York"sv, 1663568128, offset(-1, 4, 00, 00), Yes); // September 19, 2022 2:15:28 AM New York (September 19, 2022 6:15:28 AM UTC)
    test_offset("America/New_York"sv, 1671471238, offset(-1, 5, 00, 00), No);  // December 19, 2022 12:33:58 PM New York (December 19, 2022 5:33:58 PM UTC)

    // Phoenix does not observe DST.
    test_offset("America/Phoenix"sv, 1642583728, offset(-1, 7, 00, 00), No); // January 19, 2022 2:15:28 AM Phoenix (January 19, 2022 9:15:28 AM UTC)
    test_offset("America/Phoenix"sv, 1663578928, offset(-1, 7, 00, 00), No); // September 19, 2022 2:15:28 AM Phoenix (September 19, 2022 9:15:28 AM UTC)
    test_offset("America/Phoenix"sv, 1671478438, offset(-1, 7, 00, 00), No); // December 19, 2022 12:33:58 PM Phoenix (December 19, 2022 7:33:58 PM UTC)

    // Moscow's observed DST changed several times in 1919.
    test_offset("Europe/Moscow"sv, -1609459200, offset(+1, 3, 31, 19), Yes); // January 1, 1919 12:00:00 AM UTC
    test_offset("Europe/Moscow"sv, -1596429079, offset(+1, 4, 31, 19), Yes); // June 1, 1919 12:00:00 AM Moscow (May 31, 1919 7:28:41 PM UTC)
    test_offset("Europe/Moscow"sv, -1592625600, offset(+1, 4, 00, 00), Yes); // July 15, 1919 12:00:00 AM Moscow (July 14, 1919 8:00:00 PM UTC)
    test_offset("Europe/Moscow"sv, -1589079600, offset(+1, 3, 00, 00), No);  // August 25, 1919 12:00:00 AM Moscow (August 24, 1919 9:00:00 PM UTC)

    // Paraguay begins the year in DST.
    test_offset("America/Asuncion"sv, 1642569328, offset(-1, 3, 00, 00), Yes); // January 19, 2022 2:15:28 AM Asuncion (January 19, 2022 5:15:28 AM UTC)
    test_offset("America/Asuncion"sv, 1663568128, offset(-1, 4, 00, 00), No);  // September 19, 2022 2:15:28 AM Asuncion (September 19, 2022 6:15:28 AM UTC)
    test_offset("America/Asuncion"sv, 1671464038, offset(-1, 3, 00, 00), Yes); // December 19, 2022 12:33:58 PM Asuncion (December 19, 2022 3:33:58 PM UTC)
}
