/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <LibCore/DateTime.h>
#include <LibCore/Environment.h>
#include <LibTest/TestCase.h>
#include <LibUnicode/TimeZone.h>
#include <time.h>

class TimeZoneGuard {
public:
    explicit TimeZoneGuard(StringView time_zone)
    {
        if (auto current_time_zone = Core::Environment::get("TZ"sv); current_time_zone.has_value())
            m_time_zone = MUST(String::from_utf8(*current_time_zone));

        update(time_zone);
    }

    ~TimeZoneGuard()
    {
        if (m_time_zone.has_value())
            TRY_OR_FAIL(Core::Environment::set("TZ"sv, *m_time_zone, Core::Environment::Overwrite::Yes));
        else
            TRY_OR_FAIL(Core::Environment::unset("TZ"sv));

        Unicode::clear_system_time_zone_cache();
        tzset();
    }

    void update(StringView time_zone)
    {
        TRY_OR_FAIL(Core::Environment::set("TZ"sv, time_zone, Core::Environment::Overwrite::Yes));
        Unicode::clear_system_time_zone_cache();
        tzset();
    }

private:
    Optional<String> m_time_zone;
};

TEST_CASE(parse_time_zone_name)
{
    EXPECT(!Core::DateTime::parse("%Z"sv, ""sv).has_value());
    EXPECT(!Core::DateTime::parse("%Z"sv, "123"sv).has_value());
    EXPECT(!Core::DateTime::parse("%Z"sv, "notatimezone"sv).has_value());

    auto test = [](auto format, auto time, u32 year, u32 month, u32 day, u32 hour, u32 minute) {
        auto result = Core::DateTime::parse(format, time);
        VERIFY(result.has_value());

        EXPECT_EQ(year, result->year());
        EXPECT_EQ(month, result->month());
        EXPECT_EQ(day, result->day());
        EXPECT_EQ(hour, result->hour());
        EXPECT_EQ(minute, result->minute());
    };

    TimeZoneGuard guard { "UTC"sv };
    test("%Y/%m/%d %R %Z"sv, "2023/01/23 10:50 UTC"sv, 2023, 01, 23, 10, 50);
    test("%Y/%m/%d %R %Z"sv, "2023/01/23 10:50 America/New_York"sv, 2023, 01, 23, 15, 50);
    test("%Y/%m/%d %R %Z"sv, "2023/01/23 10:50 Europe/Paris"sv, 2023, 01, 23, 9, 50);
    test("%Y/%m/%d %R %Z"sv, "2023/01/23 10:50 Australia/Perth"sv, 2023, 01, 23, 2, 50);

    guard.update("America/New_York"sv);
    test("%Y/%m/%d %R %Z"sv, "2023/01/23 10:50 UTC"sv, 2023, 01, 23, 5, 50);
    test("%Y/%m/%d %R %Z"sv, "2023/01/23 10:50 America/New_York"sv, 2023, 01, 23, 10, 50);
    test("%Y/%m/%d %R %Z"sv, "2023/01/23 10:50 Europe/Paris"sv, 2023, 01, 23, 4, 50);
    test("%Y/%m/%d %R %Z"sv, "2023/01/23 10:50 Australia/Perth"sv, 2023, 01, 22, 21, 50);

    guard.update("Europe/Paris"sv);
    test("%Y/%m/%d %R %Z"sv, "2023/01/23 10:50 UTC"sv, 2023, 01, 23, 11, 50);
    test("%Y/%m/%d %R %Z"sv, "2023/01/23 10:50 America/New_York"sv, 2023, 01, 23, 16, 50);
    test("%Y/%m/%d %R %Z"sv, "2023/01/23 10:50 Europe/Paris"sv, 2023, 01, 23, 10, 50);
    test("%Y/%m/%d %R %Z"sv, "2023/01/23 10:50 Australia/Perth"sv, 2023, 01, 23, 3, 50);

    guard.update("Australia/Perth"sv);
    test("%Y/%m/%d %R %Z"sv, "2023/01/23 10:50 UTC"sv, 2023, 01, 23, 18, 50);
    test("%Y/%m/%d %R %Z"sv, "2023/01/23 10:50 America/New_York"sv, 2023, 01, 23, 23, 50);
    test("%Y/%m/%d %R %Z"sv, "2023/01/23 10:50 Europe/Paris"sv, 2023, 01, 23, 17, 50);
    test("%Y/%m/%d %R %Z"sv, "2023/01/23 10:50 Australia/Perth"sv, 2023, 01, 23, 10, 50);
}

TEST_CASE(parse_wildcard_characters)
{
    EXPECT(!Core::DateTime::parse("%+"sv, ""sv).has_value());
    EXPECT(!Core::DateTime::parse("foo%+"sv, "foo"sv).has_value());
    EXPECT(!Core::DateTime::parse("[%*]"sv, "[foo"sv).has_value());
    EXPECT(!Core::DateTime::parse("[%*]"sv, "foo]"sv).has_value());
    EXPECT(!Core::DateTime::parse("%+%b"sv, "fooJan"sv).has_value());

    auto test = [](auto format, auto time, u32 year, u32 month, u32 day) {
        auto result = Core::DateTime::parse(format, time);
        VERIFY(result.has_value());

        EXPECT_EQ(year, result->year());
        EXPECT_EQ(month, result->month());
        EXPECT_EQ(day, result->day());
    };

    test("%Y %+ %m %d"sv, "2023 whf 01 23"sv, 2023, 01, 23);
    test("%Y %m %d %+"sv, "2023 01 23 whf"sv, 2023, 01, 23);
    test("%Y [%+] %m %d"sv, "2023 [well hello friends!] 01 23"sv, 2023, 01, 23);
    test("%Y %m %d [%+]"sv, "2023 01 23 [well hello friends!]"sv, 2023, 01, 23);
}
