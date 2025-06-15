/*
 * Copyright (c) 2025, Tomasz Strejczek.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Time.h>
#include <LibTest/TestCase.h>

TEST_CASE(time_to_string)
{
    auto test = [](auto format, auto expected, i32 year, u8 month, u8 day, u8 hour, u8 minute, u8 second) {
        auto result = AK::UnixDateTime::from_unix_time_parts(year, month, day, hour, minute, second, 0).to_string(format);
        VERIFY(!result.is_error());

        EXPECT_EQ(expected, result.value());
    };

    test("%Y/%m/%d %R"sv, "2023/01/23 10:50"sv, 2023, 1, 23, 10, 50, 10);

    // two-digit year and century
    test("%y %C"sv, "23 20"sv, 2023, 1, 23, 10, 50, 10);

    // zero- and space-padded day, and %D shortcut
    test("%d %e"sv, "05  5"sv, 2023, 1, 5, 0, 0, 0);
    test("%D"sv, "01/23/23"sv, 2023, 1, 23, 0, 0, 0);

    // full time and seconds
    test("%T"sv, "10:50:10"sv, 2023, 1, 23, 10, 50, 10);
    test("%S"sv, "05"sv, 2023, 1, 1, 0, 0, 5);

    // 12-hour clock with AM/PM
    test("%H %I %p"sv, "00 12 AM"sv, 2023, 1, 5, 0, 0, 0);
    test("%H %I %p"sv, "15 03 PM"sv, 2023, 1, 5, 15, 0, 0);

    // short/long weekday and month names
    test("%a %A"sv, "Mon Monday"sv, 2023, 1, 23, 0, 0, 0);
    test("%b %B"sv, "Jan January"sv, 2023, 1, 5, 0, 0, 0);

    // numeric weekday and day‐of‐year
    test("%w %j"sv, "1 023"sv, 2023, 1, 23, 0, 0, 0);

    // newline, tab and literal '%'
    test("%n"sv, "\n"sv, 2023, 1, 1, 0, 0, 0);
    test("%t"sv, "\t"sv, 2023, 1, 1, 0, 0, 0);
    test("%%"sv, "%"sv, 2023, 1, 1, 0, 0, 0);
}
