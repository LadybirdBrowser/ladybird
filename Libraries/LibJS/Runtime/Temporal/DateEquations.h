/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace JS::Temporal {

// 13.3 Date Equations, https://tc39.es/proposal-temporal/#sec-date-equations

u16 mathematical_days_in_year(i32 year);
u8 mathematical_in_leap_year(double time);
double epoch_time_to_day_number(double time);
double epoch_day_number_for_year(double year);
double epoch_time_for_year(double year);
i32 epoch_time_to_epoch_year(double time);
u16 epoch_time_to_day_in_year(double time);
u8 epoch_time_to_week_day(double time);

}
