/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/Temporal/DateEquations.h>

namespace JS::Temporal {

// https://tc39.es/proposal-temporal/#eqn-mathematicaldaysinyear
u16 mathematical_days_in_year(i32 year)
{
    // MathematicalDaysInYear(y)
    //     = 365 if ((y) modulo 4) ≠ 0
    //     = 366 if ((y) modulo 4) = 0 and ((y) modulo 100) ≠ 0
    //     = 365 if ((y) modulo 100) = 0 and ((y) modulo 400) ≠ 0
    //     = 366 if ((y) modulo 400) = 0
    if (modulo(year, 4) != 0)
        return 365;
    if (modulo(year, 4) == 0 && modulo(year, 100) != 0)
        return 366;
    if (modulo(year, 100) == 0 && modulo(year, 400) != 0)
        return 365;
    if (modulo(year, 400) == 0)
        return 366;
    VERIFY_NOT_REACHED();
}

// https://tc39.es/proposal-temporal/#eqn-mathematicalinleapyear
u8 mathematical_in_leap_year(double time)
{
    // MathematicalInLeapYear(t)
    //     = 0 if MathematicalDaysInYear(EpochTimeToEpochYear(t)) = 365
    //     = 1 if MathematicalDaysInYear(EpochTimeToEpochYear(t)) = 366
    auto days_in_year = mathematical_days_in_year(epoch_time_to_epoch_year(time));

    if (days_in_year == 365)
        return 0;
    if (days_in_year == 366)
        return 1;
    VERIFY_NOT_REACHED();
}

// https://tc39.es/proposal-temporal/#eqn-EpochTimeToDayNumber
double epoch_time_to_day_number(double time)
{
    // EpochTimeToDayNumber(t) = floor(t / ℝ(msPerDay))
    return floor(time / JS::ms_per_day);
}

// https://tc39.es/proposal-temporal/#eqn-epochdaynumberforyear
double epoch_day_number_for_year(double year)
{
    // EpochDayNumberForYear(y) = 365 × (y - 1970) + floor((y - 1969) / 4) - floor((y - 1901) / 100) + floor((y - 1601) / 400)
    return 365.0 * (year - 1970.0) + floor((year - 1969.0) / 4.0) - floor((year - 1901.0) / 100.0) + floor((year - 1601.0) / 400.0);
}

// https://tc39.es/proposal-temporal/#eqn-epochtimeforyear
double epoch_time_for_year(double year)
{
    // EpochTimeForYear(y) = ℝ(msPerDay) × EpochDayNumberForYear(y)
    return ms_per_day * epoch_day_number_for_year(year);
}

// https://tc39.es/proposal-temporal/#eqn-epochtimetoepochyear
i32 epoch_time_to_epoch_year(double time)
{
    // EpochTimeToEpochYear(t) = the largest integral Number y (closest to +∞) such that EpochTimeForYear(y) ≤ t
    return JS::year_from_time(time);
}

// https://tc39.es/proposal-temporal/#eqn-epochtimetodayinyear
u16 epoch_time_to_day_in_year(double time)
{
    // EpochTimeToDayInYear(t) = EpochTimeToDayNumber(t) - EpochDayNumberForYear(EpochTimeToEpochYear(t))
    return static_cast<u16>(epoch_time_to_day_number(time) - epoch_day_number_for_year(epoch_time_to_epoch_year(time)));
}

// https://tc39.es/proposal-temporal/#eqn-epochtimetoweekday
u8 epoch_time_to_week_day(double time)
{
    // EpochTimeToWeekDay(t) = (EpochTimeToDayNumber(t) + 4) modulo 7
    return static_cast<u8>(modulo(epoch_time_to_day_number(time) + 4, 7.0));
}

}
