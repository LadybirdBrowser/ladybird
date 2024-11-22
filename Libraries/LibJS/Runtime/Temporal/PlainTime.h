/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Temporal/ISORecords.h>

namespace JS::Temporal {

Time create_time_record(double hour, double minute, double second, double millisecond, double microsecond, double nanosecond, double delta_days = 0);
Time midnight_time_record();
Time noon_time_record();
bool is_valid_time(double hour, double minute, double second, double millisecond, double microsecond, double nanosecond);
Time balance_time(double hour, double minute, double second, double millisecond, double microsecond, double nanosecond);

}
