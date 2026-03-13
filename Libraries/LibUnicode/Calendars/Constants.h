/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Unicode {

// https://github.com/unicode-org/icu/blob/main/icu4c/source/i18n/gregoimp.h#L127
constexpr inline i64 EPOCH_START_AS_JULIAN_DAY = 2440588;

// https://en.wikipedia.org/wiki/Chinese_calendar_correspondence_table
constexpr inline i32 CHINESE_CALENDAR_FIRST_YEAR = -2637;

// https://en.wikipedia.org/wiki/Dangun_calendar
constexpr inline i32 DANGI_CALENDAR_FIRST_YEAR = -2333;

}
