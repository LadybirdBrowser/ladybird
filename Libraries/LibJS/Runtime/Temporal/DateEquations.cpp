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

// https://tc39.es/proposal-temporal/#eqn-epochtimetomonthinyear
u8 epoch_time_to_month_in_year(double time)
{
    auto day_in_year = epoch_time_to_day_in_year(time);
    auto in_leap_year = mathematical_in_leap_year(time);

    // EpochTimeToMonthInYear(t)
    //     = 0 if 0 ≤ EpochTimeToDayInYear(t) < 31
    //     = 1 if 31 ≤ EpochTimeToDayInYear(t) < 59 + MathematicalInLeapYear(t)
    //     = 2 if 59 + MathematicalInLeapYear(t) ≤ EpochTimeToDayInYear(t) < 90 + MathematicalInLeapYear(t)
    //     = 3 if 90 + MathematicalInLeapYear(t) ≤ EpochTimeToDayInYear(t) < 120 + MathematicalInLeapYear(t)
    //     = 4 if 120 + MathematicalInLeapYear(t) ≤ EpochTimeToDayInYear(t) < 151 + MathematicalInLeapYear(t)
    //     = 5 if 151 + MathematicalInLeapYear(t) ≤ EpochTimeToDayInYear(t) < 181 + MathematicalInLeapYear(t)
    //     = 6 if 181 + MathematicalInLeapYear(t) ≤ EpochTimeToDayInYear(t) < 212 + MathematicalInLeapYear(t)
    //     = 7 if 212 + MathematicalInLeapYear(t) ≤ EpochTimeToDayInYear(t) < 243 + MathematicalInLeapYear(t)
    //     = 8 if 243 + MathematicalInLeapYear(t) ≤ EpochTimeToDayInYear(t) < 273 + MathematicalInLeapYear(t)
    //     = 9 if 273 + MathematicalInLeapYear(t) ≤ EpochTimeToDayInYear(t) < 304 + MathematicalInLeapYear(t)
    //     = 10 if 304 + MathematicalInLeapYear(t) ≤ EpochTimeToDayInYear(t) < 334 + MathematicalInLeapYear(t)
    //     = 11 if 334 + MathematicalInLeapYear(t) ≤ EpochTimeToDayInYear(t) < 365 + MathematicalInLeapYear(t)
    if (day_in_year < 31)
        return 0;
    if (day_in_year >= 31 && day_in_year < 59 + in_leap_year)
        return 1;
    if (day_in_year >= 59 + in_leap_year && day_in_year < 90 + in_leap_year)
        return 2;
    if (day_in_year >= 90 + in_leap_year && day_in_year < 120 + in_leap_year)
        return 3;
    if (day_in_year >= 120 + in_leap_year && day_in_year < 151 + in_leap_year)
        return 4;
    if (day_in_year >= 151 + in_leap_year && day_in_year < 181 + in_leap_year)
        return 5;
    if (day_in_year >= 181 + in_leap_year && day_in_year < 212 + in_leap_year)
        return 6;
    if (day_in_year >= 212 + in_leap_year && day_in_year < 243 + in_leap_year)
        return 7;
    if (day_in_year >= 243 + in_leap_year && day_in_year < 273 + in_leap_year)
        return 8;
    if (day_in_year >= 273 + in_leap_year && day_in_year < 304 + in_leap_year)
        return 9;
    if (day_in_year >= 304 + in_leap_year && day_in_year < 334 + in_leap_year)
        return 10;
    if (day_in_year >= 334 + in_leap_year && day_in_year < 365 + in_leap_year)
        return 11;
    VERIFY_NOT_REACHED();
}

// https://tc39.es/proposal-temporal/#eqn-epochtimetoweekday
u8 epoch_time_to_week_day(double time)
{
    // EpochTimeToWeekDay(t) = (EpochTimeToDayNumber(t) + 4) modulo 7
    return static_cast<u8>(modulo(epoch_time_to_day_number(time) + 4, 7.0));
}

// https://tc39.es/proposal-temporal/#eqn-epochtimetodate
u8 epoch_time_to_date(double time)
{
    auto day_in_year = epoch_time_to_day_in_year(time);
    auto month_in_year = epoch_time_to_month_in_year(time);
    auto in_leap_year = mathematical_in_leap_year(time);

    // EpochTimeToDate(t)
    //     = EpochTimeToDayInYear(t) + 1 if EpochTimeToMonthInYear(t) = 0
    //     = EpochTimeToDayInYear(t) - 30 if EpochTimeToMonthInYear(t) = 1
    //     = EpochTimeToDayInYear(t) - 58 - MathematicalInLeapYear(t) if EpochTimeToMonthInYear(t) = 2
    //     = EpochTimeToDayInYear(t) - 89 - MathematicalInLeapYear(t) if EpochTimeToMonthInYear(t) = 3
    //     = EpochTimeToDayInYear(t) - 119 - MathematicalInLeapYear(t) if EpochTimeToMonthInYear(t) = 4
    //     = EpochTimeToDayInYear(t) - 150 - MathematicalInLeapYear(t) if EpochTimeToMonthInYear(t) = 5
    //     = EpochTimeToDayInYear(t) - 180 - MathematicalInLeapYear(t) if EpochTimeToMonthInYear(t) = 6
    //     = EpochTimeToDayInYear(t) - 211 - MathematicalInLeapYear(t) if EpochTimeToMonthInYear(t) = 7
    //     = EpochTimeToDayInYear(t) - 242 - MathematicalInLeapYear(t) if EpochTimeToMonthInYear(t) = 8
    //     = EpochTimeToDayInYear(t) - 272 - MathematicalInLeapYear(t) if EpochTimeToMonthInYear(t) = 9
    //     = EpochTimeToDayInYear(t) - 303 - MathematicalInLeapYear(t) if EpochTimeToMonthInYear(t) = 10
    //     = EpochTimeToDayInYear(t) - 333 - MathematicalInLeapYear(t) if EpochTimeToMonthInYear(t) = 11
    if (month_in_year == 0)
        return day_in_year + 1;
    if (month_in_year == 1)
        return day_in_year - 30;
    if (month_in_year == 2)
        return day_in_year - 58 - in_leap_year;
    if (month_in_year == 3)
        return day_in_year - 89 - in_leap_year;
    if (month_in_year == 4)
        return day_in_year - 119 - in_leap_year;
    if (month_in_year == 5)
        return day_in_year - 150 - in_leap_year;
    if (month_in_year == 6)
        return day_in_year - 180 - in_leap_year;
    if (month_in_year == 7)
        return day_in_year - 211 - in_leap_year;
    if (month_in_year == 8)
        return day_in_year - 242 - in_leap_year;
    if (month_in_year == 9)
        return day_in_year - 272 - in_leap_year;
    if (month_in_year == 10)
        return day_in_year - 303 - in_leap_year;
    if (month_in_year == 11)
        return day_in_year - 333 - in_leap_year;
    VERIFY_NOT_REACHED();
}

}
