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
