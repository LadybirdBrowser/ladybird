/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace JS::Temporal {

// 4.5.1 Time Records, https://tc39.es/proposal-temporal/#sec-temporal-time-records
struct Time {
    double days { 0 };
    u8 hour { 0 };
    u8 minute { 0 };
    u8 second { 0 };
    u16 millisecond { 0 };
    u16 microsecond { 0 };
    u16 nanosecond { 0 };
};

Time create_time_record(double hour, double minute, double second, double millisecond, double microsecond, double nanosecond, double delta_days = 0);
Time noon_time_record();
bool is_valid_time(double hour, double minute, double second, double millisecond, double microsecond, double nanosecond);

}
