/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Temporal/PlainDate.h>
#include <LibJS/Runtime/Temporal/PlainTime.h>

namespace JS::Temporal {

// 5.5.1 ISO Date-Time Records, https://tc39.es/proposal-temporal/#sec-temporal-iso-date-time-records
struct ISODateTime {
    ISODate iso_date;
    Time time;
};

ISODateTime combine_iso_date_and_time_record(ISODate, Time);
bool iso_date_time_within_limits(ISODateTime);

}
