/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! 13.3 Date Equations, https://tc39.es/proposal-temporal/#sec-date-equations

use crate::runtime::abstract_operations::{integer_modulo, modulo};
use crate::runtime::date::{MS_PER_DAY, year_from_time};

// https://tc39.es/proposal-temporal/#eqn-mathematicaldaysinyear
pub fn mathematical_days_in_year(year: i32) -> u16 {
    let year = i64::from(year);

    // MathematicalDaysInYear(y)
    //     = 365 if ((y) modulo 4) ≠ 0
    //     = 366 if ((y) modulo 4) = 0 and ((y) modulo 100) ≠ 0
    //     = 365 if ((y) modulo 100) = 0 and ((y) modulo 400) ≠ 0
    //     = 366 if ((y) modulo 400) = 0
    if integer_modulo(year, 4) != 0 {
        return 365;
    }
    if integer_modulo(year, 4) == 0 && integer_modulo(year, 100) != 0 {
        return 366;
    }
    if integer_modulo(year, 100) == 0 && integer_modulo(year, 400) != 0 {
        return 365;
    }
    if integer_modulo(year, 400) == 0 {
        return 366;
    }
    unreachable!()
}

// https://tc39.es/proposal-temporal/#eqn-mathematicalinleapyear
pub fn mathematical_in_leap_year(time: f64) -> u8 {
    // MathematicalInLeapYear(t)
    //     = 0 if MathematicalDaysInYear(EpochTimeToEpochYear(t)) = 365
    //     = 1 if MathematicalDaysInYear(EpochTimeToEpochYear(t)) = 366
    let days_in_year = mathematical_days_in_year(epoch_time_to_epoch_year(time));

    if days_in_year == 365 {
        return 0;
    }
    if days_in_year == 366 {
        return 1;
    }
    unreachable!()
}

// https://tc39.es/proposal-temporal/#eqn-EpochTimeToDayNumber
pub fn epoch_time_to_day_number(time: f64) -> f64 {
    // EpochTimeToDayNumber(t) = floor(t / ℝ(msPerDay))
    (time / MS_PER_DAY).floor()
}

// https://tc39.es/proposal-temporal/#eqn-epochdaynumberforyear
pub fn epoch_day_number_for_year(year: f64) -> f64 {
    // EpochDayNumberForYear(y) = 365 × (y - 1970) + floor((y - 1969) / 4) - floor((y - 1901) / 100) + floor((y - 1601) / 400)
    365.0 * (year - 1970.0) + ((year - 1969.0) / 4.0).floor() - ((year - 1901.0) / 100.0).floor()
        + ((year - 1601.0) / 400.0).floor()
}

// https://tc39.es/proposal-temporal/#eqn-epochtimeforyear
pub fn epoch_time_for_year(year: f64) -> f64 {
    // EpochTimeForYear(y) = ℝ(msPerDay) × EpochDayNumberForYear(y)
    MS_PER_DAY * epoch_day_number_for_year(year)
}

// https://tc39.es/proposal-temporal/#eqn-epochtimetoepochyear
pub fn epoch_time_to_epoch_year(time: f64) -> i32 {
    // EpochTimeToEpochYear(t) = the largest integral Number y (closest to +∞) such that EpochTimeForYear(y) ≤ t
    year_from_time(time)
}

// https://tc39.es/proposal-temporal/#eqn-epochtimetodayinyear
pub fn epoch_time_to_day_in_year(time: f64) -> u16 {
    // EpochTimeToDayInYear(t) = EpochTimeToDayNumber(t) - EpochDayNumberForYear(EpochTimeToEpochYear(t))
    (epoch_time_to_day_number(time) - epoch_day_number_for_year(f64::from(epoch_time_to_epoch_year(time)))) as u16
}

// https://tc39.es/proposal-temporal/#eqn-epochtimetomonthinyear
pub fn epoch_time_to_month_in_year(time: f64) -> u8 {
    let day_in_year = epoch_time_to_day_in_year(time);
    let in_leap_year = u16::from(mathematical_in_leap_year(time));

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
    if day_in_year < 31 {
        return 0;
    }
    if (31..59 + in_leap_year).contains(&day_in_year) {
        return 1;
    }
    if (59 + in_leap_year..90 + in_leap_year).contains(&day_in_year) {
        return 2;
    }
    if (90 + in_leap_year..120 + in_leap_year).contains(&day_in_year) {
        return 3;
    }
    if (120 + in_leap_year..151 + in_leap_year).contains(&day_in_year) {
        return 4;
    }
    if (151 + in_leap_year..181 + in_leap_year).contains(&day_in_year) {
        return 5;
    }
    if (181 + in_leap_year..212 + in_leap_year).contains(&day_in_year) {
        return 6;
    }
    if (212 + in_leap_year..243 + in_leap_year).contains(&day_in_year) {
        return 7;
    }
    if (243 + in_leap_year..273 + in_leap_year).contains(&day_in_year) {
        return 8;
    }
    if (273 + in_leap_year..304 + in_leap_year).contains(&day_in_year) {
        return 9;
    }
    if (304 + in_leap_year..334 + in_leap_year).contains(&day_in_year) {
        return 10;
    }
    if (334 + in_leap_year..365 + in_leap_year).contains(&day_in_year) {
        return 11;
    }
    unreachable!()
}

// https://tc39.es/proposal-temporal/#eqn-epochtimetoweekday
pub fn epoch_time_to_week_day(time: f64) -> u8 {
    // EpochTimeToWeekDay(t) = (EpochTimeToDayNumber(t) + 4) modulo 7
    modulo(epoch_time_to_day_number(time) + 4.0, 7.0) as u8
}

// https://tc39.es/proposal-temporal/#eqn-epochtimetodate
pub fn epoch_time_to_date(time: f64) -> u8 {
    let day_in_year = u32::from(epoch_time_to_day_in_year(time));
    let month_in_year = epoch_time_to_month_in_year(time);
    let in_leap_year = u32::from(mathematical_in_leap_year(time));

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
    let date = match month_in_year {
        0 => day_in_year + 1,
        1 => day_in_year.wrapping_sub(30),
        2 => day_in_year.wrapping_sub(58).wrapping_sub(in_leap_year),
        3 => day_in_year.wrapping_sub(89).wrapping_sub(in_leap_year),
        4 => day_in_year.wrapping_sub(119).wrapping_sub(in_leap_year),
        5 => day_in_year.wrapping_sub(150).wrapping_sub(in_leap_year),
        6 => day_in_year.wrapping_sub(180).wrapping_sub(in_leap_year),
        7 => day_in_year.wrapping_sub(211).wrapping_sub(in_leap_year),
        8 => day_in_year.wrapping_sub(242).wrapping_sub(in_leap_year),
        9 => day_in_year.wrapping_sub(272).wrapping_sub(in_leap_year),
        10 => day_in_year.wrapping_sub(303).wrapping_sub(in_leap_year),
        11 => day_in_year.wrapping_sub(333).wrapping_sub(in_leap_year),
        _ => unreachable!(),
    };
    date as u8
}
