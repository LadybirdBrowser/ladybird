/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Time.h>
#include <LibJS/Runtime/Date.h>
#include <LibTest/TestCase.h>
#include <LibUnicode/TimeZone.h>

TEST_CASE(unresolvable_named_time_zone_offset_is_utc)
{
    // ICU can't resolve an offset for a bogus identifier — so the lookup returns nothing. The function must treat that
    // as UTC (a zero offset) rather than VERIFY-crashing. Regression test for issue #9884.
    auto offset = JS::get_named_time_zone_offset_milliseconds("Area/DoesNotExist"sv, 0.0);
    EXPECT_EQ(offset.offset.to_milliseconds(), 0);
}
